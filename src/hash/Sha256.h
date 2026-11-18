#pragma once

#include "core/Result.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace synkro {

using Sha256Digest = std::array<std::uint8_t, 32>;

std::string toHex(const Sha256Digest& digest);

// SHA-256 incremental via Windows CNG (BCrypt). Usado para calcular o hash
// durante a cópia, sem reler o arquivo.
class Sha256 {
public:
    Sha256();
    ~Sha256();

    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;
    Sha256(Sha256&&) noexcept;
    Sha256& operator=(Sha256&&) noexcept;

    // false se o provedor CNG não pôde ser inicializado (situação anormal).
    bool valid() const noexcept;

    Status update(const void* data, std::size_t size);
    Status update(std::string_view data) { return update(data.data(), data.size()); }

    // Finaliza e devolve o digest. Depois disso o objeto volta ao estado
    // inicial e pode ser reutilizado.
    Result<Sha256Digest> finish();

    // Conveniências.
    static Result<Sha256Digest> ofBytes(std::string_view data);
    static Result<Sha256Digest> ofFile(const std::wstring& path, bool bypassCache = false);

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace synkro
