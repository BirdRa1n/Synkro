#include "scan/ChangeDetector.h"

#include "platform/windows/Win32.h"

#include <unordered_map>

namespace synkro {

double ChangeSet::deletedRatio() const noexcept
{
    return previousCount == 0 ? 0.0 : static_cast<double>(deleted.size()) / static_cast<double>(previousCount);
}

double ChangeSet::modifiedRatio() const noexcept
{
    return previousCount == 0 ? 0.0 : static_cast<double>(modified.size()) / static_cast<double>(previousCount);
}

ChangeSet detectChanges(const std::vector<KnownFile>& previous,
                        const std::vector<FileEntry>& current,
                        const DetectOptions& options)
{
    ChangeSet set;
    set.previousCount = previous.size();

    std::unordered_map<std::wstring, std::size_t> previousByKey;
    previousByKey.reserve(previous.size());
    for (std::size_t i = 0; i < previous.size(); ++i)
        previousByKey.emplace(win::foldCase(previous[i].meta.relativePath), i);

    std::vector<bool> seen(previous.size(), false);

    for (std::size_t ci = 0; ci < current.size(); ++ci) {
        const FileEntry& cur = current[ci];
        const auto it = previousByKey.find(win::foldCase(cur.relativePath));

        if (it == previousByKey.end()) {
            set.added.push_back({ChangeKind::Added, cur.relativePath, ReasonNone, ci, Change::npos});
            continue;
        }

        const std::size_t pi = it->second;
        seen[pi] = true;
        const FileEntry& old = previous[pi].meta;

        std::uint32_t reasons = ReasonNone;
        if (cur.size != old.size)
            reasons |= ReasonSize;
        if (cur.lastWriteTime != old.lastWriteTime)
            reasons |= ReasonWriteTime;
        if (cur.hasExtendedInfo && old.hasExtendedInfo) {
            if (options.compareChangeTime && cur.changeTime != old.changeTime)
                reasons |= ReasonChangeTime;
            // O número de série do volume não entra: um snapshot VSS tem outro
            // número, mas o FileId do arquivo é o mesmo.
            if (options.compareFileId && cur.fileId != old.fileId)
                reasons |= ReasonFileId;
        }
        if (cur.relativePath != old.relativePath)
            reasons |= ReasonCaseRename;

        if (reasons == ReasonNone)
            ++set.unchanged;
        else
            set.modified.push_back({ChangeKind::Modified, cur.relativePath, reasons, ci, pi});
    }

    for (std::size_t pi = 0; pi < previous.size(); ++pi) {
        if (!seen[pi])
            set.deleted.push_back({ChangeKind::Deleted, previous[pi].meta.relativePath, ReasonNone, Change::npos, pi});
    }

    return set;
}

} // namespace synkro
