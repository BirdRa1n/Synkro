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

// Item da fila de notificações externas (um por canal).
struct OutboxRow {
    std::int64_t id = 0;
    std::string channel;
    std::string kind;       // alert | reminder | resolved | report | test
    std::string code, subject;
    std::string severity;   // warning | critical | info
    std::string message;    // pt-BR
    QDateTime createdAt;    // UTC
    QDateTime openedAt;     // abertura do alerta (lembretes e resoluções)
    int attempts = 0;
    QDateTime nextAttemptAt;
    std::string status;     // pending | sent | expired | dropped
    std::string lastError;
};

// Banco local de estado (<dados>\state\synkro.db, architecture.md §5): histórico
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

    // Fila de notificações (notify/Notifier.h).
    Status enqueueNotification(const OutboxRow& row);
    std::vector<OutboxRow> pendingNotifications(const QDateTime& nowUtc, int limit = 500);
    // Última notificação de abertura/lembrete deste alerta para o canal, desde a abertura.
    std::optional<OutboxRow> lastAlertNotification(const std::string& channel, const std::string& code,
                                                   const std::string& subject, const QDateTime& openedSince);
    Status markNotifications(const std::vector<std::int64_t>& ids, const std::string& status);
    Status deferNotifications(const std::vector<std::int64_t>& ids, const std::string& error, const QDateTime& next);
    Result<int> expireNotifications(const QDateTime& olderThanUtc);
    // Canais com envio falhando repetidamente: canal -> último erro.
    std::vector<std::pair<std::string, std::string>> failingChannels(int minAttempts);
    std::vector<OutboxRow> recentNotifications(int limit);
    // Alertas resolvidos desde `sinceUtc` que foram notificados ao canal e
    // ainda não tiveram a resolução enfileirada.
    std::vector<AlertRow> resolvedUnannounced(const std::string& channel, const QDateTime& sinceUtc);

    std::string getValue(const std::string& key);
    Status setValue(const std::string& key, const std::string& value);
    // Grava o valor só se for diferente do atual; true se gravou (atômico
    // entre processos: serve para "uma vez por dia").
    Result<bool> claimValue(const std::string& key, const std::string& value);

private:
    explicit StateStore(db::SqliteDb db) : m_db(std::move(db)) {}
    Status migrate();
    db::SqliteDb m_db;
};

const char* nameOf(AlertSeverity s) noexcept;

} // namespace synkro
