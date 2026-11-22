#include "engine/Restore.h"

#include "catalog/Manifest.h"
#include "core/RunId.h"
#include "crypto/VaultKey.h"
#include "engine/VaultOps.h"
#include "platform/windows/Win32.h"
#include "transfer/SafeCopy.h"
#include "vault/Vault.h"

#include <algorithm>
#include <unordered_map>

namespace synkro {

std::vector<std::wstring> listRuns(const vault::JobLayout& layout)
{
    std::vector<std::wstring> runs;
    const std::wstring suffix = L".manifest.jsonl";
    for (const auto& name : ops::listFiles(layout.runs(), suffix)) {
        const std::wstring id = name.substr(0, name.size() - suffix.size());
        if (isValidRunId(id))
            runs.push_back(id);
    }
    return runs;
}

Result<std::vector<RestorableFile>> stateAtRun(const vault::JobLayout& layout, const std::wstring& runId,
                                               std::wstring* resolvedRunId)
{
    auto catalog = Catalog::load(layout.catalogFile());
    if (!catalog)
        return catalog.error();

    const std::wstring latest = catalog.value().runId;
    const std::wstring target = runId.empty() ? latest : runId;
    if (resolvedRunId)
        *resolvedRunId = target;

    std::unordered_map<std::wstring, RestorableFile> state;
    state.reserve(catalog.value().entries.size());
    for (const auto& e : catalog.value().entries)
        state[win::foldCase(e.meta.relativePath)] = {e, {}};

    if (target != latest) {
        const auto runs = listRuns(layout);
        if (std::find(runs.begin(), runs.end(), target) == runs.end())
            return Error::make(ErrorCode::NotFound, "execução " + win::toUtf8(target) + " não está disponível neste cofre");
        if (target > latest)
            return Error::make(ErrorCode::InvalidArgument, "execução posterior ao catálogo atual");

        // Desfaz da mais nova para a mais antiga as execuções após `target`.
        for (auto it = runs.rbegin(); it != runs.rend(); ++it) {
            if (*it <= target)
                break;
            if (*it > latest)
                continue; // execução bloqueada/não aplicada ao catálogo
            auto manifest = Manifest::load(layout.manifestFile(*it));
            if (!manifest)
                return manifest.error();
            // Desfaz na ordem inversa: numa reorganização, os arquivos são
            // renomeados antes de a execução mexer neles.
            const auto& records = manifest.value().records;
            for (auto rit = records.rbegin(); rit != records.rend(); ++rit) {
                const ManifestRecord& r = *rit;
                const std::wstring key = win::foldCase(r.path);
                switch (r.op) {
                case ManifestOp::Renamed:
                    if (r.previous) {
                        auto found = state.find(key);
                        if (found == state.end())
                            break;
                        RestorableFile moved = found->second;
                        if (moved.storedPath.empty())
                            moved.storedPath = moved.entry.meta.relativePath; // continua guardado no lugar novo
                        moved.entry.meta.relativePath = r.previous->meta.relativePath;
                        state.erase(found);
                        state[win::foldCase(r.previous->meta.relativePath)] = std::move(moved);
                    }
                    break;
                case ManifestOp::Added:
                    state.erase(key);
                    break;
                case ManifestOp::Modified:
                case ManifestOp::Deleted:
                    if (r.previous) {
                        if (r.entry && win::foldCase(r.entry->meta.relativePath) != win::foldCase(r.previous->meta.relativePath))
                            state.erase(win::foldCase(r.entry->meta.relativePath));
                        state[win::foldCase(r.previous->meta.relativePath)] = {*r.previous, *it};
                    }
                    break;
                case ManifestOp::Failed:
                    break;
                }
            }
        }
    }

    std::vector<RestorableFile> out;
    out.reserve(state.size());
    for (auto& [key, value] : state)
        out.push_back(std::move(value));
    std::sort(out.begin(), out.end(), [](const RestorableFile& a, const RestorableFile& b) {
        return a.entry.meta.relativePath < b.entry.meta.relativePath;
    });
    return out;
}

static bool directoryHasContent(const std::wstring& dir)
{
    return !ops::listSubdirectories(dir).empty() || !ops::listFiles(dir, L"").empty();
}

static bool underPrefix(const std::wstring& path, const std::wstring& prefix)
{
    if (prefix.empty())
        return true;
    const std::wstring p = win::foldCase(path);
    std::wstring pre = win::foldCase(win::toCatalogPath(prefix));
    while (!pre.empty() && pre.back() == L'/')
        pre.pop_back();
    if (p == pre)
        return true;
    return p.size() > pre.size() && p.compare(0, pre.size(), pre) == 0 && p[pre.size()] == L'/';
}

Result<RestoreResult> restore(const RestoreRequest& req)
{
    if (req.targetDirectory.empty())
        return Error::make(ErrorCode::InvalidArgument, "pasta de destino da restauração não informada");

    auto identity = vault::readIdentity(req.vaultLocation);
    if (!identity)
        return identity.error();

    const vault::JobLayout layout(req.vaultLocation, req.jobId);
    if (!ops::pathExists(layout.root()))
        return Error::make(ErrorCode::NotFound, "a tarefa " + win::toUtf8(req.jobId) + " não existe neste cofre");

    if (ops::pathExists(req.targetDirectory) && directoryHasContent(req.targetDirectory) && !req.allowNonEmptyTarget)
        return Error::make(ErrorCode::AlreadyExists,
                           "a pasta de destino não está vazia; a restauração nunca sobrescreve dados por padrão");

    // Cofre cifrado: KeyRequired se a chave não estiver disponível (a
    // interface pede a chave de recuperação e tenta de novo).
    auto cipher = crypto::cipherFor(req.vaultLocation, req.secretsFile);
    if (!cipher)
        return cipher.error();
    CopyOptions copyOptions;
    copyOptions.decrypt = cipher.value().get();

    RestoreResult result;
    auto state = stateAtRun(layout, req.runId, &result.runId);
    if (!state)
        return state.error();

    for (const auto& f : state.value()) {
        const std::wstring& path = f.entry.meta.relativePath;
        if (!underPrefix(path, req.pathPrefix))
            continue;

        const std::wstring source = f.versionRunId.empty() ? layout.currentFile(f.vaultPath())
                                                           : layout.versionFile(f.versionRunId, f.vaultPath());
        if (!ops::pathExists(source)) {
            result.failures.emplace_back(path, f.versionRunId.empty()
                                                   ? "arquivo ausente em current/ do cofre"
                                                   : "versão expirada ou ausente em versions/" + win::toUtf8(f.versionRunId));
            continue;
        }

        const std::wstring dest = win::joinPath(req.targetDirectory, win::fromCatalogPath(path));
        auto copied = copyFileVerified(source, dest, copyOptions);
        if (!copied) {
            result.failures.emplace_back(path, copied.error().message);
            continue;
        }
        if (copied.value().sha256 != f.entry.sha256) {
            ::DeleteFileW(win::toExtendedPath(dest).c_str());
            result.failures.emplace_back(path, "conteúdo no cofre não confere com o catálogo (SHA-256 divergente)");
            continue;
        }
        ++result.restored;
        result.bytes += copied.value().bytes;
    }
    return result;
}

} // namespace synkro
