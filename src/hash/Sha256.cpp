#include "hash/Sha256.h"

#include "platform/windows/AlignedBuffer.h"
#include "platform/windows/Win32.h"

#include <bcrypt.h>

#ifndef NT_SUCCESS
#define NT_SUCCESS(status) (static_cast<NTSTATUS>(status) >= 0)
#endif

namespace synkro {

std::string toHex(const Sha256Digest& digest)
{
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(digest.size() * 2);
    for (std::uint8_t byte : digest) {
        out.push_back(kHex[byte >> 4]);
        out.push_back(kHex[byte & 0x0F]);
    }
    return out;
}

struct Sha256::Impl {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;

    ~Impl()
    {
        if (hash)
            ::BCryptDestroyHash(hash);
        if (alg)
            ::BCryptCloseAlgorithmProvider(alg, 0);
    }

    bool open()
    {
        if (!NT_SUCCESS(::BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0)))
            return false;
        // BCRYPT_HASH_REUSABLE_FLAG permite reutilizar o objeto após FinishHash.
        if (!NT_SUCCESS(::BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, BCRYPT_HASH_REUSABLE_FLAG)))
            return false;
        return true;
    }
};

Sha256::Sha256() : d(std::make_unique<Impl>())
{
    if (!d->open())
        d.reset();
}

Sha256::~Sha256() = default;
Sha256::Sha256(Sha256&&) noexcept = default;
Sha256& Sha256::operator=(Sha256&&) noexcept = default;

bool Sha256::valid() const noexcept
{
    return d != nullptr;
}

Status Sha256::update(const void* data, std::size_t size)
{
    if (!d)
        return Error::make(ErrorCode::Internal, "provedor SHA-256 (CNG) indisponível");

    auto* bytes = static_cast<PUCHAR>(const_cast<void*>(data));
    while (size > 0) {
        const ULONG chunk = size > 0x40000000u ? 0x40000000u : static_cast<ULONG>(size);
        const NTSTATUS st = ::BCryptHashData(d->hash, bytes, chunk, 0);
        if (!NT_SUCCESS(st))
            return Error::make(ErrorCode::Internal, "BCryptHashData falhou", static_cast<unsigned long>(st));
        bytes += chunk;
        size -= chunk;
    }
    return Status::success();
}

Result<Sha256Digest> Sha256::finish()
{
    if (!d)
        return Error::make(ErrorCode::Internal, "provedor SHA-256 (CNG) indisponível");

    Sha256Digest digest{};
    const NTSTATUS st = ::BCryptFinishHash(d->hash, digest.data(), static_cast<ULONG>(digest.size()), 0);
    if (!NT_SUCCESS(st))
        return Error::make(ErrorCode::Internal, "BCryptFinishHash falhou", static_cast<unsigned long>(st));
    return digest;
}

Result<Sha256Digest> Sha256::ofBytes(std::string_view data)
{
    Sha256 h;
    if (auto st = h.update(data); !st)
        return st.error();
    return h.finish();
}

Result<Sha256Digest> Sha256::ofFile(const std::wstring& path, bool bypassCache)
{
    DWORD flags = FILE_FLAG_SEQUENTIAL_SCAN;
    if (bypassCache)
        flags |= FILE_FLAG_NO_BUFFERING;

    win::UniqueHandle file(::CreateFileW(win::toExtendedPath(path).c_str(), GENERIC_READ,
                                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                         nullptr, OPEN_EXISTING, flags, nullptr));
    if (!file)
        return win::lastError("abrir arquivo para hash");

    // Com FILE_FLAG_NO_BUFFERING o buffer e o tamanho de leitura precisam ser
    // alinhados ao setor; 1 MiB alinhado a 4 KiB atende a todos os discos atuais.
    win::AlignedBuffer buffer(1u << 20);
    if (!buffer)
        return Error::make(ErrorCode::Internal, "sem memória para buffer de leitura");

    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(file.get(), &size))
        return win::lastError("ler o tamanho do arquivo para hash");

    // Para no tamanho do arquivo em vez de ler até receber 0: depois de uma
    // leitura curta o ponteiro fica desalinhado, e o redirecionador SMB recusa
    // a leitura seguinte sem cache ("parâmetro incorreto"); o NTFS local aceita.
    Sha256 h;
    std::uint64_t total = 0;
    while (total < static_cast<std::uint64_t>(size.QuadPart)) {
        DWORD read = 0;
        if (!::ReadFile(file.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr))
            return win::lastError("ler arquivo para hash");
        if (read == 0)
            break; // encolheu durante a leitura: o hash não vai bater e a cópia é refeita
        if (auto st = h.update(buffer.data(), read); !st)
            return st.error();
        total += read;
        if (read < buffer.size())
            break; // leitura curta: fim do arquivo
    }
    return h.finish();
}

} // namespace synkro
