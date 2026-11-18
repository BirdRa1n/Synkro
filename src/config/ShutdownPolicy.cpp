#include "config/ShutdownPolicy.h"

#include <algorithm>

namespace synkro::config {

ShutdownDecision decideShutdown(const ConfigSet& set, const JobConfig& job, RunStatus status, bool otherJobsRunning)
{
    if (!job.shutdownAfter)
        return {false, "a tarefa não pede desligamento"};
    if (status != RunStatus::Success)
        return {false, std::string("o computador NÃO será desligado: a execução terminou como ") + nameOf(status)};
    const auto& groups = set.global.shutdownAllowedGroups;
    if (!groups.empty() && std::find(groups.begin(), groups.end(), job.group) == groups.end())
        return {false, "o grupo \"" + job.group + "\" não está em shutdown.allowed_after"};
    for (const auto& other : set.jobs) {
        if (std::find(other.dependsOn.begin(), other.dependsOn.end(), job.id) != other.dependsOn.end() && other.enabled)
            return {false, "a tarefa \"" + other.name + "\" depende desta e ainda vai rodar"};
    }
    if (otherJobsRunning)
        return {false, "outra tarefa do Synkro está em execução"};
    return {true, "todas as condições atendidas"};
}

} // namespace synkro::config
