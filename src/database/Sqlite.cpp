#include "database/Sqlite.h"

#include "platform/windows/Win32.h"

#include <sqlite3.h>

#include <thread>

namespace synkro::db {

Result<SqliteDb> SqliteDb::open(const std::wstring& path, Mode mode, int busyTimeoutMs)
{
    SqliteDb db;
    const std::string utf8 = win::toUtf8(path);
    const int flags = (mode == Mode::ReadOnly ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE)
                      | SQLITE_OPEN_FULLMUTEX;
    const int rc = sqlite3_open_v2(utf8.c_str(), &db.m_db, flags, nullptr);
    if (rc != SQLITE_OK) {
        std::string msg = db.m_db ? sqlite3_errmsg(db.m_db) : sqlite3_errstr(rc);
        return Error::make(rc == SQLITE_CANTOPEN ? ErrorCode::NotFound : ErrorCode::IoError,
                           "abrir banco SQLite: " + msg, static_cast<unsigned long>(rc));
    }
    sqlite3_busy_timeout(db.m_db, busyTimeoutMs);
    sqlite3_extended_result_codes(db.m_db, 1);
    return db;
}

SqliteDb::SqliteDb(SqliteDb&& other) noexcept : m_db(other.m_db)
{
    other.m_db = nullptr;
}

SqliteDb& SqliteDb::operator=(SqliteDb&& other) noexcept
{
    if (this != &other) {
        if (m_db)
            sqlite3_close_v2(m_db);
        m_db = other.m_db;
        other.m_db = nullptr;
    }
    return *this;
}

SqliteDb::~SqliteDb()
{
    if (m_db)
        sqlite3_close_v2(m_db);
}

std::string SqliteDb::lastError() const
{
    return m_db ? sqlite3_errmsg(m_db) : "sem conexão";
}

Status SqliteDb::exec(const std::string& sql)
{
    char* err = nullptr;
    const int rc = sqlite3_exec(m_db, sql.c_str(), nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        std::string msg = err ? err : sqlite3_errstr(rc);
        sqlite3_free(err);
        return Error::make(ErrorCode::IoError, "SQLite: " + msg, static_cast<unsigned long>(rc));
    }
    return Status::success();
}

std::vector<std::string> listTables(SqliteDb& db)
{
    std::vector<std::string> out;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db.handle(),
                           "SELECT name FROM sqlite_master WHERE type = 'table' AND name NOT LIKE 'sqlite_%' ORDER BY name",
                           -1, &stmt, nullptr)
        != SQLITE_OK)
        return out;
    while (sqlite3_step(stmt) == SQLITE_ROW)
        out.emplace_back(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
    sqlite3_finalize(stmt);
    return out;
}

Result<SqliteBackupInfo> backupDatabase(const std::wstring& source, const std::wstring& destination, int busyTimeoutMs)
{
    if (::GetFileAttributesW(win::toExtendedPath(source).c_str()) == INVALID_FILE_ATTRIBUTES)
        return win::lastError("acessar banco SQLite de origem");

    auto src = SqliteDb::open(source, SqliteDb::Mode::ReadOnly, busyTimeoutMs);
    if (!src)
        return src.error();
    auto dst = SqliteDb::open(destination, SqliteDb::Mode::ReadWriteCreate, busyTimeoutMs);
    if (!dst)
        return dst.error();

    sqlite3_backup* backup = sqlite3_backup_init(dst.value().handle(), "main", src.value().handle(), "main");
    if (!backup)
        return Error::make(ErrorCode::IoError, "iniciar backup SQLite: " + dst.value().lastError());

    SqliteBackupInfo info;
    int rc = SQLITE_OK;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(busyTimeoutMs * 4);
    do {
        rc = sqlite3_backup_step(backup, 256); // em lotes, sem travar a aplicação por muito tempo
        if (rc == SQLITE_BUSY || rc == SQLITE_LOCKED) {
            if (std::chrono::steady_clock::now() > deadline)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    } while (rc == SQLITE_OK || rc == SQLITE_BUSY || rc == SQLITE_LOCKED);
    info.pages = static_cast<std::uint64_t>(sqlite3_backup_pagecount(backup));
    const int finishRc = sqlite3_backup_finish(backup);
    if (rc != SQLITE_DONE || finishRc != SQLITE_OK)
        return Error::make(ErrorCode::IoError, std::string("backup SQLite falhou: ") + sqlite3_errstr(rc != SQLITE_DONE ? rc : finishRc),
                           static_cast<unsigned long>(rc));

    // A cópia sai em modo rollback journal (sem -wal/-shm ao lado).
    (void)dst.value().exec("PRAGMA journal_mode=DELETE;");
    info.tables = listTables(dst.value());
    return info;
}

Result<std::string> integrityCheck(const std::wstring& path)
{
    auto db = SqliteDb::open(path, SqliteDb::Mode::ReadOnly);
    if (!db)
        return db.error();
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db.value().handle(), "PRAGMA integrity_check", -1, &stmt, nullptr) != SQLITE_OK)
        return Error::make(ErrorCode::Corrupted, "integrity_check: " + db.value().lastError());
    std::string result = "sem resposta";
    if (sqlite3_step(stmt) == SQLITE_ROW)
        result = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    sqlite3_finalize(stmt);
    return result;
}

} // namespace synkro::db
