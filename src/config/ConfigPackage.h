#pragma once

// Exportar/importar a configuração entre PDVs (docs/configuration.md §11).
//
// O pacote é um YAML com vários documentos: um cabeçalho (origem, data,
// segredos necessários, papéis dos cofres) e um documento por arquivo de
// configuração. Nunca leva segredos (a configuração só os referencia pelo
// nome) nem a identidade dos discos (vault_id e path): os cofres de cada
// máquina são outros discos, ligados na importação com --bind.

#include "config/Config.h"
#include "core/Result.h"

#include <map>
#include <string>
#include <vector>

namespace synkro::config {

struct PackageFile {
    std::wstring relativePath; // "synkro.yml" ou "jobs\\x.yml"
    std::string content;
};

struct ConfigPackage {
    std::string exportedFrom;
    std::string exportedAt;              // ISO 8601 UTC
    std::vector<std::string> secrets;    // nomes referenciados pelas tarefas
    std::vector<VaultConfig> vaults;     // papéis dos cofres (sem vault_id/path)
    std::vector<PackageFile> files;
};

// Gera o pacote. `jobIds` vazio = todas as tarefas. A configuração precisa
// estar sem erros.
Result<std::string> exportPackage(const ConfigSet& set, const std::vector<std::string>& jobIds = {});

Result<ConfigPackage> parsePackage(const std::string& text);

struct ImportOptions {
    std::wstring configDir;
    // Cofre do pacote (id) -> disco nesta máquina (raiz ou pasta com SynkroVault).
    std::map<std::string, std::wstring> bindings;
    bool replaceJobs = false;   // sobrescrever tarefas com o mesmo arquivo
    bool dryRun = false;        // só valida e mostra o que faria
    std::wstring secretsFile;   // vazio = padrão (para listar segredos faltando)
    bool checkPhysicalDisks = true; // validação do resultado (testes usam false)
};

struct ImportReport {
    std::vector<std::string> written;        // arquivos gravados (ou que seriam)
    std::vector<std::string> kept;           // arquivos já existentes, mantidos
    std::vector<std::string> missingSecrets; // cadastrar com "secret set"
    std::vector<std::string> warnings;
    std::vector<Diagnostic> diagnostics;     // validação do resultado
};

// Importa: monta a configuração resultante numa pasta temporária, valida e
// só então grava. Com erros de validação, nada é alterado.
Result<ImportReport> importPackage(const ConfigPackage& package, const ImportOptions& options);

} // namespace synkro::config
