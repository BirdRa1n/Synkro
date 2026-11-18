#include "transfer/SafeCopy.h"

#include "platform/windows/AlignedBuffer.h"
#include "platform/windows/Win32.h"

#include <QUuid>

namespace synkro {

namespace {

constexpr DWORD kPreservedAttributes =
    FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_ARCHIVE;

struct SourceState {
    std::uint64_t size = 0;
    FILE_BASIC_INFO basic{};
};

Result<SourceState> querySource(HANDLE h)
{
    SourceState s;
    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(h, &size))
        return win::lastError("obter tamanho da origem");
    if (!::GetFileInformationByHandleEx(h, FileBasicInfo, &s.basic, sizeof(s.basic)))
        return win::lastError("ler atributos da origem");
    s.size = static_cast<std::uint64_t>(size.QuadPart);
    return s;
}

// Remove o temporário ao sair do escopo, a menos que tenha sido consumido.
class TempFileGuard {
public:
    explicit TempFileGuard(std::wstring path) : m_path(std::move(path)) {}
    ~TempFileGuard()
    {
        if (!m_path.empty())
            ::DeleteFileW(win::toExtendedPath(m_path).c_str());
    }
    void release() { m_path.clear(); }

private:
    std::wstring m_path;
};

Status writeAll(HANDLE h, const void* data, DWORD size)
{
    const auto* bytes = static_cast<const char*>(data);
    DWORD offset = 0;
    while (offset < size) {
        DWORD written = 0;
        if (!::WriteFile(h, bytes + offset, size - offset, &written, nullptr))
            return win::lastError("gravar no destino");
        if (written == 0)
            return Error::make(ErrorCode::IoError, "gravação no destino não avançou");
        offset += written;
    }
    return Status::success();
}

} // namespace

Result<CopyOutcome> copyFileVerified(const std::wstring& source,
                                     const std::wstring& destination,
                                     const CopyOptions& options)
{
    if (source.empty() || destination.empty())
        return Error::make(ErrorCode::InvalidArgument, "origem e destino são obrigatórios");

    // FILE_FLAG_BACKUP_SEMANTICS permite ler arquivos protegidos quando o
    // worker roda como SYSTEM com SeBackupPrivilege.
    win::UniqueHandle src(::CreateFileW(win::toExtendedPath(source).c_str(), GENERIC_READ,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                        OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_BACKUP_SEMANTICS,
                                        nullptr));
    if (!src)
        return win::lastError("abrir origem");

    auto before = querySource(src.get());
    if (!before)
        return before.error();
    if (before.value().basic.FileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        return Error::make(ErrorCode::InvalidArgument, "a origem é um diretório");

    const std::wstring destDir = win::parentPath(destination);
    if (!destDir.empty()) {
        if (auto st = win::createDirectories(destDir); !st)
            return st.error();
    }
    const std::wstring tempDir = options.tempDirectory.empty() ? destDir : options.tempDirectory;
    if (!options.tempDirectory.empty()) {
        if (auto st = win::createDirectories(tempDir); !st)
            return st.error();
    }

    const std::wstring tempPath = win::joinPath(
        tempDir, L"synkro-" + QUuid::createUuid().toString(QUuid::WithoutBraces).toStdWString() + L".part");
    TempFileGuard guard(tempPath);

    CopyOutcome outcome;
    outcome.sourceLastWriteTime = win::toTicks(before.value().basic.LastWriteTime);

    {
        win::UniqueHandle tmp(::CreateFileW(win::toExtendedPath(tempPath).c_str(), GENERIC_WRITE, 0, nullptr,
                                            CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!tmp)
            return win::lastError("criar arquivo temporário no destino");

        win::AlignedBuffer buffer(options.bufferSize);
        if (!buffer)
            return Error::make(ErrorCode::Internal, "sem memória para buffer de cópia");

        Sha256 hasher;
        if (!hasher.valid())
            return Error::make(ErrorCode::Internal, "provedor SHA-256 indisponível");

        for (;;) {
            DWORD read = 0;
            if (!::ReadFile(src.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr))
                return win::lastError("ler origem");
            if (read == 0)
                break;
            if (auto st = hasher.update(buffer.data(), read); !st)
                return st.error();
            if (auto st = writeAll(tmp.get(), buffer.data(), read); !st)
                return st.error();
            outcome.bytes += read;
        }

        auto digest = hasher.finish();
        if (!digest)
            return digest.error();
        outcome.sha256 = digest.value();

        // A origem não pode ter mudado enquanto era lida.
        auto after = querySource(src.get());
        if (!after)
            return after.error();
        if (after.value().size != before.value().size
            || after.value().basic.LastWriteTime.QuadPart != before.value().basic.LastWriteTime.QuadPart
            || outcome.bytes != before.value().size) {
            return Error::make(ErrorCode::SourceChangedDuringCopy,
                               "a origem foi modificada durante a cópia; a cópia foi descartada");
        }

        if (options.preserveTimestamps) {
            FILETIME created{}, written{};
            created.dwLowDateTime = before.value().basic.CreationTime.LowPart;
            created.dwHighDateTime = static_cast<DWORD>(before.value().basic.CreationTime.HighPart);
            written.dwLowDateTime = before.value().basic.LastWriteTime.LowPart;
            written.dwHighDateTime = static_cast<DWORD>(before.value().basic.LastWriteTime.HighPart);
            if (!::SetFileTime(tmp.get(), &created, nullptr, &written))
                return win::lastError("preservar datas do arquivo");
        }

        if (!::FlushFileBuffers(tmp.get()))
            return win::lastError("sincronizar arquivo no disco");
    }

    // Verificação do temporário antes de torná-lo visível.
    if (options.verify == VerifyMode::RereadUncached) {
        auto reread = Sha256::ofFile(tempPath, /*bypassCache=*/true);
        if (!reread)
            return reread.error();
        if (reread.value() != outcome.sha256)
            return Error::make(ErrorCode::VerificationFailed,
                               "o conteúdo gravado difere da origem (hash SHA-256 divergente)");
    } else {
        WIN32_FILE_ATTRIBUTE_DATA info{};
        if (!::GetFileAttributesExW(win::toExtendedPath(tempPath).c_str(), GetFileExInfoStandard, &info))
            return win::lastError("conferir arquivo gravado");
        const std::uint64_t written = (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
        if (written != outcome.bytes)
            return Error::make(ErrorCode::VerificationFailed, "tamanho gravado difere do lido da origem");
    }

    if (options.beforeCommit) {
        if (auto st = options.beforeCommit(); !st)
            return st.error();
    }

    // Um destino somente leitura impediria a substituição.
    const std::wstring extDest = win::toExtendedPath(destination);
    const DWORD destAttrs = ::GetFileAttributesW(extDest.c_str());
    if (destAttrs != INVALID_FILE_ATTRIBUTES && (destAttrs & FILE_ATTRIBUTE_READONLY))
        ::SetFileAttributesW(extDest.c_str(), destAttrs & ~static_cast<DWORD>(FILE_ATTRIBUTE_READONLY));

    if (!::MoveFileExW(win::toExtendedPath(tempPath).c_str(), extDest.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return win::lastError("substituir arquivo de destino");
    guard.release();

    if (options.preserveAttributes) {
        const DWORD attrs = before.value().basic.FileAttributes & kPreservedAttributes;
        ::SetFileAttributesW(extDest.c_str(), attrs == 0 ? FILE_ATTRIBUTE_NORMAL : attrs);
    }

    return outcome;
}

} // namespace synkro
