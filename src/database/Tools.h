#pragma once

#include "core/Result.h"

#include <map>
#include <string>
#include <vector>

namespace synkro::db {

// Localiza ferramentas cliente (pg_dump, mysqldump...) na ordem de
// docs/database-backup.md §4.1: caminho explícito, pastas padrão de
// instalação (versão mais alta) e PATH.
Result<std::wstring> locateTool(const std::string& tool, const std::map<std::string, std::wstring>& explicitPaths);

// Primeira linha de "<ferramenta> --version" (ex.: "pg_dump (PostgreSQL) 16.4").
std::string toolVersion(const std::wstring& toolPath);

// Pasta temporária da execução, acessível somente ao dono e a SYSTEM.
// Recebe os arquivos de credenciais e os dumps em preparo; é apagada inteira
// no destrutor, também em caso de falha.
class PrivateDirectory {
public:
    static Result<PrivateDirectory> create(const std::wstring& path);
    PrivateDirectory(PrivateDirectory&& other) noexcept;
    PrivateDirectory& operator=(PrivateDirectory&&) = delete;
    ~PrivateDirectory();

    const std::wstring& path() const { return m_path; }

    // Grava um arquivo pequeno (credenciais) dentro da pasta.
    Result<std::wstring> writeFile(const std::wstring& name, const std::string& content);

private:
    PrivateDirectory() = default;
    std::wstring m_path;
};

// Conteúdo de arquivo PGPASSFILE (escapando ':' e '\').
std::string pgpassLine(const std::string& host, int port, const std::string& user, const std::string& password);

} // namespace synkro::db
