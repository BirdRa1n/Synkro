// Transporte das notificações: HTTP(S) para webhooks/Telegram e SMTP para
// e-mail. Tudo síncrono (o worker é de linha de comando) e com tempo limite.
//
// Mensagens de erro nunca incluem a URL nem credenciais: a URL de um webhook
// do Slack/Discord e a do Telegram contêm o próprio token.

#include "notify/Notifier.h"

#include "core/Version.h"

#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkProxyFactory>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSslSocket>
#include <QSysInfo>
#include <QTimer>
#include <QUrl>

#include <mutex>

namespace synkro::notify {

namespace {

std::string oneLine(const QByteArray& raw, int max = 200)
{
    QString s = QString::fromUtf8(raw.left(max * 4)).simplified();
    if (s.size() > max)
        s = s.left(max) + QStringLiteral("…");
    return s.toStdString();
}

std::string networkError(QNetworkReply::NetworkError e)
{
    switch (e) {
    case QNetworkReply::HostNotFoundError:
        return "servidor não encontrado (DNS); confira o endereço e a conexão com a internet";
    case QNetworkReply::ConnectionRefusedError:
        return "conexão recusada pelo servidor";
    case QNetworkReply::RemoteHostClosedError:
        return "o servidor fechou a conexão";
    case QNetworkReply::TimeoutError:
    case QNetworkReply::OperationCanceledError:
        return "tempo esgotado";
    case QNetworkReply::SslHandshakeFailedError:
        return "falha no TLS (certificado inválido ou não confiável)";
    case QNetworkReply::ProxyConnectionRefusedError:
    case QNetworkReply::ProxyNotFoundError:
    case QNetworkReply::ProxyTimeoutError:
    case QNetworkReply::ProxyAuthenticationRequiredError:
        return "falha no proxy da rede";
    case QNetworkReply::TemporaryNetworkFailureError:
    case QNetworkReply::NetworkSessionFailedError:
        return "rede indisponível";
    default:
        return "erro de rede (código " + std::to_string(static_cast<int>(e)) + ")";
    }
}

void useSystemProxy()
{
    static std::once_flag once;
    std::call_once(once, [] { QNetworkProxyFactory::setUseSystemConfiguration(true); });
}

// ---- SMTP --------------------------------------------------------------------

class SmtpSession {
public:
    SmtpSession(const config::NotificationChannel& ch) : m_ch(ch), m_timeoutMs(static_cast<int>(ch.timeoutSeconds * 1000))
    {
        if (ch.allowInsecure)
            m_sock.setPeerVerifyMode(QSslSocket::VerifyNone);
    }

    Status connect()
    {
        const QString host = QString::fromStdString(m_ch.smtpHost);
        const auto port = static_cast<quint16>(m_ch.smtpPort);
        if (m_ch.smtpSecurity != config::SmtpSecurity::None && !QSslSocket::supportsSsl())
            return Error::make(ErrorCode::Unsupported, "TLS indisponível nesta instalação (plugin tls do Qt ausente)");
        if (m_ch.smtpSecurity == config::SmtpSecurity::Tls) {
            m_sock.connectToHostEncrypted(host, port);
            if (!m_sock.waitForEncrypted(m_timeoutMs))
                return socketError("conectar com TLS");
        } else {
            m_sock.connectToHost(host, port);
            if (!m_sock.waitForConnected(m_timeoutMs))
                return socketError("conectar");
        }
        if (auto st = expect(220, 0, "saudação"); !st)
            return st;
        if (auto st = ehlo(); !st)
            return st;
        if (m_ch.smtpSecurity == config::SmtpSecurity::StartTls) {
            if (!m_caps.contains(QStringLiteral("STARTTLS")))
                return Error::make(ErrorCode::Unsupported, "o servidor SMTP não oferece STARTTLS; use security: tls na porta 465");
            if (auto st = command("STARTTLS", 220, 0, "STARTTLS"); !st)
                return st;
            m_sock.startClientEncryption();
            if (!m_sock.waitForEncrypted(m_timeoutMs))
                return socketError("negociar TLS");
            if (auto st = ehlo(); !st)
                return st;
        }
        return Status::success();
    }

