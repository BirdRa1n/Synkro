#pragma once

#include "hash/Sha256.h"
#include "scan/FileScanner.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace synkro {

// Arquivo como registrado no inventário (catálogo do cofre) na última
// execução bem-sucedida.
struct KnownFile {
    FileEntry meta;
    std::optional<Sha256Digest> sha256;
};

enum class ChangeKind { Added, Modified, Deleted };

// Motivos de uma modificação (bitmask).
enum ChangeReason : std::uint32_t {
    ReasonNone       = 0,
    ReasonSize       = 1u << 0,
    ReasonWriteTime  = 1u << 1,
    ReasonChangeTime = 1u << 2, // alterado por software que preserva a data de modificação
    ReasonFileId     = 1u << 3, // arquivo substituído por outro com o mesmo nome
    ReasonCaseRename = 1u << 4, // apenas maiúsculas/minúsculas do nome mudaram
};

struct Change {
    static constexpr std::size_t npos = static_cast<std::size_t>(-1);

    ChangeKind kind;
    std::wstring relativePath;          // caminho atual (ou o antigo, para Deleted)
    std::uint32_t reasons = ReasonNone;
    std::size_t currentIndex = npos;    // índice em `current`
    std::size_t previousIndex = npos;   // índice em `previous`
};

struct ChangeSet {
    std::vector<Change> added;
    std::vector<Change> modified;
    std::vector<Change> deleted;
    std::size_t unchanged = 0;
    std::size_t previousCount = 0;

    bool empty() const noexcept { return added.empty() && modified.empty() && deleted.empty(); }

    // Proporções em relação ao inventário anterior, usadas pelas guardas de
    // exclusão/modificação em massa. 0 quando não havia inventário.
    double deletedRatio() const noexcept;
    double modifiedRatio() const noexcept;
};

struct DetectOptions {
    // Só têm efeito quando os dois lados possuem hasExtendedInfo.
    bool compareChangeTime = true;
    bool compareFileId = true;
};

// Compara o inventário anterior com a varredura atual. Caminhos são
// comparados sem diferenciar maiúsculas/minúsculas, como no NTFS.
//
// A detecção é apenas por metadados: o hash é calculado depois, durante a
// cópia dos candidatos.
ChangeSet detectChanges(const std::vector<KnownFile>& previous,
                        const std::vector<FileEntry>& current,
                        const DetectOptions& options = {});

} // namespace synkro
