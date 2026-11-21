#include "database/DatabaseBackup.h"

#include "core/Paths.h"
#include "core/RunId.h"
#include "database/Drivers.h"
#include "database/DumpStore.h"
#include "database/SqlClient.h"
#include "database/Tools.h"
#include "engine/RunSummary.h"
#include "engine/VaultOps.h"
#include "platform/windows/Locks.h"
#include "platform/windows/Win32.h"
#include "secrets/SecretStore.h"
#include "transfer/SafeCopy.h"
#include "vault/JobLayout.h"
#include "vault/Vault.h"

#include <QDateTime>
#include <QSysInfo>

#include <map>
#include <optional>
#include <set>

namespace synkro::db {

Result<std::string> resolveSecret(const config::SecretRef& ref, const std::wstring& secretsFilePath)
{
    switch (ref.kind) {
    case config::SecretRef::Kind::None:
        return std::string();
    case config::SecretRef::Kind::Env: {
        wchar_t buf[32768];
        const DWORD n = ::GetEnvironmentVariableW(win::fromUtf8(ref.name).c_str(), buf, 32768);
        if (n == 0)
            return Error::make(ErrorCode::NotFound, "variável de ambiente " + ref.name + " não definida");
        return win::toUtf8(std::wstring(buf, n));
    }
    case config::SecretRef::Kind::Secret:
        return SecretStore(secretsFilePath.empty() ? secretsFile() : secretsFilePath).get(ref.name);
    }
    return std::string();
}

namespace {

struct PlannedArtifact {
    std::string database;
    std::optional<DumpArtifact> artifact;
    std::string error;          // falha ao gerar/validar
    GuardVerdict guard;         // bloqueado pelas guardas
    bool suspicious = false;
    QDateTime startedAt, finishedAt;
};

// Compara com o último dump válido do mesmo banco (no primeiro cofre que o tiver).
void applyGuards(const config::DatabaseJob& job, PlannedArtifact& p, const std::vector<vault::JobLayout>& layouts,
                 std::vector<std::string>& warnings)
{
    if (!p.artifact)
        return;
    std::optional<DumpRecord> previous;
    for (const auto& l : layouts) {
        for (auto& d : listDumps(l, p.database)) {
            if (d.validationResult == "ok") {
                previous = std::move(d);
                break;
            }
        }
        if (previous)
            break;
    }
    if (!previous)
        return;

    const DumpArtifact& a = *p.artifact;
    if (a.tablesKnown && previous->tablesKnown) {
        if (a.tables.empty() && !previous->tables.empty()) {
            const std::string msg = "o banco " + p.database + " está sem tabelas, mas o último dump tinha "
                                    + std::to_string(previous->tables.size());
            if (job.blockOnEmptyDatabase)
                p.guard = {GuardCode::EmptySource, msg};
            else
                warnings.push_back(msg);
            if (p.guard.blocked())
                return;
        }
        std::set<std::string> now(a.tables.begin(), a.tables.end());
        std::vector<std::string> missing;
        for (const auto& t : previous->tables) {
            if (!now.count(t))
                missing.push_back(t);
        }
        if (!missing.empty() && !a.tables.empty()) {
            std::string list;
            for (size_t i = 0; i < missing.size() && i < 10; ++i)
                list += (i ? ", " : "") + missing[i];
            const std::string msg = std::to_string(missing.size()) + " tabela(s) de " + p.database
                                    + " sumiram desde o último dump: " + list;
            if (job.blockOnMissingTables)
                p.guard = {GuardCode::MassDeletion, msg};
            else
                warnings.push_back(msg);
        }
    }
    if (previous->size > 0 && static_cast<double>(a.size) < job.minSizeRatio * static_cast<double>(previous->size)) {
        p.suspicious = true;
        warnings.push_back("o dump de " + p.database + " ficou com " + std::to_string(a.size) + " bytes, menos de "
                           + std::to_string(static_cast<int>(job.minSizeRatio * 100)) + "% do anterior ("
                           + std::to_string(previous->size)
                           + " bytes); foi guardado, mas nenhum dump antigo será apagado");
    }
}

} // namespace

BackupResult runDatabaseBackup(const DatabaseBackupRequest& req)
{
    BackupResult result;
    result.runId = newRunId();
    const QDateTime runStarted = QDateTime::currentDateTimeUtc();
    auto say = [&](const std::string& m) {
        if (req.progress)
            req.progress(m);
    };

    if (!isValidJobId(req.jobId)) {
        result.error = "ID de tarefa inválido";
        return result;
    }
    if (req.vaults.empty()) {
        result.error = "nenhum cofre informado";
        return result;
    }

    auto jobLock = win::JobLock::tryAcquire(req.jobId);
    if (!jobLock) {
        result.status = jobLock.error().code == ErrorCode::AlreadyExists ? RunStatus::AlreadyRunning : RunStatus::Failed;
        result.error = jobLock.error().message;
        return result;
    }
    for (const auto& dep : req.dependsOn) {
        if (!win::JobLock::acquire(dep, static_cast<unsigned long>(req.timeoutSeconds * 1000))) {
            result.error = "a tarefa " + win::toUtf8(dep) + " (depends_on) não terminou a tempo";
            return result;
        }
    }

    const std::map<std::wstring, std::wstring> vars = {
        {L"job", req.jobId},
        {L"run_id", result.runId},
        {L"date", QDate::currentDate().toString(Qt::ISODate).toStdWString()},
    };
    auto finishHooks = [&](BackupResult& r) {
        if (!runHooks(req.hooks.after, "after", vars, r.hooks) && r.status == RunStatus::Success)
            r.status = RunStatus::Degraded;
        if (r.status == RunStatus::Success)
            runHooks(req.hooks.onSuccess, "on_success", vars, r.hooks);
        else
            runHooks(req.hooks.onFailure, "on_failure", vars, r.hooks);
    };
    if (!runHooks(req.hooks.before, "before", vars, result.hooks)) {
        result.error = "um comando de hooks.before falhou; o backup não foi executado";
        runHooks(req.hooks.onFailure, "on_failure", vars, result.hooks);
        return result;
    }

    // Pasta privada da execução: dumps em preparo e credenciais temporárias.
    const std::wstring stagingRoot = req.stagingRoot.empty() ? stagingDirectory() : req.stagingRoot;
    auto work = PrivateDirectory::create(win::joinPath(stagingRoot, result.runId));
    if (!work) {
        result.error = "não foi possível criar a área de preparo: " + work.error().message;
        finishHooks(result);
        return result;
    }

    DumpContext ctx;
    ctx.workDir = work.value().path();
    ctx.tools = req.job.tools;
    ctx.vars = vars;
    ctx.timeoutSeconds = req.timeoutSeconds;
    ctx.retries = req.job.dumpRetries;
    ctx.retryDelaySeconds = req.retryDelaySeconds;

    // ---- 1. Gerar e validar os dumps ------------------------------------------
    std::vector<PlannedArtifact> planned;
    const config::DatabaseJob& job = req.job;
    const bool server = job.engine == "postgresql" || job.engine == "mysql" || job.engine == "mariadb"
                        || job.engine == "firebird" || job.engine == "sqlserver";

    Connection conn;
    if (server) {
        auto pass = resolveSecret(job.password, req.secretsFile);
        if (!pass) {
            result.error = "credencial indisponível: " + pass.error().message;
            finishHooks(result);
            return result;
        }
        conn = connectionFor(job, std::move(pass).value());
    }

    auto dumpOne = [&](const std::string& name, auto&& fn) {
        PlannedArtifact p;
        p.database = name;
        p.startedAt = QDateTime::currentDateTimeUtc();
        say("gerando dump de " + name + "...");
        auto a = fn();
        p.finishedAt = QDateTime::currentDateTimeUtc();
        if (a) {
            p.artifact = std::move(a).value();
            for (const auto& wmsg : p.artifact->warnings)
                result.warnings.push_back(name + ": " + wmsg);
        } else {
            p.error = a.error().message;
        }
        planned.push_back(std::move(p));
    };

    if (server) {
        std::vector<config::DatabaseEntry> dbs = job.databases;
        if (job.allDatabases) {
            auto listed = listServerDatabases(job, conn, ctx);
            if (!listed) {
                wipe(conn.password);
                result.error = listed.error().message;
                finishHooks(result);
                return result;
            }
            for (const auto& n : listed.value())
                dbs.push_back({n, {}, {}});
        }
        // Trava no servidor (§4A.3): segura durante o dump de cada banco.
        SqlTarget lockTarget{job.engine, job.tools, conn};
        const std::int64_t hold = ctx.timeoutSeconds * (ctx.retries + 1) + 600;
        std::size_t skipped = 0;
        for (const auto& db : dbs) {
            std::unique_ptr<ServerLock> held;
            if (job.serverLock) {
                say("pedindo a trava no servidor para " + db.name + "...");
                auto l = ServerLock::acquire(lockTarget, db.name, ctx.workDir, hold);
                if (!l) {
                    if (l.error().code == ErrorCode::AlreadyExists) {
                        ++skipped;
                        result.warnings.push_back(db.name + ": ignorado — " + l.error().message);
                    } else {
                        PlannedArtifact p;
                        p.database = db.name;
                        p.error = "trava no servidor: " + l.error().message;
                        planned.push_back(std::move(p));
                    }
                    continue;
                }
                held = std::move(l).value();
            }
            if (job.engine == "postgresql")
                dumpOne(db.name, [&] { return dumpPostgres(job, conn, db, ctx); });
            else if (job.engine == "firebird")
                dumpOne(db.name, [&] { return dumpFirebird(job, conn, db, ctx); });
            else if (job.engine == "sqlserver")
                dumpOne(db.name, [&] { return dumpSqlServer(job, conn, db.name, ctx); });
            else
                dumpOne(db.name, [&] { return dumpMysql(job, conn, db.name, ctx); });
        }
        wipe(lockTarget.connection.password);
        if (skipped > 0 && planned.empty()) {
            wipe(conn.password);
            result.status = RunStatus::AlreadyRunning;
            result.error = "dump em andamento em outra máquina (trava no servidor); nada foi gerado nesta execução";
            runHooks(req.hooks.after, "after", vars, result.hooks);
            return result;
        }
        if (job.engine == "postgresql" && job.options.count("include_globals") && job.options.at("include_globals") == "true")
            dumpOne("_globals", [&] { return dumpPostgresGlobals(job, conn, ctx); });
    } else if (job.engine == "sqlite") {
        std::set<std::string> used;
        for (const auto& file : expandSqliteFiles(job.sqliteFiles)) {
            std::string stem = win::toUtf8(file.substr(file.find_last_of(L'\\') + 1));
            stem = stem.substr(0, stem.find('.'));
            std::string name = stem;
            for (int i = 2; used.count(name); ++i)
                name = stem + "-" + std::to_string(i);
            used.insert(name);
            dumpOne(name, [&] { return dumpSqlite(job, file, name, ctx); });
        }
        if (planned.empty()) {
            result.error = "nenhum arquivo SQLite encontrado em sqlite.files";
            finishHooks(result);
            return result;
        }
    } else if (job.engine == "custom") {
        std::map<std::wstring, std::wstring> env;
        for (const auto& [var, ref] : job.customEnv) {
            auto v = resolveSecret(ref, req.secretsFile);
            if (!v) {
                result.error = "credencial indisponível para " + var + ": " + v.error().message;
                finishHooks(result);
                return result;
            }
            env[win::fromUtf8(var)] = win::fromUtf8(v.value());
            std::string tmp = std::move(v).value();
            wipe(tmp);
        }
        std::string name = job.options.count("output_name") ? job.options.at("output_name") : "custom";
        name = safeName(name.substr(0, name.find('.')));
        dumpOne(name, [&] { return dumpCustom(job, env, ctx); });
    } else {
        result.error = "motor não suportado: " + job.engine;
        finishHooks(result);
        return result;
    }
    wipe(conn.password);

    // ---- 2. Guardas ------------------------------------------------------------
    std::vector<vault::JobLayout> connected;
    for (const auto& t : req.vaults) {
        if (!t.location.empty() && vault::readIdentity(t.location))
            connected.emplace_back(t.location, req.jobId);
    }
    for (auto& p : planned)
        applyGuards(job, p, connected, result.warnings);

    // ---- 3. Gravar nos cofres ----------------------------------------------------
    for (const auto& target : req.vaults) {
        VaultRunResult vr;
        vr.location = target.location;
        auto fail = [&](std::string msg) {
            vr.status = VaultRunStatus::Failed;
            vr.error = std::move(msg);
            result.vaults.push_back(vr);
        };
        if (target.location.empty()) {
            vr.vaultId = target.expectedVaultId;
            fail("o cofre \"" + target.label.toStdString() + "\" não está conectado"
                 + (target.unavailableReason.empty() ? "" : ": " + target.unavailableReason));
            continue;
        }
        auto identity = vault::readIdentity(target.location);
        if (!identity) {
            fail("cofre não encontrado ou inválido em " + win::toUtf8(target.location) + ": " + identity.error().message);
            continue;
        }
        vr.vaultId = identity.value().vaultId;
        if (!target.expectedVaultId.isEmpty() && identity.value().vaultId != target.expectedVaultId) {
            fail("o disco em " + win::toUtf8(target.location) + " não é o cofre esperado");
            continue;
        }
        say("gravando no cofre " + win::toUtf8(target.location) + "...");

        const vault::JobLayout layout(target.location, req.jobId);
        bool dirsOk = true;
        for (const auto& dir : {layout.runs(), layout.tmp(), win::joinPath(layout.root(), L"dumps")}) {
            if (!win::createDirectories(dir)) {
                dirsOk = false;
                break;
            }
        }
        if (!dirsOk) {
            fail("não foi possível criar as pastas da tarefa no cofre");
            continue;
        }
        auto lock = win::FileLock::tryAcquire(layout.lockFile());
        if (!lock) {
            fail(lock.error().message);
            continue;
        }
        (void)ops::cleanTempDirectory(layout);

        bool anyBlocked = false;
        for (const auto& p : planned) {
            if (!p.artifact) {
                ++vr.failed;
                vr.fileErrors.emplace_back(win::fromUtf8(p.database), p.error);
                continue;
            }
            if (p.guard.blocked()) {
                anyBlocked = true;
                vr.guard = p.guard;
                continue;
            }
            const DumpArtifact& a = *p.artifact;
            const std::wstring rel = dumpRelativePath(p.database, p.startedAt, result.runId, a.extension);
            const std::wstring dest = win::joinPath(layout.root(), win::fromCatalogPath(rel));

            CopyOptions opt;
            opt.verify = VerifyMode::RereadUncached;
            opt.tempDirectory = layout.tmp();
            opt.preserveTimestamps = false;
            auto copied = copyFileVerified(a.stagedFile, dest, opt);
            if (!copied || copied.value().sha256 != a.sha256) {
                ++vr.failed;
                vr.fileErrors.emplace_back(win::fromUtf8(p.database),
                                           copied ? "o dump gravado no cofre não confere com o original"
                                                  : copied.error().message);
                ::DeleteFileW(win::toExtendedPath(dest).c_str());
                continue;
            }

            DumpRecord rec;
            rec.jobId = req.jobId;
            rec.runId = result.runId;
            rec.engine = job.engine;
            rec.database = p.database;
            rec.relativePath = rel;
            rec.dumpFormat = a.dumpFormat;
            rec.compression = a.compression;
            rec.size = a.size;
            rec.sha256 = a.sha256;
            rec.startedAt = p.startedAt;
            rec.finishedAt = p.finishedAt;
            rec.host = server ? job.host + ":" + std::to_string(job.port) : std::string();
            rec.toolPath = a.toolPath;
            rec.toolVersion = a.toolVersion;
            rec.command = a.command;
            rec.validationMethod = a.validationMethod;
            rec.validationResult = a.validationResult;
            rec.tablesKnown = a.tablesKnown;
            rec.tables = a.tables;
            rec.warnings = a.warnings;
            rec.suspicious = p.suspicious;
            rec.restoreHint = a.restoreHint;
            rec.createdByHost = QSysInfo::machineHostName().toStdString();
            const QByteArray meta = rec.serialize();
            if (auto st = win::writeFileAtomically(metadataPathFor(dest),
                                                   std::string_view(meta.constData(), static_cast<size_t>(meta.size())));
                !st) {
                ++vr.failed;
                vr.fileErrors.emplace_back(win::fromUtf8(p.database), st.error().message);
                continue;
            }
            ++vr.added;
            vr.bytesWritten += a.size;
        }

        (void)rewriteDumpIndex(layout);

        if (anyBlocked)
            vr.status = VaultRunStatus::Blocked;
        else if (vr.failed == 0)
            vr.status = VaultRunStatus::Success;
        else
            vr.status = vr.added > 0 ? VaultRunStatus::Partial : VaultRunStatus::Failed;

        // Retenção só para bancos cujo dump desta execução saiu bom.
        if (req.applyRetention) {
            const GfsPolicy policy{job.keepLast, job.keepDaily, job.keepWeekly, job.keepMonthly};
            for (const auto& p : planned) {
                if (p.artifact && !p.guard.blocked() && !p.suspicious)
                    (void)applyDumpRetention(layout, p.database, policy, QDateTime::currentDateTime());
            }
            (void)rewriteDumpIndex(layout);
        }

        RunSummary s;
        s.runId = result.runId;
        s.jobId = req.jobId;
        s.vaultId = vr.vaultId;
        s.source = QString::fromStdString(job.engine + (server ? " " + job.host : std::string()));
        s.status = QString::fromLatin1(nameOf(vr.status));
        s.startedAt = runStarted;
        s.finishedAt = QDateTime::currentDateTimeUtc();
        s.filesScanned = planned.size();
        s.added = vr.added;
        s.failed = vr.failed;
        s.bytesWritten = vr.bytesWritten;
        if (vr.guard.blocked())
            s.note = QString::fromStdString(vr.guard.message);
        (void)s.save(layout.summaryFile(result.runId));
        result.vaults.push_back(vr);
    }

    result.filesScanned = planned.size();
    for (const auto& p : planned) {
        if (p.artifact)
            result.bytesScanned += p.artifact->size;
    }
    result.status = aggregateRunStatus(result.vaults, req.minVaultsOk);
    finishHooks(result);
    return result;
}

} // namespace synkro::db
