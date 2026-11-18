#pragma once

#include "core/Result.h"

#include <string>
#include <vector>

namespace synkro {

// Cofre de segredos local (docs/database-backup.md §6).
//
// Os valores são cifrados com DPAPI em escopo de MÁQUINA (o worker roda como
// SYSTEM e o desktop como usuário: ambos precisam ler). Por isso a proteção
// real é a ACL do arquivo, restrita a SYSTEM e Administradores quando o
// arquivo é criado por um processo elevado.
//
// Os nomes ficam em texto claro (permitem validar a configuração sem
// decifrar nada); os valores nunca.
class SecretStore {
public:
    explicit SecretStore(std::wstring path);

    const std::wstring& path() const { return m_path; }

    Status set(const std::string& name, const std::string& value);
    Result<std::string> get(const std::string& name) const;
    Status remove(const std::string& name);
    Result<std::vector<std::string>> names() const;
    bool contains(const std::string& name) const;

    static bool isValidName(const std::string& name);

private:
    std::wstring m_path;
};

// Apaga a memória de uma string com segredo antes de liberá-la.
void wipe(std::string& s);

} // namespace synkro
