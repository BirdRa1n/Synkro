#include "database/DatabaseRestore.h"

#include "core/Paths.h"
#include "core/RunId.h"
#include "database/DatabaseBackup.h"
#include "database/Sqlite.h"
#include "database/Tools.h"
#include "database/Zstd.h"
#include "engine/Hooks.h"
#include "engine/VaultOps.h"
#include "platform/windows/Win32.h"
#include "process/ProcessRunner.h"
#include "secrets/SecretStore.h"
#include "transfer/SafeCopy.h"
#include "vault/Vault.h"

namespace synkro::db {

namespace {

std::wstring w(const std::string& s)
{
    return win::fromUtf8(s);
}

std::string tail(const std::string& s)
{
    return s.size() > 600 ? s.substr(s.size() - 600) : s;
}

bool validIdentifier(const std::string& s)
{
    if (s.empty() || s.size() > 63)
        return false;
    for (char c : s) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'))
            return false;
    }
    return true;
}

Status runChecked(ProcessSpec spec, const std::string& what)
{
    auto r = runProcess(spec);
    if (!r)
        return r.error();
    if (r.value().exitCode != 0 || r.value().timedOut)
        return Error::make(ErrorCode::IoError, what + " falhou: " + tail(r.value().stderrText));
    return Status::success();
}

} // namespace

