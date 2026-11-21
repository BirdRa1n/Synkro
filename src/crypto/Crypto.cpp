#include "crypto/Crypto.h"

#include "platform/windows/Win32.h"

#include <QByteArray>

#include <bcrypt.h>

#include <mutex>

#ifndef NT_SUCCESS
#define NT_SUCCESS(status) (static_cast<NTSTATUS>(status) >= 0)
#endif
#ifndef STATUS_AUTH_TAG_MISMATCH
#define STATUS_AUTH_TAG_MISMATCH static_cast<NTSTATUS>(0xC000A002L)
#endif

namespace synkro::crypto {

namespace {

// Provedores abertos uma vez por processo (são thread-safe para criar chaves).
struct Providers {
    BCRYPT_ALG_HANDLE aesGcm = nullptr;
    BCRYPT_ALG_HANDLE hmac = nullptr;
    BCRYPT_ALG_HANDLE pbkdf2 = nullptr;
    bool ok = false;
};

const Providers& providers()
{
    static Providers p;
    static std::once_flag once;
    std::call_once(once, [] {
        bool ok = NT_SUCCESS(::BCryptOpenAlgorithmProvider(&p.aesGcm, BCRYPT_AES_ALGORITHM, nullptr, 0));
        ok = ok
             && NT_SUCCESS(::BCryptSetProperty(p.aesGcm, BCRYPT_CHAINING_MODE,
                                               reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
                                               sizeof(BCRYPT_CHAIN_MODE_GCM), 0));
        ok = ok && NT_SUCCESS(::BCryptOpenAlgorithmProvider(&p.hmac, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG));
        ok = ok && NT_SUCCESS(::BCryptOpenAlgorithmProvider(&p.pbkdf2, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG));
        p.ok = ok;
    });
    return p;
}

Error cngError(const char* what, NTSTATUS st)
{
    return Error::make(ErrorCode::Internal, std::string(what) + " falhou (CNG)", static_cast<unsigned long>(st));
}

} // namespace

Status randomBytes(void* out, std::size_t size)
{
    const NTSTATUS st = ::BCryptGenRandom(nullptr, static_cast<PUCHAR>(out), static_cast<ULONG>(size),
                                          BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (!NT_SUCCESS(st))
        return cngError("BCryptGenRandom", st);
    return Status::success();
}

Result<std::array<std::uint8_t, 32>> hmacSha256(const Key& key, std::string_view data)
{
    const auto& p = providers();
    if (!p.ok)
        return Error::make(ErrorCode::Internal, "provedores criptográficos (CNG) indisponíveis");
    BCRYPT_HASH_HANDLE h = nullptr;
    NTSTATUS st = ::BCryptCreateHash(p.hmac, &h, nullptr, 0, const_cast<PUCHAR>(key.data()), static_cast<ULONG>(key.size()), 0);
    if (!NT_SUCCESS(st))
        return cngError("BCryptCreateHash", st);
    std::array<std::uint8_t, 32> out{};
    st = ::BCryptHashData(h, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())), static_cast<ULONG>(data.size()), 0);
    if (NT_SUCCESS(st))
        st = ::BCryptFinishHash(h, out.data(), static_cast<ULONG>(out.size()), 0);
    ::BCryptDestroyHash(h);
    if (!NT_SUCCESS(st))
        return cngError("HMAC-SHA256", st);
    return out;
}

Result<Key> pbkdf2Sha256(std::string_view password, std::string_view salt, std::uint64_t iterations)
{
    const auto& p = providers();
    if (!p.ok)
        return Error::make(ErrorCode::Internal, "provedores criptográficos (CNG) indisponíveis");
    Key out{};
    const NTSTATUS st = ::BCryptDeriveKeyPBKDF2(
        p.pbkdf2, reinterpret_cast<PUCHAR>(const_cast<char*>(password.data())), static_cast<ULONG>(password.size()),
        reinterpret_cast<PUCHAR>(const_cast<char*>(salt.data())), static_cast<ULONG>(salt.size()), iterations, out.data(),
        static_cast<ULONG>(out.size()), 0);
    if (!NT_SUCCESS(st))
        return cngError("PBKDF2", st);
    return out;
}

