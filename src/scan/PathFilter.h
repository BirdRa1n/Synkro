#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace synkro {

// Padrões de inclusão/exclusão no estilo .gitignore, comparados sem
// diferenciar maiúsculas/minúsculas sobre caminhos do catálogo ('/').
//
//   *      qualquer sequência sem '/'
//   ?      um caractere (exceto '/')
//   **     qualquer sequência, inclusive '/'
//   **/    zero ou mais diretórios
//
// Padrão sem '/' casa com o nome em qualquer nível ("*.tmp" == "**/*.tmp").
// Um '/' inicial ancora o padrão na raiz da origem.
bool matchGlob(std::wstring_view pattern, std::wstring_view path);

class PathFilter {
public:
    PathFilter() = default;
    PathFilter(std::vector<std::wstring> include, std::vector<std::wstring> exclude);

    // Diretórios: excluídos se algum padrão de exclusão casar com "dir" ou
    // "dir/". Arquivos: excluídos por exclusão; se houver padrões de
    // inclusão, precisam casar com pelo menos um.
    bool accepts(std::wstring_view catalogPath, bool isDirectory) const;

    bool empty() const noexcept { return m_include.empty() && m_exclude.empty(); }

private:
    static std::wstring normalize(const std::wstring& pattern);

    std::vector<std::wstring> m_include;
    std::vector<std::wstring> m_exclude;
};

} // namespace synkro
