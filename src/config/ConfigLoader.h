#pragma once

#include "config/Config.h"

#include <string>

namespace synkro::config {

// Pasta padrão: %SYNKRO_CONFIG_DIR% ou C:\ProgramData\Synkro\config.
std::wstring defaultConfigDirectory();

struct LoadOptions {
    // Confere discos físicos de cofres e origens (exige os discos conectados;
    // cofres ausentes geram aviso, não erro).
    bool checkPhysicalDisks = true;

    // Avisa sobre segredos referenciados e não cadastrados.
    bool checkSecrets = true;
    std::wstring secretsFile; // vazio = padrão (<dados>\secrets.dat)
};

// Lê synkro.yml e jobs/*.yml, valida sintaxe, campos e regras de negócio
// (docs/configuration.md §10). Nunca falha: problemas vão para
// ConfigSet::diagnostics. Use hasErrors() antes de aplicar a configuração.
ConfigSet loadConfig(const std::wstring& directory, const LoadOptions& options = {});

// Valida um único documento de tarefa (usado pelos testes e pelo editor do
// desktop). `global` fornece os cofres e padrões.
JobConfig parseJobDocument(const std::string& yaml, const std::wstring& fileName, const GlobalConfig& global,
                           std::vector<Diagnostic>& diagnostics);

GlobalConfig parseGlobalDocument(const std::string& yaml, const std::wstring& fileName,
                                 std::vector<Diagnostic>& diagnostics);

std::string formatDiagnostic(const Diagnostic& d);

} // namespace synkro::config
