#pragma once

#include "config/Config.h"
#include "vault/VaultLocator.h"

#include <optional>
#include <string>

namespace synkro::config {

// Localiza um cofre declarado no synkro.yml.
//
// - Disco local/USB: pelo vault_id em qualquer volume (a letra pode mudar).
// - Em rede (path UNC): sonda o servidor, conecta com as credenciais (se
//   houver) e confere o vault.json naquele caminho; nunca procura em outro
//   lugar.
//
// `why` recebe o motivo quando o cofre não foi encontrado (pt-BR). Sem acesso
// aos segredos (desktop como usuário comum), tenta com a sessão atual.
std::optional<vault::LocatedVault> locateVault(const VaultConfig& v, std::string* why = nullptr,
                                               const std::wstring& secretsFile = {});

} // namespace synkro::config
