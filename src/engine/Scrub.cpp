#include "engine/Scrub.h"

#include "catalog/Catalog.h"
#include "config/VaultAccess.h"
#include "database/DumpStore.h"
#include "engine/VaultOps.h"
#include "engine/Verify.h"
#include "hash/Sha256.h"
#include "platform/windows/Win32.h"
#include "transfer/SafeCopy.h"
#include "vault/VaultLocator.h"

#include <QRandomGenerator>

namespace synkro {

namespace {

struct Located {
    std::string label;
    vault::JobLayout layout;
};

// Copia `from` sobre `to` e confere que o resultado tem o hash esperado.
bool repairFile(const std::wstring& from, const std::wstring& to, const Sha256Digest& expected, const std::wstring& tmpDir)
{
    auto good = Sha256::ofFile(from, true);
    if (!good || good.value() != expected)
        return false;
    CopyOptions opt;
    opt.verify = VerifyMode::RereadUncached;
    opt.tempDirectory = tmpDir;
    auto copied = copyFileVerified(from, to, opt);
    return copied && copied.value().sha256 == expected;
}

} // namespace

ScrubReport runScrub(const config::ConfigSet& set, double sampleRatio, bool repair)
{
    ScrubReport report;
    for (const auto& job : set.jobs) {
        std::vector<Located> vaults;
        for (const auto& vid : job.vaults) {
            const config::VaultConfig* vc = set.global.findVault(vid);
            if (!vc)
                continue;
            std::string why;
            if (auto loc = config::locateVault(*vc, &why))
                vaults.push_back({vc->label, vault::JobLayout(loc->location, win::fromUtf8(job.id))});
            else
                report.notes.push_back("\"" + vc->label + "\" desconectado (" + why + "); não verificado para " + job.id);
        }

        for (const auto& v : vaults) {
            if (job.type == "files") {
                auto r = verifyVault(v.layout, sampleRatio * 100.0);
                if (!r) {
                    if (r.error().code != ErrorCode::NotFound)
                        report.problems.push_back(job.id + " em \"" + v.label + "\": " + r.error().message);
                    continue;
                }
                report.checked += r.value().checked;
                report.ok += r.value().ok;
                std::vector<std::wstring> bad = r.value().corrupted;
                bad.insert(bad.end(), r.value().missing.begin(), r.value().missing.end());
                if (bad.empty())
                    continue;
                auto catalog = Catalog::load(v.layout.catalogFile());
                for (const auto& path : bad) {
                    bool fixed = false;
                    const CatalogEntry* entry = catalog ? catalog.value().find(path) : nullptr;
                    if (repair && entry) {
                        for (const auto& other : vaults) {
                            if (&other == &v)
                                continue;
                            auto otherCat = Catalog::load(other.layout.catalogFile());
                            const CatalogEntry* oe = otherCat ? otherCat.value().find(path) : nullptr;
                            if (oe && oe->sha256 == entry->sha256
                                && repairFile(other.layout.currentFile(path), v.layout.currentFile(path), entry->sha256,
                                              v.layout.tmp())) {
                                fixed = true;
                                ++report.repaired;
                                report.notes.push_back(win::toUtf8(path) + " recuperado em \"" + v.label + "\" a partir de \""
                                                       + other.label + "\"");
                                break;
                            }
                        }
                    }
                    if (!fixed)
                        report.problems.push_back(job.id + " em \"" + v.label + "\": " + win::toUtf8(path)
                                                  + " corrompido ou ausente");
                }
            } else {
                for (const auto& d : db::listDumps(v.layout)) {
                    if (sampleRatio < 1.0 && QRandomGenerator::global()->bounded(1.0) >= sampleRatio)
                        continue;
                    ++report.checked;
                    const std::wstring file = win::joinPath(v.layout.root(), win::fromCatalogPath(d.relativePath));
                    auto h = Sha256::ofFile(file, true);
                    if (h && h.value() == d.sha256) {
                        ++report.ok;
                        continue;
                    }
                    bool fixed = false;
                    if (repair) {
                        for (const auto& other : vaults) {
                            if (&other == &v)
                                continue;
                            const std::wstring src = win::joinPath(other.layout.root(), win::fromCatalogPath(d.relativePath));
                            if (ops::pathExists(src) && repairFile(src, file, d.sha256, v.layout.tmp())) {
                                fixed = true;
                                ++report.repaired;
                                report.notes.push_back("dump " + win::toUtf8(d.relativePath) + " recuperado em \"" + v.label
                                                       + "\" a partir de \"" + other.label + "\"");
                                break;
                            }
                        }
                    }
                    if (!fixed)
                        report.problems.push_back(job.id + " em \"" + v.label + "\": dump " + win::toUtf8(d.relativePath)
                                                  + " corrompido ou ausente");
                }
            }
        }
    }
    return report;
}

} // namespace synkro
