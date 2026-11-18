#pragma once

#include "core/Result.h"
#include "database/Sqlite.h"
#include "engine/BackupEngine.h"

#include <QDateTime>

#include <optional>
#include <string>
#include <vector>

namespace synkro {

struct RunRow {
    std::wstring runId;
    std::wstring jobId;
    std::string type;       // files | database
    std::string trigger;    // manual | schedule | ...
    QDateTime startedAt, finishedAt;
    std::string status;
    std::string error;
};

struct VaultSeenRow {
    QString vaultId;
    std::wstring location;
    QDateTime lastSeenAt;
    QDateTime lastSuccessAt;
    std::uint64_t freeBytes = 0, totalBytes = 0;
};

enum class AlertSeverity { Warning, Critical };

struct AlertRow {
    std::string code;       // no_success, last_run_failed, vault_absent, low_space...
    std::string subject;    // tarefa ou cofre
    AlertSeverity severity = AlertSeverity::Warning;
    std::string message;    // pt-BR
    QDateTime openedAt;
    QDateTime lastSeenAt;
};

// Banco local de estado (<dados>\synkro.db, architecture.md §5): histórico
// de execuções, último contato com cada cofre e alertas. É cache: os cofres
// continuam sendo a fonte da verdade dos backups.
class StateStore {
public:
    static Result<StateStore> open(const std::wstring& path);

    Status recordRun(const std::wstring& jobId, const std::string& type, const std::string& trigger,
                     const BackupResult& result, const QDateTime& startedAt, const QDateTime& finishedAt);
    Status recordVaultSeen(const QString& vaultId, const std::wstring& location, std::uint64_t freeBytes,
                           std::uint64_t totalBytes, bool backupSucceeded);

    std::optional<RunRow> lastRun(const std::wstring& jobId);
    std::optional<RunRow> lastSuccess(const std::wstring& jobId);
    std::vector<RunRow> recentRuns(int limit);
    std::optional<VaultSeenRow> vaultSeen(const QString& vaultId);

    // Abre (ou atualiza) um alerta. Devolve true se ele é novo.
    Result<bool> raiseAlert(const AlertRow& alert);
    Status resolveAlert(const std::string& code, const std::string& subject);
    std::vector<AlertRow> openAlerts();

    std::string getValue(const std::string& key);
    Status setValue(const std::string& key, const std::string& value);

private:
    explicit StateStore(db::SqliteDb db) : m_db(std::move(db)) {}
    Status migrate();
    db::SqliteDb m_db;
};

const char* nameOf(AlertSeverity s) noexcept;

} // namespace synkro
