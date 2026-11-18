#pragma once

#include "config/Config.h"
#include "platform/windows/TaskScheduler.h"

#include <string>
#include <vector>

namespace synkro::config {

// Hash do conteúdo de synkro.yml e jobs/*.yml (detecta mudanças feitas à mão).
std::string configHash(const std::wstring& directory);

// Tarefas que o Agendador do Windows deve ter (puro, testável):
//   \Synkro\job-<id>      uma por tarefa ativa com agendamento
//   \Synkro\Maintenance   a cada 15 minutos e na inicialização
//   \Synkro\Scrub         verificação periódica dos cofres (maintenance.scrub)
std::vector<win::TaskDefinition> desiredTasks(const ConfigSet& set, const std::wstring& workerExe);

struct ApplyOptions {
    std::wstring workerExe;
    bool registerTasks = true;
    bool copyToVaults = true;
};

struct ApplyReport {
    std::vector<std::string> done;     // pt-BR
    std::vector<std::string> warnings;
};

// `config apply` (docs/configuration.md §3): recusa configuração com erros;
// grava .lastgood\, envia job.yml (sem segredos) aos cofres conectados e
// sincroniza as tarefas do Agendador (cria, atualiza e remove as antigas).
Result<ApplyReport> applyConfig(const ConfigSet& set, const ApplyOptions& options);

} // namespace synkro::config
