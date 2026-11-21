#include "Updater.h"

#include "core/SemVer.h"
#include "core/Version.h"

#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QWidget>

#include <memory>

#include <windows.h>
#include <shellapi.h>

namespace {

constexpr int kCheckIntervalMs = 6 * 3600 * 1000;
const QString kBetaKey = QStringLiteral("updates/beta");
const QString kAutoKey = QStringLiteral("updates/autoDownload");
const QString kLastCheckKey = QStringLiteral("updates/lastCheck");

QNetworkRequest githubRequest(const QUrl& url, const char* accept)
{
    QNetworkRequest req(url);
    req.setRawHeader("Accept", accept);
    req.setRawHeader("X-GitHub-Api-Version", "2022-11-28");
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Synkro/%1").arg(QString::fromLatin1(synkro::versionString())));
    // Os downloads de assets redirecionam para o armazenamento do GitHub; nunca para http.
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setTransferTimeout(60000);
    return req;
}

QString networkError(QNetworkReply* r)
{
    const int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status == 403 || status == 429)
        return QStringLiteral("o GitHub limitou as consultas; tente de novo em alguns minutos");
    if (status >= 400)
        return QStringLiteral("o GitHub respondeu HTTP %1").arg(status);
    switch (r->error()) {
    case QNetworkReply::HostNotFoundError:
        return QStringLiteral("sem acesso à internet (GitHub não encontrado)");
    case QNetworkReply::TimeoutError:
    case QNetworkReply::OperationCanceledError:
        return QStringLiteral("tempo esgotado");
    case QNetworkReply::SslHandshakeFailedError:
        return QStringLiteral("falha no TLS ao falar com o GitHub");
    default:
        return QStringLiteral("erro de rede (%1)").arg(static_cast<int>(r->error()));
    }
}

QByteArray sha256Of(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    QCryptographicHash h(QCryptographicHash::Sha256);
    if (!h.addData(&f))
        return {};
    return h.result().toHex();
}

QString updateDirectory()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation)).filePath(QStringLiteral("Synkro-update"));
}

} // namespace

Updater& Updater::instance()
{
    static Updater u;
    return u;
}

Updater::Updater(QObject* parent) : QObject(parent), m_nam(new QNetworkAccessManager(this)), m_timer(new QTimer(this))
{
    if (QString::fromLatin1(synkro::updateRepository()).isEmpty())
        m_state = State::Disabled;
    m_timer->setInterval(kCheckIntervalMs);
    connect(m_timer, &QTimer::timeout, this, [this] { check(false); });
}

QDateTime Updater::lastCheck() const
{
    return QSettings().value(kLastCheckKey).toDateTime();
}

bool Updater::betaChannel() const
{
    // Quem roda uma beta continua recebendo betas, salvo se desligar.
    return QSettings().value(kBetaKey, synkro::parseSemVer(synkro::versionString()).isPrerelease()).toBool();
}

void Updater::setBetaChannel(bool on)
{
    QSettings().setValue(kBetaKey, on);
    check(true);
}

bool Updater::autoDownload() const
{
    return QSettings().value(kAutoKey, true).toBool();
}

void Updater::setAutoDownload(bool on)
{
    QSettings().setValue(kAutoKey, on);
}

void Updater::start()
{
    if (m_state == State::Disabled)
        return;
    m_timer->start();
    // Primeira verificação pouco depois de abrir, sem atrasar a janela.
    QTimer::singleShot(20000, this, [this] { check(false); });
}

void Updater::setState(State s, const QString& error)
{
    m_state = s;
    m_error = error;
    emit changed();
}

QString Updater::stateText(State s)
{
    switch (s) {
    case State::Disabled:    return QStringLiteral("atualização automática desligada nesta compilação");
    case State::Idle:        return QStringLiteral("ainda não verificado");
    case State::Checking:    return QStringLiteral("procurando atualizações…");
    case State::UpToDate:    return QStringLiteral("você está na versão mais recente");
    case State::Available:   return QStringLiteral("atualização disponível");
    case State::Downloading: return QStringLiteral("baixando a atualização…");
    case State::Ready:       return QStringLiteral("atualização baixada, pronta para instalar");
    case State::Error:       return QStringLiteral("não foi possível atualizar");
    }
    return {};
}

