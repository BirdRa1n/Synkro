#include "config/VaultAccess.h"

#include "core/Paths.h"
#include "database/DatabaseBackup.h"
#include "platform/windows/NetworkShare.h"
#include "platform/windows/Win32.h"
#include "secrets/SecretStore.h"

#include <QUuid>

namespace synkro::config {

std::optional<vault::LocatedVault> locateVault(const VaultConfig& v, std::string* why, const std::wstring& secretsFile)
{
    auto fail = [&](std::string reason) -> std::optional<vault::LocatedVault> {
        if (why)
            *why = std::move(reason);
        return std::nullopt;
    };
    const QString expected = QString::fromStdString(v.vaultId);

    if (!v.network) {
        if (auto located = vault::locateVault(expected, v.path))
            return located;
        return fail("o disco não está conectado a esta máquina");
    }

    if (auto st = win::probeSmbServer(win::uncServer(v.path)); !st)
        return fail(st.error().message);

    if (!v.netUser.empty()) {
        auto pass = db::resolveSecret(v.netPassword, secretsFile.empty() ? synkro::secretsFile() : secretsFile);
        if (pass) {
            std::string p = std::move(pass).value();
            auto st = win::connectShare(v.path, win::fromUtf8(v.netUser), p);
            wipe(p);
            // Conexão existente com outro usuário (desktop): segue com ela.
            if (!st && st.error().code != ErrorCode::AlreadyExists)
                return fail(st.error().message);
        }
        // Sem acesso ao secrets.dat (usuário comum): segue com a sessão atual.
    }

    auto identity = vault::readIdentity(v.path);
    if (!identity) {
        if (identity.error().code == ErrorCode::NotFound)
            return fail("o compartilhamento respondeu, mas não há cofre do Synkro em " + win::toUtf8(v.path));
        return fail(identity.error().message);
    }
    if (!expected.isEmpty() && QUuid::fromString(identity.value().vaultId) != QUuid::fromString(expected))
        return fail("a pasta " + win::toUtf8(v.path) + " contém outro cofre (" + identity.value().vaultId.toStdString() + ")");
    return vault::LocatedVault{v.path, std::move(identity).value()};
}

} // namespace synkro::config
