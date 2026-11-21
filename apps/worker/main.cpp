// synkro-worker — executor de linha de comando do Synkro. É o processo que o
// Agendador de Tarefas inicia (como SYSTEM) e a ferramenta de diagnóstico e
// recuperação em campo (architecture.md §10.2).

#include "Cli.h"

#include "config/ConfigApply.h"
#include "config/ConfigLoader.h"
#include "config/PlanDescriber.h"
#include "core/Version.h"
#include "engine/BackupEngine.h"
#include "engine/Restore.h"
#include "engine/RunSummary.h"
#include "engine/Verify.h"
#include "hash/Sha256.h"
#include "core/Paths.h"
#include "crypto/VaultKey.h"
#include "platform/windows/NetworkShare.h"
#include "platform/windows/Volume.h"
#include "platform/windows/Win32.h"
#include "scan/FileScanner.h"
#include "secrets/SecretStore.h"
#include "transfer/SafeCopy.h"
#include "vault/Vault.h"
#include "vault/VaultCandidateCheck.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QElapsedTimer>

#include <windows.h>

using namespace cli;

namespace {

int cmdConfig(const QStringList& args)
{
    if (args.isEmpty())
        return (usage(), ExitUsage);
    const QString sub = args.first();
    if (sub == QLatin1String("apply"))
        return cmdConfigApply(args.mid(1));
    if (sub == QLatin1String("export") || sub == QLatin1String("import"))
        return cmdConfigPackage(sub, args.mid(1));

    QCommandLineParser p;
    p.addOption({QStringLiteral("config-dir"), QStringLiteral("Pasta de configuração."), QStringLiteral("pasta")});
    p.addOption({QStringLiteral("job"), QStringLiteral("Tarefa."), QStringLiteral("id")});
    if (!parseSub(p, args.mid(1)))
        return (usage(), ExitUsage);

    synkro::config::ConfigSet set;
    const bool ok = loadAndReport(configDirFrom(p), set, sub == QLatin1String("plan"));

    if (sub == QLatin1String("validate")) {
        if (ok)
            out() << "Configuração válida: " << set.jobs.size() << " tarefa(s), " << set.global.vaults.size() << " cofre(s)."
                  << Qt::endl;
        return ok ? ExitOk : ExitFailure;
    }
    if (sub == QLatin1String("plan")) {
        const QString only = p.value(QStringLiteral("job"));
        bool any = false;
        for (const auto& job : set.jobs) {
            if (!only.isEmpty() && QString::fromStdString(job.id) != only)
                continue;
            if (any)
                out() << "\n────────────────────────────────────────\n\n";
            out() << q(synkro::config::describeJob(set, job));
            any = true;
        }
        out().flush();
        if (!any) {
            err() << "nenhuma tarefa encontrada" << (only.isEmpty() ? QString() : QStringLiteral(": ") + only) << Qt::endl;
            return ExitFailure;
        }
        return ok ? ExitOk : ExitFailure;
    }
    return (usage(), ExitUsage);
}

int cmdBackup(const QStringList& args)
{
    QCommandLineParser p;
    p.addOption({QStringLiteral("job"), QStringLiteral("Tarefa da configuração."), QStringLiteral("id")});
    p.addOption({QStringLiteral("config-dir"), QStringLiteral("Pasta de configuração."), QStringLiteral("pasta")});
    p.addOption({QStringLiteral("trigger"), QStringLiteral("Origem da execução (schedule, manual...)."), QStringLiteral("t")});
    p.addOption({QStringLiteral("no-shutdown"), QStringLiteral("Não desliga o computador ao final.")});
    p.addOption({QStringLiteral("source"), QStringLiteral("Origem (backup avulso)."), QStringLiteral("pasta")});
    p.addOption({QStringLiteral("vault"), QStringLiteral("Cofre (backup avulso)."), QStringLiteral("disco")});
    p.addOption({QStringLiteral("job-id"), QStringLiteral("ID da tarefa (backup avulso)."), QStringLiteral("id")});
    p.addOption({QStringLiteral("exclude"), QStringLiteral("Padrão de exclusão (avulso)."), QStringLiteral("padrão")});
    p.addOption({QStringLiteral("uncached"), QStringLiteral("Verificação relendo sem cache (avulso).")});
    p.addOption({QStringLiteral("confirm-mass-change"), QStringLiteral("Aplica mesmo com exclusão/modificação em massa.")});
    if (!parseSub(p, args))
        return (usage(), ExitUsage);

    if (p.isSet(QStringLiteral("job"))) {
        synkro::config::ConfigSet set;
        if (!loadAndReport(configDirFrom(p), set, true))
            return ExitFailure;
        const auto* job = set.findJob(p.value(QStringLiteral("job")).toStdString());
        if (!job) {
            err() << "tarefa não encontrada: " << p.value(QStringLiteral("job")) << Qt::endl;
            return ExitFailure;
        }
        RunOptions opt;
        opt.trigger = p.isSet(QStringLiteral("trigger")) ? p.value(QStringLiteral("trigger")).toStdString() : "manual";
        opt.overrideGuards = p.isSet(QStringLiteral("confirm-mass-change"));
        opt.allowShutdown = !p.isSet(QStringLiteral("no-shutdown"));
        return runConfiguredJob(set, *job, opt);
    }

    // Backup avulso de arquivos, sem configuração.
    if (!p.isSet(QStringLiteral("source")) || p.values(QStringLiteral("vault")).isEmpty() || !p.isSet(QStringLiteral("job-id")))
        return (usage(), ExitUsage);
    synkro::BackupRequest req;
    req.jobId = p.value(QStringLiteral("job-id")).toStdWString();
    req.sourceRoot = nativeArg(p.value(QStringLiteral("source")));
    for (const auto& v : p.values(QStringLiteral("vault")))
        req.vaults.push_back({vaultLocationArg(v), {}, v});
    std::vector<std::wstring> excludes;
    for (const auto& e : p.values(QStringLiteral("exclude")))
        excludes.push_back(e.toStdWString());
    req.filter = synkro::PathFilter({}, excludes);
    if (p.isSet(QStringLiteral("uncached")))
        req.verify = synkro::VerifyMode::RereadUncached;
    req.overrideGuards = p.isSet(QStringLiteral("confirm-mass-change"));
    req.progress = [](const synkro::ProgressEvent& e) {
        if (e.kind == synkro::ProgressKind::ScanFinished)
            out() << "origem analisada: " << e.total << " arquivos" << Qt::endl;
        else if (e.kind == synkro::ProgressKind::VaultStarted)
            out() << "cofre " << q(e.vault) << "..." << Qt::endl;
    };
    const auto result = synkro::runBackup(req);
    printBackupResult(result);
    return exitFor(result.status);
}

int cmdRestore(const QStringList& args)
{
    QCommandLineParser p;
    p.addOption({QStringLiteral("vault"), QStringLiteral("Cofre."), QStringLiteral("disco")});
    p.addOption({QStringLiteral("job"), QStringLiteral("Tarefa."), QStringLiteral("id")});
    p.addOption({QStringLiteral("to"), QStringLiteral("Pasta de destino (nova)."), QStringLiteral("pasta")});
    p.addOption({QStringLiteral("run"), QStringLiteral("Execução (padrão: a última)."), QStringLiteral("run-id")});
    p.addOption({QStringLiteral("path"), QStringLiteral("Restringe a uma pasta ou arquivo."), QStringLiteral("caminho")});
    if (!parseSub(p, args) || !p.isSet(QStringLiteral("vault")) || !p.isSet(QStringLiteral("job")) || !p.isSet(QStringLiteral("to")))
        return (usage(), ExitUsage);

    synkro::RestoreRequest req;
    req.vaultLocation = vaultLocationArg(p.value(QStringLiteral("vault")));
    req.jobId = p.value(QStringLiteral("job")).toStdWString();
    req.targetDirectory = nativeArg(p.value(QStringLiteral("to")));
    req.runId = p.value(QStringLiteral("run")).toStdWString();
    req.pathPrefix = p.value(QStringLiteral("path")).toStdWString();
    if (!ensureVaultKey(req.vaultLocation))
        return ExitFailure;

    auto r = synkro::restore(req);
    if (!r)
        return fail(r.error());
    out() << "restaurado o estado da execução " << q(r.value().runId) << " em " << q(req.targetDirectory) << "\n"
          << "  arquivos: " << r.value().restored << " (" << humanBytes(r.value().bytes) << "), todos conferidos por SHA-256\n";
    for (const auto& [path, msg] : r.value().failures)
        out() << "  ! " << q(path) << ": " << q(msg) << "\n";
    out().flush();
    return r.value().failures.empty() ? ExitOk : ExitFailure;
}

int cmdVerify(const QStringList& args)
{
    QCommandLineParser p;
    p.addOption({QStringLiteral("vault"), QStringLiteral("Cofre."), QStringLiteral("disco")});
    p.addOption({QStringLiteral("job"), QStringLiteral("Tarefa."), QStringLiteral("id")});
    p.addOption({QStringLiteral("sample"), QStringLiteral("Percentual a conferir (padrão 100)."), QStringLiteral("N")});
    if (!parseSub(p, args) || !p.isSet(QStringLiteral("vault")) || !p.isSet(QStringLiteral("job")))
        return (usage(), ExitUsage);

    const synkro::vault::JobLayout layout(vaultLocationArg(p.value(QStringLiteral("vault"))),
                                          p.value(QStringLiteral("job")).toStdWString());
    const double sample = p.isSet(QStringLiteral("sample")) ? p.value(QStringLiteral("sample")).toDouble() : 100.0;
    if (!ensureVaultKey(layout.vaultLocation()))
        return ExitFailure;
    auto r = synkro::verifyVault(layout, sample);
    if (!r)
        return fail(r.error());
    const auto& v = r.value();
    out() << "conferidos " << v.checked << " arquivos (" << humanBytes(v.bytes) << "): " << v.ok << " íntegros\n";
    for (const auto& m : v.missing)
        out() << "  AUSENTE    " << q(m) << "\n";
    for (const auto& c : v.corrupted)
        out() << "  CORROMPIDO " << q(c) << "\n";
    for (const auto& [path, msg] : v.unreadable)
        out() << "  ILEGÍVEL   " << q(path) << ": " << q(msg) << "\n";
    out() << (v.healthy() ? "cofre íntegro" : "PROBLEMAS ENCONTRADOS") << Qt::endl;
    return v.healthy() ? ExitOk : ExitFailure;
}

int cmdRuns(const QStringList& args)
{
    QCommandLineParser p;
    p.addOption({QStringLiteral("vault"), QStringLiteral("Cofre."), QStringLiteral("disco")});
    p.addOption({QStringLiteral("job"), QStringLiteral("Tarefa."), QStringLiteral("id")});
    if (!parseSub(p, args) || !p.isSet(QStringLiteral("vault")) || !p.isSet(QStringLiteral("job")))
        return (usage(), ExitUsage);

    const synkro::vault::JobLayout layout(vaultLocationArg(p.value(QStringLiteral("vault"))),
                                          p.value(QStringLiteral("job")).toStdWString());
    const auto runs = synkro::listRuns(layout);
    if (runs.empty()) {
        out() << "nenhuma execução encontrada" << Qt::endl;
        return ExitOk;
    }
    for (const auto& id : runs) {
        auto s = synkro::RunSummary::load(layout.summaryFile(id));
        out() << q(id) << "  ";
        if (s) {
            const auto& v = s.value();
            out() << v.status.leftJustified(12) << " +" << v.added << " ~" << v.modified << " -" << v.deleted
                  << (v.failed ? QStringLiteral(" !%1").arg(v.failed) : QString()) << "  "
                  << v.finishedAt.toLocalTime().toString(QStringLiteral("dd/MM/yyyy HH:mm"));
        } else {
            out() << "(sem resumo)";
        }
        out() << "\n";
    }
    out().flush();
    return ExitOk;
}

int cmdHash(const QStringList& args)
{
    QCommandLineParser p;
    p.addOption({QStringLiteral("uncached"), QStringLiteral("Lê sem o cache do Windows.")});
    if (!parseSub(p, args) || p.positionalArguments().size() != 1)
        return (usage(), ExitUsage);
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
        return (usage(), ExitUsage);
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
        return (usage(), ExitUsage);
    synkro::CopyOptions opt;
    opt.verify = p.isSet(QStringLiteral("uncached")) ? synkro::VerifyMode::RereadUncached : synkro::VerifyMode::HashOnCopy;
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
    p.addOption({QStringLiteral("folder"), QStringLiteral("Cria o cofre na própria pasta informada, e não na raiz do disco (testes).")});
    p.addOption({QStringLiteral("user"), QStringLiteral("Cofre em rede: usuário do compartilhamento."), QStringLiteral("usuário")});
    p.addOption({QStringLiteral("password-secret"), QStringLiteral("Cofre em rede: segredo com a senha (secret set)."),
                 QStringLiteral("nome")});
    p.addOption({QStringLiteral("encrypt"), QStringLiteral("Cifra o cofre (AES-256); mostra a chave de recuperação uma vez.")});
}

void printRecoveryKey(const synkro::crypto::EnableResult& r)
{
    out() << "\n================================================================\n"
          << "  CHAVE DE RECUPERAÇÃO DO COFRE (anote agora; ela não é exibida de novo)\n\n"
          << "      " << q(r.recoveryKey) << "\n\n"
          << "  Sem ela, o cofre só pode ser lido nesta máquina. Se este computador\n"
          << "  for perdido, é a ÚNICA forma de restaurar os backups. Guarde-a fora\n"
          << "  do PDV (impressa no cofre da loja, gerenciador de senhas...).\n"
          << "================================================================\n";
    if (r.storedOnMachine)
        out() << "chave guardada nesta máquina (DPAPI): o worker agendado cifra sem pedir nada" << Qt::endl;
    else
        err() << "aviso: a chave NÃO ficou guardada nesta máquina (" << q(r.storeError)
              << "); rode como administrador: synkro-worker vault unlock <disco>" << Qt::endl;
}

synkro::vault::CandidateReport runCheck(const QCommandLineParser& p, const QString& disk)
{
    // Cofre em rede com credencial própria: conecta antes de examinar.
    if (synkro::win::isUncPath(nativeArg(disk)) && p.isSet(QStringLiteral("user"))) {
        std::string password;
        if (p.isSet(QStringLiteral("password-secret"))) {
            auto v = synkro::SecretStore(synkro::secretsFile()).get(p.value(QStringLiteral("password-secret")).toStdString());
            if (!v)
                err() << "aviso: segredo indisponível (" << q(v.error().message) << ")" << Qt::endl;
            else
                password = std::move(v).value();
        }
        if (auto st = synkro::win::connectShare(nativeArg(disk), p.value(QStringLiteral("user")).toStdWString(), password); !st)
            err() << "aviso: não foi possível conectar ao compartilhamento: " << q(st.error().message) << Qt::endl;
        synkro::wipe(password);
    }
    synkro::vault::CandidateRequest req;
    req.candidatePath = nativeArg(disk);
    if (p.isSet(QStringLiteral("folder")))
        req.vaultLocation = nativeArg(disk);
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
        const bool network = v.kind == synkro::win::DriveKind::Network;
        out() << "volume:  " << q(v.mountPoint) << "  " << q(v.label) << "\n"
              << "tipo:    " << synkro::win::nameOf(v.kind) << " · " << q(v.fileSystem)
              << (network ? QStringLiteral(" · compartilhamento de rede")
                          : QStringLiteral(" · disco físico ")
                                + (disks.isEmpty() ? QStringLiteral("?") : disks.join(QLatin1Char(','))))
              << "\n"
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
    out() << "\n" << (report.accepted() ? (report.hasWarnings() ? "ACEITO COM AVISOS" : "ACEITO") : "RECUSADO") << Qt::endl;
}

int cmdVault(const QStringList& args)
{
    if (args.isEmpty())
        return (usage(), ExitUsage);
    const QString sub = args.first();
    QCommandLineParser p;
    addCandidateOptions(p);
    if (!parseSub(p, args.mid(1)) || p.positionalArguments().size() != 1)
        return (usage(), ExitUsage);
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
        const std::wstring native = nativeArg(disk);
        const std::wstring location = p.isSet(QStringLiteral("folder")) || synkro::win::isUncPath(native)
                                          ? synkro::win::normalizeUnc(native)
                                          : report.volume.mountPoint;
        if (auto existing = synkro::vault::readIdentity(location)) {
            out() << "\ncofre já existente reaproveitado: " << existing.value().vaultId
                  << (synkro::crypto::isEncrypted(location) ? " (cifrado)" : "") << Qt::endl;
            if (p.isSet(QStringLiteral("encrypt")) && !synkro::crypto::isEncrypted(location)) {
                auto enabled = synkro::crypto::enableEncryption(location);
                if (!enabled)
                    return fail(enabled.error());
                printRecoveryKey(enabled.value());
            }
            return ExitOk;
        }
        auto id = synkro::vault::initialize(location, label.trimmed());
        if (!id)
            return fail(id.error());
        out() << "\ncofre inicializado: " << id.value().vaultId << " em " << q(synkro::vault::vaultRoot(location)) << Qt::endl;
        if (p.isSet(QStringLiteral("encrypt"))) {
            auto enabled = synkro::crypto::enableEncryption(location);
            if (!enabled)
                return fail(enabled.error());
            printRecoveryKey(enabled.value());
        }
        return ExitOk;
    }
    if (sub == QLatin1String("show")) {
        const std::wstring location = vaultLocationArg(disk);
        auto id = synkro::vault::readIdentity(location);
        if (!id)
            return fail(id.error());
        out() << synkro::vault::serialize(id.value());
        if (synkro::crypto::isEncrypted(location)) {
            const auto cipher = synkro::crypto::cipherFor(location);
            out() << "\ncifrado: sim (AES-256-GCM); chave "
                  << (cipher ? QStringLiteral("disponível nesta máquina")
                             : QStringLiteral("NÃO disponível nesta máquina (vault unlock)"))
                  << "\n";
        } else {
            out() << "\ncifrado: não\n";
        }
        out().flush();
        return ExitOk;
    }
    if (sub == QLatin1String("unlock")) {
        // Guarda a chave do cofre nesta máquina (DPAPI) a partir da chave de
        // recuperação: PDV reinstalado ou cofre trazido de outra loja.
        const std::wstring location = vaultLocationArg(disk);
        if (!synkro::crypto::isEncrypted(location)) {
            out() << "o cofre não é cifrado; nada a fazer" << Qt::endl;
            return ExitOk;
        }
        std::string text = readSecretInteractively(QStringLiteral("Chave de recuperação (XXXX-XXXX-...): "));
        auto key = synkro::crypto::unlockWithRecoveryKey(location, text);
        synkro::wipe(text);
        if (!key)
            return fail(key.error());
        auto id = synkro::vault::readIdentity(location);
        auto st = synkro::crypto::storeKeyOnMachine(id.value().vaultId, key.value());
        synkro::crypto::wipe(key.value());
        if (!st)
            return fail(st.error());
        out() << "chave do cofre \"" << id.value().label << "\" guardada nesta máquina" << Qt::endl;
        return ExitOk;
    }
    return (usage(), ExitUsage);
}

} // namespace