    Status login(const std::string& user, const std::string& password)
    {
        if (user.empty())
            return Status::success();
        QString mechs;
        for (const auto& c : m_caps)
            if (c.startsWith(QStringLiteral("AUTH ")) || c.startsWith(QStringLiteral("AUTH=")))
                mechs += c.mid(5) + ' ';
        if (mechs.isEmpty())
            return Error::make(ErrorCode::Unsupported, "o servidor SMTP não aceita autenticação nesta conexão");
        const QStringList list = mechs.toUpper().split(' ', Qt::SkipEmptyParts);
        if (list.contains(QStringLiteral("PLAIN"))) {
            const std::string token = QByteArray::fromStdString(std::string(1, '\0') + user + '\0' + password)
                                          .toBase64().toStdString();
            return command("AUTH PLAIN " + token, 235, 0, "autenticação");
        }
        if (list.contains(QStringLiteral("LOGIN"))) {
            if (auto st = command("AUTH LOGIN", 334, 0, "autenticação"); !st)
                return st;
            if (auto st = command(QByteArray::fromStdString(user).toBase64().toStdString(), 334, 0, "autenticação"); !st)
                return st;
            return command(QByteArray::fromStdString(password).toBase64().toStdString(), 235, 0, "autenticação");
        }
        return Error::make(ErrorCode::Unsupported, "o servidor SMTP só oferece " + mechs.trimmed().toStdString()
                                                       + "; o Synkro usa PLAIN ou LOGIN");
    }

    Status sendMail(const std::string& from, const std::vector<std::string>& to, const std::string& message)
    {
        if (auto st = command("MAIL FROM:<" + from + ">", 250, 0, "remetente"); !st)
            return st;
        for (const auto& t : to)
            if (auto st = command("RCPT TO:<" + t + ">", 250, 251, "destinatário " + t); !st)
                return st;
        if (auto st = command("DATA", 354, 0, "DATA"); !st)
            return st;
        // "Dot-stuffing": linhas que começam com ponto ganham outro ponto.
        std::string data;
        std::size_t pos = 0;
        while (pos < message.size()) {
            std::size_t nl = message.find("\r\n", pos);
            const std::string line = message.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
            data += (line.rfind('.', 0) == 0 ? "." : "") + line + "\r\n";
            if (nl == std::string::npos)
                break;
            pos = nl + 2;
        }
        data += ".\r\n";
        if (!write(data))
            return socketError("enviar a mensagem");
        if (auto st = expect(250, 0, "envio da mensagem"); !st)
            return st;
        (void)command("QUIT", 221, 0, "QUIT");
        return Status::success();
    }

private:
    Status ehlo()
    {
        const std::string name = QSysInfo::machineHostName().isEmpty() ? "synkro" : QSysInfo::machineHostName().toStdString();
        if (!write("EHLO " + name + "\r\n"))
            return socketError("EHLO");
        std::vector<std::string> lines;
        auto code = readReply(&lines);
        if (!code)
            return code.error();
        if (code.value() != 250)
            return Error::make(ErrorCode::IoError, "o servidor SMTP recusou o EHLO: " + m_last);
        m_caps.clear();
        for (std::size_t i = 1; i < lines.size(); ++i)
            m_caps << QString::fromStdString(lines[i]).trimmed().toUpper();
        return Status::success();
    }

    bool write(const std::string& s)
    {
        if (m_sock.write(s.data(), static_cast<qint64>(s.size())) != static_cast<qint64>(s.size()))
            return false;
        while (m_sock.bytesToWrite() > 0)
            if (!m_sock.waitForBytesWritten(m_timeoutMs))
                return false;
        return true;
    }

    // Lê uma resposta (talvez de várias linhas "250-...") e devolve o código.
    Result<int> readReply(std::vector<std::string>* lines = nullptr)
    {
        for (;;) {
            while (!m_sock.canReadLine()) {
                if (!m_sock.waitForReadyRead(m_timeoutMs))
                    return socketError("ler a resposta do servidor SMTP").error();
            }
            const std::string line = QString::fromUtf8(m_sock.readLine()).trimmed().toStdString();
            if (line.size() < 3)
                return Error::make(ErrorCode::IoError, "resposta SMTP inválida");
            m_last = line;
            if (lines)
                lines->push_back(line.size() > 4 ? line.substr(4) : std::string());
            if (line.size() == 3 || line[3] == ' ')
                return std::atoi(line.substr(0, 3).c_str());
        }
    }

