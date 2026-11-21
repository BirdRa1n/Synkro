#include "config/VaultRemoval.h"

#include "config/ConfigWriter.h"
#include "platform/windows/Win32.h"

#include <algorithm>

namespace synkro::config {

Result<VaultRemovalPlan> planVaultRemoval(const ConfigSet& set, const std::string& vaultId,
                                          const std::function<std::string(const std::wstring&)>& readFile)
{
    const VaultConfig* vault = set.global.findVault(vaultId);
    if (!vault)
        return Error::make(ErrorCode::NotFound, "cofre \"" + vaultId + "\" não está na configuração");

    VaultRemovalPlan plan;
    for (const auto& job : set.jobs) {
        if (std::find(job.vaults.begin(), job.vaults.end(), vaultId) == job.vaults.end())
            continue;
        std::vector<std::string> remaining;
        for (const auto& v : job.vaults)
            if (v != vaultId)
                remaining.push_back(v);
        if (remaining.empty()) {
            plan.blockers.push_back("a tarefa \"" + job.name + "\" só grava neste cofre: acrescente outro cofre a ela ou "
                                    "remova a tarefa antes");
            continue;
        }
        const std::string text = readFile(job.file);
        if (text.empty() || !isPristineGenerated(text)) {
            plan.blockers.push_back(win::toUtf8(job.file) + " foi editado à mão: tire \"" + vaultId
                                    + "\" da lista vaults no editor YAML (Tarefas → Editar YAML)");
            continue;
        }
        JobConfig updated = job;
        updated.vaults = remaining;
        if (updated.minVaultsOk > static_cast<int>(remaining.size()))
            updated.minVaultsOk = 0; // "todos" os que sobraram
        plan.files.emplace_back(job.file, emitJob(updated));
        plan.affectedJobs.push_back(job.name);
    }

    const std::string global = readFile(L"synkro.yml");
    if (global.empty() || !isPristineGenerated(global)) {
        plan.blockers.push_back("synkro.yml foi editado à mão: tire o cofre \"" + vaultId
                                + "\" de vaults (e de defaults.vaults) no editor YAML (Configurações → Editar synkro.yml)");
    } else {
        GlobalConfig g = set.global;
        g.vaults.erase(std::remove_if(g.vaults.begin(), g.vaults.end(), [&](const VaultConfig& v) { return v.id == vaultId; }),
                       g.vaults.end());
        g.defaultVaults.erase(std::remove(g.defaultVaults.begin(), g.defaultVaults.end(), vaultId), g.defaultVaults.end());
        if (g.defaultMinVaultsOk > static_cast<int>(g.defaultVaults.size()))
            g.defaultMinVaultsOk = 0;
        plan.files.emplace_back(L"synkro.yml", emitGlobal(g));
    }
    if (!plan.possible())
        plan.files.clear(); // tudo ou nada
    return plan;
}

} // namespace synkro::config
