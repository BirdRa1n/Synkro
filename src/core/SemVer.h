#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace synkro {

// Versão no formato semver 2.0: 0.9.0, 0.9.0-beta.1 (aceita "v" no início e
// ignora metadados de build "+...").
struct SemVer {
    int major = 0, minor = 0, patch = 0;
    std::vector<std::string> prerelease; // "beta", "1"
    bool valid = false;

    bool isPrerelease() const { return !prerelease.empty(); }
};

SemVer parseSemVer(std::string_view text);

// Precedência do semver: <0, 0, >0. Pré-lançamento vem antes da versão final
// (0.9.0-beta.2 < 0.9.0); identificadores numéricos comparam como números.
int compareSemVer(const SemVer& a, const SemVer& b);

// `candidate` é mais nova que `current`? (false se alguma for inválida)
bool isNewerVersion(std::string_view candidate, std::string_view current);

} // namespace synkro