    Status expect(int ok1, int ok2, const std::string& what)
    {
        auto code = readReply();
        if (!code)
            return code.error();
        if (code.value() == ok1 || (ok2 && code.value() == ok2))
            return Status::success();
        return Error::make(ErrorCode::IoError, "o servidor SMTP recusou " + what + ": " + m_last.substr(0, 200));
    }

    // O comando em si nunca vai para a mensagem de erro (AUTH leva a senha).
    Status command(const std::string& line, int ok1, int ok2, const std::string& what)
    {
        if (!write(line + "\r\n"))
            return socketError(what);
        return expect(ok1, ok2, what);
    }

    Status socketError(const std::string& what)
    {
        const auto e = m_sock.error();
        std::string why;
        switch (e) {
        case QAbstractSocket::HostNotFoundError:
            why = "servidor não encontrado (DNS)";
            break;
        case QAbstractSocket::ConnectionRefusedError:
            why = "conexão recusada (porta " + std::to_string(m_ch.smtpPort) + ")";
            break;
        case QAbstractSocket::SocketTimeoutError:
            why = "tempo esgotado";
            break;
        case QAbstractSocket::RemoteHostClosedError:
            why = "o servidor fechou a conexão";
            break;
        case QAbstractSocket::SslHandshakeFailedError:
            why = "falha no TLS (certificado inválido ou não confiável)";
            break;
        default:
            why = m_sock.errorString().toStdString();
        }
        return Error::make(ErrorCode::IoError, "SMTP " + m_ch.smtpHost + ": " + what + ": " + why);
    }

    const config::NotificationChannel& m_ch;
    int m_timeoutMs;
    QSslSocket m_sock;
    QStringList m_caps;
    std::string m_last;
};

} // namespace

Status sendHttp(const HttpRequest& req, std::int64_t timeoutSeconds, std::string* responseBody)
{
    useSystemProxy();
    QNetworkAccessManager nam;
    QNetworkRequest r(QUrl(QString::fromStdString(req.url)));
    r.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    r.setTransferTimeout(static_cast<int>(timeoutSeconds * 1000));
    r.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Synkro/%1").arg(QString::fromLatin1(versionString())));
    for (const auto& [k, v] : req.headers)
        r.setRawHeader(QByteArray::fromStdString(k), QByteArray::fromStdString(v));

    QNetworkReply* reply = nam.post(r, QByteArray::fromStdString(req.body));
    // Certificado inválido derruba a conexão, a menos que o canal tenha
    // allow_insecure (servidor interno com certificado próprio).
    if (req.insecureTls)
        QObject::connect(reply, &QNetworkReply::sslErrors, reply, [reply](const QList<QSslError>&) { reply->ignoreSslErrors(); });
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer guard;
    guard.setSingleShot(true);
    QObject::connect(&guard, &QTimer::timeout, &loop, &QEventLoop::quit);
    guard.start(static_cast<int>((timeoutSeconds + 5) * 1000));
    if (!reply->isFinished())
        loop.exec();
    if (!reply->isFinished())
        reply->abort();

    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray body = reply->readAll();
    const auto error = reply->error();
    reply->deleteLater();
    if (responseBody)
        *responseBody = body.left(64 * 1024).toStdString();

    if (status >= 200 && status < 300)
        return Status::success();
    if (status >= 300 && status < 400)
        return Error::make(ErrorCode::IoError, "o servidor respondeu com redirecionamento HTTP " + std::to_string(status)
                                                   + "; use a URL final do webhook");
    if (status > 0) {
        std::string msg = "o servidor respondeu HTTP " + std::to_string(status);
        if (status == 401 || status == 403)
            msg += " (token recusado)";
        else if (status == 404)
            msg += " (endereço do webhook não existe mais?)";
        else if (status == 429)
            msg += " (limite de envios; nova tentativa mais tarde)";
        const std::string snippet = oneLine(body);
        return Error::make(ErrorCode::IoError, snippet.empty() ? msg : msg + ": " + snippet);
    }
    return Error::make(ErrorCode::IoError, networkError(error));
}

Status sendSmtp(const config::NotificationChannel& ch, const std::string& password, const std::string& message)
{
    SmtpSession s(ch);
    if (auto st = s.connect(); !st)
        return st;
    if (auto st = s.login(ch.smtpUser, password); !st)
        return st;
    return s.sendMail(ch.from, ch.to, message);
}

} // namespace synkro::notify
