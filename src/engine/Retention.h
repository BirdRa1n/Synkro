#pragma once

#include "engine/BackupEngine.h"
#include "vault/JobLayout.h"

#include <QDateTime>

#include <cstddef>
#include <cstdint>

namespace synkro {

struct RetentionResult {
    std::size_t runsExpired = 0;      // pastas versions/<run> removidas por completo
    std::size_t filesRemoved = 0;
    std::size_t deletedFilesKept = 0; // excluídos na origem ainda protegidos por keep_deleted
    std::uint64_t bytesFreed = 0;
};

// Expira versões antigas (architecture.md §9). Regras:
//  - só atua em versions/ (nunca em current/);
//  - as `keepMinRuns` pastas de versões mais recentes nunca são tocadas;
//  - pastas com mais de `keepVersionsDays` são removidas, exceto arquivos
//    excluídos da origem naquela execução, que ficam até `keepDeletedDays`.
//
// Deve ser chamada somente após uma execução com status Sucesso.
Result<RetentionResult> applyRetention(const vault::JobLayout& layout, const RetentionPolicy& policy,
                                       const QDateTime& nowUtc);

} // namespace synkro
