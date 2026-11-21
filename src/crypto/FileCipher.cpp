#include "crypto/FileCipher.h"

#include "platform/windows/AlignedBuffer.h"
#include "platform/windows/Win32.h"

#include <cstring>

namespace synkro::crypto {

namespace {

Nonce chunkNonce(std::uint64_t index, bool last)
{
    Nonce n{};
    for (int i = 0; i < 8; ++i)
        n[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(index >> (56 - 8 * i));
    n[11] = last ? 1 : 0;
    return n;
}

std::uint32_t readU32(const char* p)
{
    return static_cast<std::uint32_t>(static_cast<std::uint8_t>(p[0])) | static_cast<std::uint32_t>(static_cast<std::uint8_t>(p[1])) << 8
           | static_cast<std::uint32_t>(static_cast<std::uint8_t>(p[2])) << 16
           | static_cast<std::uint32_t>(static_cast<std::uint8_t>(p[3])) << 24;
}

Error badFormat(const std::string& why)
{
    return Error::make(ErrorCode::Corrupted, "arquivo cifrado inválido: " + why);
}

} // namespace

FileCipher::~FileCipher()
{
    wipe(m_key);
}

Result<std::shared_ptr<const FileCipher>> FileCipher::fromKey(const Key& vaultKey)
{
    std::shared_ptr<FileCipher> c(new FileCipher);
    c->m_key = vaultKey;
    auto id = hmacSha256(vaultKey, "synkro-key-id-v1");
    if (!id)
        return id.error();
    std::memcpy(c->m_keyId.data(), id.value().data(), c->m_keyId.size());
    return std::shared_ptr<const FileCipher>(std::move(c));
}

Result<Key> FileCipher::fileKey(const std::uint8_t* salt) const
{
    std::string info = "synkro-file-v1";
    info.append(reinterpret_cast<const char*>(salt), 16);
    auto k = hmacSha256(m_key, info);
    if (!k)
        return k.error();
    return k.value();
}

Result<FileCipher::Encryptor> FileCipher::encryptor() const
{
    Encryptor e;
    std::uint8_t salt[16];
    if (auto st = randomBytes(salt, sizeof(salt)); !st)
        return st.error();
    auto key = fileKey(salt);
    if (!key)
        return key.error();
    auto aes = AesGcm::create(key.value());
    wipe(key.value());
    if (!aes)
        return aes.error();
    e.m_aes = std::make_unique<AesGcm>(std::move(aes).value());
    e.m_header.assign(kFileMagic, 8);
    e.m_header.append(reinterpret_cast<const char*>(m_keyId.data()), m_keyId.size());
    e.m_header.append(reinterpret_cast<const char*>(salt), sizeof(salt));
    const std::uint32_t chunk = kChunkSize;
    for (int i = 0; i < 4; ++i)
        e.m_header.push_back(static_cast<char>((chunk >> (8 * i)) & 0xFF));
    e.m_pending.reserve(kChunkSize * 2);
    return e;
}

Status FileCipher::Encryptor::sealChunk(bool last, const Sink& out)
{
    if (!m_headerSent) {
        if (auto st = out(reinterpret_cast<const std::uint8_t*>(m_header.data()), m_header.size()); !st)
            return st;
        m_headerSent = true;
    }
    const std::size_t n = last ? m_pending.size() : kChunkSize;
    m_cipher.resize(n + kTagSize);
    Tag tag{};
    if (auto st = m_aes->encrypt(chunkNonce(m_index++, last), m_header, m_pending.data(), n, m_cipher.data(), tag); !st)
        return st;
    std::memcpy(m_cipher.data() + n, tag.data(), kTagSize);
    if (auto st = out(m_cipher.data(), m_cipher.size()); !st)
        return st;
    m_pending.erase(m_pending.begin(), m_pending.begin() + static_cast<std::ptrdiff_t>(n));
    return Status::success();
}

Status FileCipher::Encryptor::write(const std::uint8_t* data, std::size_t size, const Sink& out)
{
    m_pending.insert(m_pending.end(), data, data + size);
    // Só emite blocos cheios quando sabe que há mais dados depois deles: o
    // último bloco precisa sair com a marca de "último".
    while (m_pending.size() > kChunkSize) {
        if (auto st = sealChunk(false, out); !st)
            return st;
    }
    return Status::success();
}

Status FileCipher::Encryptor::finish(const Sink& out)
{
    return sealChunk(true, out);
}

FileCipher::Decryptor FileCipher::decryptor() const
{
    Decryptor d;
    d.m_owner = this;
    return d;
}

Status FileCipher::Decryptor::openChunk(std::size_t size, bool last, const Sink& out)
{
    if (size < kTagSize)
        return badFormat("bloco incompleto");
    const std::size_t n = size - kTagSize;
    m_plain.resize(n);
    Tag tag{};
    std::memcpy(tag.data(), m_pending.data() + n, kTagSize);
    if (auto st = m_aes->decrypt(chunkNonce(m_index++, last), m_header, m_pending.data(), n, m_plain.data(), tag); !st)
        return st;
    if (auto st = out(m_plain.data(), n); !st)
        return st;
    m_pending.erase(m_pending.begin(), m_pending.begin() + static_cast<std::ptrdiff_t>(size));
    return Status::success();
}

Status FileCipher::Decryptor::write(const std::uint8_t* data, std::size_t size, const Sink& out)
{
    m_pending.insert(m_pending.end(), data, data + size);
    if (!m_aes) {
        if (m_pending.size() < kHeaderSize)
            return Status::success();
        m_header.assign(reinterpret_cast<const char*>(m_pending.data()), kHeaderSize);
        if (m_header.compare(0, 8, kFileMagic, 8) != 0)
            return badFormat("cabeçalho desconhecido (o arquivo não foi cifrado pelo Synkro)");
        if (std::memcmp(m_header.data() + 8, m_owner->keyId().data(), 8) != 0)
            return Error::make(ErrorCode::Corrupted, "o arquivo foi cifrado com outra chave (de outro cofre)");
        m_chunk = readU32(m_header.data() + 32);
        if (m_chunk == 0 || m_chunk > (64u << 20))
            return badFormat("tamanho de bloco inválido");
        auto key = m_owner->fileKey(reinterpret_cast<const std::uint8_t*>(m_header.data() + 16));
        if (!key)
            return key.error();
        auto aes = AesGcm::create(key.value());
        wipe(key.value());
        if (!aes)
            return aes.error();
        m_aes = std::make_unique<AesGcm>(std::move(aes).value());
        m_pending.erase(m_pending.begin(), m_pending.begin() + static_cast<std::ptrdiff_t>(kHeaderSize));
    }
    const std::size_t full = m_chunk + kTagSize;
    while (m_pending.size() > full) {
        if (auto st = openChunk(full, false, out); !st)
            return st;
    }
    return Status::success();
}

Status FileCipher::Decryptor::finish(const Sink& out)
{
    if (!m_aes)
        return badFormat("arquivo menor que o cabeçalho");
    if (m_pending.size() > m_chunk + kTagSize)
        return badFormat("bloco final grande demais");
    return openChunk(m_pending.size(), true, out);
}

Result<std::string> FileCipher::encryptBytes(std::string_view plain) const
{
    auto e = encryptor();
    if (!e)
        return e.error();
    std::string out;
    const Sink sink = [&](const std::uint8_t* d, std::size_t n) {
        out.append(reinterpret_cast<const char*>(d), n);
        return Status::success();
    };
    if (auto st = e.value().write(reinterpret_cast<const std::uint8_t*>(plain.data()), plain.size(), sink); !st)
        return st.error();
    if (auto st = e.value().finish(sink); !st)
        return st.error();
    return out;
}

Result<std::string> FileCipher::decryptBytes(std::string_view cipher) const
{
    auto d = decryptor();
    std::string out;
    const Sink sink = [&](const std::uint8_t* p, std::size_t n) {
        out.append(reinterpret_cast<const char*>(p), n);
        return Status::success();
    };
    if (auto st = d.write(reinterpret_cast<const std::uint8_t*>(cipher.data()), cipher.size(), sink); !st)
        return st.error();
    if (auto st = d.finish(sink); !st)
        return st.error();
    return out;
}

bool looksEncrypted(const std::wstring& path)
{
    win::UniqueHandle h(::CreateFileW(win::toExtendedPath(path).c_str(), GENERIC_READ,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr));
    if (!h)
        return false;
    char magic[8] = {};
    DWORD read = 0;
    return ::ReadFile(h.get(), magic, sizeof(magic), &read, nullptr) && read == 8 && std::memcmp(magic, kFileMagic, 8) == 0;
}

Result<Sha256Digest> hashVaultFile(const std::wstring& path, const FileCipher* cipher, bool bypassCache)
{
    if (!cipher)
        return Sha256::ofFile(path, bypassCache);

    win::UniqueHandle file(::CreateFileW(win::toExtendedPath(path).c_str(), GENERIC_READ,
                                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                         FILE_FLAG_SEQUENTIAL_SCAN | (bypassCache ? FILE_FLAG_NO_BUFFERING : 0), nullptr));
    if (!file)
        return win::lastError("abrir arquivo para hash");
    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(file.get(), &size))
        return win::lastError("ler o tamanho do arquivo para hash");
    win::AlignedBuffer buffer(1u << 20);
    if (!buffer)
        return Error::make(ErrorCode::Internal, "sem memória para buffer de leitura");

    Sha256 h;
    auto dec = cipher->decryptor();
    const Sink sink = [&](const std::uint8_t* d, std::size_t n) { return h.update(d, n); };
    std::uint64_t total = 0;
    // Mesma regra do Sha256::ofFile: para no tamanho, sem leitura desalinhada
    // depois do fim (o SMB recusa sem cache).
    while (total < static_cast<std::uint64_t>(size.QuadPart)) {
        DWORD read = 0;
        if (!::ReadFile(file.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr))
            return win::lastError("ler arquivo para hash");
        if (read == 0)
            break;
        if (auto st = dec.write(reinterpret_cast<const std::uint8_t*>(buffer.data()), read, sink); !st)
            return st.error();
        total += read;
        if (read < buffer.size())
            break;
    }
    if (auto st = dec.finish(sink); !st)
        return st.error();
    return h.finish();
}

} // namespace synkro::crypto
