#pragma once

#include "core/Result.h"
#include "vault/JobLayout.h"

#include <cstdint>
#include <string>
#include <vector>

namespace synkro::ops {

// Move current/<path> para versions/<run>/<path> (rename no mesmo volume).
Status moveCurrentToVersions(const vault::JobLayout& layout, const std::wstring& runId,
                             const std::wstring& catalogPath);

// Desfaz moveCurrentToVersions (usado quando a gravação da nova versão falha
// e na recuperação de execuções interrompidas).
Status moveVersionBackToCurrent(const vault::JobLayout& layout, const std::wstring& runId,
                                const std::wstring& catalogPath);

// Remove todos os arquivos de .tmp (restos de execuções interrompidas).
Status cleanTempDirectory(const vault::JobLayout& layout);

// Remove uma árvore de diretórios inteira. `bytesFreed` recebe o total apagado.
Status removeTree(const std::wstring& path, std::uint64_t* bytesFreed = nullptr);

// Remove diretórios vazios abaixo de `root` (inclusive o próprio root, se
// `includeRoot`).
void removeEmptyDirectories(const std::wstring& root, bool includeRoot);

// Lista os subdiretórios imediatos (apenas nomes), ordenados.
std::vector<std::wstring> listSubdirectories(const std::wstring& path);

// Lista arquivos imediatos que terminam com `suffix` (apenas nomes), ordenados.
std::vector<std::wstring> listFiles(const std::wstring& path, const std::wstring& suffix);

bool pathExists(const std::wstring& path);

} // namespace synkro::ops
