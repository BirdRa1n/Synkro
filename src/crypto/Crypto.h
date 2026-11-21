#pragma once

// Primitivas criptográficas via Windows CNG (bcrypt.dll), sem bibliotecas de
// terceiros: AES-256-GCM, HMAC-SHA256, PBKDF2-SHA256 e o gerador aleatório
// do sistema.

#include "core/Result.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace synkro::crypto {

using Key = std::array<std::uint8_t, 32>;
using Tag = std::array<std::uint8_t, 16>;
using Nonce = std::array<std::uint8_t, 12>;

Status randomBytes(void* out, std::size_t size);

Result<std::array<std::uint8_t, 32>> hmacSha256(const Key& key, std::string_view data);

// PBKDF2-HMAC-SHA256 com saída de 32 bytes.
Result<Key> pbkdf2Sha256(std::string_view password, std::string_view salt, std::uint64_t iterations);

// Chave AES-256 em modo GCM (tag de 16 bytes).
class AesGcm {
public:
    static Result<AesGcm> create(const Key& key);
    ~AesGcm();
    AesGcm(AesGcm&&) noexcept;
    AesGcm& operator=(AesGcm&&) noexcept;
    AesGcm(const AesGcm&) = delete;
    AesGcm& operator=(const AesGcm&) = delete;

    Status encrypt(const Nonce& nonce, std::string_view aad, const std::uint8_t* in, std::size_t size, std::uint8_t* out,
                   Tag& tag) const;
    // Corrupted se a autenticação falhar (dado alterado, chave errada ou
    // bloco fora de ordem).
    Status decrypt(const Nonce& nonce, std::string_view aad, const std::uint8_t* in, std::size_t size, std::uint8_t* out,
                   const Tag& tag) const;

private:
    AesGcm() = default;
    struct Impl;
    std::unique_ptr<Impl> d;
};

// Apaga a memória de uma chave.
void wipe(Key& key);

std::string toBase64(const void* data, std::size_t size);
Result<std::string> fromBase64(std::string_view text);

} // namespace synkro::crypto
