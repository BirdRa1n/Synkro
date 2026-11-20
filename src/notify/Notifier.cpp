#include "notify/Notifier.h"

#include "config/Values.h"
#include "core/Paths.h"
#include "core/Version.h"
#include "database/DatabaseBackup.h"
#include "platform/windows/Locks.h"
#include "platform/windows/Win32.h"
#include "secrets/SecretStore.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QSysInfo>
#include <QUrl>
#include <QUuid>

#include <algorithm>
#include <map>

namespace synkro::notify {

namespace {

constexpr int kMaxItemsPerMessage = 50;
constexpr qint64 kExpireAfterSeconds = 3 * 86400;
constexpr qint64 kResolvedLookbackSeconds = 7 * 86400;

std::string localTime(const QDateTime& utc, const char* fmt = "dd/MM/yyyy HH:mm")
{
    return utc.isValid() ? utc.toLocalTime().toString(QString::fromLatin1(fmt)).toStdString() : std::string();
}

std::string plural(int n, const char* one, const char* many)
{
    return std::to_string(n) + " " + (n == 1 ? one : many);
}

// Ordem de leitura: críticos, avisos, resoluções, relatório, teste.
int rank(const OutboxRow& r)
{
    if (r.kind == "alert" || r.kind == "reminder")
        return r.severity == "critical" ? 0 : 1;
    if (r.kind == "resolved")
        return 2;
    return r.kind == "report" ? 3 : 4;
}

std::string label(const OutboxRow& r)
{
    const std::string sev = r.severity == "critical" ? "CRÍTICO" : "AVISO";
    if (r.kind == "alert")
        return sev;
    if (r.kind == "reminder")
        return sev + ", ainda aberto desde " + localTime(r.openedAt, "dd/MM HH:mm");
    if (r.kind == "resolved")
        return "RESOLVIDO";
    return {};
}

std::string truncateUtf8(const std::string& s, std::size_t max)
{
    if (s.size() <= max)
        return s;
    std::size_t n = max;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80)
        --n; // não corta no meio de um caractere
    return s.substr(0, n) + "\n…";
}

std::string base64(const std::string& s)
{
    return QByteArray::fromStdString(s).toBase64().toStdString();
}

// Cabeçalho com acentos (RFC 2047).
std::string encodedWord(const std::string& s)
{
    const bool ascii = std::all_of(s.begin(), s.end(), [](char c) { return static_cast<unsigned char>(c) < 0x80; });
    return ascii ? s : "=?UTF-8?B?" + base64(s) + "?=";
}

QString jsonText(const QJsonObject& o)
{
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

std::string dailyReport(const config::ConfigSet& set, StateStore& state, const QDateTime& now)
{
    std::string o = "Relatório diário do Synkro\n";
    const auto alerts = state.openAlerts();
    const auto critical = std::count_if(alerts.begin(), alerts.end(),
                                        [](const AlertRow& a) { return a.severity == AlertSeverity::Critical; });
    if (alerts.empty())
        o += "Nenhum alerta aberto.\n";
    else
        o += "Alertas abertos: " + std::to_string(alerts.size()) + " (" + std::to_string(critical) + " crítico(s)).\n";

    o += "\nTarefas:\n";
    for (const auto& j : set.jobs) {
        if (!j.enabled)
            continue;
        const auto last = state.lastSuccess(win::fromUtf8(j.id));
        o += "• " + j.name + ": ";
        if (last)
            o += "último backup bem-sucedido em " + localTime(last->finishedAt) + " (há "
                 + config::describeDuration(std::max<qint64>(last->finishedAt.secsTo(now) / 60, 1) * 60) + ")";
        else
            o += "nenhum backup bem-sucedido ainda";
        o += "\n";
    }

    if (!set.global.vaults.empty()) {
        o += "\nCofres:\n";
        for (const auto& v : set.global.vaults) {
            const auto seen = state.vaultSeen(QString::fromStdString(v.vaultId));
            o += "• " + v.label + ": ";
            if (!seen || !seen->lastSeenAt.isValid()) {
                o += "nunca conectado\n";
                continue;
            }
            o += "visto em " + localTime(seen->lastSeenAt);
            if (seen->totalBytes > 0)
                o += ", " + config::describePercent(static_cast<double>(seen->freeBytes) / static_cast<double>(seen->totalBytes))
                     + " livre";
            o += "\n";
        }
    }
    return o;
}

Result<std::string> secretValue(const config::SecretRef& ref, const std::wstring& secretsFile, const char* what)
{
    if (ref.kind == config::SecretRef::Kind::None)
        return std::string();
    auto v = db::resolveSecret(ref, secretsFile.empty() ? synkro::secretsFile() : secretsFile);
    if (!v)
        return Error::make(v.error().code, std::string(what) + ": " + v.error().message, v.error().systemCode);
    return v;
}

} // namespace

std::string instanceName(const config::GlobalConfig& g)
{
    return g.instanceName.empty() ? QSysInfo::machineHostName().toStdString() : g.instanceName;
}

Composed compose(const std::string& instance, const std::vector<OutboxRow>& input)
{
    Composed m;
    m.instance = instance;
    m.items = input;
    std::stable_sort(m.items.begin(), m.items.end(), [](const OutboxRow& a, const OutboxRow& b) { return rank(a) < rank(b); });

    int critical = 0, warning = 0, resolved = 0, reports = 0, tests = 0;
    for (const auto& r : m.items) {
        if (r.kind == "alert" || r.kind == "reminder")
            (r.severity == "critical" ? critical : warning)++;
        else if (r.kind == "resolved")
            ++resolved;
        else if (r.kind == "report")
            ++reports;
        else
            ++tests;
    }

    std::vector<std::string> parts;
    if (critical)
        parts.push_back(plural(critical, "alerta crítico", "alertas críticos"));
    if (warning)
        parts.push_back(plural(warning, "aviso", "avisos"));
    if (resolved)
        parts.push_back(plural(resolved, "resolvido", "resolvidos"));
    if (parts.empty())
        parts.push_back(reports ? "relatório diário" : tests ? "mensagem de teste" : "notificação");
    std::string summary;
    for (std::size_t i = 0; i < parts.size(); ++i)
        summary += (i ? ", " : "") + parts[i];
    m.title = "[Synkro] " + instance + ": " + summary;

    std::string t;
    for (const auto& r : m.items) {
        if (r.kind == "report" || r.kind == "test") {
            t += (t.empty() ? "" : "\n") + r.message + "\n";
            continue;
        }
        t += label(r) + ": " + r.message + "\n";
        if (r.kind == "alert" && r.openedAt.isValid())
            t += "  desde " + localTime(r.openedAt) + "\n";
    }
    t += "\n--\nSynkro " + std::string(versionString()) + " em " + QSysInfo::machineHostName().toStdString() + ", "
         + localTime(QDateTime::currentDateTimeUtc()) + ".\n"
         + "Detalhes: abra o Synkro nesta máquina ou rode \"synkro-worker status\".\n";
    m.text = t;
    return m;
}

HttpRequest buildWebhook(const config::NotificationChannel& ch, const Composed& m, const std::string& token)
{
    HttpRequest req;
    req.url = ch.url;
    req.insecureTls = ch.allowInsecure;
    req.headers.emplace_back("Content-Type", "application/json; charset=utf-8");
    if (!token.empty())
        req.headers.emplace_back("Authorization", "Bearer " + token);

    const QString title = QString::fromStdString(m.title);
    const QString text = QString::fromStdString(m.text);
    QJsonObject body;
    switch (ch.format) {
    case config::WebhookFormat::Slack:
        body["text"] = QString::fromStdString(truncateUtf8("*" + m.title + "*\n" + m.text, 38000));
        break;
    case config::WebhookFormat::Discord:
        body["content"] = QString::fromStdString(truncateUtf8("**" + m.title + "**\n" + m.text, 1900));
        break;
    case config::WebhookFormat::Teams: {
        // Cartão adaptável: aceito pelos fluxos do Teams (Workflows) e pelos
        // conectores de entrada.
        QJsonArray blocks{QJsonObject{{"type", "TextBlock"}, {"text", title}, {"weight", "Bolder"}, {"wrap", true}},
                          QJsonObject{{"type", "TextBlock"}, {"text", QString::fromStdString(truncateUtf8(m.text, 20000))},
                                      {"wrap", true}}};
        QJsonObject card{{"type", "AdaptiveCard"}, {"version", "1.4"},
                         {"$schema", "http://adaptivecards.io/schemas/adaptive-card.json"}, {"body", blocks}};
        body["type"] = "message";
        body["attachments"] = QJsonArray{QJsonObject{{"contentType", "application/vnd.microsoft.card.adaptive"},
                                                     {"content", card}}};
        break;
    }
    case config::WebhookFormat::Synkro: {
        QJsonArray events;
        for (const auto& r : m.items) {
            QJsonObject e{{"kind", QString::fromStdString(r.kind)},
                          {"severity", QString::fromStdString(r.severity)},
                          {"message", QString::fromStdString(r.message)},
                          {"created_at", r.createdAt.toUTC().toString(Qt::ISODate)}};
            if (!r.code.empty())
                e["code"] = QString::fromStdString(r.code);
            if (!r.subject.empty())
                e["subject"] = QString::fromStdString(r.subject);
            if (r.openedAt.isValid())
                e["opened_at"] = r.openedAt.toUTC().toString(Qt::ISODate);
            events.append(e);
        }
        body = QJsonObject{{"source", "synkro"},
                           {"version", QString::fromLatin1(versionString())},
                           {"instance", QString::fromStdString(m.instance)},
                           {"computer", QSysInfo::machineHostName()},
                           {"sent_at", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
                           {"title", title},
                           {"text", text},
                           {"events", events}};
        break;
    }
    }
    req.body = jsonText(body).toStdString();
    return req;
}

HttpRequest buildTelegram(const config::NotificationChannel& ch, const Composed& m, const std::string& botToken)
{
    HttpRequest req;
    req.url = "https://api.telegram.org/bot" + botToken + "/sendMessage";
    req.headers.emplace_back("Content-Type", "application/json; charset=utf-8");
    QJsonObject body{{"chat_id", QString::fromStdString(ch.chatId)},
                     {"text", QString::fromStdString(truncateUtf8(m.title + "\n\n" + m.text, 4000))},
                     {"disable_web_page_preview", true}};
    req.body = jsonText(body).toStdString();
    return req;
}

std::string buildEmail(const config::NotificationChannel& ch, const Composed& m, const QDateTime& date)
{
    std::string to;
    for (const auto& t : ch.to)
        to += (to.empty() ? "" : ", ") + t;
    const QString domain = QString::fromStdString(ch.from.substr(ch.from.find('@') + 1));

    std::string body = m.text;
    std::string crlf;
    for (char c : body) {
        if (c == '\n')
            crlf += "\r\n";
        else if (c != '\r')
            crlf += c;
    }
    std::string b64 = base64(crlf);
    std::string wrapped;
    for (std::size_t i = 0; i < b64.size(); i += 76)
        wrapped += b64.substr(i, 76) + "\r\n";

    std::string o;
    // Data no formato da RFC 5322, com o fuso em +hhmm.
    const QDateTime local = date.toLocalTime();
    const int off = local.offsetFromUtc() / 60;
    char tz[8];
    std::snprintf(tz, sizeof(tz), "%c%02d%02d", off < 0 ? '-' : '+', std::abs(off) / 60, std::abs(off) % 60);
    o += "Date: " + QLocale::c().toString(local, QStringLiteral("ddd, dd MMM yyyy HH:mm:ss ")).toStdString() + tz + "\r\n";
    o += "From: " + encodedWord("Synkro " + m.instance) + " <" + ch.from + ">\r\n";
    o += "To: " + to + "\r\n";
    o += "Subject: " + encodedWord(m.title) + "\r\n";
    o += "Message-ID: <" + QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString() + "@" + domain.toStdString() + ">\r\n";
    o += "MIME-Version: 1.0\r\n";
    o += "Content-Type: text/plain; charset=UTF-8\r\n";
    o += "Content-Transfer-Encoding: base64\r\n";
    o += "X-Mailer: Synkro " + std::string(versionString()) + "\r\n";
    o += "\r\n" + wrapped;
    return o;
}

EnqueueResult enqueue(const config::ConfigSet& set, StateStore& state, const QDateTime& nowUtc)
{
    EnqueueResult res;
    const auto& cfg = set.global.notifications;
    if (cfg.channels.empty())
        return res;
    const auto open = state.openAlerts();
    const std::string instance = instanceName(set.global);

    for (const auto& ch : cfg.channels) {
        if (!ch.enabled)
            continue;
        for (const auto& a : open) {
            if (a.code == "notify_failed" && a.subject == ch.id)
                continue; // o próprio canal com defeito não consegue avisar de si mesmo
            if (ch.criticalOnly && a.severity != AlertSeverity::Critical)
                continue;
            const auto last = state.lastAlertNotification(ch.id, a.code, a.subject, a.openedAt);
            std::string kind;
            if (!last)
                kind = "alert";
            else if (last->severity == "warning" && a.severity == AlertSeverity::Critical)
                kind = "alert"; // piorou
            else if (cfg.repeatEverySeconds > 0 && last->createdAt.secsTo(nowUtc) >= cfg.repeatEverySeconds)
                kind = "reminder";
            else
                continue;
            OutboxRow row;
            row.channel = ch.id;
            row.kind = kind;
            row.code = a.code;
            row.subject = a.subject;
            row.severity = nameOf(a.severity);
            row.message = a.message;
            row.createdAt = nowUtc;
            row.openedAt = a.openedAt;
            if (state.enqueueNotification(row))
                (kind == "alert" ? res.alerts : res.reminders)++;
        }

        if (ch.resolved) {
            for (const auto& a : state.resolvedUnannounced(ch.id, nowUtc.addSecs(-kResolvedLookbackSeconds))) {
                OutboxRow row;
                row.channel = ch.id;
                row.kind = "resolved";
                row.code = a.code;
                row.subject = a.subject;
                row.severity = "info";
                row.message = a.message;
                row.createdAt = nowUtc;
                row.openedAt = a.openedAt;
                if (state.enqueueNotification(row))
                    ++res.resolved;
            }
        }

        if (ch.dailyReportAt >= 0) {
            const QDateTime local = nowUtc.toLocalTime();
            const int minutes = local.time().hour() * 60 + local.time().minute();
            if (minutes >= ch.dailyReportAt) {
                auto claimed = state.claimValue("notify_report:" + ch.id, local.date().toString(Qt::ISODate).toStdString());
                if (claimed && claimed.value()) {
                    OutboxRow row;
                    row.channel = ch.id;
                    row.kind = "report";
                    row.code = "daily_report";
                    row.subject = instance;
                    row.severity = "info";
                    row.message = dailyReport(set, state, nowUtc);
                    row.createdAt = nowUtc;
                    if (state.enqueueNotification(row))
                        ++res.reports;
                }
            }
        }
    }
    return res;
}

Status send(const config::NotificationChannel& ch, const Composed& m, const std::wstring& secretsFile)
{
    switch (ch.type) {
    case config::ChannelType::Email: {
        auto pass = secretValue(ch.smtpPassword, secretsFile, "senha SMTP");
        if (!pass)
            return pass.error();
        std::string p = std::move(pass).value();
        auto st = sendSmtp(ch, p, buildEmail(ch, m, QDateTime::currentDateTimeUtc()));
        wipe(p);
        return st;
    }
    case config::ChannelType::Webhook: {
        auto token = secretValue(ch.token, secretsFile, "token do webhook");
        if (!token)
            return token.error();
        std::string t = std::move(token).value();
        HttpRequest req = buildWebhook(ch, m, t);
        wipe(t);
        auto st = sendHttp(req, ch.timeoutSeconds);
        for (auto& h : req.headers)
            wipe(h.second);
        return st;
    }
    case config::ChannelType::Telegram: {
        auto token = secretValue(ch.botToken, secretsFile, "token do bot");
        if (!token)
            return token.error();
        std::string t = std::move(token).value();
        if (t.empty())
            return Error::make(ErrorCode::InvalidArgument, "token do bot vazio");
        HttpRequest req = buildTelegram(ch, m, t);
        wipe(t);
        std::string response;
        auto st = sendHttp(req, ch.timeoutSeconds, &response);
        wipe(req.url);
        if (!st) {
            // A API devolve {"ok":false,"description":"..."}: mais útil que o código HTTP.
            const auto doc = QJsonDocument::fromJson(QByteArray::fromStdString(response));
            const QString desc = doc.object().value("description").toString();
            if (!desc.isEmpty())
                return Error::make(st.error().code, st.error().message + " (" + desc.toStdString() + ")");
        }
        return st;
    }
    }
    return Error::make(ErrorCode::Internal, "tipo de canal desconhecido");
}

DeliveryReport deliver(const config::ConfigSet& set, StateStore& state, const DeliveryOptions& opt)
{
    DeliveryReport report;
    const QDateTime now = opt.now.isValid() ? opt.now : QDateTime::currentDateTimeUtc();
    const auto& cfg = set.global.notifications;

    // Um envio por vez na máquina: um backup e a manutenção podem terminar juntos.
    auto lock = win::JobLock::acquire(L"_notify", 120000);
    if (!lock)
        return report;

    if (auto n = state.expireNotifications(now.addSecs(-kExpireAfterSeconds)))
        report.expired = n.value();

    std::map<std::string, std::vector<OutboxRow>> byChannel;
    for (auto& r : state.pendingNotifications(opt.ignoreBackoff ? now.addYears(1) : now))
        byChannel[r.channel].push_back(std::move(r));

    const std::string instance = instanceName(set.global);
    for (auto& [id, rows] : byChannel) {
        const config::NotificationChannel* ch = cfg.find(id);
        std::vector<std::int64_t> ids;
        for (const auto& r : rows)
            ids.push_back(r.id);
        if (!ch || !ch->enabled) {
            (void)state.markNotifications(ids, "dropped"); // canal removido ou desligado
            continue;
        }
        // Lotes de até 50 itens; o restante vai na próxima mensagem.
        for (std::size_t start = 0; start < rows.size(); start += kMaxItemsPerMessage) {
            const std::size_t end = std::min(rows.size(), start + kMaxItemsPerMessage);
            std::vector<OutboxRow> batch(rows.begin() + static_cast<std::ptrdiff_t>(start),
                                         rows.begin() + static_cast<std::ptrdiff_t>(end));
            std::vector<std::int64_t> batchIds(ids.begin() + static_cast<std::ptrdiff_t>(start),
                                               ids.begin() + static_cast<std::ptrdiff_t>(end));
            ChannelDelivery d;
            d.channel = id;
            d.items = static_cast<int>(batch.size());
            const auto st = send(*ch, compose(instance, batch), opt.secretsFile);
            if (st) {
                d.ok = true;
                (void)state.markNotifications(batchIds, "sent");
            } else if (st.error().code == ErrorCode::AccessDenied || st.error().systemCode == ERROR_ACCESS_DENIED) {
                // Usuário comum sem acesso ao secrets.dat: não é falha do canal,
                // o worker agendado (SYSTEM) entrega depois.
                d.error = "sem acesso aos segredos; o envio fica para o worker agendado (ou rode como administrador)";
            } else {
                d.error = st.error().message;
                int attempts = 0;
                for (const auto& r : batch)
                    attempts = std::max(attempts, r.attempts);
                const qint64 delay = std::min<qint64>(3600, 300LL << std::min(attempts, 4));
                (void)state.deferNotifications(batchIds, d.error, now.addSecs(delay));
            }
            report.channels.push_back(std::move(d));
            if (!st)
                break; // canal fora do ar: não insiste com os outros lotes agora
        }
    }
    return report;
}

Status sendTest(const config::ConfigSet& set, const std::string& channelId, const std::wstring& secretsFile)
{
    const config::NotificationChannel* ch = set.global.notifications.find(channelId);
    if (!ch)
        return Error::make(ErrorCode::NotFound, "canal \"" + channelId + "\" não está em notifications.channels");
    OutboxRow row;
    row.channel = ch->id;
    row.kind = "test";
    row.severity = "info";
    row.message = "Mensagem de teste do Synkro: se você recebeu, o canal \"" + ch->id + "\" está funcionando.";
    row.createdAt = QDateTime::currentDateTimeUtc();
    return send(*ch, compose(instanceName(set.global), {row}), secretsFile);
}

} // namespace synkro::notify
