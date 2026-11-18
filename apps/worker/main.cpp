// synkro-worker — executor de linha de comando do Synkro.
//
// Nesta fase expõe as operações básicas do motor, úteis para testes manuais
// e diagnóstico em campo. Os comandos de backup/restauração entram conforme
// o roadmap (architecture.md §10.2).

#include "core/Version.h"
#include "hash/Sha256.h"
#include "platform/windows/Volume.h"
#include "platform/windows/Win32.h"
#include "scan/FileScanner.h"
#include "transfer/SafeCopy.h"
#include "vault/Vault.h"
#include "vault/VaultCandidateCheck.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QTextStream>

#include <windows.h>

namespace {

// Códigos de saída documentados (architecture.md §10.2).
enum ExitCode : int {
    ExitOk = 0,
    ExitFailure = 2,
    ExitBlocked = 3,
    ExitUsage = 64,
};

QTextStream& out()
{
    static QTextStream s(stdout);
    return s;
}

QTextStream& err()
{
    static QTextStream s(stderr);
    return s;
}

std::wstring nativeArg(const QString& path)
{
    return QDir::toNativeSeparators(QDir::cleanPath(QDir(path).absolutePath())).toStdWString();
}

QString q(const std::wstring& s) { return QString::fromStdWString(s); }
QString q(const std::string& s) { return QString::fromStdString(s); }

int fail(const synkro::Error& e)
{
    err() << "erro [" << synkro::toString(e.code) << "]: " << q(e.message) << Qt::endl;
    return ExitFailure;
}

QString humanBytes(std::uint64_t bytes)
{
    return QLocale::system().formattedDataSize(static_cast<qint64>(bytes), 1, QLocale::DataSizeTraditionalFormat);
}

void usage()
{
    out() << "synkro-worker " << synkro::versionString() << "\n\n"
          << "Uso: synkro-worker <comando> [opções]\n\n"
          << "Comandos:\n"
          << "  version                              mostra a versão\n"
          << "  hash <arquivo> [--uncached]          calcula o SHA-256\n"
          << "  scan <pasta> [--ids]                 varre uma origem e resume o resultado\n"
          << "  copy <origem> <destino> [--uncached] cópia segura com verificação\n"
          << "  vault check <disco> [--source P]... [--vault V]...\n"
          << "                                       verifica se o disco pode ser um cofre\n"
          << "  vault init <disco> --label <nome> [--source P]... [--vault V]...\n"
          << "                                       verifica e inicializa o cofre\n"
          << "  vault show <disco>                   mostra a identidade do cofre\n"
          << Qt::endl;
}

// Faz o parse das opções de um subcomando (argumentos após o nome do comando).
bool parseSub(QCommandLineParser& p, const QStringList& args)
{
    p.setSingleDashWordOptionMode(QCommandLineParser::ParseAsLongOptions);
    if (!p.parse(QStringList{QStringLiteral("synkro-worker")} + args)) {
        err() << p.errorText() << Qt::endl;
        return false;
    }
    return true;
}

int cmdHash(const QStringList& args)
{
    QCommandLineParser p;
    p.addOption({QStringLiteral("uncached"), QStringLiteral("Lê sem o cache do Windows.")});
    if (!parseSub(p, args) || p.positionalArguments().size() != 1)
        return usage(), ExitUsage;

    auto r = synkro::Sha256::ofFile(nativeArg(p.positionalArguments().first()), p.isSet(QStringLiteral("uncached")));
    if (!r)
        return fail(r.error());
    out() << q(synkro::toHex(r.value())) << "  " << p.positionalArguments().first() << Qt::endl;
    return ExitOk;
}

int cmdScan(const QStringList& args)
{
    QCommandLineParser p;
    p.addOption({QStringLiteral("ids"), QStringLiteral("Lê ChangeTime e FileId de cada arquivo.")});
    if (!parseSub(p, args) || p.positionalArguments().size() != 1)
        return usage(), ExitUsage;

    synkro::ScanOptions opt;
    opt.readFileIds = p.isSet(QStringLiteral("ids"));

    QElapsedTimer timer;
    timer.start();
    auto r = synkro::scanDirectory(nativeArg(p.positionalArguments().first()), opt);
    if (!r)
        return fail(r.error());

    const auto& s = r.value();
    out() << "arquivos:     " << s.files.size() << "\n"
          << "pastas:       " << s.directoryCount << "\n"
          << "tamanho:      " << humanBytes(s.totalBytes) << "\n"
          << "links ignor.: " << s.skippedLinks.size() << "\n"
          << "problemas:    " << s.issues.size() << "\n"
          << "tempo:        " << timer.elapsed() << " ms" << Qt::endl;
    for (const auto& issue : s.issues)
        out() << "  ! " << q(issue.relativePath) << ": " << q(issue.error.message) << "\n";
    out().flush();
    return s.issues.empty() ? ExitOk : ExitFailure;
}

int cmdCopy(const QStringList& args)
{
    QCommandLineParser p;
    p.addOption({QStringLiteral("uncached"), QStringLiteral("Verifica relendo o destino sem cache.")});
    if (!parseSub(p, args) || p.positionalArguments().size() != 2)
        return usage(), ExitUsage;

    synkro::CopyOptions opt;
    opt.verify = p.isSet(QStringLiteral("uncached")) ? synkro::VerifyMode::RereadUncached
                                                     : synkro::VerifyMode::HashOnCopy;
    auto r = synkro::copyFileVerified(nativeArg(p.positionalArguments()[0]), nativeArg(p.positionalArguments()[1]), opt);
    if (!r)
        return fail(r.error());
    out() << "copiado " << humanBytes(r.value().bytes) << "  sha256 " << q(synkro::toHex(r.value().sha256)) << Qt::endl;
    return ExitOk;
}

void addCandidateOptions(QCommandLineParser& p)
{
    p.addOption({QStringLiteral("source"), QStringLiteral("Pasta de origem que usará o cofre."), QStringLiteral("pasta")});
    p.addOption({QStringLiteral("vault"), QStringLiteral("Outro cofre da mesma tarefa."), QStringLiteral("disco")});
    p.addOption({QStringLiteral("label"), QStringLiteral("Nome do cofre."), QStringLiteral("nome")});
}

synkro::vault::CandidateReport runCheck(const QCommandLineParser& p, const QString& disk)
{
    synkro::vault::CandidateRequest req;
    req.candidatePath = nativeArg(disk);
    for (const auto& s : p.values(QStringLiteral("source")))
        req.sourcePaths.push_back(nativeArg(s));
    for (const auto& v : p.values(QStringLiteral("vault")))
        req.otherVaults.push_back(nativeArg(v));
    return synkro::vault::checkCandidate(req);
}

void printReport(const synkro::vault::CandidateReport& report)
{
    using synkro::vault::CheckSeverity;
    const auto& v = report.volume;
    if (!v.mountPoint.empty()) {
        QStringList disks;
        for (auto d : v.physicalDisks)
            disks << QString::number(d);
        out() << "volume:  " << q(v.mountPoint) << "  " << q(v.label) << "\n"
              << "tipo:    " << synkro::win::toString(v.kind) << " · " << q(v.fileSystem)
              << " · disco físico " << (disks.isEmpty() ? QStringLiteral("?") : disks.join(QLatin1Char(','))) << "\n"
              << "espaço:  " << humanBytes(v.freeBytes) << " livres de " << humanBytes(v.totalBytes) << "\n\n";
    }
    for (const auto& f : report.findings) {
        const char* mark = f.severity == CheckSeverity::Ok      ? "[ ok ]"
                         : f.severity == CheckSeverity::Info    ? "[info]"
                         : f.severity == CheckSeverity::Warning ? "[ !! ]"
                                                                : "[ XX ]";
        out() << mark << " " << q(f.title);
        if (!f.detail.empty())
            out() << " — " << q(f.detail);
        out() << "\n";
    }
    out() << "\n" << (report.accepted() ? (report.hasWarnings() ? "ACEITO COM AVISOS" : "ACEITO") : "RECUSADO")
          << Qt::endl;
}

int cmdVault(const QStringList& args)
{
    if (args.isEmpty())
        return usage(), ExitUsage;
    const QString sub = args.first();
    const QStringList rest = args.mid(1);

    QCommandLineParser p;
    addCandidateOptions(p);
    if (!parseSub(p, rest) || p.positionalArguments().size() != 1)
        return usage(), ExitUsage;
    const QString disk = p.positionalArguments().first();

    if (sub == QLatin1String("check")) {
        const auto report = runCheck(p, disk);
        printReport(report);
        return report.accepted() ? ExitOk : ExitBlocked;
    }

    if (sub == QLatin1String("init")) {
        const QString label = p.value(QStringLiteral("label"));
        if (label.trimmed().isEmpty()) {
            err() << "informe o nome do cofre com --label" << Qt::endl;
            return ExitUsage;
        }
        const auto report = runCheck(p, disk);
        printReport(report);
        if (!report.accepted())
            return ExitBlocked;

        auto existing = synkro::vault::readIdentity(report.volume.mountPoint);
        if (existing) {
            out() << "\ncofre já existente reaproveitado: " << existing.value().vaultId << Qt::endl;
            return ExitOk;
        }
        auto id = synkro::vault::initialize(report.volume.mountPoint, label.trimmed());
        if (!id)
            return fail(id.error());
        out() << "\ncofre inicializado: " << id.value().vaultId << " em "
              << q(synkro::vault::vaultRoot(report.volume.mountPoint)) << Qt::endl;
        return ExitOk;
    }

    if (sub == QLatin1String("show")) {
        auto volume = synkro::win::queryVolume(nativeArg(disk));
        const std::wstring location = volume ? volume.value().mountPoint : nativeArg(disk);
        auto id = synkro::vault::readIdentity(location);
        if (!id)
            return fail(id.error());
        out() << synkro::vault::serialize(id.value());
        out().flush();
        return ExitOk;
    }

    return usage(), ExitUsage;
}

} // namespace

int main(int argc, char* argv[])
{
    ::SetConsoleOutputCP(CP_UTF8);

    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("synkro-worker"));
    QCoreApplication::setApplicationVersion(QString::fromLatin1(synkro::versionString()));

    const QStringList args = QCoreApplication::arguments().mid(1);
    if (args.isEmpty())
        return usage(), ExitUsage;

    const QString command = args.first();
    const QStringList rest = args.mid(1);

    if (command == QLatin1String("version") || command == QLatin1String("--version")) {
        out() << "synkro-worker " << synkro::versionString() << Qt::endl;
        return ExitOk;
    }
    if (command == QLatin1String("help") || command == QLatin1String("--help") || command == QLatin1String("-h"))
        return usage(), ExitOk;
    if (command == QLatin1String("hash"))
        return cmdHash(rest);
    if (command == QLatin1String("scan"))
        return cmdScan(rest);
    if (command == QLatin1String("copy"))
        return cmdCopy(rest);
    if (command == QLatin1String("vault"))
        return cmdVault(rest);

    err() << "comando desconhecido: " << command << "\n" << Qt::endl;
    usage();
    return ExitUsage;
}
