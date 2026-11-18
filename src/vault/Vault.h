#pragma once

#include "core/Result.h"

#include <QDateTime>
#include <QString>

#include <string>

namespace synkro::vault {

// Pasta raiz do cofre dentro do disco: X:\SynkroVault
inline constexpr wchar_t kVaultDirName[] = L"SynkroVault";
inline constexpr wchar_t kIdentityFileName[] = L"vault.json";
inline constexpr char kFormatName[] = "synkro-vault";

// Identidade gravada em X:\SynkroVault\vault.json. É ela — e não a letra da
// unidade — que identifica um disco de backup (architecture.md §3.2).
struct VaultIdentity {
    QString vaultId;          // UUID, sem chaves
    QString label;
    QDateTime createdAt;      // UTC
    QString createdByHost;
    QString synkroVersion;
    int formatVersion = 0;
};

// X:\ -> X:\SynkroVault
std::wstring vaultRoot(const std::wstring& location);

// Cria X:\SynkroVault\, X:\SynkroVault\jobs\ e o vault.json (gravação
// atômica). Falha com AlreadyExists se já houver um cofre nesse local —
// um cofre existente nunca é reinicializado silenciosamente.
Result<VaultIdentity> initialize(const std::wstring& location, const QString& label);

// Lê e valida o vault.json. Corrupted se o arquivo estiver inválido;
// Unsupported se o formato for de uma versão mais nova do Synkro.
Result<VaultIdentity> readIdentity(const std::wstring& location);

QByteArray serialize(const VaultIdentity& identity);
Result<VaultIdentity> parse(const QByteArray& json);

} // namespace synkro::vault
