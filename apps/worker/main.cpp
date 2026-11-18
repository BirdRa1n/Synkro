// synkro-worker — executor de linha de comando do Synkro.
//
// Nesta fase expõe as operações básicas do motor, úteis para testes manuais
// e diagnóstico em campo. Os comandos de backup/restauração entram conforme
// o roadmap (architecture.md §10.2).

#include "config/ConfigLoader.h"
#include "config/JobRequest.h"
#include "config/PlanDescriber.h"
#include "core/Version.h"
#include "engine/BackupEngine.h"
#include "engine/Restore.h"
#include "engine/RunSummary.h"
#include "engine/Verify.h"
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
          << "\n"
          << "  backup --job <id> [--config-dir D] [--confirm-mass-change]\n"
          << "                                       executa uma tarefa da configuração\n"
          << "  backup --source P --vault V... --job-id ID [--confirm-mass-change]\n"
          << "                                       backup avulso, sem configuração\n"
          << "  restore --vault V --job ID --to PASTA [--run R] [--path P]\n"
          << "                                       restaura para uma pasta nova (point-in-time com --run)\n"
          << "  verify --vault V --job ID [--sample N]\n"
          << "                                       relê o cofre sem cache e confere o SHA-256\n"
          << "  runs --vault V --job ID              lista as execuções guardadas no cofre\n"
          << "\n"
          << "  config validate [--config-dir D]     valida os YAMLs\n"
          << "  config plan [--job ID] [--config-dir D]\n"
          << "                                       descreve o que cada tarefa vai fazer\n"
          << Qt::endl;
}

bool parseSub(QCommandLineParser& p, const QStringList& args);

std::wstring configDirFrom(const QCommandLineParser& p)
{
    const QString dir = p.value(QStringLiteral("config-dir"));
    return dir.isEmpty() ? synkro::config::defaultConfigDirectory() : nativeArg(dir);
}

// Carrega a configuração e imprime os diagnósticos. Devolve false se houver erros.
bool loadAndReport(const std::wstring& dir, synkro::config::ConfigSet& set, bool quietWhenClean)
{
    set = synkro::config::loadConfig(dir);
    for (const auto& d : set.diagnostics)
        out() << q(synkro::config::formatDiagnostic(d)) << "\n";
    if (!set.diagnostics.empty() || !quietWhenClean) {
        out() << "\n" << set.errorCount() << " erro(s), " << set.warningCount() << " aviso(s) em " << q(dir) << ".";
        if (set.hasErrors())
            out() << " Configuração NÃO aplicada.";
        out() << Qt::endl;
    }
    return !set.hasErrors();
}

