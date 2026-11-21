// synkro-desktop — interface do Synkro (Qt Widgets). Configura cofres e
// tarefas, mostra a situação e os alertas, restaura e fica na bandeja.
//
// O desktop roda como usuário comum; operações que exigem administrador
// (registrar tarefas no Agendador) são delegadas ao synkro-worker com UAC.

#include "AppContext.h"
#include "Icons.h"
#include "MainWindow.h"
#include "Theme.h"

#include "core/Version.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QTimer>

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
    parser.process(app);

    Theme::instance().apply();
    AppContext::instance().reload();

    MainWindow window;
    if (parser.isSet(QStringLiteral("screenshot"))) {
        window.show();
        window.saveScreenshots(parser.value(QStringLiteral("screenshot")));
        return 0;
    }
    if (!parser.isSet(QStringLiteral("tray")))
        window.show();
    return QApplication::exec();
}
