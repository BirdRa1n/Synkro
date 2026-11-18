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
