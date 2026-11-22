#include "config/ShutdownPolicy.h"

#include <algorithm>

namespace synkro::config {

ShutdownDecision decideShutdown(const ConfigSet& set, const JobConfig& job, RunStatus status, bool otherJobsRunning,
                                bool rotationOnly)
{
    if (!job.shutdownAfter)
        return {false, false, "a tarefa não pede desligamento"};
    const bool degradedByRotation = status == RunStatus::Degraded && rotationOnly;
    if (status != RunStatus::Success && !degradedByRotation)
        return {false, false, std::string("o computador NÃO será desligado: a execução terminou como ") + nameOf(status)};
    const auto& groups = set.global.shutdownAllowedGroups;
    if (!groups.empty() && std::find(groups.begin(), groups.end(), job.group) == groups.end())
        return {false, false, "o grupo \"" + job.group + "\" não está em shutdown.allowed_after"};
    for (const auto& other : set.jobs) {
        if (std::find(other.dependsOn.begin(), other.dependsOn.end(), job.id) != other.dependsOn.end() && other.enabled)
            return {false, true,
                    "a tarefa \"" + other.name + "\" depende desta: o computador desliga quando ela terminar"};
    }
    if (otherJobsRunning)
        return {false, true, "outra tarefa do Synkro está em execução: o computador desliga quando ela terminar"};
    return {true, false,
            degradedByRotation ? "backup concluído (o disco de rodízio fora do local é esperado)" : "todas as condições atendidas"};
}

bool onlyRotationVaultsMissing(const ConfigSet& set, const BackupResult& result)
{
    bool anyOk = false, anyMissing = false;
    for (const auto& v : result.vaults) {
        if (v.status == VaultRunStatus::Success) {
            anyOk = true;
            continue;
        }
        const VaultConfig* vc = nullptr;
        for (const auto& c : set.global.vaults)
            if (QString::fromStdString(c.vaultId).compare(v.vaultId, Qt::CaseInsensitive) == 0)
                vc = &c;
        // Só conta como "ausência esperada" o disco de rodízio desconectado;
        // falha de gravação, guarda ou disco errado continuam impedindo.
        if (v.status != VaultRunStatus::Failed || !v.location.empty() || !vc || !vc->rotation)
            return false;
        anyMissing = true;
    }
    return anyOk && anyMissing;
}

} // namespace synkro::config
