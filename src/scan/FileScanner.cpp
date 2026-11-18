#include "scan/FileScanner.h"

#include "platform/windows/Win32.h"

#include <algorithm>
#include <cstring>

namespace synkro {

namespace {

bool isDotEntry(const wchar_t* name)
{
    return (name[0] == L'.' && name[1] == L'\0') || (name[0] == L'.' && name[1] == L'.' && name[2] == L'\0');
}

// Links simbólicos e junções/pontos de montagem. Outros reparse points (ex.:
// arquivos de nuvem, deduplicação) são arquivos reais e devem ser copiados.
bool isLink(const WIN32_FIND_DATAW& data)
{
    if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0)
        return false;
    return data.dwReserved0 == IO_REPARSE_TAG_SYMLINK || data.dwReserved0 == IO_REPARSE_TAG_MOUNT_POINT;
}

std::uint64_t combine(DWORD high, DWORD low)
{
    return (static_cast<std::uint64_t>(high) << 32) | low;
}

Status readExtendedInfo(const std::wstring& absolutePath, FileEntry& entry)
{
    win::UniqueHandle h(::CreateFileW(win::toExtendedPath(absolutePath).c_str(), FILE_READ_ATTRIBUTES,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                      nullptr));
    if (!h)
        return win::lastError("abrir atributos do arquivo");

    FILE_BASIC_INFO basic{};
    if (!::GetFileInformationByHandleEx(h.get(), FileBasicInfo, &basic, sizeof(basic)))
        return win::lastError("ler FILE_BASIC_INFO");

    FILE_ID_INFO id{};
    if (!::GetFileInformationByHandleEx(h.get(), FileIdInfo, &id, sizeof(id)))
        return win::lastError("ler FILE_ID_INFO");

    entry.changeTime = win::toTicks(basic.ChangeTime);
    entry.volumeSerial = id.VolumeSerialNumber;
    static_assert(sizeof(id.FileId.Identifier) == sizeof(FileId));
    std::memcpy(entry.fileId.data(), id.FileId.Identifier, entry.fileId.size());
    entry.hasExtendedInfo = true;
    return Status::success();
}

} // namespace

Result<ScanResult> scanDirectory(const std::wstring& root, const ScanOptions& options)
{
    if (root.empty())
        return Error::make(ErrorCode::InvalidArgument, "pasta de origem não informada");

    const DWORD rootAttrs = ::GetFileAttributesW(win::toExtendedPath(root).c_str());
    if (rootAttrs == INVALID_FILE_ATTRIBUTES)
        return win::lastError("acessar pasta de origem");
    if ((rootAttrs & FILE_ATTRIBUTE_DIRECTORY) == 0)
        return Error::make(ErrorCode::InvalidArgument, "a origem não é uma pasta");

    ScanResult result;

    // Pilha de diretórios relativos à raiz (forma Windows, '\').
    std::vector<std::wstring> pending;
    pending.emplace_back();

    while (!pending.empty()) {
        const std::wstring relDir = std::move(pending.back());
        pending.pop_back();

        const std::wstring absDir = relDir.empty() ? root : win::joinPath(root, relDir);
        const std::wstring pattern = win::toExtendedPath(win::joinPath(absDir, L"*"));

        WIN32_FIND_DATAW data{};
        HANDLE find = ::FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch,
                                         nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (find == INVALID_HANDLE_VALUE) {
            const DWORD err = ::GetLastError();
            if (err == ERROR_FILE_NOT_FOUND) // diretório vazio
                continue;
            if (relDir.empty())
                return win::errorFromWin32(err, "listar pasta de origem");
            result.issues.push_back({win::toCatalogPath(relDir), win::errorFromWin32(err, "listar pasta")});
            continue;
        }

        ++result.directoryCount;
        do {
            if (isDotEntry(data.cFileName))
                continue;

            const std::wstring relPath = relDir.empty() ? std::wstring(data.cFileName)
                                                        : relDir + L'\\' + data.cFileName;
            const std::wstring catalogPath = win::toCatalogPath(relPath);
            const bool isDir = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

            if (isLink(data)) {
                result.skippedLinks.push_back(catalogPath);
                continue;
            }
            if (options.filter && !options.filter(catalogPath, isDir))
                continue;

            if (isDir) {
                pending.push_back(relPath);
                continue;
            }

            FileEntry entry;
            entry.relativePath = catalogPath;
            entry.size = combine(data.nFileSizeHigh, data.nFileSizeLow);
            entry.lastWriteTime = win::toTicks(data.ftLastWriteTime);
            entry.creationTime = win::toTicks(data.ftCreationTime);
            entry.attributes = data.dwFileAttributes;

            if (options.readFileIds) {
                if (auto st = readExtendedInfo(win::joinPath(root, relPath), entry); !st) {
                    result.issues.push_back({catalogPath, st.error()});
                    continue;
                }
            }

            result.totalBytes += entry.size;
            result.files.push_back(std::move(entry));
        } while (::FindNextFileW(find, &data));

        const DWORD endErr = ::GetLastError();
        ::FindClose(find);
        if (endErr != ERROR_NO_MORE_FILES)
            result.issues.push_back({win::toCatalogPath(relDir), win::errorFromWin32(endErr, "continuar listagem")});
    }

    std::sort(result.files.begin(), result.files.end(),
              [](const FileEntry& a, const FileEntry& b) { return a.relativePath < b.relativePath; });
    std::sort(result.skippedLinks.begin(), result.skippedLinks.end());
    return result;
}

} // namespace synkro
