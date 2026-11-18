#pragma once

#include "config/Config.h"
#include "core/Result.h"

#include <string>
#include <vector>

namespace synkro::win {

// Definição de uma tarefa do Agendador de Tarefas (architecture.md §10.1).
struct TaskDefinition {
    std::wstring name;                 // dentro da pasta \Synkro
    std::wstring description;
    std::wstring executable;
    std::wstring arguments;
    std::vector<config::ScheduleTrigger> triggers;
    bool runAsSystem = true;           // SYSTEM, privilégios mais altos, com ou sem usuário logado
    bool startWhenAvailable = true;    // executa ao ligar se perdeu o horário
    std::int64_t executionTimeLimitSeconds = 12 * 3600;
};

// XML do Task Scheduler 2.0 (puro, testável).
std::wstring buildTaskXml(const TaskDefinition& def);

// Duração ISO 8601 (PT1H30M, P1D...).
std::wstring isoDuration(std::int64_t seconds);

// Registra (cria ou atualiza) a tarefa em \Synkro\<nome>. Exige administrador
// para tarefas que rodam como SYSTEM.
Status registerTask(const TaskDefinition& def);
Status deleteTask(const std::wstring& name);
Result<std::vector<std::wstring>> listTasks();
Status runTaskNow(const std::wstring& name);

} // namespace synkro::win
