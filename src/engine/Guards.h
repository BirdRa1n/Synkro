#pragma once

#include "scan/ChangeDetector.h"
#include "scan/FileScanner.h"

#include <cstddef>
#include <string>

namespace synkro {

// Guardas contra os cenários em que "o backup destrói o backup"
// (architecture.md §7). Um run bloqueado não altera nenhum cofre.
struct GuardPolicy {
    double maxDeletedRatio = 0.20;     // > 20% dos arquivos excluídos num run
    double maxModifiedRatio = 0.50;    // > 50% modificados (indício de ransomware)
    std::size_t minPreviousFiles = 20; // proporções só valem a partir deste tamanho
    std::wstring sentinelFile;         // caminho do catálogo que precisa existir na origem
};

enum class GuardCode { None, EmptySource, SentinelMissing, MassDeletion, MassModification };

const char* toString(GuardCode code) noexcept;

struct GuardVerdict {
    GuardCode code = GuardCode::None;
    std::string message; // pt-BR

    bool blocked() const noexcept { return code != GuardCode::None; }
};

// Guardas que dependem só da varredura da origem.
GuardVerdict evaluateSourceGuards(const GuardPolicy& policy, const ScanResult& scan);

// Guardas que comparam a varredura com o inventário de um cofre.
GuardVerdict evaluateChangeGuards(const GuardPolicy& policy, const ChangeSet& changes, std::size_t currentCount);

} // namespace synkro
