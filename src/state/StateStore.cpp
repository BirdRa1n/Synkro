#include "state/StateStore.h"

#include "platform/windows/Win32.h"

#include <QUuid>

#include <sqlite3.h>

namespace synkro {

const char* nameOf(AlertSeverity s) noexcept
{
    return s == AlertSeverity::Critical ? "critical" : "warning";
}

namespace {

// Statement preparado com RAII e bind por posição.
class Stmt {
public:
    Stmt(sqlite3* db, const char* sql) { m_ok = sqlite3_prepare_v2(db, sql, -1, &m_stmt, nullptr) == SQLITE_OK; }
    ~Stmt() { sqlite3_finalize(m_stmt); }
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;

    bool ok() const { return m_ok; }
    Stmt& bind(int i, const std::string& v)
    {
        sqlite3_bind_text(m_stmt, i, v.c_str(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
        return *this;
    }
    Stmt& bind(int i, const std::wstring& v) { return bind(i, win::toUtf8(v)); }
    Stmt& bind(int i, const QString& v) { return bind(i, v.toStdString()); }
    Stmt& bind(int i, const QDateTime& v)
    {
        if (!v.isValid()) {
            sqlite3_bind_null(m_stmt, i);
            return *this;
        }
        return bind(i, v.toUTC().toString(Qt::ISODate).toStdString());
    }
    Stmt& bind(int i, std::int64_t v)
    {
        sqlite3_bind_int64(m_stmt, i, v);
        return *this;
    }
    int step() { return sqlite3_step(m_stmt); }
    std::string text(int col) const
    {
        const auto* t = sqlite3_column_text(m_stmt, col);
        return t ? reinterpret_cast<const char*>(t) : "";
    }
    QDateTime time(int col) const
    {
        const std::string t = text(col);
        return t.empty() ? QDateTime() : QDateTime::fromString(QString::fromStdString(t), Qt::ISODate);
    }
    std::int64_t integer(int col) const { return sqlite3_column_int64(m_stmt, col); }

private:
    sqlite3_stmt* m_stmt = nullptr;
    bool m_ok = false;
};

Status stepDone(Stmt& s, const char* what)
{
    if (!s.ok())
        return Error::make(ErrorCode::Internal, std::string("SQL inválido: ") + what);
    const int rc = s.step();
    if (rc != SQLITE_DONE && rc != SQLITE_ROW)
        return Error::make(ErrorCode::IoError, std::string("gravar estado: ") + what, static_cast<unsigned long>(rc));
    return Status::success();
}

RunRow readRun(const Stmt& s)
{
    RunRow r;
    r.runId = win::fromUtf8(s.text(0));
    r.jobId = win::fromUtf8(s.text(1));
    r.type = s.text(2);
    r.trigger = s.text(3);
    r.startedAt = s.time(4);
    r.finishedAt = s.time(5);
    r.status = s.text(6);
    r.error = s.text(7);
    return r;
}

QString normalizedId(const QString& id)
{
    const QUuid u = QUuid::fromString(id);
    return u.isNull() ? id : u.toString(QUuid::WithoutBraces);
}

constexpr char kRunColumns[] = "run_id, job_id, type, trigger, started_at, finished_at, status, error";

} // namespace

Result<StateStore> StateStore::open(const std::wstring& path)
{
    const std::wstring parent = win::parentPath(path);
    if (!parent.empty()) {
        if (auto st = win::createDirectories(parent); !st)
            return st.error();
    }
    auto db = db::SqliteDb::open(path, db::SqliteDb::Mode::ReadWriteCreate);
    if (!db)
        return db.error();
    StateStore store(std::move(db).value());
    if (auto st = store.migrate(); !st)
        return st.error();
    return store;
}

Status StateStore::migrate()
{
    // WAL: o desktop lê enquanto o worker grava. synchronous=FULL: o
    // histórico não pode sumir numa queda de energia.
    return m_db.exec(R"SQL(
PRAGMA journal_mode = WAL;
PRAGMA synchronous = FULL;
CREATE TABLE IF NOT EXISTS kv (key TEXT PRIMARY KEY, value TEXT);
CREATE TABLE IF NOT EXISTS runs (
    run_id TEXT NOT NULL, job_id TEXT NOT NULL, type TEXT, trigger TEXT,
    started_at TEXT, finished_at TEXT, status TEXT, error TEXT,
    PRIMARY KEY (run_id, job_id));
CREATE INDEX IF NOT EXISTS runs_job ON runs (job_id, started_at);
CREATE TABLE IF NOT EXISTS run_vaults (
    run_id TEXT NOT NULL, job_id TEXT NOT NULL, vault_id TEXT, location TEXT, status TEXT,
    added INTEGER, modified INTEGER, deleted INTEGER, failed INTEGER, bytes INTEGER, error TEXT);
CREATE TABLE IF NOT EXISTS vaults_seen (
    vault_id TEXT PRIMARY KEY, location TEXT, last_seen_at TEXT, last_success_at TEXT,
    free_bytes INTEGER, total_bytes INTEGER);
CREATE TABLE IF NOT EXISTS alerts (
    id INTEGER PRIMARY KEY, code TEXT NOT NULL, subject TEXT NOT NULL, severity TEXT, message TEXT,
    opened_at TEXT, last_seen_at TEXT, resolved_at TEXT);
CREATE UNIQUE INDEX IF NOT EXISTS alerts_open ON alerts (code, subject) WHERE resolved_at IS NULL;
INSERT OR IGNORE INTO kv (key, value) VALUES ('schema_version', '1');
)SQL");
}

Status StateStore::recordRun(const std::wstring& jobId, const std::string& type, const std::string& trigger,
                             const BackupResult& result, const QDateTime& startedAt, const QDateTime& finishedAt)
{
    if (auto st = m_db.exec("BEGIN IMMEDIATE"); !st)
        return st;
    Stmt run(m_db.handle(), "INSERT OR REPLACE INTO runs (run_id, job_id, type, trigger, started_at, finished_at, "
                            "status, error) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)");
    run.bind(1, result.runId).bind(2, jobId).bind(3, type).bind(4, trigger).bind(5, startedAt).bind(6, finishedAt)
        .bind(7, std::string(nameOf(result.status))).bind(8, result.error);
    if (auto st = stepDone(run, "runs"); !st) {
        (void)m_db.exec("ROLLBACK");
        return st;
    }
    for (const auto& v : result.vaults) {
        Stmt rv(m_db.handle(), "INSERT INTO run_vaults (run_id, job_id, vault_id, location, status, added, modified, "
                               "deleted, failed, bytes, error) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11)");
        rv.bind(1, result.runId).bind(2, jobId).bind(3, v.vaultId).bind(4, v.location)
            .bind(5, std::string(nameOf(v.status))).bind(6, static_cast<std::int64_t>(v.added))
            .bind(7, static_cast<std::int64_t>(v.modified)).bind(8, static_cast<std::int64_t>(v.deleted))
            .bind(9, static_cast<std::int64_t>(v.failed)).bind(10, static_cast<std::int64_t>(v.bytesWritten))
            .bind(11, v.error);
        if (auto st = stepDone(rv, "run_vaults"); !st) {
            (void)m_db.exec("ROLLBACK");
            return st;
        }
    }
    return m_db.exec("COMMIT");
}

Status StateStore::recordVaultSeen(const QString& vaultId, const std::wstring& location, std::uint64_t freeBytes,
                                   std::uint64_t totalBytes, bool backupSucceeded)
{
    const QDateTime now = QDateTime::currentDateTimeUtc();
    Stmt s(m_db.handle(),
           "INSERT INTO vaults_seen (vault_id, location, last_seen_at, last_success_at, free_bytes, total_bytes) "
           "VALUES (?1, ?2, ?3, ?4, ?5, ?6) "
           "ON CONFLICT(vault_id) DO UPDATE SET location = excluded.location, last_seen_at = excluded.last_seen_at, "
           "last_success_at = COALESCE(excluded.last_success_at, vaults_seen.last_success_at), "
           "free_bytes = excluded.free_bytes, total_bytes = excluded.total_bytes");
    s.bind(1, normalizedId(vaultId)).bind(2, location).bind(3, now).bind(4, backupSucceeded ? now : QDateTime())
        .bind(5, static_cast<std::int64_t>(freeBytes)).bind(6, static_cast<std::int64_t>(totalBytes));
    return stepDone(s, "vaults_seen");
}

std::optional<RunRow> StateStore::lastRun(const std::wstring& jobId)
{
    Stmt s(m_db.handle(), (std::string("SELECT ") + kRunColumns
                           + " FROM runs WHERE job_id = ?1 ORDER BY started_at DESC, run_id DESC LIMIT 1").c_str());
    s.bind(1, jobId);
    if (s.ok() && s.step() == SQLITE_ROW)
        return readRun(s);
    return std::nullopt;
}

std::optional<RunRow> StateStore::lastSuccess(const std::wstring& jobId)
{
    Stmt s(m_db.handle(), (std::string("SELECT ") + kRunColumns
                           + " FROM runs WHERE job_id = ?1 AND status = 'success' ORDER BY started_at DESC LIMIT 1").c_str());
    s.bind(1, jobId);
    if (s.ok() && s.step() == SQLITE_ROW)
        return readRun(s);
    return std::nullopt;
}

std::vector<RunRow> StateStore::recentRuns(int limit)
{
    std::vector<RunRow> out;
    Stmt s(m_db.handle(), (std::string("SELECT ") + kRunColumns + " FROM runs ORDER BY started_at DESC LIMIT ?1").c_str());
    s.bind(1, static_cast<std::int64_t>(limit));
    while (s.ok() && s.step() == SQLITE_ROW)
        out.push_back(readRun(s));
    return out;
}

std::optional<VaultSeenRow> StateStore::vaultSeen(const QString& vaultId)
{
    Stmt s(m_db.handle(), "SELECT vault_id, location, last_seen_at, last_success_at, free_bytes, total_bytes "
                          "FROM vaults_seen WHERE vault_id = ?1");
    s.bind(1, normalizedId(vaultId));
    if (!s.ok() || s.step() != SQLITE_ROW)
        return std::nullopt;
    VaultSeenRow r;
    r.vaultId = QString::fromStdString(s.text(0));
    r.location = win::fromUtf8(s.text(1));
    r.lastSeenAt = s.time(2);
    r.lastSuccessAt = s.time(3);
    r.freeBytes = static_cast<std::uint64_t>(s.integer(4));
    r.totalBytes = static_cast<std::uint64_t>(s.integer(5));
    return r;
}

Result<bool> StateStore::raiseAlert(const AlertRow& a)
{
    const QDateTime now = QDateTime::currentDateTimeUtc();
    Stmt upd(m_db.handle(), "UPDATE alerts SET last_seen_at = ?1, message = ?2, severity = ?3 "
                            "WHERE code = ?4 AND subject = ?5 AND resolved_at IS NULL");
    upd.bind(1, now).bind(2, a.message).bind(3, std::string(nameOf(a.severity))).bind(4, a.code).bind(5, a.subject);
    if (auto st = stepDone(upd, "alerts"); !st)
        return st.error();
    if (sqlite3_changes(m_db.handle()) > 0)
        return false;
    Stmt ins(m_db.handle(), "INSERT INTO alerts (code, subject, severity, message, opened_at, last_seen_at) "
                            "VALUES (?1, ?2, ?3, ?4, ?5, ?5)");
    ins.bind(1, a.code).bind(2, a.subject).bind(3, std::string(nameOf(a.severity))).bind(4, a.message).bind(5, now);
    if (auto st = stepDone(ins, "alerts"); !st)
        return st.error();
    return true;
}

Status StateStore::resolveAlert(const std::string& code, const std::string& subject)
{
    Stmt s(m_db.handle(), "UPDATE alerts SET resolved_at = ?1 WHERE code = ?2 AND subject = ?3 AND resolved_at IS NULL");
    s.bind(1, QDateTime::currentDateTimeUtc()).bind(2, code).bind(3, subject);
    return stepDone(s, "alerts");
}

std::vector<AlertRow> StateStore::openAlerts()
{
    std::vector<AlertRow> out;
    Stmt s(m_db.handle(), "SELECT code, subject, severity, message, opened_at, last_seen_at FROM alerts "
                          "WHERE resolved_at IS NULL ORDER BY severity DESC, opened_at");
    while (s.ok() && s.step() == SQLITE_ROW) {
        AlertRow a;
        a.code = s.text(0);
        a.subject = s.text(1);
        a.severity = s.text(2) == "critical" ? AlertSeverity::Critical : AlertSeverity::Warning;
        a.message = s.text(3);
        a.openedAt = s.time(4);
        a.lastSeenAt = s.time(5);
        out.push_back(std::move(a));
    }
    return out;
}

std::string StateStore::getValue(const std::string& key)
{
    Stmt s(m_db.handle(), "SELECT value FROM kv WHERE key = ?1");
    s.bind(1, key);
    if (s.ok() && s.step() == SQLITE_ROW)
        return s.text(0);
    return {};
}

Status StateStore::setValue(const std::string& key, const std::string& value)
{
    Stmt s(m_db.handle(), "INSERT INTO kv (key, value) VALUES (?1, ?2) ON CONFLICT(key) DO UPDATE SET value = excluded.value");
    s.bind(1, key).bind(2, value);
    return stepDone(s, "kv");
}

} // namespace synkro
