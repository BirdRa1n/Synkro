#include "database/Zstd.h"

#include "platform/windows/Win32.h"

#include <zstd.h>

#include <vector>

namespace synkro::db {

namespace {

Status writeAll(HANDLE h, const void* data, std::size_t size)
{
    const auto* p = static_cast<const char*>(data);
    while (size > 0) {
        DWORD written = 0;
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(size, 1u << 20));
        if (!::WriteFile(h, p, chunk, &written, nullptr) || written == 0)
            return win::lastError("gravar arquivo comprimido");
        p += written;
        size -= written;
    }
    return Status::success();
}

} // namespace

struct ZstdFileWriter::Impl {
    win::UniqueHandle file;
    ZSTD_CCtx* ctx = nullptr;
    std::vector<char> out;
    Sha256 hasher;
    Sha256Digest digest{};
    std::uint64_t in = 0, written = 0;
    bool finished = false;

    ~Impl()
    {
        if (ctx)
            ZSTD_freeCCtx(ctx);
    }

    Status emit(const char* data, std::size_t size)
    {
        if (size == 0)
            return Status::success();
        if (auto st = hasher.update(data, size); !st)
            return st;
        written += size;
        return writeAll(file.get(), data, size);
    }

    Status compress(const void* data, std::size_t size, ZSTD_EndDirective mode)
    {
        ZSTD_inBuffer input{data, size, 0};
        for (;;) {
            ZSTD_outBuffer output{out.data(), out.size(), 0};
            const std::size_t remaining = ZSTD_compressStream2(ctx, &output, &input, mode);
            if (ZSTD_isError(remaining))
                return Error::make(ErrorCode::Internal, std::string("zstd: ") + ZSTD_getErrorName(remaining));
            if (auto st = emit(out.data(), output.pos); !st)
                return st;
            const bool done = mode == ZSTD_e_end ? remaining == 0 : input.pos == input.size;
            if (done)
                return Status::success();
        }
    }
};

ZstdFileWriter::ZstdFileWriter() : d(std::make_unique<Impl>()) {}
ZstdFileWriter::ZstdFileWriter(ZstdFileWriter&&) noexcept = default;
ZstdFileWriter& ZstdFileWriter::operator=(ZstdFileWriter&&) noexcept = default;
ZstdFileWriter::~ZstdFileWriter() = default;

Result<ZstdFileWriter> ZstdFileWriter::create(const std::wstring& path, int level)
{
    ZstdFileWriter w;
    w.d->file.reset(::CreateFileW(win::toExtendedPath(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!w.d->file)
        return win::lastError("criar arquivo comprimido");
    w.d->ctx = ZSTD_createCCtx();
    if (!w.d->ctx)
        return Error::make(ErrorCode::Internal, "zstd: sem memória");
    ZSTD_CCtx_setParameter(w.d->ctx, ZSTD_c_compressionLevel, level);
    ZSTD_CCtx_setParameter(w.d->ctx, ZSTD_c_checksumFlag, 1); // o próprio .zst detecta corrupção
    w.d->out.resize(ZSTD_CStreamOutSize());
    return w;
}

Status ZstdFileWriter::write(const void* data, std::size_t size)
{
    if (d->finished)
        return Error::make(ErrorCode::Internal, "escrita após finish()");
    d->in += size;
    return d->compress(data, size, ZSTD_e_continue);
}

Status ZstdFileWriter::finish()
{
    if (d->finished)
        return Status::success();
    if (auto st = d->compress(nullptr, 0, ZSTD_e_end); !st)
        return st;
    if (!::FlushFileBuffers(d->file.get()))
        return win::lastError("sincronizar arquivo comprimido");
    d->file.reset();
    auto digest = d->hasher.finish();
    if (!digest)
        return digest.error();
    d->digest = digest.value();
    d->finished = true;
    return Status::success();
}

std::uint64_t ZstdFileWriter::inputBytes() const
{
    return d->in;
}

std::uint64_t ZstdFileWriter::outputBytes() const
{
    return d->written;
}

Sha256Digest ZstdFileWriter::sha256() const
{
    return d->digest;
}

Status compressFile(const std::wstring& source, const std::wstring& destination, int level)
{
    win::UniqueHandle src(::CreateFileW(win::toExtendedPath(source).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                        OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!src)
        return win::lastError("abrir arquivo para compressão");
    auto writer = ZstdFileWriter::create(destination, level);
    if (!writer)
        return writer.error();
    std::vector<char> buf(1u << 20);
    for (;;) {
        DWORD n = 0;
        if (!::ReadFile(src.get(), buf.data(), static_cast<DWORD>(buf.size()), &n, nullptr))
            return win::lastError("ler arquivo para compressão");
        if (n == 0)
            break;
        if (auto st = writer.value().write(buf.data(), n); !st)
            return st;
    }
    return writer.value().finish();
}

Status decompressFile(const std::wstring& source, const std::wstring& destination)
{
    win::UniqueHandle src(::CreateFileW(win::toExtendedPath(source).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                        OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!src)
        return win::lastError("abrir arquivo comprimido");
    win::UniqueHandle dst(::CreateFileW(win::toExtendedPath(destination).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                        FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!dst)
        return win::lastError("criar arquivo descomprimido");

    ZSTD_DCtx* ctx = ZSTD_createDCtx();
    if (!ctx)
        return Error::make(ErrorCode::Internal, "zstd: sem memória");
    std::vector<char> in(ZSTD_DStreamInSize()), out(ZSTD_DStreamOutSize());
    Status result;
    std::size_t lastRet = 0;
    for (;;) {
        DWORD n = 0;
        if (!::ReadFile(src.get(), in.data(), static_cast<DWORD>(in.size()), &n, nullptr)) {
            result = win::lastError("ler arquivo comprimido");
            break;
        }
        if (n == 0)
            break;
        ZSTD_inBuffer input{in.data(), n, 0};
        while (input.pos < input.size) {
            ZSTD_outBuffer output{out.data(), out.size(), 0};
            lastRet = ZSTD_decompressStream(ctx, &output, &input);
            if (ZSTD_isError(lastRet)) {
                result = Error::make(ErrorCode::Corrupted, std::string("arquivo .zst inválido: ") + ZSTD_getErrorName(lastRet));
                break;
            }
            if (auto st = writeAll(dst.get(), out.data(), output.pos); !st) {
                result = st;
                break;
            }
        }
        if (!result.ok())
            break;
    }
    ZSTD_freeDCtx(ctx);
    if (result.ok() && lastRet != 0)
        result = Error::make(ErrorCode::Corrupted, "arquivo .zst truncado");
    if (result.ok() && !::FlushFileBuffers(dst.get()))
        result = win::lastError("sincronizar arquivo descomprimido");
    if (!result.ok()) {
        dst.reset();
        ::DeleteFileW(win::toExtendedPath(destination).c_str());
    }
    return result;
}

} // namespace synkro::db
