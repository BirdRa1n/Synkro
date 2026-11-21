#pragma once

// Formato de arquivo cifrado do cofre (architecture.md §3.5).
//
//   cabeçalho (36 bytes, autenticado em todos os blocos):
//     "SYNKENC1" | key_id (8) | salt (16) | tamanho do bloco (u32 LE)
//   blocos: até `tamanho do bloco` bytes cifrados + tag GCM de 16 bytes
//
// Cada arquivo usa uma chave própria, HMAC-SHA256(chave do cofre,
// "synkro-file-v1" | salt), com salt aleatório: os nonces nunca se repetem
// entre arquivos. O nonce de cada bloco é o índice do bloco mais a marca de
// último bloco (construção STREAM): blocos trocados de ordem, removidos ou um
// arquivo truncado falham na autenticação. Um arquivo vazio tem um único
// bloco final sem dados.

#include "crypto/Crypto.h"
#include "hash/Sha256.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace synkro::crypto {

inline constexpr char kFileMagic[] = "SYNKENC1"; // 8 bytes (sem o terminador)
inline constexpr std::size_t kHeaderSize = 36;
inline constexpr std::size_t kTagSize = 16;
inline constexpr std::uint32_t kChunkSize = 1u << 20;

using KeyId = std::array<std::uint8_t, 8>;

// Recebe os bytes produzidos (cifrados ou decifrados).
using Sink = std::function<Status(const std::uint8_t* data, std::size_t size)>;

// Chave do cofre pronta para cifrar e decifrar arquivos.
class FileCipher {
public:
    static Result<std::shared_ptr<const FileCipher>> fromKey(const Key& vaultKey);
    ~FileCipher();

    const KeyId& keyId() const { return m_keyId; }

    class Encryptor {
    public:
        Status write(const std::uint8_t* data, std::size_t size, const Sink& out);
        Status finish(const Sink& out);

    private:
        friend class FileCipher;
        Status sealChunk(bool last, const Sink& out);
        std::unique_ptr<AesGcm> m_aes;
        std::string m_header;
        std::vector<std::uint8_t> m_pending, m_cipher;
        std::uint64_t m_index = 0;
        bool m_headerSent = false;
    };

    class Decryptor {
    public:
        Status write(const std::uint8_t* data, std::size_t size, const Sink& out);
        Status finish(const Sink& out);

    private:
        friend class FileCipher;
        Status openChunk(std::size_t size, bool last, const Sink& out);
        const FileCipher* m_owner = nullptr;
        std::unique_ptr<AesGcm> m_aes;
        std::string m_header;
        std::vector<std::uint8_t> m_pending, m_plain;
        std::uint32_t m_chunk = 0;
        std::uint64_t m_index = 0;
    };

    Result<Encryptor> encryptor() const;
    Decryptor decryptor() const;

    // Cifra/decifra um bloco de memória inteiro (metadados pequenos e testes).
    Result<std::string> encryptBytes(std::string_view plain) const;
    Result<std::string> decryptBytes(std::string_view cipher) const;

private:
    FileCipher() = default;
    Result<Key> fileKey(const std::uint8_t* salt) const;
    Key m_key{};
    KeyId m_keyId{};
};

// O arquivo começa com o cabeçalho de um arquivo cifrado do Synkro?
bool looksEncrypted(const std::wstring& path);

// SHA-256 do conteúdo ORIGINAL de um arquivo do cofre: decifra quando há
// `cipher` (a autenticação GCM também é conferida). Sem cipher, é o
// Sha256::ofFile de sempre.
Result<Sha256Digest> hashVaultFile(const std::wstring& path, const FileCipher* cipher, bool bypassCache);

} // namespace synkro::crypto
