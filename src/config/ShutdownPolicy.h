#pragma once

#include "config/Config.h"
#include "engine/BackupEngine.h"

#include <string>

namespace synkro::config {

struct ShutdownDecision {
    bool shutdown = false;  // desligar agora
    bool deferred = false;  // desligar quando as outras tarefas terminarem (pedido pendente)
    std::string reason;     // pt-BR: por que desliga, espera ou não desliga
};

// Regras de architecture.md §10.4. Desliga se a tarefa pede (shutdown_after),
// terminou com Sucesso — ou Degradado só porque um disco de rodízio está fora,
// o que é o esperado num rodízio — e o grupo está em shutdown.allowed_after
// (quando a lista existe).
//
// Outra tarefa do Synkro em execução, ou uma tarefa que depende desta, não
// cancela o desligamento: ele fica pendente e quem terminar por último desliga
// (antes, o pedido era descartado e o computador ficava ligado quando duas
// tarefas rodavam no mesmo horário).
ShutdownDecision decideShutdown(const ConfigSet& set, const JobConfig& job, RunStatus status, bool otherJobsRunning,
                                bool onlyRotationVaultsMissing = false);

// true se os únicos cofres que não receberam a cópia são de rodízio e estavam
// desconectados (ausência esperada), e pelo menos um cofre recebeu.
bool onlyRotationVaultsMissing(const ConfigSet& set, const BackupResult& result);

} // namespace synkro::config