int cmdConfig(const QStringList& args)
{
    if (args.isEmpty())
        return usage(), ExitUsage;
    const QString sub = args.first();
    QCommandLineParser p;
    p.addOption({QStringLiteral("config-dir"), QStringLiteral("Pasta de configuração."), QStringLiteral("pasta")});
    p.addOption({QStringLiteral("job"), QStringLiteral("Tarefa."), QStringLiteral("id")});
    if (!parseSub(p, args.mid(1)))
        return usage(), ExitUsage;

    synkro::config::ConfigSet set;
    const bool ok = loadAndReport(configDirFrom(p), set, sub == QLatin1String("plan"));

    if (sub == QLatin1String("validate")) {
        if (ok)
            out() << "Configuração válida: " << set.jobs.size() << " tarefa(s), " << set.global.vaults.size()
                  << " cofre(s)." << Qt::endl;
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
    return usage(), ExitUsage;
}

void printBackupResult(const synkro::BackupResult& r)
{
    out() << "\nexecução " << q(r.runId) << ": " << synkro::toString(r.status) << "\n";
    if (!r.error.empty())
        out() << "  " << q(r.error) << "\n";
    out() << "  origem: " << r.filesScanned << " arquivos, " << humanBytes(r.bytesScanned) << "\n";
    for (const auto& v : r.vaults) {
        out() << "  cofre " << (v.vaultId.isEmpty() ? q(v.location) : v.vaultId) << " [" << synkro::toString(v.status)
              << "]";
        if (!v.location.empty())
            out() << " em " << q(v.location);
        out() << "\n";
        if (!v.error.empty())
            out() << "    erro: " << q(v.error) << "\n";
        if (v.guard.blocked())
            out() << "    BLOQUEADO (" << synkro::toString(v.guard.code) << "): " << q(v.guard.message)
                  << "\n    Revise a origem; para aplicar mesmo assim use --confirm-mass-change.\n";
        if (v.status == synkro::VaultRunStatus::Success || v.status == synkro::VaultRunStatus::Partial)
            out() << "    novos " << v.added << " · alterados " << v.modified << " · excluídos " << v.deleted
                  << " · inalterados " << v.unchanged << " · falhas " << v.failed << " · gravados "
                  << humanBytes(v.bytesWritten) << (v.recoveredRuns ? " · execução interrompida recuperada" : "")
                  << "\n";
        for (const auto& [path, msg] : v.fileErrors)
            out() << "    ! " << q(path) << ": " << q(msg) << "\n";
    }
    out().flush();
}

int exitFor(synkro::RunStatus s)
{
    switch (s) {
    case synkro::RunStatus::Success:        return ExitOk;
    case synkro::RunStatus::Degraded:       return 1;
    case synkro::RunStatus::Blocked:        return ExitBlocked;
    case synkro::RunStatus::AlreadyRunning: return 4;
    case synkro::RunStatus::Failed:         break;
    }
    return ExitFailure;
}

int cmdBackup(const QStringList& args)
{
    QCommandLineParser p;
    p.addOption({QStringLiteral("job"), QStringLiteral("Tarefa da configuração."), QStringLiteral("id")});
    p.addOption({QStringLiteral("config-dir"), QStringLiteral("Pasta de configuração."), QStringLiteral("pasta")});
    p.addOption({QStringLiteral("source"), QStringLiteral("Origem (backup avulso)."), QStringLiteral("pasta")});
    p.addOption({QStringLiteral("vault"), QStringLiteral("Cofre (backup avulso)."), QStringLiteral("disco")});
    p.addOption({QStringLiteral("job-id"), QStringLiteral("ID da tarefa (backup avulso)."), QStringLiteral("id")});
    p.addOption({QStringLiteral("exclude"), QStringLiteral("Padrão de exclusão (avulso)."), QStringLiteral("padrão")});
    p.addOption({QStringLiteral("uncached"), QStringLiteral("Verificação relendo sem cache (avulso).")});
    p.addOption({QStringLiteral("confirm-mass-change"), QStringLiteral("Aplica mesmo com exclusão/modificação em massa.")});
    if (!parseSub(p, args))
        return usage(), ExitUsage;

    synkro::BackupRequest req;
    if (p.isSet(QStringLiteral("job"))) {
        synkro::config::ConfigSet set;
        if (!loadAndReport(configDirFrom(p), set, true))
            return ExitFailure;
        const auto* job = set.findJob(p.value(QStringLiteral("job")).toStdString());
        if (!job) {
            err() << "tarefa não encontrada: " << p.value(QStringLiteral("job")) << Qt::endl;
            return ExitFailure;
        }
        auto built = synkro::config::makeBackupRequest(set, *job);
        if (!built)
            return fail(built.error());
        req = std::move(built).value();
        out() << "tarefa \"" << q(job->name) << "\"" << Qt::endl;
    } else {
        if (!p.isSet(QStringLiteral("source")) || p.values(QStringLiteral("vault")).isEmpty()
            || !p.isSet(QStringLiteral("job-id")))
            return usage(), ExitUsage;
        req.jobId = p.value(QStringLiteral("job-id")).toStdWString();
        req.sourceRoot = nativeArg(p.value(QStringLiteral("source")));
        for (const auto& v : p.values(QStringLiteral("vault"))) {
            auto vol = synkro::win::queryVolume(nativeArg(v));
            req.vaults.push_back({vol ? vol.value().mountPoint : nativeArg(v), {}, v});
            // Pastas comuns (não raiz de volume) também podem conter um cofre.
            if (synkro::vault::readIdentity(nativeArg(v)))
                req.vaults.back().location = nativeArg(v);
        }
        std::vector<std::wstring> excludes;
        for (const auto& e : p.values(QStringLiteral("exclude")))
            excludes.push_back(e.toStdWString());
        req.filter = synkro::PathFilter({}, excludes);
        if (p.isSet(QStringLiteral("uncached")))
            req.verify = synkro::VerifyMode::RereadUncached;
    }
    req.overrideGuards = p.isSet(QStringLiteral("confirm-mass-change"));

    std::size_t lastPercent = 0;
    req.progress = [&](const synkro::ProgressEvent& e) {
        switch (e.kind) {
        case synkro::ProgressKind::ScanFinished:
            out() << "origem analisada: " << e.total << " arquivos" << Qt::endl;
            break;
        case synkro::ProgressKind::VaultStarted:
            out() << "cofre " << q(e.vault) << "..." << Qt::endl;
            lastPercent = 0;
            break;
        case synkro::ProgressKind::FileDone:
            if (e.total > 0) {
                const std::size_t pct = e.done * 100 / e.total;
                if (pct / 10 > lastPercent / 10) {
                    out() << "  " << pct << "% (" << e.done << "/" << e.total << ")" << Qt::endl;
                    lastPercent = pct;
                }
            }
            break;
        case synkro::ProgressKind::VaultFinished:
            break;
        }
    };

    const auto result = synkro::runBackup(req);
    printBackupResult(result);
    return exitFor(result.status);
}

std::wstring vaultLocationArg(const QString& v)
{
    const std::wstring path = nativeArg(v);
    if (synkro::vault::readIdentity(path))
        return path;
    auto vol = synkro::win::queryVolume(path);
    return vol ? vol.value().mountPoint : path;
}

int cmdRestore(const QStringList& args)
{
    QCommandLineParser p;
    p.addOption({QStringLiteral("vault"), QStringLiteral("Cofre."), QStringLiteral("disco")});
    p.addOption({QStringLiteral("job"), QStringLiteral("Tarefa."), QStringLiteral("id")});
    p.addOption({QStringLiteral("to"), QStringLiteral("Pasta de destino (nova)."), QStringLiteral("pasta")});
    p.addOption({QStringLiteral("run"), QStringLiteral("Execução (padrão: a última)."), QStringLiteral("run-id")});
    p.addOption({QStringLiteral("path"), QStringLiteral("Restringe a uma pasta ou arquivo."), QStringLiteral("caminho")});
    if (!parseSub(p, args) || !p.isSet(QStringLiteral("vault")) || !p.isSet(QStringLiteral("job"))
        || !p.isSet(QStringLiteral("to")))
        return usage(), ExitUsage;

    synkro::RestoreRequest req;
    req.vaultLocation = vaultLocationArg(p.value(QStringLiteral("vault")));
    req.jobId = p.value(QStringLiteral("job")).toStdWString();
    req.targetDirectory = nativeArg(p.value(QStringLiteral("to")));
    req.runId = p.value(QStringLiteral("run")).toStdWString();
    req.pathPrefix = p.value(QStringLiteral("path")).toStdWString();

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
        return usage(), ExitUsage;

    const synkro::vault::JobLayout layout(vaultLocationArg(p.value(QStringLiteral("vault"))),
                                          p.value(QStringLiteral("job")).toStdWString());
    const double sample = p.isSet(QStringLiteral("sample")) ? p.value(QStringLiteral("sample")).toDouble() : 100.0;
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
        return usage(), ExitUsage;

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
    p.addOption({QStringLiteral("folder"),
                 QStringLiteral("Cria o cofre na própria pasta informada, e não na raiz do disco (testes).")});
}

synkro::vault::CandidateReport runCheck(const QCommandLineParser& p, const QString& disk)
{
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

        const std::wstring location = p.isSet(QStringLiteral("folder")) ? nativeArg(disk) : report.volume.mountPoint;
        auto existing = synkro::vault::readIdentity(location);
        if (existing) {
            out() << "\ncofre já existente reaproveitado: " << existing.value().vaultId << Qt::endl;
            return ExitOk;
        }
        auto id = synkro::vault::initialize(location, label.trimmed());
        if (!id)
            return fail(id.error());
        out() << "\ncofre inicializado: " << id.value().vaultId << " em " << q(synkro::vault::vaultRoot(location))
              << Qt::endl;
        return ExitOk;
    }

    if (sub == QLatin1String("show")) {
        auto volume = synkro::win::queryVolume(nativeArg(disk));
        std::wstring location = volume ? volume.value().mountPoint : nativeArg(disk);
        if (synkro::vault::readIdentity(nativeArg(disk)))
            location = nativeArg(disk);
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
    // Sem caixas de diálogo do Windows ("insira um disco") em execução agendada.
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);

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
    if (command == QLatin1String("backup"))
        return cmdBackup(rest);
    if (command == QLatin1String("restore"))
        return cmdRestore(rest);
    if (command == QLatin1String("verify"))
        return cmdVerify(rest);
    if (command == QLatin1String("runs"))
        return cmdRuns(rest);
    if (command == QLatin1String("config"))
        return cmdConfig(rest);

    err() << "comando desconhecido: " << command << "\n" << Qt::endl;
    usage();
    return ExitUsage;
}
