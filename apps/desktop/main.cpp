// synkro-desktop — interface do Synkro (Qt Widgets). Configura cofres e
// tarefas, mostra a situação e os alertas, restaura e fica na bandeja.
//
// Roda sempre como administrador (manifesto requireAdministrator). Se mesmo
// assim abrir sem elevação (ex.: iniciado por outro programa com um token
// limitado), reabre a si mesmo pedindo elevação. Uma instância por sessão:
// abrir de novo só traz a janela da que já está na bandeja.

#include "AppContext.h"
#include "Icons.h"
#include "MainWindow.h"
#include "Theme.h"

#include "core/Version.h"
#include "platform/windows/Snapshot.h" // isProcessElevated

#include <QApplication>
#include <QCommandLineParser>
#include <QLocalServer>
#include <QLocalSocket>
#include <QTimer>

#include <windows.h>
#include <shellapi.h>

namespace {

#if SYNKRO_DESKTOP_REQUIRE_ADMIN
// Reabre o processo elevado (UAC). true se a nova instância foi iniciada.
bool relaunchElevated()
{
    wchar_t exe[MAX_PATH] = {};
    if (!::GetModuleFileNameW(nullptr, exe, MAX_PATH))
        return false;
    std::wstring args = L"--elevation-retried ";
    const QStringList all = QCoreApplication::arguments();
    for (int i = 1; i < all.size(); ++i)
        args += L"\"" + all[i].toStdWString() + L"\" ";
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.lpVerb = L"runas";
    info.lpFile = exe;
    info.lpParameters = args.c_str();
    info.nShow = SW_SHOWNORMAL;
    return ::ShellExecuteExW(&info) != FALSE;
}
#endif

QString instanceName()
{
    DWORD session = 0;
    ::ProcessIdToSessionId(::GetCurrentProcessId(), &session);
    return QStringLiteral("synkro-desktop-%1").arg(session);
}

// Se já há um Synkro nesta sessão, pede para ele aparecer e devolve true.
bool signalRunningInstance(bool tray)
{
    QLocalSocket socket;
    socket.connectToServer(instanceName());
    if (!socket.waitForConnected(500))
        return false;
    ::AllowSetForegroundWindow(ASFW_ANY);
    socket.write(tray ? "tray\n" : "show\n");
    socket.flush();
    socket.waitForBytesWritten(500);
    socket.disconnectFromServer();
    return true;
}

} // namespace

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("Synkro"));
    QApplication::setApplicationName(QStringLiteral("Synkro Desktop"));
    QApplication::setApplicationVersion(QString::fromLatin1(synkro::versionString()));
    QApplication::setWindowIcon(icons::appIcon()); // janelas e diálogos
    QApplication::setQuitOnLastWindowClosed(false); // continua na bandeja

    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({QStringLiteral("tray"), QStringLiteral("Inicia minimizado na bandeja (inicialização do Windows).")});
    parser.addOption({QStringLiteral("screenshot"), QStringLiteral("Grava capturas das telas na pasta e sai (desenvolvimento)."),
                      QStringLiteral("pasta")});
    QCommandLineOption retried(QStringLiteral("elevation-retried"));
    retried.setFlags(QCommandLineOption::HiddenFromHelp);
    parser.addOption(retried);
    parser.process(app);
    const bool screenshot = parser.isSet(QStringLiteral("screenshot"));
    const bool tray = parser.isSet(QStringLiteral("tray"));

    if (!screenshot) {
#if SYNKRO_DESKTOP_REQUIRE_ADMIN
        // Uma tentativa só: se a reabertura também vier sem elevação (UAC
        // desligado para usuário comum), segue assim em vez de entrar em laço.
        if (!synkro::win::isProcessElevated() && !parser.isSet(retried) && relaunchElevated())
            return 0; // a instância elevada assume daqui
#endif
        if (signalRunningInstance(tray))
            return 0;
    }

    Theme::instance().apply();
    AppContext::instance().reload();

    MainWindow window;
    if (screenshot) {
        window.show();
        window.saveScreenshots(parser.value(QStringLiteral("screenshot")));
        return 0;
    }

    // Recebe o "apareça" de uma segunda instância (menu Iniciar, instalador...).
    QLocalServer server;
    QLocalServer::removeServer(instanceName());
    if (server.listen(instanceName())) {
        QObject::connect(&server, &QLocalServer::newConnection, &window, [&server, &window] {
            while (QLocalSocket* s = server.nextPendingConnection()) {
                QObject::connect(s, &QLocalSocket::readyRead, &window, [s, &window] {
                    const QByteArray what = s->readAll().trimmed();
                    if (what != "tray") {
                        window.showNormal();
                        window.raise();
                        window.activateWindow();
                    }
                });
                QObject::connect(s, &QLocalSocket::disconnected, s, &QObject::deleteLater);
            }
        });
    }

    if (!tray)
        window.show();
    return QApplication::exec();
}
