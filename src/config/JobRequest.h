#pragma once

#include "config/Config.h"
#include "database/DatabaseBackup.h"
#include "engine/BackupEngine.h"

#include <string>
#include <vector>

namespace synkro::config {

// Converte uma tarefa `files` validada em BackupRequest, localizando cada
// cofre pelo vault_id (a letra da unidade pode ter mudado). Cofres não
// conectados entram com localização vazia e falham na execução, o que torna
// o run Degradado em vez de silenciosamente ignorá-los.
Result<BackupRequest> makeBackupRequest(const ConfigSet& set, const JobConfig& job);

// Idem para tarefas `database`.
Result<db::DatabaseBackupRequest> makeDatabaseBackupRequest(const ConfigSet& set, const JobConfig& job);

} // namespace synkro::config
