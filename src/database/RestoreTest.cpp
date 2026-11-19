#include "database/RestoreTest.h"

#include "core/Paths.h"
#include "core/RunId.h"
#include "database/DatabaseBackup.h"
#include "database/DatabaseRestore.h"
#include "database/DumpStore.h"
#include "database/SqlClient.h"
#include "database/Sqlite.h"
#include "database/Tools.h"
#include "platform/windows/Locks.h"
#include "platform/windows/Win32.h"
#include "secrets/SecretStore.h"
#include "vault/JobLayout.h"
#include "vault/Vault.h"

#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <map>
#include <set>

namespace synkro::db {

bool RestoreTestResult::ok() const
{
    if (items.empty())
        return false;
    return std::all_of(items.begin(), items.end(), [](const RestoreTestItem& i) { return i.ok; });
}

std::string verifyDatabaseName(const std::wstring& runId)
{
    std::string id = win::toUtf8(runId.size() > 2 ? runId.substr(2) : runId);
    for (char& c : id) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')))
            c = '_';
    }
    return "synkro_verify_" + id;
}

bool isTruthy(const std::string& value)
{
    std::string v;
    for (char c : value) {
        if (!std::isspace(static_cast<unsigned char>(c)))
            v += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return v == "t" || v == "true" || v == "1" || v == "yes" || v == "sim" || v == "ok";
}

namespace {

std::string firstLine(const std::string& output)
{
    auto lines = outputLines(output);
    return lines.empty() ? std::string() : lines.front();
}

// Primeira coluna da primeira linha de uma consulta num arquivo SQLite.
Result<std::string> sqliteScalar(const std::wstring& path, const std::string& sql)
{
    auto db = SqliteDb::open(path, SqliteDb::Mode::ReadOnly);
    if (!db)
        return db.error();
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db.value().handle(), sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
        return Error::make(ErrorCode::InvalidArgument, "consulta inválida: " + db.value().lastError());
    std::string out;
    const int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        if (const unsigned char* t = sqlite3_column_text(stmt, 0))
            out = reinterpret_cast<const char*>(t);
    } else if (rc != SQLITE_DONE) {
        const std::string err = db.value().lastError();
        sqlite3_finalize(stmt);
        return Error::make(ErrorCode::IoError, "a consulta falhou: " + err);
    }
    sqlite3_finalize(stmt);
    return out;
}

void compareTables(const DumpRecord& dump, const std::vector<std::string>& found, RestoreTestItem& item)
{
    item.tablesFound = found.size();
    if (!dump.tablesKnown)
        return;
    item.tablesExpected = dump.tables.size();
    std::set<std::string> have;
    for (auto t : found) {
        std::transform(t.begin(), t.end(), t.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        have.insert(t);
    }
    for (const auto& t : dump.tables) {
        std::string k = t;
        std::transform(k.begin(), k.end(), k.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (!have.count(k))
            item.missingTables.push_back(t);
    }
}

std::string summarize(const RestoreTestItem& item)
{
    std::string s;
    if (!item.detail.empty())
        return item.detail;
    s = std::to_string(item.tablesFound) + " tabela(s) restaurada(s)";
    if (item.tablesExpected)
        s += " de " + std::to_string(item.tablesExpected) + " no dump";
    if (!item.missingTables.empty()) {
        s += "; faltando: ";
        for (size_t i = 0; i < item.missingTables.size() && i < 10; ++i)
            s += (i ? ", " : "") + item.missingTables[i];
    }
    std::size_t okChecks = 0;
    for (const auto& c : item.checks)
        okChecks += c.ok ? 1 : 0;
    if (!item.checks.empty())
        s += "; verificações " + std::to_string(okChecks) + "/" + std::to_string(item.checks.size());
    for (const auto& c : item.checks) {
        if (!c.ok)
            s += "; \"" + c.name + "\" falhou" + (c.detail.empty() ? "" : " (" + c.detail + ")");
    }
    return s;
}

void finish(RestoreTestItem& item)
{
    const bool checksOk = std::all_of(item.checks.begin(), item.checks.end(), [](const auto& c) { return c.ok; });
    if (item.detail.empty())
        item.ok = item.missingTables.empty() && checksOk;
    item.detail = summarize(item);
    if (!item.cleanupWarning.empty())
        item.detail += "; " + item.cleanupWarning;
}

// Grava o resultado no .dump.json do dump testado.
void recordResult(const vault::JobLayout& layout, DumpRecord rec, const RestoreTestItem& item)
{
    // Um backup em andamento segura o job.lock; espera até 1 minuto.
    auto lock = win::FileLock::tryAcquire(layout.lockFile());
    for (int i = 0; !lock && i < 60; ++i) {
        ::Sleep(1000);
        lock = win::FileLock::tryAcquire(layout.lockFile());
    }
    if (!lock)
        return;
    const std::wstring dumpFile = win::joinPath(layout.root(), win::fromCatalogPath(rec.relativePath));
    if (::GetFileAttributesW(win::toExtendedPath(dumpFile).c_str()) == INVALID_FILE_ATTRIBUTES)
        return; // apagado pela retenção enquanto o teste rodava
    rec.restoreTestedAt = QDateTime::currentDateTimeUtc();
    rec.restoreTestResult = item.ok ? "ok" : "failed";
    rec.restoreTestDetail = item.detail;
    const QByteArray meta = rec.serialize();
    (void)win::writeFileAtomically(metadataPathFor(dumpFile),
                                   std::string_view(meta.constData(), static_cast<size_t>(meta.size())));
    (void)rewriteDumpIndex(layout);
}

} // namespace

Result<RestoreTestResult> runRestoreTest(const RestoreTestRequest& req)
{
    auto say = [&](const std::string& m) {
        if (req.progress)
            req.progress(m);
    };
    auto identity = vault::readIdentity(req.vaultLocation);
    if (!identity)
        return Error::make(ErrorCode::NotFound, "cofre não encontrado em " + win::toUtf8(req.vaultLocation));
    const config::DatabaseJob& job = req.job;
    if (job.engine == "custom")
        return Error::make(ErrorCode::Unsupported,
                           "o teste de restauração automático não se aplica a comandos personalizados; "
                           "use custom.restore manualmente com restore-db");
    const vault::JobLayout layout(req.vaultLocation, req.jobId);

    // Dump válido mais recente de cada banco (listDumps vem do mais novo).
    std::map<std::string, DumpRecord> latest;
    for (auto& d : listDumps(layout, req.database)) {
        if (d.validationResult != "ok" || d.database == "_globals")
            continue;
        latest.try_emplace(d.database, std::move(d));
    }
    if (latest.empty())
        return Error::make(ErrorCode::NotFound, "nenhum dump válido "
                                                    + (req.database.empty() ? std::string() : "de " + req.database + " ")
                                                    + "neste cofre para testar");

    // Conexão do teste: a mesma da tarefa ou a de restore_test.connection.
    config::DatabaseJob testJob = job;
    const auto& rtc = job.restoreTestConfig;
    if (!rtc.sameConnection) {
        testJob.host = rtc.host;
        testJob.port = rtc.port;
        testJob.user = rtc.user;
        testJob.password = rtc.password;
    }
    const bool server = job.engine == "postgresql" || job.engine == "mysql" || job.engine == "mariadb";

    const std::wstring stagingRoot = req.stagingRoot.empty() ? stagingDirectory() : req.stagingRoot;
    const std::wstring testId = newRunId();
    auto work = PrivateDirectory::create(win::joinPath(stagingRoot, L"restoretest-" + testId));
    if (!work)
        return work.error();

    SqlTarget target;
    if (server) {
        auto pass = resolveSecret(testJob.password, req.secretsFile);
        if (!pass)
            return Error::make(ErrorCode::NotFound, "credencial do teste indisponível: " + pass.error().message);
        target = {job.engine, job.tools,
                  Connection{testJob.host, testJob.port, testJob.user, std::move(pass).value(), testJob.sslMode,
                             testJob.sslCa, testJob.connectTimeoutSeconds}};
    }

    RestoreTestResult result;
    result.vaultId = identity.value().vaultId;
    int seq = 0;
    for (auto& [name, dump] : latest) {
        RestoreTestItem item;
        item.database = name;
        item.runId = dump.runId;
        say("testando a restauração de " + name + "...");

        DatabaseRestoreRequest rr;
        rr.vaultLocation = req.vaultLocation;
        rr.jobId = req.jobId;
        rr.database = name;
        rr.runId = dump.runId;
        rr.job = testJob;
        rr.secretsFile = req.secretsFile;
        rr.stagingRoot = work.value().path();

        if (job.engine == "sqlite") {
            const std::wstring file = win::joinPath(work.value().path(), L"verify-" + std::to_wstring(++seq) + L".db");
            rr.targetPath = file;
            auto restored = restoreDatabase(rr);
            if (!restored) {
                item.detail = "a restauração falhou: " + restored.error().message;
            } else {
                auto db = SqliteDb::open(file, SqliteDb::Mode::ReadOnly);
                if (!db) {
                    item.detail = "o arquivo restaurado não abriu: " + db.error().message;
                } else {
                    compareTables(dump, listTables(db.value()), item);
                }
                for (const auto& ck : rtc.checks) {
                    auto v = sqliteScalar(file, ck.sql);
                    item.checks.push_back(
                        {ck.name, v && isTruthy(v.value()), v ? (isTruthy(v.value()) ? "" : "devolveu " + v.value()) : v.error().message});
                }
            }
            ::DeleteFileW(win::toExtendedPath(file).c_str());
        } else {
            const std::string temp = verifyDatabaseName(testId) + "_" + std::to_string(++seq);
            rr.targetDatabase = temp;
            auto restored = restoreDatabase(rr);
            if (!restored) {
                item.detail = "a restauração falhou: " + restored.error().message;
            } else {
                const std::string listSql =
                    job.engine == "postgresql"
                        ? "SELECT table_schema || '.' || table_name FROM information_schema.tables WHERE table_type = 'BASE TABLE' "
                          "AND table_schema NOT IN ('pg_catalog', 'information_schema') ORDER BY 1"
                        : "SHOW TABLES";
                auto tables = runSql(target, temp, listSql, work.value().path());
                if (!tables)
                    item.detail = "não foi possível listar as tabelas restauradas: " + tables.error().message;
                else
                    compareTables(dump, outputLines(tables.value()), item);
                for (const auto& ck : rtc.checks) {
                    auto v = runSql(target, temp, ck.sql, work.value().path());
                    const std::string first = v ? firstLine(v.value()) : std::string();
                    item.checks.push_back({ck.name, v && isTruthy(first),
                                           v ? (isTruthy(first) ? "" : "devolveu " + (first.empty() ? std::string("nada") : first))
                                             : v.error().message});
                }
            }
            // Apaga o banco temporário mesmo se a restauração falhou no meio.
            const std::string drop = "DROP DATABASE IF EXISTS " + quoteIdentifier(job.engine, temp);
            auto dropped = runSql(target, job.engine == "postgresql" ? "postgres" : "", drop, work.value().path());
            if (!dropped)
                item.cleanupWarning = "o banco temporário " + temp + " não pôde ser apagado: " + dropped.error().message;
        }
        finish(item);
        if (req.recordInVault)
            recordResult(layout, dump, item);
        result.items.push_back(std::move(item));
    }
    wipe(target.connection.password);
    return result;
}

} // namespace synkro::db
