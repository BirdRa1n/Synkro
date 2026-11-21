#pragma once

#include "vault/JobLayout.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace synkro {

struct VerifyResult {
    std::size_t checked = 0;
    std::size_t ok = 0;
    std::uint64_t bytes = 0;
    std::vector<std::wstring> missing;    // no catálogo, mas ausentes em current/
    std::vector<std::wstring> corrupted;  // SHA-256 divergente
    std::vector<std::pair<std::wstring, std::string>> unreadable;

    bool healthy() const noexcept { return missing.empty() && corrupted.empty() && unreadable.empty(); }
};

// Verificação periódica do cofre (scrub, architecture.md §11): relê os
// arquivos de current/ SEM cache e compara com o SHA-256 do catálogo.
// `samplePercent` em (0, 100]: 100 confere tudo.
// Cofre cifrado: a chave vem da memória do processo ou de `secretsFile`
// (vazio = <dados>\secrets.dat); sem ela, KeyRequired.
Result<VerifyResult> verifyVault(const vault::JobLayout& layout, double samplePercent = 100.0,
                                 const std::wstring& secretsFile = {});

} // namespace synkro
