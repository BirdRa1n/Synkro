#pragma once

// Checagem pré-piloto (docs/pilot.md): tudo o que impediria o Synkro de fazer
// backup sozinho nesta máquina — instalação, configuração, segredos, Agendador,
// cofres, ferramentas de banco e notificações. Só lê; não grava nada.

#include "config/Config.h"

#include <string>
#include <vector>

namespace synkro::support {

enum class CheckLevel { Ok, Info, Warn, Fail };

struct DoctorCheck {
    std::string area;    // Instalação, Configuração, Agendador, Cofres, Tarefas, Notificações
    CheckLevel level = CheckLevel::Ok;
    std::string title;   // pt-BR, curto
    std::string detail;  // o que fazer, quando não está ok
};

struct DoctorOptions {
    std::wstring workerExe;     // para comparar as tarefas do Agendador
    std::wstring secretsFile;   // vazio = padrão
    bool checkScheduler = true;
};

std::vector<DoctorCheck> runDoctor(const config::ConfigSet& set, const DoctorOptions& options);

const char* nameOf(CheckLevel level) noexcept;

// Pior nível encontrado (Ok se a lista estiver vazia).
CheckLevel worst(const std::vector<DoctorCheck>& checks);

} // namespace synkro::support
