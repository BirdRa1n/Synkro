#pragma once

#include "config/Config.h"
#include "engine/BackupEngine.h"

#include <string>
#include <vector>

namespace synkro::db {

struct DatabaseBackupRequest {
    std::wstring jobId;
    config::DatabaseJob job;
    std::vector<VaultTarget> vaults;
    std::size_t minVaultsOk = 0;
    HookSet hooks;
    std::vector<std::wstring> dependsOn;
    std::int64_t timeoutSeconds = 7200;

    std::wstring secretsFile;     // vazio = padrão
    std::wstring stagingRoot;     // vazio = <dados>\staging
    int retryDelaySeconds = 30;   // entre novas tentativas (testes usam 0)
    bool applyRetention = true;

    std::function<void(const std::string& message)> progress;
};

// Executa uma tarefa `database` (docs/database-backup.md §3): gera cada
// dump numa pasta privada de preparo, valida, aplica as guardas, grava em
// cada cofre com verificação (releitura sem cache) junto com o .dump.json e
// aplica a retenção GFS. A pasta de preparo e as credenciais temporárias
// são apagadas ao final, também em falha.
//
// VaultRunResult::added = dumps gravados; failed = bancos que falharam.
BackupResult runDatabaseBackup(const DatabaseBackupRequest& request);

// Resolve uma referência de segredo ({ secret } ou { env }).
Result<std::string> resolveSecret(const config::SecretRef& ref, const std::wstring& secretsFile);

} // namespace synkro::db
