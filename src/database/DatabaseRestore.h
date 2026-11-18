#pragma once

#include "config/Config.h"
#include "database/DumpStore.h"

#include <string>

namespace synkro::db {

struct DatabaseRestoreRequest {
    std::wstring vaultLocation;
    std::wstring jobId;
    std::string database;        // nome lógico do dump (pasta em dumps/)
    std::wstring runId;          // vazio = dump válido mais recente
    std::string targetDatabase;  // PostgreSQL/MySQL: banco NOVO a criar
    std::wstring targetPath;     // SQLite/custom: arquivo ou pasta de destino (não pode existir)
    config::DatabaseJob job;     // conexão, ferramentas e comandos da tarefa
    std::wstring secretsFile;
    std::wstring stagingRoot;
};

struct DatabaseRestoreResult {
    DumpRecord dump;
    std::string target;
    std::string detail;
};

// Restaura um dump (docs/database-backup.md §9). Nunca sobrescreve: o banco
// ou arquivo de destino precisa ser novo. O SHA-256 do dump é conferido antes.
Result<DatabaseRestoreResult> restoreDatabase(const DatabaseRestoreRequest& request);

} // namespace synkro::db
