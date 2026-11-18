#pragma once

#include "core/Result.h"

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace synkro {

// Identificador NTFS de 128 bits (FILE_ID_INFO). Permite detectar um arquivo
// substituído por outro com o mesmo nome, tamanho e data.
using FileId = std::array<std::uint8_t, 16>;

struct FileEntry {
    std::wstring relativePath;        // forma do catálogo: separador '/'
    std::uint64_t size = 0;
    std::uint64_t lastWriteTime = 0;  // FILETIME em ticks de 100 ns (UTC)
    std::uint64_t creationTime = 0;
    std::uint32_t attributes = 0;

    // Preenchidos apenas com ScanOptions::readFileIds.
    bool hasExtendedInfo = false;
    std::uint64_t changeTime = 0;     // FILE_BASIC_INFO.ChangeTime
    std::uint64_t volumeSerial = 0;
    FileId fileId{};
};

struct ScanIssue {
    std::wstring relativePath; // diretório ou arquivo afetado ('/' como separador)
    Error error;
};

struct ScanOptions {
    // Abre cada arquivo para ler ChangeTime e o FileId de 128 bits. Mais caro,
    // mas detecta alterações que preservam a data de modificação.
    bool readFileIds = false;

    // Filtro opcional (include/exclude). Recebe o caminho relativo no formato
    // do catálogo; devolver false ignora o item (e, para diretórios, tudo
    // abaixo dele).
    std::function<bool(std::wstring_view relativePath, bool isDirectory)> filter;
};

struct ScanResult {
    std::vector<FileEntry> files;              // ordenados por relativePath
    std::vector<std::wstring> skippedLinks;    // links simbólicos e junções (não seguidos)
    std::vector<ScanIssue> issues;             // diretórios/arquivos inacessíveis
    std::uint64_t totalBytes = 0;
    std::uint64_t directoryCount = 0;
};

// Varre recursivamente `root` usando FindFirstFileExW (FindExInfoBasic +
// FIND_FIRST_EX_LARGE_FETCH). Falha somente se a própria raiz não puder ser
// lida; problemas em subpastas são registrados em ScanResult::issues.
//
// Links simbólicos e pontos de montagem nunca são seguidos, para evitar laços
// e cópia de dados fora da origem.
Result<ScanResult> scanDirectory(const std::wstring& root, const ScanOptions& options = {});

} // namespace synkro
