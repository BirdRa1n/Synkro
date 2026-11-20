// Notificações externas: configuração, montagem das mensagens, fila no banco
// de estado e entrega por HTTP e SMTP (servidores falsos em 127.0.0.1).

#include "config/ConfigLoader.h"
#include "config/ConfigWriter.h"
#include "notify/Notifier.h"
#include "state/Alerts.h"
#include "state/StateStore.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>

#include <atomic>
#include <mutex>
#include <thread>

using namespace synkro;

namespace {

std::vector<config::Diagnostic> errorsOf(const std::vector<config::Diagnostic>& all)
{
    std::vector<config::Diagnostic> out;
    for (const auto& d : all)
        if (d.severity == config::Severity::Error)
            out.push_back(d);
    return out;
}

bool hasErrorAt(const std::vector<config::Diagnostic>& all, const std::string& key)
{
    for (const auto& d : all)
        if (d.severity == config::Severity::Error && d.key == key)
            return true;
    return false;
}

const char* kGlobal =
    "version: 1\n"
    "instance: { name: \"Loja Centro\" }\n"
    "vaults: []\n"
    "notifications:\n"
    "  repeat_every: 12h\n"
    "  channels:\n"
    "    - id: suporte\n"
    "      type: email\n"
    "      min_severity: critical\n"
    "      daily_report: \"08:30\"\n"
    "      smtp: { host: smtp.example.com, port: 587, security: starttls, user: backup@example.com,\n"
    "              password: { secret: smtp-suporte } }\n"
    "      from: backup@example.com\n"
    "      to: [ti@example.com, dono@example.com]\n"
    "    - id: central\n"
    "      type: webhook\n"
    "      url: https://monitor.example.com/synkro\n"
    "      format: teams\n"
    "      token: { secret: central-token }\n"
    "      resolved: false\n"
    "    - id: celular\n"
    "      type: telegram\n"
    "      bot_token: { secret: telegram-bot }\n"
    "      chat_id: \"-1001234\"\n";

// Servidor HTTP mínimo: atende `count` requisições e responde `status`.
class FakeHttp {
public:
    explicit FakeHttp(int status, int count) : m_status(status)
    {
        m_thread = std::thread([this, count] {
            QTcpServer server;
            if (!server.listen(QHostAddress::LocalHost, 0)) {
                m_port = -1;
                return;
            }
            m_port = server.serverPort();
            for (int i = 0; i < count; ++i) {
                if (!server.waitForNewConnection(15000))
                    return;
                std::unique_ptr<QTcpSocket> s(server.nextPendingConnection());
                QByteArray data;
                int headerEnd = -1;
                qsizetype length = 0;
                while (s->waitForReadyRead(5000)) {
                    data += s->readAll();
                    if (headerEnd < 0 && (headerEnd = static_cast<int>(data.indexOf("\r\n\r\n"))) >= 0) {
                        for (const auto& line : data.left(headerEnd).split('\n'))
                            if (line.toLower().startsWith("content-length:"))
                                length = line.mid(15).trimmed().toLongLong();
                    }
                    if (headerEnd >= 0 && data.size() >= headerEnd + 4 + length)
                        break;
                }
                {
                    std::lock_guard<std::mutex> g(m_mutex);
                    m_requests.push_back(data);
                }
                const QByteArray reply = "HTTP/1.1 " + QByteArray::number(m_status)
                                         + " X\r\nContent-Type: application/json\r\nContent-Length: 11\r\n"
                                           "Connection: close\r\n\r\n{\"ok\":true}";
                s->write(reply);
                s->waitForBytesWritten(5000);
                s->disconnectFromHost();
                if (s->state() != QAbstractSocket::UnconnectedState)
                    s->waitForDisconnected(2000);
            }
        });
        while (m_port == 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ~FakeHttp() { m_thread.join(); }

    int port() const { return m_port; }
    std::vector<QByteArray> requests()
    {
        std::lock_guard<std::mutex> g(m_mutex);
        return m_requests;
    }

private:
    int m_status;
    std::atomic<int> m_port{0};
    std::thread m_thread;
    std::mutex m_mutex;
    std::vector<QByteArray> m_requests;
};

// Servidor SMTP mínimo (sem TLS), com AUTH PLAIN.
class FakeSmtp {
public:
    FakeSmtp()
    {
        m_thread = std::thread([this] {
            QTcpServer server;
            if (!server.listen(QHostAddress::LocalHost, 0)) {
                m_port = -1;
                return;
            }
            m_port = server.serverPort();
            if (!server.waitForNewConnection(15000))
                return;
            std::unique_ptr<QTcpSocket> s(server.nextPendingConnection());
            auto send = [&](const QByteArray& b) {
                s->write(b);
                s->waitForBytesWritten(5000);
            };
            auto readLine = [&]() -> QByteArray {
                while (!s->canReadLine())
                    if (!s->waitForReadyRead(5000))
                        return {};
                return s->readLine();
            };
            send("220 fake ESMTP\r\n");
            for (;;) {
                const QByteArray line = readLine();
                if (line.isEmpty())
                    return;
                std::lock_guard<std::mutex> g(m_mutex);
                m_commands.push_back(line.trimmed());
                const QByteArray up = line.toUpper();
                if (up.startsWith("EHLO")) {
                    send("250-fake\r\n250-AUTH PLAIN LOGIN\r\n250 OK\r\n");
                } else if (up.startsWith("AUTH PLAIN")) {
                    m_auth = QByteArray::fromBase64(line.mid(11).trimmed());
                    send("235 ok\r\n");
                } else if (up.startsWith("MAIL") || up.startsWith("RCPT")) {
                    send("250 ok\r\n");
                } else if (up.startsWith("DATA")) {
                    send("354 go\r\n");
                    QByteArray body;
                    for (;;) {
                        const QByteArray l = readLine();
                        if (l.isEmpty() || l == ".\r\n")
                            break;
                        body += l;
                    }
                    m_data = body;
                    send("250 queued\r\n");
                } else if (up.startsWith("QUIT")) {
                    send("221 bye\r\n");
                    s->disconnectFromHost();
                    return;
                } else {
                    send("500 ?\r\n");
                }
            }
        });
        while (m_port == 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ~FakeSmtp() { m_thread.join(); }

    int port() const { return m_port; }
    QByteArray auth()
    {
        std::lock_guard<std::mutex> g(m_mutex);
        return m_auth;
    }
    QByteArray data()
    {
        std::lock_guard<std::mutex> g(m_mutex);
        return m_data;
    }
    std::vector<QByteArray> commands()
    {
        std::lock_guard<std::mutex> g(m_mutex);
        return m_commands;
    }

private:
    std::atomic<int> m_port{0};
    std::thread m_thread;
    std::mutex m_mutex;
    QByteArray m_auth, m_data;
    std::vector<QByteArray> m_commands;
};

config::NotificationChannel webhookChannel(int port)
{
    config::NotificationChannel ch;
    ch.id = "central";
    ch.type = config::ChannelType::Webhook;
    ch.url = "http://127.0.0.1:" + std::to_string(port) + "/hook";
    ch.timeoutSeconds = 10;
    return ch;
}

AlertRow alert(const std::string& code, const std::string& subject, AlertSeverity sev, const std::string& msg)
{
    return {code, subject, sev, msg, {}, {}};
}

QByteArray bodyOf(const QByteArray& request)
{
    return request.mid(request.indexOf("\r\n\r\n") + 4);
}

} // namespace

class TestNotify : public QObject {
    Q_OBJECT

private slots:
    void parsesChannels()
    {
        std::vector<config::Diagnostic> diags;
        const auto g = config::parseGlobalDocument(kGlobal, L"synkro.yml", diags);
        QVERIFY2(errorsOf(diags).empty(), errorsOf(diags).empty() ? "" : errorsOf(diags).front().message.c_str());
        const auto& n = g.notifications;
        QCOMPARE(n.repeatEverySeconds, std::int64_t(12 * 3600));
        QCOMPARE(n.channels.size(), std::size_t(3));

        const auto* mail = n.find("suporte");
        QVERIFY(mail);
        QVERIFY(mail->type == config::ChannelType::Email);
        QVERIFY(mail->criticalOnly);
        QCOMPARE(mail->dailyReportAt, 8 * 60 + 30);
        QCOMPARE(mail->smtpPort, 587);
        QCOMPARE(mail->smtpPassword.name, std::string("smtp-suporte"));
        QCOMPARE(mail->to.size(), std::size_t(2));

        const auto* hook = n.find("central");
        QVERIFY(hook && hook->format == config::WebhookFormat::Teams && !hook->resolved);
        QCOMPARE(hook->token.name, std::string("central-token"));

        const auto* tg = n.find("celular");
        QVERIFY(tg && tg->type == config::ChannelType::Telegram);
        QCOMPARE(tg->chatId, std::string("-1001234"));

        // Os segredos do synkro.yml entram na lista (validação e pacote de exportação).
        QCOMPARE(g.secrets.size(), std::size_t(3));
    }

    void rejectsUnsafeChannels()
    {
        const char* yaml =
            "version: 1\n"
            "notifications:\n"
            "  repeat_every: 10m\n"
            "  channels:\n"
            "    - { id: a, type: email, from: nao-e-email, to: [x@y.com],\n"
            "        smtp: { host: smtp.example.com, security: none, user: u, password: \"segredo\" } }\n"
            "    - { id: b, type: webhook, url: \"http://monitor.example.com/x\" }\n"
            "    - { id: c, type: webhook, url: \"https://user:pw@monitor.example.com/x\" }\n"
            "    - { id: d, type: telegram, bot_token: { secret: tg } }\n"
            "    - { id: e, type: pombo }\n"
            "    - { id: b, type: webhook, url: \"https://ok.example.com\", cor: azul }\n";
        std::vector<config::Diagnostic> d;
        (void)config::parseGlobalDocument(yaml, L"synkro.yml", d);
        QVERIFY(hasErrorAt(d, "notifications.repeat_every"));
        QVERIFY(hasErrorAt(d, "notifications.channels[0].from"));
        QVERIFY(hasErrorAt(d, "notifications.channels[0].smtp.password")); // texto puro
        QVERIFY(hasErrorAt(d, "notifications.channels[0].smtp.security")); // sem TLS fora daqui
        QVERIFY(hasErrorAt(d, "notifications.channels[1].url"));            // http remoto
        QVERIFY(hasErrorAt(d, "notifications.channels[2].url"));            // credencial na URL
        QVERIFY(hasErrorAt(d, "notifications.channels[3].chat_id"));
        QVERIFY(hasErrorAt(d, "notifications.channels[4].type"));
        QVERIFY(hasErrorAt(d, "notifications.channels[5].id"));             // repetido
        QVERIFY(hasErrorAt(d, "notifications.channels[5].cor"));

        // http na própria máquina e allow_insecure explícito são aceitos.
        const char* ok =
            "version: 1\n"
            "notifications:\n"
            "  channels:\n"
            "    - { id: a, type: webhook, url: \"http://127.0.0.1:8080/x\" }\n"
            "    - { id: b, type: webhook, url: \"http://monitor.lan/x\", allow_insecure: true }\n";
        std::vector<config::Diagnostic> d2;
        (void)config::parseGlobalDocument(ok, L"synkro.yml", d2);
        QVERIFY(errorsOf(d2).empty());
    }

    void writerRoundTrip()
    {
        std::vector<config::Diagnostic> d;
        const auto g = config::parseGlobalDocument(kGlobal, L"synkro.yml", d);
        const std::string text = config::emitGlobal(g);
        std::vector<config::Diagnostic> d2;
        const auto again = config::parseGlobalDocument(text, L"synkro.yml", d2);
        QVERIFY2(errorsOf(d2).empty(), text.c_str());
        QCOMPARE(again.notifications.channels.size(), g.notifications.channels.size());
        QCOMPARE(again.notifications.repeatEverySeconds, g.notifications.repeatEverySeconds);
        for (std::size_t i = 0; i < g.notifications.channels.size(); ++i) {
            const auto& a = g.notifications.channels[i];
            const auto& b = again.notifications.channels[i];
            QCOMPARE(b.id, a.id);
            QVERIFY(b.type == a.type);
            QCOMPARE(b.criticalOnly, a.criticalOnly);
            QCOMPARE(b.resolved, a.resolved);
            QCOMPARE(b.dailyReportAt, a.dailyReportAt);
            QCOMPARE(b.smtpHost, a.smtpHost);
            QCOMPARE(b.to, a.to);
            QCOMPARE(b.url, a.url);
            QVERIFY(b.format == a.format);
            QCOMPARE(b.token.name, a.token.name);
            QCOMPARE(b.botToken.name, a.botToken.name);
            QCOMPARE(b.chatId, a.chatId);
        }
    }

    void composesMessages()
    {
        OutboxRow crit;
        crit.kind = "alert";
        crit.severity = "critical";
        crit.code = "last_run_failed";
        crit.subject = "pdv-arquivos";
        crit.message = "a última execução de \"PDV\" falhou";
        crit.createdAt = QDateTime::currentDateTimeUtc();
        OutboxRow ok = crit;
        ok.kind = "resolved";
        ok.severity = "info";
        OutboxRow warn = crit;
        warn.severity = "warning";
        warn.code = "low_space";

        const auto m = notify::compose("Loja Centro", {ok, warn, crit});
        QCOMPARE(m.items.front().severity, std::string("critical")); // críticos primeiro
        QVERIFY(m.title.find("Loja Centro") != std::string::npos);
        QVERIFY(m.title.find("1 alerta crítico") != std::string::npos);
        QVERIFY(m.title.find("1 aviso") != std::string::npos);
        QVERIFY(m.title.find("1 resolvido") != std::string::npos);
        QVERIFY(m.text.find("CRÍTICO: a última execução") != std::string::npos);
        QVERIFY(m.text.find("RESOLVIDO:") != std::string::npos);

        config::NotificationChannel ch = webhookChannel(1);
        const auto synkroJson = QJsonDocument::fromJson(QByteArray::fromStdString(notify::buildWebhook(ch, m, "tok").body)).object();
        QCOMPARE(synkroJson["source"].toString(), QStringLiteral("synkro"));
        QCOMPARE(synkroJson["instance"].toString(), QStringLiteral("Loja Centro"));
        QCOMPARE(synkroJson["events"].toArray().size(), 3);
        QCOMPARE(synkroJson["events"].toArray()[0].toObject()["code"].toString(), QStringLiteral("last_run_failed"));

        const auto req = notify::buildWebhook(ch, m, "tok");
        bool bearer = false;
        for (const auto& [k, v] : req.headers)
            bearer = bearer || (k == "Authorization" && v == "Bearer tok");
        QVERIFY(bearer);

        ch.format = config::WebhookFormat::Slack;
        QVERIFY(QJsonDocument::fromJson(QByteArray::fromStdString(notify::buildWebhook(ch, m, {}).body)).object().contains("text"));
        ch.format = config::WebhookFormat::Discord;
        QVERIFY(QJsonDocument::fromJson(QByteArray::fromStdString(notify::buildWebhook(ch, m, {}).body)).object().contains("content"));
        ch.format = config::WebhookFormat::Teams;
        const auto teams = QJsonDocument::fromJson(QByteArray::fromStdString(notify::buildWebhook(ch, m, {}).body)).object();
        QCOMPARE(teams["attachments"].toArray()[0].toObject()["contentType"].toString(),
                 QStringLiteral("application/vnd.microsoft.card.adaptive"));

        config::NotificationChannel tg;
        tg.type = config::ChannelType::Telegram;
        tg.chatId = "-100";
        const auto t = notify::buildTelegram(tg, m, "123:ABC");
        QCOMPARE(t.url, std::string("https://api.telegram.org/bot123:ABC/sendMessage"));
        QCOMPARE(QJsonDocument::fromJson(QByteArray::fromStdString(t.body)).object()["chat_id"].toString(), QStringLiteral("-100"));

        config::NotificationChannel mail;
        mail.type = config::ChannelType::Email;
        mail.from = "backup@example.com";
        mail.to = {"ti@example.com"};
        const std::string eml = notify::buildEmail(mail, m, QDateTime::currentDateTimeUtc());
        QVERIFY(eml.find("Subject: =?UTF-8?B?") != std::string::npos);
        QVERIFY(eml.find("To: ti@example.com\r\n") != std::string::npos);
        const auto split = eml.find("\r\n\r\n");
        QVERIFY(split != std::string::npos);
        const QByteArray decoded = QByteArray::fromBase64(QByteArray::fromStdString(eml.substr(split + 4)));
        QVERIFY(QString::fromUtf8(decoded).contains(QStringLiteral("CRÍTICO: a última execução")));
    }

    void queueDecidesWhatToSend()
    {
        QTemporaryDir dir;
        auto opened = StateStore::open(QDir(dir.path()).filePath("state.db").toStdWString());
        QVERIFY(opened);
        StateStore state = std::move(opened).value();

        config::ConfigSet set;
        set.global.instanceName = "Loja";
        config::NotificationChannel all = webhookChannel(1);
        all.id = "todos";
        config::NotificationChannel critical = webhookChannel(1);
        critical.id = "criticos";
        critical.criticalOnly = true;
        critical.dailyReportAt = 0; // relatório "a partir da meia-noite": sai na primeira passada do dia
        set.global.notifications.channels = {all, critical};

        QVERIFY(state.raiseAlert(alert("last_run_failed", "pdv", AlertSeverity::Critical, "falhou")));
        QVERIFY(state.raiseAlert(alert("low_space", "hd1", AlertSeverity::Warning, "pouco espaço")));

        const QDateTime now = QDateTime::currentDateTimeUtc();
        auto r = notify::enqueue(set, state, now);
        QCOMPARE(r.alerts, 3);  // 2 para "todos", 1 (só o crítico) para "criticos"
        QCOMPARE(r.reports, 1); // relatório diário do canal "criticos"

        // Nada de novo: nada a enfileirar (nem um segundo relatório no mesmo dia).
        r = notify::enqueue(set, state, now);
        QCOMPARE(r.alerts + r.reminders + r.reports, 0);

        // Piorou de aviso para crítico: o canal dos críticos passa a receber.
        QVERIFY(state.raiseAlert(alert("low_space", "hd1", AlertSeverity::Critical, "quase sem espaço")));
        r = notify::enqueue(set, state, now);
        QCOMPARE(r.alerts, 2); // "todos" (piorou) e "criticos" (primeira vez)

        // Depois de repeat_every (24h), lembrete dos que continuam abertos.
        r = notify::enqueue(set, state, now.addSecs(25 * 3600));
        QCOMPARE(r.reminders, 4);
        QCOMPARE(r.reports, 1); // outro dia, outro relatório

        // Resolvido: avisa quem foi avisado da abertura.
        QVERIFY(state.resolveAlert("last_run_failed", "pdv"));
        r = notify::enqueue(set, state, now.addSecs(25 * 3600));
        QCOMPARE(r.resolved, 2);
        r = notify::enqueue(set, state, now.addSecs(25 * 3600));
        QCOMPARE(r.resolved, 0); // só uma vez

        const auto pending = state.pendingNotifications(now.addSecs(26 * 3600));
        QCOMPARE(pending.size(), std::size_t(3 + 1 + 2 + 4 + 1 + 2));
    }

    void deliversWebhookAndRetries()
    {
        QTemporaryDir dir;
        auto opened = StateStore::open(QDir(dir.path()).filePath("state.db").toStdWString());
        QVERIFY(opened);
        StateStore state = std::move(opened).value();
        QVERIFY(state.raiseAlert(alert("never_run", "pdv", AlertSeverity::Warning, "ainda não foi executada")));

        // Falha (HTTP 500) três vezes: fica na fila, com tentativas e erro registrados.
        {
            FakeHttp server(500, 3);
            QVERIFY(server.port() > 0);
            config::ConfigSet set;
            set.global.notifications.channels = {webhookChannel(server.port())};
            QCOMPARE(notify::enqueue(set, state, QDateTime::currentDateTimeUtc()).alerts, 1);
            notify::DeliveryOptions opt;
            opt.ignoreBackoff = true;
            for (int i = 0; i < 3; ++i) {
                const auto rep = notify::deliver(set, state, opt);
                QCOMPARE(rep.channels.size(), std::size_t(1));
                QVERIFY(!rep.channels[0].ok);
                QVERIFY(rep.channels[0].error.find("HTTP 500") != std::string::npos);
            }
            const auto failing = state.failingChannels(3);
            QCOMPARE(failing.size(), std::size_t(1));
            QCOMPARE(failing[0].first, std::string("central"));

            // O alerta local "notify_failed" aparece para o operador.
            const auto alerts = refreshAlerts(set, state);
            QVERIFY(std::any_of(alerts.begin(), alerts.end(), [](const AlertRow& a) { return a.code == "notify_failed"; }));

            // Sem --now, a próxima tentativa respeita o intervalo.
            QVERIFY(notify::deliver(set, state, {}).channels.empty());
        }

        // Servidor voltou: entrega tudo num lote só.
        {
            FakeHttp server(200, 1);
            config::ConfigSet set;
            set.global.instanceName = "Loja";
            set.global.notifications.channels = {webhookChannel(server.port())};
            notify::DeliveryOptions opt;
            opt.ignoreBackoff = true;
            const auto rep = notify::deliver(set, state, opt);
            QCOMPARE(rep.channels.size(), std::size_t(1));
            QVERIFY2(rep.channels[0].ok, rep.channels[0].error.c_str());
            QVERIFY(rep.channels[0].items >= 1);
            const auto reqs = server.requests();
            QCOMPARE(reqs.size(), std::size_t(1));
            QVERIFY(reqs[0].startsWith("POST /hook"));
            const auto json = QJsonDocument::fromJson(bodyOf(reqs[0])).object();
            QCOMPARE(json["instance"].toString(), QStringLiteral("Loja"));
            QVERIFY(QString::fromUtf8(bodyOf(reqs[0])).contains(QStringLiteral("ainda não foi executada")));
            QVERIFY(state.pendingNotifications(QDateTime::currentDateTimeUtc().addYears(1)).empty());
            QVERIFY(state.failingChannels(1).empty());
        }

        // Canal removido da configuração: os pendentes são descartados.
        {
            QVERIFY(state.enqueueNotification({0, "sumiu", "test", {}, {}, "info", "x", {}, {}, 0, {}, {}, {}}));
            config::ConfigSet set;
            QVERIFY(notify::deliver(set, state, {}).channels.empty());
            QVERIFY(state.pendingNotifications(QDateTime::currentDateTimeUtc().addYears(1)).empty());
        }
    }

    void deliversEmail()
    {
        FakeSmtp server;
        QVERIFY(server.port() > 0);
        config::NotificationChannel ch;
        ch.id = "suporte";
        ch.type = config::ChannelType::Email;
        ch.smtpHost = "127.0.0.1";
        ch.smtpPort = server.port();
        ch.smtpSecurity = config::SmtpSecurity::None;
        ch.smtpUser = "backup@example.com";
        ch.from = "backup@example.com";
        ch.to = {"ti@example.com", "dono@example.com"};
        ch.timeoutSeconds = 10;

        OutboxRow row;
        row.kind = "alert";
        row.severity = "critical";
        row.message = "o cofre \"HD 1\" não é conectado há 3 dias";
        row.createdAt = QDateTime::currentDateTimeUtc();
        const auto m = notify::compose("Loja", {row});
        const auto st = notify::sendSmtp(ch, "s3nh@", notify::buildEmail(ch, m, QDateTime::currentDateTimeUtc()));
        QVERIFY2(st, st ? "" : st.error().message.c_str());

        QCOMPARE(server.auth(), QByteArray("\0backup@example.com\0s3nh@", 25));
        const auto cmds = server.commands();
        QVERIFY(std::count_if(cmds.begin(), cmds.end(), [](const QByteArray& c) { return c.startsWith("RCPT TO:"); }) == 2);
        const QByteArray data = server.data();
        QVERIFY(data.contains("From: Synkro Loja <backup@example.com>"));
        const QByteArray body = data.mid(data.indexOf("\r\n\r\n") + 4);
        QVERIFY(QString::fromUtf8(QByteArray::fromBase64(body)).contains(QStringLiteral("não é conectado há 3 dias")));
    }

    void reportsUnreachableServer()
    {
        // Porta fechada: erro legível e sem expor a URL (que pode conter token).
        QTcpServer probe;
        QVERIFY(probe.listen(QHostAddress::LocalHost, 0));
        const int port = probe.serverPort();
        probe.close();
        notify::HttpRequest req;
        req.url = "http://127.0.0.1:" + std::to_string(port) + "/segredo-na-url";
        req.body = "{}";
        const auto st = notify::sendHttp(req, 5);
        QVERIFY(!st);
        QVERIFY(st.error().message.find("segredo-na-url") == std::string::npos);
        QVERIFY(st.error().message.find("recusada") != std::string::npos);
    }
};

QTEST_GUILESS_MAIN(TestNotify)
#include "tst_notify.moc"
