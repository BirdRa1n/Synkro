#pragma once

#include "core/Result.h"
#include "hash/Sha256.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace synkro::db {

// Grava um arquivo .zst em fluxo: os dados chegam em blocos (saída do
// mysqldump, por exemplo) e são comprimidos sem arquivo intermediário. O
// SHA-256 calculado é o do arquivo comprimido, que é o que vai para o cofre.
class ZstdFileWriter {
public:
    static Result<ZstdFileWriter> create(const std::wstring& path, int level = 3);

    ZstdFileWriter(ZstdFileWriter&&) noexcept;
    ZstdFileWriter& operator=(ZstdFileWriter&&) noexcept;
    ~ZstdFileWriter();

    Status write(const void* data, std::size_t size);

    // Finaliza o quadro zstd, sincroniza o arquivo em disco e fecha.
    Status finish();

    std::uint64_t inputBytes() const;
    std::uint64_t outputBytes() const;
    Sha256Digest sha256() const; // válido após finish()

private:
    ZstdFileWriter();
    struct Impl;
    std::unique_ptr<Impl> d;
};

// Comprime um arquivo inteiro.
Status compressFile(const std::wstring& source, const std::wstring& destination, int level = 3);

// Descomprime um .zst para outro arquivo.
Status decompressFile(const std::wstring& source, const std::wstring& destination);

} // namespace synkro::db
