#include "config/JobRequest.h"

#include "platform/windows/Win32.h"
#include "vault/VaultLocator.h"

namespace synkro::config {

Result<BackupRequest> makeBackupRequest(const ConfigSet& set, const JobConfig& job)
{
    if (job.type != "files")
        return Error::make(ErrorCode::Unsupported, "backup de banco de dados entra na Fase 2 do Synkro");
    if (!job.enabled)
        return Error::make(ErrorCode::InvalidArgument, "a tarefa está desativada (enabled: false)");

    BackupRequest req;
    req.jobId = win::fromUtf8(job.id);
    req.sourceRoot = job.files.source;
    req.minVaultsOk = static_cast<std::size_t>(job.minVaultsOk);
    req.filter = PathFilter(job.files.include, job.files.exclude);
    req.verify = job.files.verifyUncached ? VerifyMode::RereadUncached : VerifyMode::HashOnCopy;
    req.guards.maxDeletedRatio = job.files.maxDeleted;
    req.guards.maxModifiedRatio = job.files.maxModified;
    req.guards.sentinelFile = job.files.sentinelFile;
    req.retention.keepVersionsDays = static_cast<int>(job.files.keepVersionsSeconds / 86400);
    req.retention.keepMinRuns = job.files.keepMinVersions;
    req.retention.keepDeletedDays = static_cast<int>(job.files.keepDeletedSeconds / 86400);
    req.useSnapshot = job.files.vss;
    req.continueWithoutSnapshot = job.files.continueWithoutSnapshot;
    for (const auto& d : job.dependsOn)
        req.dependsOn.push_back(win::fromUtf8(d));
    if (job.timeoutSeconds > 0)
        req.dependencyWaitSeconds = job.timeoutSeconds;

    auto convert = [](const std::vector<HookCommand>& in, std::vector<HookSpec>& out) {
        for (const auto& h : in) {
            HookSpec s;
            for (const auto& a : h.run)
                s.args.push_back(win::fromUtf8(a));
            s.timeoutSeconds = h.timeoutSeconds;
            s.failOnError = h.failOnError;
            out.push_back(std::move(s));
        }
    };
    convert(job.hooks.before, req.hooks.before);
    convert(job.hooks.after, req.hooks.after);
    convert(job.hooks.onSuccess, req.hooks.onSuccess);
    convert(job.hooks.onFailure, req.hooks.onFailure);

    for (const auto& id : job.vaults) {
        const VaultConfig* vc = set.global.findVault(id);
        if (!vc)
            return Error::make(ErrorCode::InvalidArgument, "cofre \"" + id + "\" não declarado");
        VaultTarget target;
        target.expectedVaultId = QString::fromStdString(vc->vaultId);
        target.label = QString::fromStdString(vc->label);
        if (auto located = vault::locateVault(target.expectedVaultId, vc->path))
            target.location = located->location;
        req.vaults.push_back(std::move(target));
    }
    return req;
}

} // namespace synkro::config