int main(int argc, char* argv[])
{
    ::SetConsoleOutputCP(CP_UTF8);
    // Sem caixas de diálogo do Windows ("insira um disco") em execução agendada.
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);

    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("synkro-worker"));
    QCoreApplication::setApplicationVersion(QString::fromLatin1(synkro::versionString()));

    const QStringList args = QCoreApplication::arguments().mid(1);
    if (args.isEmpty())
        return (usage(), ExitUsage);
    const QString command = args.first();
    const QStringList rest = args.mid(1);

    if (command == QLatin1String("version") || command == QLatin1String("--version")) {
        out() << "synkro-worker " << synkro::versionString() << Qt::endl;
        return ExitOk;
    }
    if (command == QLatin1String("help") || command == QLatin1String("--help") || command == QLatin1String("-h"))
        return (usage(), ExitOk);

    using Handler = int (*)(const QStringList&);
    static const std::pair<const char*, Handler> commands[] = {
        {"hash", cmdHash},         {"scan", cmdScan},           {"copy", cmdCopy},
        {"vault", cmdVault},       {"backup", cmdBackup},       {"restore", cmdRestore},
        {"restore-db", cmdRestoreDb}, {"restore-test", cmdRestoreTest}, {"verify", cmdVerify},    {"runs", cmdRuns},
        {"dumps", cmdDumps},       {"config", cmdConfig},       {"secret", cmdSecret},
        {"status", cmdStatus},     {"maintenance", cmdMaintenance}, {"schedule", cmdSchedule},
        {"shutdown", cmdShutdown}, {"notify", cmdNotify},
    };
    for (const auto& [name, handler] : commands) {
        if (command == QLatin1String(name))
            return handler(rest);
    }
    err() << "comando desconhecido: " << command << "\n" << Qt::endl;
    usage();
    return ExitUsage;
}
