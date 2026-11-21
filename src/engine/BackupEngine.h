#pragma once

#include "engine/Guards.h"
#include "engine/Hooks.h"
#include "scan/PathFilter.h"
#include "transfer/SafeCopy.h"

#include <QString>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace synkro {

struct RetentionPolicy {
    int keepVersionsDays = 90;   // versions/<run> mais antigos que isso são expirados
    int keepMinRuns = 3;         // as N pastas de versões mais recentes nunca expiram
    int keepDeletedDays = 180;   // arquivos excluídos na origem ficam pelo menos isso
};

struct VaultTarget {
    std::wstring location;        // pasta que contém SynkroVault (ex.: "E:\"); vazio = não conectado
    QString expectedVaultId;      // se preenchido, o vault.json precisa ter este ID
    QString label;                // nome amigável, para mensagens
    std::string unavailableReason{}; // por que não foi localizado (ex.: servidor do NAS não responde)
};

enum class ProgressKind { ScanFinished, VaultStarted, FileDone, VaultFinished };

struct ProgressEvent {
    ProgressKind kind;
    std::wstring vault;
    std::wstring path;
    std::size_t done = 0;
    std::size_t total = 0;
};

// Pasta adicional de uma tarefa: no cofre, seus arquivos ficam em <alias>/.
struct SourceRoot {
    std::wstring path;
    std::wstring alias;
};

struct BackupRequest {
    std::wstring jobId;
    std::wstring sourceRoot;               // pasta principal: raiz do cofre
    std::vector<SourceRoot> moreSources;   // demais pastas (sources:), cada uma em <alias>/
    std::vector<VaultTarget> vaults;
    std::size_t minVaultsOk = 0;  // 0 = todos os cofres
    PathFilter filter;
    bool readFileIds = true;
    VerifyMode verify = VerifyMode::HashOnCopy;
    GuardPolicy guards;
    bool overrideGuards = false;  // confirmação explícita do usuário
    bool applyRetention = true;
    RetentionPolicy retention;
    HookSet hooks;

    // Snapshot VSS do volume de origem (exige administrador). Sem privilégio
    // ou se o snapshot falhar: copia direto se `continueWithoutSnapshot`,
    // senão o run falha.
    bool useSnapshot = false;
    bool continueWithoutSnapshot = true;

    // Tarefas que precisam terminar antes desta começar (depends_on). Se
    // estiverem rodando, esta execução espera até `dependencyWaitSeconds`.
    std::vector<std::wstring> dependsOn;
    std::int64_t dependencyWaitSeconds = 4 * 3600;

    // Onde está a chave dos cofres cifrados (vazio = <dados>\secrets.dat).
    std::wstring secretsFile;

    std::function<void(const ProgressEvent&)> progress;
};

enum class VaultRunStatus { Success, Partial, Failed, Blocked };
enum class RunStatus { Success, Degraded, Failed, Blocked, AlreadyRunning };

const char* nameOf(VaultRunStatus s) noexcept;
const char* nameOf(RunStatus s) noexcept;

struct VaultRunResult {
    std::wstring location;
    QString vaultId;
    VaultRunStatus status = VaultRunStatus::Failed;
    std::uint64_t added = 0, modified = 0, deleted = 0, failed = 0, unchanged = 0;
    std::uint64_t bytesWritten = 0;
    std::size_t recoveredRuns = 0;
    std::string error;            // falha do cofre inteiro
    GuardVerdict guard;
    std::vector<std::pair<std::wstring, std::string>> fileErrors; // arquivo -> erro (amostra)
};

struct BackupResult {
    std::wstring runId;
    RunStatus status = RunStatus::Failed;
    std::string error;            // falha antes de tocar nos cofres
    GuardVerdict sourceGuard;
    std::uint64_t filesScanned = 0;
    std::uint64_t bytesScanned = 0;
    std::vector<VaultRunResult> vaults;
    std::vector<HookOutcome> hooks;
    bool snapshotUsed = false;
    std::string snapshotNote;     // por que o snapshot não foi usado, quando pedido
    std::vector<std::string> warnings; // avisos que não mudam o status (ex.: tabelas MyISAM)
};

// Status do run a partir dos cofres, conforme min_vaults_ok (0 = todos).
RunStatus aggregateRunStatus(const std::vector<VaultRunResult>& vaults, std::size_t minVaultsOk);

// Executa um backup de arquivos (architecture.md §6): trava a tarefa, varre a
// origem uma única vez e aplica as mudanças em cada cofre, de forma
// independente, com versionamento, diário e verificação.
BackupResult runBackup(const BackupRequest& request);

} // namespace synkro
