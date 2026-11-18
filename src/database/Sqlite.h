#pragma once

#include "core/Result.h"

#include <cstdint>
#include <string>
#include <vector>

struct sqlite3;

namespace synkro::db {

// Conexão SQLite com RAII (usada pelo estado local e pelo backup de bancos
// SQLite do PDV).
class SqliteDb {
public:
    enum class Mode { ReadOnly, ReadWriteCreate };

    static Result<SqliteDb> open(const std::wstring& path, Mode mode, int busyTimeoutMs = 10000);

    SqliteDb(SqliteDb&& other) noexcept;
    SqliteDb& operator=(SqliteDb&& other) noexcept;
    ~SqliteDb();

    sqlite3* handle() const { return m_db; }

    Status exec(const std::string& sql);
    std::string lastError() const;

private:
    SqliteDb() = default;
    sqlite3* m_db = nullptr;
};

struct SqliteBackupInfo {
    std::uint64_t pages = 0;
    std::vector<std::string> tables;
};

// Copia `source` para `destination` com a Online Backup API: o resultado é
// consistente mesmo com a aplicação gravando (a cópia recomeça sozinha se o
// banco mudar no meio), inclusive em modo WAL.
Result<SqliteBackupInfo> backupDatabase(const std::wstring& source, const std::wstring& destination,
                                        int busyTimeoutMs = 30000);

// PRAGMA integrity_check: "ok" ou a primeira mensagem de erro.
Result<std::string> integrityCheck(const std::wstring& path);

std::vector<std::string> listTables(SqliteDb& db);

} // namespace synkro::db
