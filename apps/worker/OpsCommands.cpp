// Comandos de operação do synkro-worker: execução de tarefas configuradas,
// estado, manutenção, segredos, restauração de bancos e agendamento.

#include "Cli.h"

#include "config/ConfigApply.h"
#include "config/ConfigLoader.h"
#include "config/JobRequest.h"
#include "config/ShutdownPolicy.h"
#include "config/Values.h"
#include "core/Paths.h"
#include "database/DatabaseBackup.h"
#include "database/DatabaseRestore.h"
#include "database/DumpStore.h"
#include "engine/Scrub.h"
#include "engine/VaultOps.h"
#include "platform/windows/Locks.h"
#include "platform/windows/Shutdown.h"
#include "platform/windows/Snapshot.h"
#include "platform/windows/TaskScheduler.h"
#include "platform/windows/Volume.h"
#include "platform/windows/Win32.h"
#include "secrets/SecretStore.h"
#include "state/Alerts.h"
#include "state/StateStore.h"
#include "vault/JobLayout.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QTimeZone>

#include <iostream>
#include <optional>

namespace cli {

namespace {

std::optional<synkro::StateStore> openState()
{
    auto st = synkro::StateStore::open(synkro::stateDatabaseFile());
    if (!st) {
        err() << "aviso: banco de estado indisponível (" << q(st.error().message) << "); histórico e alertas não atualizados"
              << Qt::endl;
        return std::nullopt;
    }
    return std::move(st).value();
}

// Avalia os alertas, grava no estado e envia novidades ao Visualizador de Eventos.
std::vector<synkro::AlertRow> evaluateAlerts(const synkro::config::ConfigSet& set, synkro::StateStore& state)
{
    return synkro::refreshAlerts(set, state);
}

bool anotherJobRunning(const synkro::config::ConfigSet& set, const std::string& self)
{
    for (const auto& j : set.jobs) {
        if (j.id == self)
            continue;
        auto lock = synkro::win::JobLock::tryAcquire(synkro::win::fromUtf8(j.id));
        if (!lock && lock.error().code == synkro::ErrorCode::AlreadyExists)
            return true;
    }
    return false;
}

std::string readSecretInteractively(const QString& prompt)
{
    HANDLE in = ::GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    const bool console = ::GetConsoleMode(in, &mode) != 0;
    if (!console) {
        std::string line;
        std::getline(std::cin, line);
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            line.pop_back();
        return line;
    }
    err() << prompt;
    err().flush();
    ::SetConsoleMode(in, mode & ~static_cast<DWORD>(ENABLE_ECHO_INPUT));
    wchar_t buf[1024] = {};
    DWORD read = 0;
    ::ReadConsoleW(in, buf, 1023, &read, nullptr);
    ::SetConsoleMode(in, mode);
    err() << Qt::endl;
    std::wstring value(buf, read);
    while (!value.empty() && (value.back() == L'\r' || value.back() == L'\n'))
        value.pop_back();
    std::string utf8 = synkro::win::toUtf8(value);
    ::SecureZeroMemory(buf, sizeof(buf));
    ::SecureZeroMemory(value.data(), value.size() * sizeof(wchar_t));
    return utf8;
}

} // namespace

int runConfiguredJob(const synkro::config::ConfigSet& set, const synkro::config::JobConfig& job, const RunOptions& options)
{
    const QDateTime started = QDateTime::currentDateTimeUtc();
    out() << "tarefa \"" << q(job.name) << "\" (" << q(job.type) << ")" << Qt::endl;

    synkro::BackupResult result;
    if (job.type == "files") {
        auto built = synkro::config::makeBackupRequest(set, job);
        if (!built)
            return fail(built.error());
        synkro::BackupRequest req = std::move(built).value();
        req.overrideGuards = options.overrideGuards;
        std::size_t lastPercent = 0;
        req.progress = [&](const synkro::ProgressEvent& e) {
            if (e.kind == synkro::ProgressKind::ScanFinished) {
                out() << "origem analisada: " << e.total << " arquivos" << Qt::endl;
            } else if (e.kind == synkro::ProgressKind::VaultStarted) {
                out() << "cofre " << (e.vault.empty() ? QStringLiteral("(desconectado)") : q(e.vault)) << "..." << Qt::endl;
                lastPercent = 0;
            } else if (e.kind == synkro::ProgressKind::FileDone && e.total > 0) {
                const std::size_t pct = e.done * 100 / e.total;
                if (pct / 10 > lastPercent / 10) {
                    out() << "  " << pct << "% (" << e.done << "/" << e.total << ")" << Qt::endl;
                    lastPercent = pct;
                }
            }
        };
        result = synkro::runBackup(req);
    } else {
        auto built = synkro::config::makeDatabaseBackupRequest(set, job);
        if (!built)
            return fail(built.error());
        auto req = std::move(built).value();
        req.progress = [](const std::string& m) { out() << q(m) << Qt::endl; };
        result = synkro::db::runDatabaseBackup(req);
    }
    printBackupResult(result);

    if (auto state = openState()) {
        (void)state->recordRun(synkro::win::fromUtf8(job.id), job.type, options.trigger, result, started,
                               QDateTime::currentDateTimeUtc());
        for (const auto& v : result.vaults) {
            if (v.vaultId.isEmpty() || v.location.empty())
                continue;
            std::uint64_t freeBytes = 0, total = 0;
            if (auto vol = synkro::win::queryVolume(v.location)) {
                freeBytes = vol.value().freeBytes;
                total = vol.value().totalBytes;
            }
            const bool ok = v.status == synkro::VaultRunStatus::Success || v.status == synkro::VaultRunStatus::Partial;
            (void)state->recordVaultSeen(v.vaultId, v.location, freeBytes, total, ok);
        }
        const auto alerts = evaluateAlerts(set, *state);
        if (!alerts.empty())
            out() << "\nalertas abertos: " << alerts.size() << " (synkro-worker status)" << Qt::endl;
    }

    if (job.shutdownAfter) {
        const auto decision = synkro::config::decideShutdown(set, job, result.status, anotherJobRunning(set, job.id));
        if (!options.allowShutdown) {
            out() << "desligamento ignorado (--no-shutdown)" << Qt::endl;
        } else if (!decision.shutdown) {
            out() << "desligamento: " << q(decision.reason) << Qt::endl;
        } else {
            const auto seconds = static_cast<unsigned long>(set.global.shutdownCountdownSeconds);
            auto st = synkro::win::scheduleShutdown(
                seconds, L"Synkro: backup concluído com sucesso. O computador será desligado. Para cancelar: "
                         L"synkro-worker shutdown cancel (ou shutdown /a).");
            if (st)
                out() << "desligamento agendado em " << seconds << " s (cancelar: synkro-worker shutdown cancel)" << Qt::endl;
            else
                err() << "não foi possível agendar o desligamento: " << q(st.error().message) << Qt::endl;
        }
    }
    return exitFor(result.status);
}

int cmdStatus(const QStringList& args)
{
    QCommandLineParser p;
    p.addOption({QStringLiteral("config-dir"), QStringLiteral("Pasta de configuração."), QStringLiteral("pasta")});
    if (!parseSub(p, args))
        return (usage(), ExitUsage);
    synkro::config::ConfigSet set;
    loadAndReport(configDirFrom(p), set, true);
    auto state = openState();
    if (!state)
        return ExitFailure;

    out() << "\nTAREFAS\n";
    for (const auto& j : set.jobs) {
        const std::wstring id = synkro::win::fromUtf8(j.id);
        const auto last = state->lastRun(id);
        const auto ok = state->lastSuccess(id);
        out() << "  " << q(j.id).leftJustified(22) << (j.enabled ? "" : "(desativada) ");
        if (last)
            out() << "última: " << q(last->status).leftJustified(9) << " "
                  << last->finishedAt.toLocalTime().toString(QStringLiteral("dd/MM HH:mm"));
        else
            out() << "nunca executada";
        if (ok)
            out() << " · último sucesso " << ok->finishedAt.toLocalTime().toString(QStringLiteral("dd/MM HH:mm"));
        out() << "\n";
    }

    out() << "\nCOFRES\n";
    for (const auto& v : synkro::observeVaults(set, *state)) {
        const auto* vc = set.global.findVault(v.id);
        out() << "  " << q(vc ? vc->label : v.id).leftJustified(30);
        if (v.connected) {
            out() << "conectado em " << q(v.location) << " · " << humanBytes(v.freeBytes) << " livres";
        } else {
            const auto seen = state->vaultSeen(v.vaultId);
            out() << "desconectado" << (seen && seen->lastSeenAt.isValid()
                                            ? QStringLiteral(" · visto em ") + seen->lastSeenAt.toLocalTime().toString(QStringLiteral("dd/MM HH:mm"))
                                            : QString());
        }
        out() << "\n";
    }

    const auto alerts = evaluateAlerts(set, *state);
    out() << "\nALERTAS\n";
    bool critical = false;
    if (alerts.empty())
        out() << "  nenhum\n";
    for (const auto& a : alerts) {
        critical = critical || a.severity == synkro::AlertSeverity::Critical;
        out() << (a.severity == synkro::AlertSeverity::Critical ? "  [CRÍTICO] " : "  [atenção] ") << q(a.message) << "\n";
    }
    out().flush();
    return critical ? ExitFailure : ExitOk;
}

int cmdMaintenance(const QStringList& args)
{
    QCommandLineParser p;
    p.addOption({QStringLiteral("config-dir"), QStringLiteral("Pasta de configuração."), QStringLiteral("pasta")});
    p.addOption({QStringLiteral("scrub"), QStringLiteral("Também verifica os cofres (releitura sem cache).")});
    p.addOption({QStringLiteral("no-tasks"), QStringLiteral("Não mexe no Agendador de Tarefas.")});
    p.addOption({QStringLiteral("sample"), QStringLiteral("Percentual a verificar no --scrub (padrão: maintenance.scrub.sample)."),
                 QStringLiteral("N")});
    if (!parseSub(p, args))
        return (usage(), ExitUsage);

    const std::wstring dir = configDirFrom(p);
    synkro::config::ConfigSet set;
    const bool valid = loadAndReport(dir, set, true);
    if (p.isSet(QStringLiteral("sample")))
        set.global.scrubSample = std::clamp(p.value(QStringLiteral("sample")).toDouble() / 100.0, 0.001, 1.0);
    auto state = openState();
    if (!state)
        return ExitFailure;

    // 1. Mudanças na configuração feitas à mão são aplicadas automaticamente.
    const std::string hash = synkro::config::configHash(dir);
    if (valid && hash != state->getValue("config_hash")) {
        synkro::config::ApplyOptions opt;
        opt.workerExe = QDir::toNativeSeparators(QCoreApplication::applicationFilePath()).toStdWString();
        opt.registerTasks = !p.isSet(QStringLiteral("no-tasks")) && synkro::win::isProcessElevated();
        auto applied = synkro::config::applyConfig(set, opt);
        if (applied) {
            (void)state->setValue("config_hash", hash);
            out() << "configuração aplicada (" << applied.value().done.size() << " ação(ões))"
                  << (opt.registerTasks ? "" : "; tarefas do Agendador não atualizadas (requer administrador)") << Qt::endl;
        } else {
            err() << "falha ao aplicar a configuração: " << q(applied.error().message) << Qt::endl;
        }
    }

    // 2. Restos de preparo com mais de 24 h (execuções que morreram).
    const QDateTime limit = QDateTime::currentDateTimeUtc().addDays(-1);
    for (const auto& name : synkro::ops::listSubdirectories(synkro::stagingDirectory())) {
        const std::wstring full = synkro::win::joinPath(synkro::stagingDirectory(), name);
        WIN32_FILE_ATTRIBUTE_DATA info{};
        if (::GetFileAttributesExW(synkro::win::toExtendedPath(full).c_str(), GetFileExInfoStandard, &info)) {
            ULARGE_INTEGER t{{info.ftLastWriteTime.dwLowDateTime, info.ftLastWriteTime.dwHighDateTime}};
            const QDateTime when = QDateTime::fromMSecsSinceEpoch(
                static_cast<qint64>((t.QuadPart - 116444736000000000ULL) / 10000), QTimeZone::UTC);
            if (when < limit && synkro::ops::removeTree(full))
                out() << "área de preparo antiga removida: " << q(name) << Qt::endl;
        }
    }

    // 3. Verificação periódica.
    if (p.isSet(QStringLiteral("scrub"))) {
        out() << "verificando cofres (" << q(synkro::config::describePercent(set.global.scrubSample)) << " por execução)..."
              << Qt::endl;
        const auto report = synkro::runScrub(set, set.global.scrubSample, set.global.scrubRepair);
        out() << "  conferidos " << report.checked << ", íntegros " << report.ok << ", reparados " << report.repaired << Qt::endl;
        for (const auto& n : report.notes)
            out() << "  · " << q(n) << "\n";
        for (const auto& pr : report.problems)
            out() << "  ! " << q(pr) << "\n";
        std::string summary;
        if (!report.problems.empty())
            summary = std::to_string(report.problems.size()) + " problema(s) na verificação dos cofres; o primeiro: "
                      + report.problems.front();
        (void)state->setValue("scrub_problems", summary);
    }

    // 4. Alertas que dependem do tempo (backup atrasado, cofre ausente...).
    const auto alerts = evaluateAlerts(set, *state);
    out() << "alertas abertos: " << alerts.size() << Qt::endl;
    return ExitOk;
}

int cmdSecret(const QStringList& args)
{
    if (args.isEmpty())
        return (usage(), ExitUsage);
    const QString sub = args.first();
    QCommandLineParser p;
    p.addOption({QStringLiteral("stdin"), QStringLiteral("Lê o valor da entrada padrão.")});
    if (!parseSub(p, args.mid(1)))
        return (usage(), ExitUsage);
    synkro::SecretStore store(synkro::secretsFile());

    if (sub == QLatin1String("list")) {
        auto names = store.names();
        if (!names)
            return fail(names.error());
        if (names.value().empty())
            out() << "nenhum segredo cadastrado" << Qt::endl;
        for (const auto& n : names.value())
            out() << q(n) << "\n";
        out().flush();
        return ExitOk;
    }
    if (p.positionalArguments().size() != 1)
        return (usage(), ExitUsage);
    const std::string name = p.positionalArguments().first().toStdString();

    if (sub == QLatin1String("set")) {
        std::string value = readSecretInteractively(QStringLiteral("valor de \"%1\" (não aparece na tela): ").arg(q(name)));
        auto st = store.set(name, value);
        synkro::wipe(value);
        if (!st)
            return fail(st.error());
        out() << "segredo \"" << q(name) << "\" gravado em " << q(store.path())
              << (synkro::win::isProcessElevated() ? "" : " (execute como administrador para restringir a ACL do arquivo)")
              << Qt::endl;
        return ExitOk;
    }
    if (sub == QLatin1String("delete")) {
        auto st = store.remove(name);
        if (!st)
            return fail(st.error());
        out() << "segredo \"" << q(name) << "\" removido" << Qt::endl;
        return ExitOk;
    }
    return (usage(), ExitUsage);
}

int cmdRestoreDb(const QStringList& args)
{
    QCommandLineParser p;
    p.addOption({QStringLiteral("vault"), QStringLiteral("Cofre."), QStringLiteral("disco")});
    p.addOption({QStringLiteral("job"), QStringLiteral("Tarefa."), QStringLiteral("id")});
    p.addOption({QStringLiteral("database"), QStringLiteral("Banco (nome do dump)."), QStringLiteral("nome")});
    p.addOption({QStringLiteral("run"), QStringLiteral("Execução (padrão: o dump válido mais recente)."), QStringLiteral("run-id")});
    p.addOption({QStringLiteral("target-database"), QStringLiteral("Banco NOVO (PostgreSQL/MySQL)."), QStringLiteral("nome")});
    p.addOption({QStringLiteral("to"), QStringLiteral("Arquivo/pasta de destino (SQLite/custom)."), QStringLiteral("caminho")});
    p.addOption({QStringLiteral("config-dir"), QStringLiteral("Pasta de configuração."), QStringLiteral("pasta")});
    if (!parseSub(p, args) || !p.isSet(QStringLiteral("vault")) || !p.isSet(QStringLiteral("job"))
        || !p.isSet(QStringLiteral("database")))
        return (usage(), ExitUsage);

    synkro::config::ConfigSet set;
    loadAndReport(configDirFrom(p), set, true);
    const auto* job = set.findJob(p.value(QStringLiteral("job")).toStdString());

    synkro::db::DatabaseRestoreRequest req;
    req.vaultLocation = vaultLocationArg(p.value(QStringLiteral("vault")));
    req.jobId = p.value(QStringLiteral("job")).toStdWString();
    req.database = p.value(QStringLiteral("database")).toStdString();
    req.runId = p.value(QStringLiteral("run")).toStdWString();
    req.targetDatabase = p.value(QStringLiteral("target-database")).toStdString();
    if (p.isSet(QStringLiteral("to")))
        req.targetPath = nativeArg(p.value(QStringLiteral("to")));
    if (job && job->type == "database")
        req.job = job->database; // conexão e ferramentas da tarefa nesta máquina

    auto r = synkro::db::restoreDatabase(req);
    if (!r)
        return fail(r.error());
    out() << "restaurado o dump de " << q(r.value().dump.database) << " ("
          << r.value().dump.startedAt.toLocalTime().toString(QStringLiteral("dd/MM/yyyy HH:mm")) << ", SHA-256 conferido) em "
          << q(r.value().target) << (r.value().detail.empty() ? QString() : QStringLiteral(" — ") + q(r.value().detail))
          << Qt::endl;
    return ExitOk;
}

int cmdDumps(const QStringList& args)
{
    QCommandLineParser p;
    p.addOption({QStringLiteral("vault"), QStringLiteral("Cofre."), QStringLiteral("disco")});
    p.addOption({QStringLiteral("job"), QStringLiteral("Tarefa."), QStringLiteral("id")});
    if (!parseSub(p, args) || !p.isSet(QStringLiteral("vault")) || !p.isSet(QStringLiteral("job")))
        return (usage(), ExitUsage);
    const synkro::vault::JobLayout layout(vaultLocationArg(p.value(QStringLiteral("vault"))),
                                          p.value(QStringLiteral("job")).toStdWString());
    const auto dumps = synkro::db::listDumps(layout);
    if (dumps.empty())
        out() << "nenhum dump encontrado" << Qt::endl;
    for (const auto& d : dumps) {
        out() << q(d.database).leftJustified(18) << " " << d.startedAt.toLocalTime().toString(QStringLiteral("dd/MM/yyyy HH:mm"))
              << "  " << humanBytes(d.size).rightJustified(10) << "  " << q(d.runId) << "  "
              << (d.validationResult == "ok" ? "válido" : "INVÁLIDO") << (d.suspicious ? " · SUSPEITO" : "")
              << (d.tablesKnown ? QStringLiteral(" · %1 tabela(s)").arg(d.tables.size()) : QString()) << "\n";
    }
    out().flush();
    return ExitOk;
}

int cmdSchedule(const QStringList& args)
{
    if (args.isEmpty() || args.first() != QLatin1String("show"))
        return (usage(), ExitUsage);
    QCommandLineParser p;
    p.addOption({QStringLiteral("job"), QStringLiteral("Tarefa (ou Maintenance/Scrub)."), QStringLiteral("id")});
    p.addOption({QStringLiteral("config-dir"), QStringLiteral("Pasta de configuração."), QStringLiteral("pasta")});
    if (!parseSub(p, args.mid(1)) || !p.isSet(QStringLiteral("job")))
        return (usage(), ExitUsage);
    synkro::config::ConfigSet set;
    loadAndReport(configDirFrom(p), set, true);
    const std::wstring exe = QDir::toNativeSeparators(QCoreApplication::applicationFilePath()).toStdWString();
    const std::wstring wanted = p.value(QStringLiteral("job")).toStdWString();
    for (const auto& t : synkro::config::desiredTasks(set, exe)) {
        if (t.name == wanted || t.name == L"job-" + wanted) {
            out() << q(synkro::win::buildTaskXml(t));
            out().flush();
            return ExitOk;
        }
    }
    err() << "nenhuma tarefa agendada para " << q(wanted) << " (a tarefa tem schedule e está ativa?)" << Qt::endl;
    return ExitFailure;
}

int cmdShutdown(const QStringList& args)
{
    if (args.isEmpty() || args.first() != QLatin1String("cancel"))
        return (usage(), ExitUsage);
    auto st = synkro::win::cancelShutdown();
    if (!st)
        return fail(st.error());
    out() << "desligamento cancelado" << Qt::endl;
    return ExitOk;
}

int cmdConfigApply(const QStringList& args)
{
    QCommandLineParser p;
    p.addOption({QStringLiteral("config-dir"), QStringLiteral("Pasta de configuração."), QStringLiteral("pasta")});
    p.addOption({QStringLiteral("no-tasks"), QStringLiteral("Não mexe no Agendador de Tarefas.")});
    if (!parseSub(p, args))
        return (usage(), ExitUsage);
    const std::wstring dir = configDirFrom(p);
    synkro::config::ConfigSet set;
    if (!loadAndReport(dir, set, true))
        return ExitFailure;

    synkro::config::ApplyOptions opt;
    opt.workerExe = QDir::toNativeSeparators(QCoreApplication::applicationFilePath()).toStdWString();
    opt.registerTasks = !p.isSet(QStringLiteral("no-tasks"));
    auto r = synkro::config::applyConfig(set, opt);
    if (!r)
        return fail(r.error());
    for (const auto& d : r.value().done)
        out() << "  ✓ " << q(d) << "\n";
    for (const auto& w : r.value().warnings)
        out() << "  ⚠ " << q(w) << "\n";
    if (auto state = openState())
        (void)state->setValue("config_hash", synkro::config::configHash(dir));
    out() << "configuração aplicada" << Qt::endl;
    return ExitOk;
}

} // namespace cli