Result<DatabaseRestoreResult> restoreDatabase(const DatabaseRestoreRequest& req)
{
    if (!vault::readIdentity(req.vaultLocation))
        return Error::make(ErrorCode::NotFound, "cofre não encontrado em " + win::toUtf8(req.vaultLocation));
    const vault::JobLayout layout(req.vaultLocation, req.jobId);

    // Escolhe o dump.
    std::optional<DumpRecord> chosen;
    for (auto& d : listDumps(layout, req.database)) {
        if (d.validationResult != "ok")
            continue;
        if (req.runId.empty() || d.runId == req.runId) {
            chosen = std::move(d);
            break;
        }
    }
    if (!chosen)
        return Error::make(ErrorCode::NotFound, "nenhum dump válido de " + req.database
                                                    + (req.runId.empty() ? "" : " na execução " + win::toUtf8(req.runId))
                                                    + " neste cofre");
    const std::wstring dumpFile = win::joinPath(layout.root(), win::fromCatalogPath(chosen->relativePath));

    // Pasta privada: cópia conferida do dump e credenciais.
    const std::wstring stagingRoot = req.stagingRoot.empty() ? stagingDirectory() : req.stagingRoot;
    auto work = PrivateDirectory::create(win::joinPath(stagingRoot, L"restore-" + newRunId()));
    if (!work)
        return work.error();

    // Copia para o preparo conferindo o SHA-256 contra os metadados.
    const std::wstring ext = chosen->relativePath.substr(chosen->relativePath.find(L'.', chosen->relativePath.find_last_of(L'/')));
    const std::wstring local = win::joinPath(work.value().path(), L"dump" + ext);
    auto copied = copyFileVerified(dumpFile, local);
    if (!copied)
        return copied.error();
    if (copied.value().sha256 != chosen->sha256)
        return Error::make(ErrorCode::Corrupted, "o dump no cofre não confere com o SHA-256 registrado; tente outro cofre");

    // Descomprime se necessário.
    std::wstring payload = local;
    if (chosen->compression == "zstd") {
        payload = local.substr(0, local.size() - 4); // remove ".zst"
        if (auto st = decompressFile(local, payload); !st)
            return st.error();
    }

    DatabaseRestoreResult result;
    result.dump = *chosen;
    const config::DatabaseJob& job = req.job;
    const std::string engine = chosen->engine;

    if (engine == "sqlite") {
        if (req.targetPath.empty())
            return Error::make(ErrorCode::InvalidArgument, "informe o arquivo de destino (--to)");
        if (ops::pathExists(req.targetPath))
            return Error::make(ErrorCode::AlreadyExists, "o arquivo de destino já existe; a restauração nunca sobrescreve");
        auto check = integrityCheck(payload);
        if (!check || check.value() != "ok")
            return Error::make(ErrorCode::Corrupted, "o dump não passou no integrity_check");
        auto placed = copyFileVerified(payload, req.targetPath);
        if (!placed)
            return placed.error();
        result.target = win::toUtf8(req.targetPath);
        result.detail = "integrity_check ok";
        return result;
    }

    if (engine == "custom") {
        if (job.customRestore.empty())
            return Error::make(ErrorCode::Unsupported, "a tarefa não define custom.restore.command");
        if (req.targetPath.empty())
            return Error::make(ErrorCode::InvalidArgument, "informe o destino (--to)");
        ProcessSpec spec;
        const std::map<std::wstring, std::wstring> vars = {{L"input", payload}, {L"target", req.targetPath}};
        for (const auto& a : job.customRestore)
            spec.args.push_back(expandVariables(w(a), vars));
        for (const auto& [var, ref] : job.customEnv) {
            auto v = resolveSecret(ref, req.secretsFile);
            if (!v)
                return v.error();
            spec.environment.emplace_back(w(var), w(v.value()));
        }
        if (auto st = runChecked(spec, "comando de restauração"); !st)
            return st.error();
        result.target = win::toUtf8(req.targetPath);
        return result;
    }

    // Servidores: sempre num banco novo.
    if (!validIdentifier(req.targetDatabase))
        return Error::make(ErrorCode::InvalidArgument,
                           "nome do banco de destino inválido (letras, números e _; até 63 caracteres)");
    auto password = resolveSecret(job.password, req.secretsFile);
    if (!password)
        return password.error();
    Connection c{job.host, job.port, job.user, password.value(), job.sslMode, job.sslCa, job.connectTimeoutSeconds};
    std::string pw = std::move(password).value();
    wipe(pw);

    if (engine == "postgresql") {
        auto pass = work.value().writeFile(L"pgpass", pgpassLine(c.host, c.port, c.user, c.password));
        if (!pass)
            return pass.error();
        std::vector<std::pair<std::wstring, std::wstring>> env = {{L"PGPASSFILE", pass.value()},
                                                                   {L"PGSSLMODE", w(c.sslMode)}};
        if (!c.sslCa.empty())
            env.emplace_back(L"PGSSLROOTCERT", c.sslCa);
        const std::vector<std::wstring> connArgs = {L"--host=" + w(c.host), L"--port=" + std::to_wstring(c.port),
                                                    L"--username=" + w(c.user), L"--no-password"};

        // createdb falha se o banco já existir: nunca sobrescreve.
        auto createdb = locateTool("createdb", job.tools);
        if (!createdb)
            return createdb.error();
        ProcessSpec create;
        create.args = {createdb.value()};
        create.args.insert(create.args.end(), connArgs.begin(), connArgs.end());
        create.args.push_back(w(req.targetDatabase));
        create.environment = env;
        if (auto st = runChecked(create, "createdb " + req.targetDatabase + " (o banco já existe?)"); !st)
            return st.error();

        ProcessSpec restore;
        if (chosen->dumpFormat == "custom") {
            auto tool = locateTool("pg_restore", job.tools);
            if (!tool)
                return tool.error();
            restore.args = {tool.value()};
            restore.args.insert(restore.args.end(), connArgs.begin(), connArgs.end());
            restore.args.push_back(L"--dbname=" + w(req.targetDatabase));
            restore.args.push_back(L"--no-owner");
            restore.args.push_back(L"--exit-on-error");
            restore.args.push_back(payload);
        } else {
            auto tool = locateTool("psql", job.tools);
            if (!tool)
                return tool.error();
            restore.args = {tool.value()};
            restore.args.insert(restore.args.end(), connArgs.begin(), connArgs.end());
            restore.args.push_back(L"--dbname=" + w(req.targetDatabase));
            restore.args.push_back(L"--set=ON_ERROR_STOP=1");
            restore.args.push_back(L"--file=" + payload);
        }
        restore.environment = env;
        if (auto st = runChecked(restore, "restauração do PostgreSQL"); !st)
            return st.error();
    } else if (engine == "mysql" || engine == "mariadb") {
        auto defaults = work.value().writeFile(L"my.cnf", mysqlDefaultsFile(c, engine));
        if (!defaults)
            return defaults.error();
        Result<std::wstring> tool = locateTool(engine == "mariadb" ? "mariadb" : "mysql", job.tools);
        if (!tool && engine == "mariadb")
            tool = locateTool("mysql", job.tools);
        if (!tool)
            return tool.error();

        // CREATE DATABASE sem IF NOT EXISTS: falha se já existir.
        ProcessSpec create;
        create.args = {tool.value(), L"--defaults-extra-file=" + defaults.value(), L"-e",
                       L"CREATE DATABASE `" + w(req.targetDatabase) + L"` CHARACTER SET utf8mb4"};
        if (auto st = runChecked(create, "CREATE DATABASE " + req.targetDatabase + " (o banco já existe?)"); !st)
            return st.error();

        ProcessSpec restore;
        restore.args = {tool.value(), L"--defaults-extra-file=" + defaults.value(), w(req.targetDatabase)};
        restore.stdinFile = payload;
        if (auto st = runChecked(restore, "restauração do MySQL"); !st)
            return st.error();
    } else {
        return Error::make(ErrorCode::Unsupported, "restauração não suportada para o motor " + engine);
    }

    wipe(c.password);
    result.target = req.targetDatabase;
    if (chosen->tablesKnown)
        result.detail = std::to_string(chosen->tables.size()) + " tabela(s) no dump";
    return result;
}

} // namespace synkro::db