void Updater::check(bool userInitiated)
{
    if (m_state == State::Disabled || m_state == State::Checking || m_state == State::Downloading)
        return;
    const QString repo = QString::fromLatin1(synkro::updateRepository());
    setState(State::Checking);
    QNetworkReply* reply = m_nam->get(githubRequest(
        QUrl(QStringLiteral("https://api.github.com/repos/%1/releases?per_page=30").arg(repo)), "application/vnd.github+json"));
    connect(reply, &QNetworkReply::finished, this, [this, reply, userInitiated] { onReleases(reply, userInitiated); });
}

void Updater::onReleases(QNetworkReply* reply, bool userInitiated)
{
    reply->deleteLater();
    (void)userInitiated;
    if (reply->error() != QNetworkReply::NoError) {
        setState(State::Error, networkError(reply));
        return;
    }
    QSettings().setValue(kLastCheckKey, QDateTime::currentDateTimeUtc());

    const QString current = QString::fromLatin1(synkro::versionString());
    const bool beta = betaChannel();
    static const QRegularExpression setupName(QStringLiteral("^Synkro-Setup-[0-9A-Za-z.+-]+\\.exe$"));

    Release best;
    synkro::SemVer bestVer;
    for (const auto& item : QJsonDocument::fromJson(reply->readAll()).array()) {
        const QJsonObject r = item.toObject();
        if (r.value("draft").toBool())
            continue;
        const bool pre = r.value("prerelease").toBool();
        if (pre && !beta)
            continue;
        QString version = r.value("tag_name").toString();
        if (version.startsWith('v'))
            version.remove(0, 1);
        const synkro::SemVer v = synkro::parseSemVer(version.toStdString());
        if (!v.valid || !synkro::isNewerVersion(version.toStdString(), current.toStdString()))
            continue;
        if (bestVer.valid && synkro::compareSemVer(v, bestVer) <= 0)
            continue;

        Release rel;
        for (const auto& a : r.value("assets").toArray()) {
            const QJsonObject asset = a.toObject();
            const QString name = asset.value("name").toString();
            if (setupName.match(name).hasMatch()) {
                rel.setupName = name;
                rel.setupUrl = asset.value("browser_download_url").toString();
                rel.setupSize = static_cast<qint64>(asset.value("size").toDouble());
            }
        }
        for (const auto& a : r.value("assets").toArray()) {
            const QJsonObject asset = a.toObject();
            if (!rel.setupName.isEmpty() && asset.value("name").toString() == rel.setupName + QStringLiteral(".sha256"))
                rel.shaUrl = asset.value("browser_download_url").toString();
        }
        // Sem instalador ou sem a soma publicada, a release não serve para o atualizador.
        if (rel.setupUrl.isEmpty() || rel.shaUrl.isEmpty() || !rel.setupUrl.startsWith(QStringLiteral("https://")))
            continue;
        rel.version = version;
        rel.title = r.value("name").toString();
        rel.notes = r.value("body").toString();
        rel.pageUrl = r.value("html_url").toString();
        rel.prerelease = pre;
        rel.publishedAt = QDateTime::fromString(r.value("published_at").toString(), Qt::ISODate);
        best = rel;
        bestVer = v;
    }

    if (!bestVer.valid) {
        m_release = {};
        setState(State::UpToDate);
        return;
    }
    // Já baixada (ex.: verificação periódica depois do download): continua pronta.
    if (m_state != State::Ready || m_release.version != best.version) {
        m_release = best;
        setState(State::Available);
        if (autoDownload())
            download();
    } else {
        setState(State::Ready);
    }
}

void Updater::download()
{
    if (m_state != State::Available && m_state != State::Error)
        return;
    if (m_release.setupUrl.isEmpty())
        return;
    m_received = 0;
    m_total = m_release.setupSize;
    setState(State::Downloading);
    // Primeiro a soma publicada; o instalador só vale se bater com ela.
    QNetworkReply* reply = m_nam->get(githubRequest(QUrl(m_release.shaUrl), "application/octet-stream"));
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            setState(State::Error, QStringLiteral("soma SHA-256 da atualização indisponível: ") + networkError(reply));
            return;
        }
        const QByteArray text = reply->read(4096).trimmed();
        const QByteArray hash = text.left(64).toLower();
        static const QRegularExpression hex(QStringLiteral("^[0-9a-f]{64}$"));
        if (!hex.match(QString::fromLatin1(hash)).hasMatch()) {
            setState(State::Error, QStringLiteral("arquivo .sha256 da atualização inválido"));
            return;
        }
        startSetupDownload(hash);
    });
}

