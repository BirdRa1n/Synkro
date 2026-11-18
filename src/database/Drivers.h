#pragma once

#include "config/Config.h"
#include "core/Result.h"
#include "hash/Sha256.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace synkro::db {

// Conexão já resolvida (senha decifrada). Nunca é gravada em disco fora da
// pasta privada da execução, nem em logs.
struct Connection {
    std::string host;
    int port = 0;
    std::string user;
    std::string password;
    std::string sslMode;
    std::wstring sslCa;
    std::int64_t connectTimeoutSeconds = 15;
};

// Resultado de um dump já validado, em preparo no staging.
struct DumpArtifact {
    std::string database;          // nome lógico (vira a pasta dumps/<database>/ no cofre)
    std::wstring stagedFile;
    std::string extension;         // ".dump", ".sql.zst", ".db.zst"...
    std::string dumpFormat;        // custom | plain | sqlite | command
    std::string compression;       // pg-internal | zstd | none
    std::uint64_t size = 0;
    Sha256Digest sha256{};
    bool tablesKnown = false;
    std::vector<std::string> tables;
    std::string validationMethod;
    std::string validationResult;  // "ok" ou motivo
    std::string toolPath;
    std::string toolVersion;
    std::string command;           // linha de comando sem segredos
    std::string restoreHint;
    std::vector<std::string> warnings;
};

struct DumpContext {
    std::wstring workDir;          // pasta privada da execução
    std::map<std::string, std::wstring> tools;
    std::map<std::wstring, std::wstring> vars; // {job}, {run_id}, {date}
    std::int64_t timeoutSeconds = 7200;
    int retries = 1;               // novas tentativas do dump inteiro
    int retryDelaySeconds = 30;
};

// ---- Construção de argumentos (pura, testada) -----------------------------

std::vector<std::wstring> pgDumpArgs(const std::wstring& tool, const Connection& c, const std::string& database,
                                     const std::wstring& outFile, const config::DatabaseJob& job,
                                     const std::vector<std::string>& excludeTables);

std::vector<std::wstring> mysqldumpArgs(const std::wstring& tool, const std::wstring& defaultsFile, const std::string& engine,
                                        const std::string& database, const config::DatabaseJob& job);

std::string mysqlDefaultsFile(const Connection& c, const std::string& engine);

// ---- Dumps -----------------------------------------------------------------

// Lista os bancos do servidor (databases: all).
Result<std::vector<std::string>> listServerDatabases(const config::DatabaseJob& job, const Connection& c,
                                                     const DumpContext& ctx);

Result<DumpArtifact> dumpPostgres(const config::DatabaseJob& job, const Connection& c, const config::DatabaseEntry& db,
                                  const DumpContext& ctx);
Result<DumpArtifact> dumpPostgresGlobals(const config::DatabaseJob& job, const Connection& c, const DumpContext& ctx);
Result<DumpArtifact> dumpMysql(const config::DatabaseJob& job, const Connection& c, const std::string& database,
                               const DumpContext& ctx);
Result<DumpArtifact> dumpSqlite(const config::DatabaseJob& job, const std::wstring& file, const std::string& logicalName,
                                const DumpContext& ctx);
Result<DumpArtifact> dumpCustom(const config::DatabaseJob& job, const std::map<std::wstring, std::wstring>& env,
                                const DumpContext& ctx);

// Expande curingas em sqlite.files ("C:\PDV\dados\*.db").
std::vector<std::wstring> expandSqliteFiles(const std::vector<std::wstring>& patterns);

// Nome seguro para pasta no cofre.
std::string safeName(const std::string& name);

} // namespace synkro::db
