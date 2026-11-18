#pragma once

#include "config/Config.h"
#include "engine/BackupEngine.h"

#include <string>

namespace synkro::config {

struct ShutdownDecision {
    bool shutdown = false;
    std::string reason; // pt-BR: por que desliga ou por que não
};

// Regras de architecture.md §10.4: só desliga se a tarefa pedir
// (shutdown_after), o run terminou com Sucesso, o grupo da tarefa está em
// shutdown.allowed_after (quando a lista existe), nenhuma outra tarefa do
// Synkro está rodando e nenhuma outra tarefa depende desta (a última da
// cadeia é quem desliga).
ShutdownDecision decideShutdown(const ConfigSet& set, const JobConfig& job, RunStatus status, bool otherJobsRunning);

} // namespace synkro::config