void Updater::startSetupDownload(const QByteArray& expectedSha)
{
    const QString dir = updateDirectory();
    QDir().mkpath(dir);
    const QString target = QDir(dir).filePath(QFileInfo(m_release.setupName).fileName());

    // Já baixado antes (app reaberto): confere e reaproveita.
    if (QFileInfo::exists(target) && sha256Of(target) == expectedSha) {
        m_file = target;
        m_received = m_total = QFileInfo(target).size();
        setState(State::Ready);
        emit readyToInstall(m_release.version);
        return;
    }

    auto file = std::make_shared<QFile>(target + QStringLiteral(".part"));
    if (!file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        setState(State::Error, QStringLiteral("não foi possível gravar em %1").arg(QDir::toNativeSeparators(dir)));
        return;
    }
    auto hash = std::make_shared<QCryptographicHash>(QCryptographicHash::Sha256);
    QNetworkRequest req = githubRequest(QUrl(m_release.setupUrl), "application/octet-stream");
    req.setTransferTimeout(5 * 60000);
    QNetworkReply* reply = m_nam->get(req);
    connect(reply, &QNetworkReply::readyRead, this, [reply, file, hash] {
        const QByteArray chunk = reply->readAll();
        hash->addData(chunk);
        file->write(chunk);
    });
    connect(reply, &QNetworkReply::downloadProgress, this, [this](qint64 got, qint64 total) {
        m_received = got;
        if (total > 0)
            m_total = total;
        emit progress(m_received, m_total);
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, file, hash, target, expectedSha] {
        reply->deleteLater();
        const QByteArray rest = reply->readAll();
        hash->addData(rest);
        file->write(rest);
        const bool writeOk = file->flush() && file->error() == QFileDevice::NoError;
        file->close();
        if (reply->error() != QNetworkReply::NoError || !writeOk) {
            QFile::remove(file->fileName());
            setState(State::Error, reply->error() != QNetworkReply::NoError ? QStringLiteral("download interrompido: ") + networkError(reply)
                                                                            : QStringLiteral("falha ao gravar o instalador"));
            return;
        }
        if (hash->result().toHex() != expectedSha) {
            QFile::remove(file->fileName());
            setState(State::Error, QStringLiteral("o instalador baixado não confere com o SHA-256 publicado; nada foi instalado"));
            return;
        }
        QFile::remove(target);
        if (!QFile::rename(file->fileName(), target)) {
            setState(State::Error, QStringLiteral("não foi possível preparar o instalador"));
            return;
        }
        m_file = target;
        setState(State::Ready);
        emit readyToInstall(m_release.version);
    });
}

QString Updater::installAndQuit()
{
    if (m_state != State::Ready || !QFileInfo::exists(m_file))
        return QStringLiteral("a atualização ainda não foi baixada");

    // O instalador pede administrador (UAC). Silencioso, com janela de progresso;
    // ao terminar, reabre o Synkro como este usuário.
    const std::wstring file = QDir::toNativeSeparators(m_file).toStdWString();
    const std::wstring params = L"/SILENT /SUPPRESSMSGBOXES /NORESTART /SP-";
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOASYNC;
    info.lpVerb = L"open";
    info.lpFile = file.c_str();
    info.lpParameters = params.c_str();
    info.nShow = SW_SHOWNORMAL;
    if (!::ShellExecuteExW(&info)) {
        const DWORD err = ::GetLastError();
        if (err == ERROR_CANCELLED)
            return QStringLiteral("a instalação precisa da permissão de administrador (o pedido foi cancelado)");
        return QStringLiteral("não foi possível iniciar o instalador (erro %1)").arg(err);
    }
    // Fecha de verdade (não só para a bandeja): o instalador substitui os programas.
    for (QWidget* w : QApplication::topLevelWidgets())
        w->setProperty("quitting", true);
    QTimer::singleShot(0, qApp, &QApplication::quit);
    return {};
}
