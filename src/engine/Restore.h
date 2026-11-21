#pragma once

#include "catalog/Catalog.h"
#include "vault/JobLayout.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace synkro {

// Onde está o conteúdo de um arquivo no cofre.
struct RestorableFile {
    CatalogEntry entry;
    std::wstring versionRunId; // vazio = current/; senão versions/<run>/
};

// Reconstrói o estado da origem numa execução retida (point-in-time).
// `runId` vazio = última execução. Parte do catálogo atual e desfaz, da mais
// nova para a mais antiga, as execuções posteriores a `runId`.
Result<std::vector<RestorableFile>> stateAtRun(const vault::JobLayout& layout, const std::wstring& runId,
                                               std::wstring* resolvedRunId = nullptr);

// Execuções disponíveis (com manifest), em ordem cronológica.
std::vector<std::wstring> listRuns(const vault::JobLayout& layout);

struct RestoreRequest {
    std::wstring vaultLocation;
    std::wstring jobId;
    std::wstring runId;          // vazio = última
    std::wstring targetDirectory;
    std::wstring pathPrefix;     // restringe a uma pasta/arquivo (caminho do catálogo)
    bool allowNonEmptyTarget = false;
    std::wstring secretsFile;    // chave de cofre cifrado (vazio = padrão)
};

struct RestoreResult {
    std::wstring runId;
    std::size_t restored = 0;
    std::uint64_t bytes = 0;
    std::vector<std::pair<std::wstring, std::string>> failures; // caminho -> motivo
};

// Restaura para uma pasta NOVA (por padrão recusa pasta com conteúdo) e
// confere o SHA-256 de cada arquivo contra o catálogo (architecture.md §13).
Result<RestoreResult> restore(const RestoreRequest& request);

} // namespace synkro
