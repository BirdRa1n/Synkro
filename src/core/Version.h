#pragma once

namespace synkro {

// Versão do Synkro (definida pelo CMake a partir de project(VERSION)).
const char* versionString() noexcept;

// "owner/repo" do GitHub cujas releases o atualizador consulta (vazio = sem
// atualização automática). Definido no CMake (SYNKRO_UPDATE_REPO).
const char* updateRepository() noexcept;

// Versão do formato de cofre gravado em vault.json. Só muda quando o layout em
// disco muda de forma incompatível.
constexpr int kVaultFormatVersion = 1;

} // namespace synkro
