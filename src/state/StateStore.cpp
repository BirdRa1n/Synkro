#include "state/StateStore.h"

#include "platform/windows/Win32.h"

#include <QTimeZone>
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
CREATE TABLE IF NOT EXISTS outbox (
    id INTEGER PRIMARY KEY, channel TEXT NOT NULL, kind TEXT NOT NULL, code TEXT, subject TEXT, severity TEXT,
    message TEXT, created_at TEXT, opened_at TEXT, attempts INTEGER NOT NULL DEFAULT 0, next_attempt_at TEXT,
    status TEXT NOT NULL DEFAULT 'pending', sent_at TEXT, last_error TEXT);
CREATE INDEX IF NOT EXISTS outbox_pending ON outbox (status, next_attempt_at);
CREATE INDEX IF NOT EXISTS outbox_alert ON outbox (channel, code, subject, created_at);
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

namespace {

constexpr char kOutboxColumns[] = "id, channel, kind, code, subject, severity, message, created_at, opened_at, attempts, "
                                  "next_attempt_at, status, last_error";

OutboxRow readOutbox(const Stmt& s)
{
    OutboxRow r;
    r.id = s.integer(0);
    r.channel = s.text(1);
    r.kind = s.text(2);
    r.code = s.text(3);
    r.subject = s.text(4);
    r.severity = s.text(5);
    r.message = s.text(6);
    r.createdAt = s.time(7);
    r.openedAt = s.time(8);
    r.attempts = static_cast<int>(s.integer(9));
    r.nextAttemptAt = s.time(10);
    r.status = s.text(11);
    r.lastError = s.text(12);
    return r;
}

std::string idList(const std::vector<std::int64_t>& ids)
{
    std::string o;
    for (auto id : ids)
        o += (o.empty() ? "" : ",") + std::to_string(id);
    return o;
}

} // namespace

Status StateStore::enqueueNotification(const OutboxRow& r)
{
    const QDateTime created = r.createdAt.isValid() ? r.createdAt : QDateTime::currentDateTimeUtc();
    Stmt s(m_db.handle(), "INSERT INTO outbox (channel, kind, code, subject, severity, message, created_at, opened_at, "
                          "next_attempt_at) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?7)");
    s.bind(1, r.channel).bind(2, r.kind).bind(3, r.code).bind(4, r.subject).bind(5, r.severity).bind(6, r.message)
        .bind(7, created).bind(8, r.openedAt);
    return stepDone(s, "outbox");
}

std::vector<OutboxRow> StateStore::pendingNotifications(const QDateTime& nowUtc, int limit)
{
    std::vector<OutboxRow> out;
    Stmt s(m_db.handle(), (std::string("SELECT ") + kOutboxColumns
                           + " FROM outbox WHERE status = 'pending' AND next_attempt_at <= ?1 ORDER BY id LIMIT ?2").c_str());
    s.bind(1, nowUtc).bind(2, static_cast<std::int64_t>(limit));
    while (s.ok() && s.step() == SQLITE_ROW)
        out.push_back(readOutbox(s));
    return out;
}

std::optional<OutboxRow> StateStore::lastAlertNotification(const std::string& channel, const std::string& code,
                                                           const std::string& subject, const QDateTime& openedSince)
{
    Stmt s(m_db.handle(), (std::string("SELECT ") + kOutboxColumns
                           + " FROM outbox WHERE channel = ?1 AND code = ?2 AND subject = ?3 AND kind IN ('alert', 'reminder') "
                             "AND created_at >= ?4 AND status IN ('pending', 'sent') ORDER BY id DESC LIMIT 1").c_str());
    s.bind(1, channel).bind(2, code).bind(3, subject).bind(4, openedSince.isValid() ? openedSince : QDateTime::fromSecsSinceEpoch(0, QTimeZone::UTC));
    if (s.ok() && s.step() == SQLITE_ROW)
        return readOutbox(s);
    return std::nullopt;
}

Status StateStore::markNotifications(const std::vector<std::int64_t>& ids, const std::string& status)
{
    if (ids.empty())
        return Status::success();
    Stmt s(m_db.handle(), ("UPDATE outbox SET status = ?1, sent_at = CASE WHEN ?1 = 'sent' THEN ?2 ELSE sent_at END "
                           "WHERE id IN (" + idList(ids) + ")").c_str());
    s.bind(1, status).bind(2, QDateTime::currentDateTimeUtc());
    return stepDone(s, "outbox");
}

