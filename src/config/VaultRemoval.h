#pragma once

#include "config/Config.h"
#include "core/Result.h"

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace synkro::config {

// Plano para tirar um cofre da configuração: o synkro.yml (cofres e padrões)
// e cada tarefa que grava nele. Não toca no disco do cofre.
struct VaultRemovalPlan {
    // Arquivos a regravar, na ordem segura (tarefas antes do synkro.yml, para a
    // configuração nunca citar um cofre que já não existe).
    std::vector<std::pair<std::wstring, std::string>> files;
    std::vector<std::string> affectedJobs;   // nomes das tarefas que deixam de usar o cofre
    std::vector<std::string> blockers;       // por que não dá para remover (pt-BR)

    bool possible() const { return blockers.empty(); }
};

// `readFile` lê um arquivo da pasta de configuração (caminho relativo).
// Recusa (blockers) quando uma tarefa ficaria sem cofre ou quando um arquivo
// foi editado à mão (o desktop nunca regrava esses).
Result<VaultRemovalPlan> planVaultRemoval(const ConfigSet& set, const std::string& vaultId,
                                          const std::function<std::string(const std::wstring&)>& readFile);

} // namespace synkro::config
