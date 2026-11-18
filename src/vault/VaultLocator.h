#pragma once

#include "vault/Vault.h"

#include <optional>
#include <string>
#include <vector>

namespace synkro::vault {

struct LocatedVault {
    std::wstring location;  // ponto de montagem que contém SynkroVault (ex.: "E:\")
    VaultIdentity identity;
};

// Procura cofres em todos os volumes montados (FindFirstVolumeW). Discos
// sem mídia ou ilegíveis são ignorados silenciosamente.
std::vector<LocatedVault> discoverVaults();

// Localiza um cofre pelo vault_id. A letra da unidade pode ter mudado
// (discos USB), por isso `hint` é só a primeira tentativa: se o cofre ali
// não tiver o ID esperado, todos os volumes são consultados.
std::optional<LocatedVault> locateVault(const QString& vaultId, const std::wstring& hint = {});

} // namespace synkro::vault
