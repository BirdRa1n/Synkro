#include "engine/Retention.h"

#include "catalog/Manifest.h"
#include "core/RunId.h"
#include "engine/VaultOps.h"
#include "platform/windows/Win32.h"
#include "scan/FileScanner.h"

#include <unordered_set>

namespace synkro {

Result<RetentionResult> applyRetention(const vault::JobLayout& layout, const RetentionPolicy& policy,
                                       const QDateTime& nowUtc)
{
    RetentionResult result;
    std::vector<std::wstring> runs;
    for (const auto& name : ops::listSubdirectories(layout.versionsRoot())) {
        if (isValidRunId(name))
            runs.push_back(name);
    }

    const std::size_t keepMin = policy.keepMinRuns < 0 ? 0 : static_cast<std::size_t>(policy.keepMinRuns);
    if (runs.size() <= keepMin)
        return result;
    runs.resize(runs.size() - keepMin); // ordenados: os mais recentes ficam de fora

    const QDateTime versionsCutoff = nowUtc.addDays(-policy.keepVersionsDays);
    const QDateTime deletedCutoff = nowUtc.addDays(-policy.keepDeletedDays);

    for (const auto& runId : runs) {
        const QDateTime when = runIdTimestamp(runId);
        if (!when.isValid() || when >= versionsCutoff)
            continue;

        const std::wstring dir = layout.versions(runId);
        const bool deletedStillProtected = when >= deletedCutoff;

        if (!deletedStillProtected) {
            std::uint64_t freed = 0;
            auto files = scanDirectory(dir);
            if (auto st = ops::removeTree(dir, &freed); !st)
                return st.error();
            result.runsExpired += 1;
            result.filesRemoved += files ? files.value().files.size() : 0;
            result.bytesFreed += freed;
            continue;
        }

        // Mantém só o que foi excluído da origem nesta execução.
        std::unordered_set<std::wstring> deleted;
        if (auto m = Manifest::load(layout.manifestFile(runId)); m) {
            for (const auto& r : m.value().records) {
                if (r.op == ManifestOp::Deleted)
                    deleted.insert(win::foldCase(r.path));
            }
        } else {
            continue; // sem manifest não dá para distinguir: não apaga nada
        }

        auto files = scanDirectory(dir);
        if (!files)
            continue;
        for (const auto& f : files.value().files) {
            if (deleted.count(win::foldCase(f.relativePath))) {
                ++result.deletedFilesKept;
                continue;
            }
            const std::wstring full = win::joinPath(dir, win::fromCatalogPath(f.relativePath));
            const std::wstring ext = win::toExtendedPath(full);
            const DWORD attrs = ::GetFileAttributesW(ext.c_str());
            if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY))
                ::SetFileAttributesW(ext.c_str(), attrs & ~static_cast<DWORD>(FILE_ATTRIBUTE_READONLY));
            if (::DeleteFileW(ext.c_str())) {
                ++result.filesRemoved;
                result.bytesFreed += f.size;
            }
        }
        ops::removeEmptyDirectories(dir, true);
    }
    return result;
}

} // namespace synkro
