#include "core/Version.h"

#include <QCoreApplication>
#include <QTextStream>

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("synkro-worker"));
    QCoreApplication::setApplicationVersion(QString::fromLatin1(synkro::versionString()));

    QTextStream out(stdout);
    out << "synkro-worker " << synkro::versionString() << Qt::endl;
    return 0;
}
