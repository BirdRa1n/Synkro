#pragma once

#include "config/Config.h"
#include "core/Result.h"

#include <QString>

#include <functional>
#include <string>
#include <vector>

namespace synkro::db {

struct RestoreTestRequest {
    std::wstring vaultLocation;
    std::wstring jobId;
    config::DatabaseJob job;      // conexão, ferramentas e restore_test
    std::string database;         // vazio = todos os bancos com dump no cofre
    std::wstring secretsFile;     // vazio = padrão
    std::wstring stagingRoot;     // vazio = <dados>\staging
    bool recordInVault = true;    // grava o resultado no .dump.json
    std::function<void(const std::string&)> progress;
};

struct RestoreCheckResult {
    std::string name;
    bool ok = false;
    std::string detail;
};

struct RestoreTestItem {
    std::string database;
    std::wstring runId;           // execução que gerou o dump testado
    bool ok = false;
    std::string detail;           // resumo legível (pt-BR)
    std::size_t tablesExpected = 0, tablesFound = 0;
    std::vector<std::string> missingTables;
    std::vector<RestoreCheckResult> checks;
    std::string cleanupWarning;   // banco temporário que não pôde ser apagado
};

struct RestoreTestResult {
    QString vaultId;
    std::vector<RestoreTestItem> items;
    bool ok() const;
};

// Teste de restauração automático (database-backup.md §9.1): para o dump
// válido mais recente de cada banco, restaura num banco temporário
// synkro_verify_<id> (ou num arquivo temporário, no SQLite), confere as
// tabelas registradas no dump, roda os `checks` do YAML e apaga o
// temporário. Nunca toca no banco de produção: o destino é sempre novo.
Result<RestoreTestResult> runRestoreTest(const RestoreTestRequest& request);

// Nome do banco temporário: synkro_verify_<AAAAMMDD>_<HHMMSS>_<xxxx>.
std::string verifyDatabaseName(const std::wstring& runId);

// Valor "verdadeiro" na primeira coluna de uma verificação (t, true, 1, yes, sim).
bool isTruthy(const std::string& value);

} // namespace synkro::db
