#include "config/JobRequest.h"

#include "platform/windows/Win32.h"
#include "config/VaultAccess.h"

namespace synkro::config {

namespace {

Result<std::vector<VaultTarget>> resolveVaults(const ConfigSet& set, const JobConfig& job)
{
    std::vector<VaultTarget> out;
    for (const auto& id : job.vaults) {
        const VaultConfig* vc = set.global.findVault(id);
        if (!vc)
            return Error::make(ErrorCode::InvalidArgument, "cofre \"" + id + "\" não declarado");
        VaultTarget target;
        target.expectedVaultId = QString::fromStdString(vc->vaultId);
        target.label = QString::fromStdString(vc->label);
        if (auto located = locateVault(*vc, &target.unavailableReason))
            target.location = located->location;
        out.push_back(std::move(target));
    }
    return out;
}

HookSet convertHooks(const Hooks& hooks)
{
    HookSet set;
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
    convert(hooks.before, set.before);
    convert(hooks.after, set.after);
    convert(hooks.onSuccess, set.onSuccess);
    convert(hooks.onFailure, set.onFailure);
    return set;
}

} // namespace

Result<BackupRequest> makeBackupRequest(const ConfigSet& set, const JobConfig& job)
{
    if (job.type != "files")
        return Error::make(ErrorCode::InvalidArgument, "a tarefa não é do tipo files");
    if (!job.enabled)
        return Error::make(ErrorCode::InvalidArgument, "a tarefa está desativada (enabled: false)");

    BackupRequest req;
    req.jobId = win::fromUtf8(job.id);
    req.sourceRoot = job.files.source;
    for (const auto& s : job.files.moreSources)
        req.moreSources.push_back({s.path, s.alias});
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
    req.hooks = convertHooks(job.hooks);

    auto vaults = resolveVaults(set, job);
    if (!vaults)
        return vaults.error();
    req.vaults = std::move(vaults).value();
    return req;
}

Result<db::DatabaseBackupRequest> makeDatabaseBackupRequest(const ConfigSet& set, const JobConfig& job)
{
    if (job.type != "database")
        return Error::make(ErrorCode::InvalidArgument, "a tarefa não é do tipo database");
    if (!job.enabled)
        return Error::make(ErrorCode::InvalidArgument, "a tarefa está desativada (enabled: false)");

    db::DatabaseBackupRequest req;
    req.jobId = win::fromUtf8(job.id);
    req.job = job.database;
    req.minVaultsOk = static_cast<std::size_t>(job.minVaultsOk);
    req.hooks = convertHooks(job.hooks);
    for (const auto& d : job.dependsOn)
        req.dependsOn.push_back(win::fromUtf8(d));
    if (job.timeoutSeconds > 0)
        req.timeoutSeconds = job.timeoutSeconds;

    auto vaults = resolveVaults(set, job);
    if (!vaults)
        return vaults.error();
    req.vaults = std::move(vaults).value();
    return req;
}

} // namespace synkro::config