struct AesGcm::Impl {
    BCRYPT_KEY_HANDLE key = nullptr;
    ~Impl()
    {
        if (key)
            ::BCryptDestroyKey(key);
    }
};

AesGcm::~AesGcm() = default;
AesGcm::AesGcm(AesGcm&&) noexcept = default;
AesGcm& AesGcm::operator=(AesGcm&&) noexcept = default;

Result<AesGcm> AesGcm::create(const Key& key)
{
    const auto& p = providers();
    if (!p.ok)
        return Error::make(ErrorCode::Internal, "provedores criptográficos (CNG) indisponíveis");
    AesGcm g;
    g.d = std::make_unique<Impl>();
    const NTSTATUS st = ::BCryptGenerateSymmetricKey(p.aesGcm, &g.d->key, nullptr, 0, const_cast<PUCHAR>(key.data()),
                                                     static_cast<ULONG>(key.size()), 0);
    if (!NT_SUCCESS(st))
        return cngError("BCryptGenerateSymmetricKey", st);
    return g;
}

Status AesGcm::encrypt(const Nonce& nonce, std::string_view aad, const std::uint8_t* in, std::size_t size, std::uint8_t* out,
                       Tag& tag) const
{
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = const_cast<PUCHAR>(nonce.data());
    info.cbNonce = static_cast<ULONG>(nonce.size());
    info.pbAuthData = reinterpret_cast<PUCHAR>(const_cast<char*>(aad.data()));
    info.cbAuthData = static_cast<ULONG>(aad.size());
    info.pbTag = tag.data();
    info.cbTag = static_cast<ULONG>(tag.size());
    ULONG written = 0;
    const NTSTATUS st = ::BCryptEncrypt(d->key, const_cast<PUCHAR>(in), static_cast<ULONG>(size), &info, nullptr, 0, out,
                                        static_cast<ULONG>(size), &written, 0);
    if (!NT_SUCCESS(st) || written != size)
        return cngError("AES-GCM (cifrar)", st);
    return Status::success();
}

Status AesGcm::decrypt(const Nonce& nonce, std::string_view aad, const std::uint8_t* in, std::size_t size, std::uint8_t* out,
                       const Tag& tag) const
{
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = const_cast<PUCHAR>(nonce.data());
    info.cbNonce = static_cast<ULONG>(nonce.size());
    info.pbAuthData = reinterpret_cast<PUCHAR>(const_cast<char*>(aad.data()));
    info.cbAuthData = static_cast<ULONG>(aad.size());
    info.pbTag = const_cast<PUCHAR>(tag.data());
    info.cbTag = static_cast<ULONG>(tag.size());
    ULONG written = 0;
    const NTSTATUS st = ::BCryptDecrypt(d->key, const_cast<PUCHAR>(in), static_cast<ULONG>(size), &info, nullptr, 0, out,
                                        static_cast<ULONG>(size), &written, 0);
    if (st == STATUS_AUTH_TAG_MISMATCH)
        return Error::make(ErrorCode::Corrupted, "conteúdo cifrado não confere (alterado, truncado ou chave errada)");
    if (!NT_SUCCESS(st) || written != size)
        return cngError("AES-GCM (decifrar)", st);
    return Status::success();
}

void wipe(Key& key)
{
    ::SecureZeroMemory(key.data(), key.size());
}

std::string toBase64(const void* data, std::size_t size)
{
    return QByteArray(static_cast<const char*>(data), static_cast<qsizetype>(size)).toBase64().toStdString();
}

Result<std::string> fromBase64(std::string_view text)
{
    auto r = QByteArray::fromBase64Encoding(QByteArray(text.data(), static_cast<qsizetype>(text.size())),
                                            QByteArray::AbortOnBase64DecodingErrors);
    if (!r)
        return Error::make(ErrorCode::Corrupted, "base64 inválido");
    return r.decoded.toStdString();
}

} // namespace synkro::crypto