Status StateStore::deferNotifications(const std::vector<std::int64_t>& ids, const std::string& error, const QDateTime& next)
{
    if (ids.empty())
        return Status::success();
    Stmt s(m_db.handle(), ("UPDATE outbox SET attempts = attempts + 1, last_error = ?1, next_attempt_at = ?2 "
                           "WHERE id IN (" + idList(ids) + ")").c_str());
    s.bind(1, error).bind(2, next);
    return stepDone(s, "outbox");
}

Result<int> StateStore::expireNotifications(const QDateTime& olderThanUtc)
{
    Stmt s(m_db.handle(), "UPDATE outbox SET status = 'expired' WHERE status = 'pending' AND created_at < ?1");
    s.bind(1, olderThanUtc);
    if (auto st = stepDone(s, "outbox"); !st)
        return st.error();
    const int n = sqlite3_changes(m_db.handle());
    // Histórico enviado fica 90 dias.
    Stmt del(m_db.handle(), "DELETE FROM outbox WHERE status <> 'pending' AND created_at < ?1");
    del.bind(1, olderThanUtc.addDays(-90));
    (void)stepDone(del, "outbox");
    return n;
}

std::vector<std::pair<std::string, std::string>> StateStore::failingChannels(int minAttempts)
{
    std::vector<std::pair<std::string, std::string>> out;
    // Coluna "solta" com MAX(): o SQLite devolve last_error da linha mais recente.
    Stmt s(m_db.handle(), "SELECT channel, last_error, MAX(id) FROM outbox WHERE status = 'pending' AND attempts >= ?1 "
                          "GROUP BY channel ORDER BY channel");
    s.bind(1, static_cast<std::int64_t>(minAttempts));
    while (s.ok() && s.step() == SQLITE_ROW)
        out.emplace_back(s.text(0), s.text(1));
    return out;
}

std::vector<OutboxRow> StateStore::recentNotifications(int limit)
{
    std::vector<OutboxRow> out;
    Stmt s(m_db.handle(), (std::string("SELECT ") + kOutboxColumns + " FROM outbox ORDER BY id DESC LIMIT ?1").c_str());
    s.bind(1, static_cast<std::int64_t>(limit));
    while (s.ok() && s.step() == SQLITE_ROW)
        out.push_back(readOutbox(s));
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

Result<bool> StateStore::claimValue(const std::string& key, const std::string& value)
{
    Stmt s(m_db.handle(), "INSERT INTO kv (key, value) VALUES (?1, ?2) ON CONFLICT(key) DO UPDATE SET value = excluded.value "
                          "WHERE kv.value IS NOT excluded.value");
    s.bind(1, key).bind(2, value);
    if (auto st = stepDone(s, "kv"); !st)
        return st.error();
    return sqlite3_changes(m_db.handle()) > 0;
}

std::vector<AlertRow> StateStore::resolvedUnannounced(const std::string& channel, const QDateTime& sinceUtc)
{
    std::vector<AlertRow> out;
    Stmt s(m_db.handle(),
           "SELECT a.code, a.subject, a.severity, a.message, a.opened_at, a.resolved_at FROM alerts a "
           "WHERE a.resolved_at IS NOT NULL AND a.resolved_at >= ?2 "
           "AND EXISTS (SELECT 1 FROM outbox o WHERE o.channel = ?1 AND o.code = a.code AND o.subject = a.subject "
           "  AND o.kind IN ('alert', 'reminder') AND o.status IN ('pending', 'sent') "
           "  AND o.created_at >= a.opened_at AND o.created_at <= a.resolved_at) "
           "AND NOT EXISTS (SELECT 1 FROM outbox r WHERE r.channel = ?1 AND r.code = a.code AND r.subject = a.subject "
           "  AND r.kind = 'resolved' AND r.opened_at = a.opened_at) "
           "ORDER BY a.resolved_at");
    s.bind(1, channel).bind(2, sinceUtc);
    while (s.ok() && s.step() == SQLITE_ROW) {
        AlertRow a;
        a.code = s.text(0);
        a.subject = s.text(1);
        a.severity = s.text(2) == "critical" ? AlertSeverity::Critical : AlertSeverity::Warning;
        a.message = s.text(3);
        a.openedAt = s.time(4);
        a.lastSeenAt = s.time(5); // aqui: momento da resolução
        out.push_back(std::move(a));
    }
    return out;
}

Status StateStore::setValue(const std::string& key, const std::string& value)
{
    Stmt s(m_db.handle(), "INSERT INTO kv (key, value) VALUES (?1, ?2) ON CONFLICT(key) DO UPDATE SET value = excluded.value");
    s.bind(1, key).bind(2, value);
    return stepDone(s, "kv");
}

} // namespace synkro
