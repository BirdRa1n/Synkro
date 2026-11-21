#pragma once

#include <QDateTime>
#include <QObject>
#include <QString>

class QFile;
class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

// Atualização automática, no espírito do electron-updater:
//
//   1. procura uma versão mais nova na aba Releases do GitHub (ao abrir e a
//      cada 6 h; versões beta só para quem ligou o canal beta);
//   2. baixa o Synkro-Setup-<versão>.exe em segundo plano e confere o SHA-256
//      publicado junto (Synkro-Setup-<versão>.exe.sha256);
//   3. quando o usuário manda instalar, executa o instalador em modo
//      silencioso (/SILENT) e fecha o app; o instalador espera um backup em
//      andamento terminar, atualiza e reabre o Synkro.
class Updater : public QObject {
    Q_OBJECT
public:
    enum class State { Disabled, Idle, Checking, UpToDate, Available, Downloading, Ready, Error };

    struct Release {
        QString version;     // 0.9.0-beta.2
        QString title;
        QString notes;       // markdown da release
        QString pageUrl;     // html_url
        QString setupUrl;    // browser_download_url do Synkro-Setup-*.exe
        QString setupName;
        qint64 setupSize = 0;
        QString shaUrl;      // .sha256 do instalador
        bool prerelease = false;
        QDateTime publishedAt;
    };

    static Updater& instance();

    State state() const { return m_state; }
    const Release& release() const { return m_release; }
    QString errorText() const { return m_error; }
    QDateTime lastCheck() const;
    qint64 received() const { return m_received; }
    qint64 total() const { return m_total; }
    QString downloadedFile() const { return m_file; }

    bool betaChannel() const;
    void setBetaChannel(bool on);
    bool autoDownload() const;
    void setAutoDownload(bool on);

    // Começa as verificações periódicas (chamado uma vez pela janela principal).
    void start();
    void check(bool userInitiated = false);
    void download();
    // Executa o instalador baixado e fecha o app. Devolve o erro (vazio = ok).
    QString installAndQuit();

    static QString stateText(State s);

signals:
    void changed();
    void progress(qint64 received, qint64 total);
    void readyToInstall(const QString& version);

private:
    explicit Updater(QObject* parent = nullptr);
    void setState(State s, const QString& error = {});
    void onReleases(QNetworkReply* reply, bool userInitiated);
    void startSetupDownload(const QByteArray& expectedSha);

    QNetworkAccessManager* m_nam;
    QTimer* m_timer;
    State m_state = State::Idle;
    Release m_release;
    QString m_error;
    QString m_file;
    qint64 m_received = 0, m_total = 0;
};
