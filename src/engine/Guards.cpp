#include "engine/Guards.h"

#include "platform/windows/Win32.h"

#include <cmath>
#include <cstdio>

namespace synkro {

const char* toString(GuardCode code) noexcept
{
    switch (code) {
    case GuardCode::None:             return "none";
    case GuardCode::EmptySource:      return "empty_source";
    case GuardCode::SentinelMissing:  return "sentinel_missing";
    case GuardCode::MassDeletion:     return "mass_deletion";
    case GuardCode::MassModification: return "mass_modification";
    }
    return "unknown";
}

static std::string percent(double ratio)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%.0f%%", ratio * 100.0);
    return buf;
}

GuardVerdict evaluateSourceGuards(const GuardPolicy& policy, const ScanResult& scan)
{
    if (!policy.sentinelFile.empty()) {
        const std::wstring key = win::foldCase(win::toCatalogPath(policy.sentinelFile));
        bool found = false;
        for (const auto& f : scan.files) {
            if (win::foldCase(f.relativePath) == key) {
                found = true;
                break;
            }
        }
        if (!found)
            return {GuardCode::SentinelMissing,
                    "o arquivo-sentinela \"" + win::toUtf8(policy.sentinelFile)
                        + "\" não existe na origem; a origem foi considerada inválida"};
    }
    return {};
}

GuardVerdict evaluateChangeGuards(const GuardPolicy& policy, const ChangeSet& changes, std::size_t currentCount)
{
    if (changes.previousCount > 0 && currentCount == 0)
        return {GuardCode::EmptySource, "a origem está vazia, mas o último backup tinha "
                                            + std::to_string(changes.previousCount) + " arquivos"};

    if (changes.previousCount < policy.minPreviousFiles)
        return {};

    const double deleted = changes.deletedRatio();
    if (deleted > policy.maxDeletedRatio)
        return {GuardCode::MassDeletion, percent(deleted) + " dos arquivos sumiram da origem ("
                                             + std::to_string(changes.deleted.size()) + " de "
                                             + std::to_string(changes.previousCount) + "); limite "
                                             + percent(policy.maxDeletedRatio)};

    const double modified = changes.modifiedRatio();
    if (modified > policy.maxModifiedRatio)
        return {GuardCode::MassModification, percent(modified) + " dos arquivos foram modificados ("
                                                 + std::to_string(changes.modified.size()) + " de "
                                                 + std::to_string(changes.previousCount)
                                                 + "), possível ransomware; limite "
                                                 + percent(policy.maxModifiedRatio)};
    return {};
}

} // namespace synkro
