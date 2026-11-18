#pragma once

namespace synkro {

// Versão do Synkro (definida pelo CMake a partir de project(VERSION)).
const char* versionString() noexcept;

// Versão do formato de cofre gravado em vault.json. Só muda quando o layout em
// disco muda de forma incompatível.
constexpr int kVaultFormatVersion = 1;

} // namespace synkro
