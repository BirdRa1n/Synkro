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
    // Em vez de SYSTEM: roda na sessão de quem fizer logon e pertencer a este
    // grupo (SID), com os privilégios mais altos dele (ex.: o desktop na bandeja).
    std::wstring groupSid;
    bool startWhenAvailable = true;    // executa ao ligar se perdeu o horário
    std::int64_t executionTimeLimitSeconds = 12 * 3600; // 0 = sem limite
    int priority = 7;                  // 4 = normal (interface); 7 = abaixo do normal (backup)
};

// Tarefa que abre o Synkro Desktop na bandeja ao fazer logon, já como
// administrador (o desktop exige administrador, e o Windows não inicia
// programas assim pela chave Run). Vale para administradores; um usuário
// comum não tem privilégio para elevar.
inline constexpr wchar_t kDesktopTaskName[] = L"Desktop";
TaskDefinition desktopAutostartTask(const std::wstring& desktopExe);

// XML do Task Scheduler 2.0 (puro, testável).
std::wstring buildTaskXml(const TaskDefinition& def);

// Duração ISO 8601 (PT1H30M, P1D...).
std::wstring isoDuration(std::int64_t seconds);

// Registra (cria ou atualiza) a tarefa em \Synkro\<nome>. Exige administrador
// para tarefas que rodam como SYSTEM.
Status registerTask(const TaskDefinition& def);
Status deleteTask(const std::wstring& name);
Result<std::vector<std::wstring>> listTasks();
bool taskExists(const std::wstring& name);
Status runTaskNow(const std::wstring& name);

} // namespace synkro::win
