#include "engine/Verify.h"

#include "catalog/Catalog.h"
#include "crypto/VaultKey.h"
#include "engine/VaultOps.h"
#include "hash/Sha256.h"

#include <QRandomGenerator>

namespace synkro {

Result<VerifyResult> verifyVault(const vault::JobLayout& layout, double samplePercent, const std::wstring& secretsFile)
{
    if (samplePercent <= 0.0 || samplePercent > 100.0)
        return Error::make(ErrorCode::InvalidArgument, "amostra deve estar entre 0 e 100%");

    auto catalog = Catalog::load(layout.catalogFile());
    if (!catalog)
        return catalog.error();

    // Cofre cifrado: confere o conteúdo original (decifrando, o que também
    // valida a autenticação de cada bloco).
    auto cipher = crypto::cipherFor(layout.vaultLocation(), secretsFile);
    if (!cipher)
        return cipher.error();

    VerifyResult r;
    auto* rng = QRandomGenerator::global();
    for (const auto& e : catalog.value().entries) {
        if (samplePercent < 100.0 && rng->bounded(100.0) >= samplePercent)
            continue;
        ++r.checked;
        const std::wstring path = layout.currentFile(e.meta.relativePath);
        if (!ops::pathExists(path)) {
            r.missing.push_back(e.meta.relativePath);
            continue;
        }
        auto digest = crypto::hashVaultFile(path, cipher.value().get(), /*bypassCache=*/true);
        if (!digest) {
            // Falha de autenticação = conteúdo alterado: é corrupção, não leitura.
            if (digest.error().code == ErrorCode::Corrupted)
                r.corrupted.push_back(e.meta.relativePath);
            else
                r.unreadable.emplace_back(e.meta.relativePath, digest.error().message);
            continue;
        }
        if (digest.value() != e.sha256) {
            r.corrupted.push_back(e.meta.relativePath);
            continue;
        }
        ++r.ok;
        r.bytes += e.meta.size;
    }
    return r;
}

} // namespace synkro
