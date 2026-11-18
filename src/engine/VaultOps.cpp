#include "engine/VaultOps.h"

#include "platform/windows/Win32.h"

#include <algorithm>

namespace synkro::ops {

bool pathExists(const std::wstring& path)
{
    return ::GetFileAttributesW(win::toExtendedPath(path).c_str()) != INVALID_FILE_ATTRIBUTES;
}

static Status moveFile(const std::wstring& from, const std::wstring& to, const char* context)
{
    const std::wstring parent = win::parentPath(to);
    if (!parent.empty()) {
        if (auto st = win::createDirectories(parent); !st)
            return st;
    }
    // Um arquivo somente leitura pode ser renomeado normalmente; não é preciso
    // alterar atributos. MOVEFILE_WRITE_THROUGH garante a operação em disco.
    if (!::MoveFileExW(win::toExtendedPath(from).c_str(), win::toExtendedPath(to).c_str(), MOVEFILE_WRITE_THROUGH))
        return win::lastError(context);
    return Status::success();
}

Status moveCurrentToVersions(const vault::JobLayout& layout, const std::wstring& runId,
                             const std::wstring& catalogPath)
{
    return moveFile(layout.currentFile(catalogPath), layout.versionFile(runId, catalogPath),
                    "guardar versão anterior");
}

Status moveVersionBackToCurrent(const vault::JobLayout& layout, const std::wstring& runId,
                                const std::wstring& catalogPath)
{
    return moveFile(layout.versionFile(runId, catalogPath), layout.currentFile(catalogPath),
                    "restaurar versão anterior em current");
}

namespace {

template <typename Fn>
void forEachEntry(const std::wstring& dir, Fn&& fn)
{
    WIN32_FIND_DATAW fd{};
    HANDLE h = ::FindFirstFileExW(win::toExtendedPath(win::joinPath(dir, L"*")).c_str(), FindExInfoBasic, &fd,
                                  FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..")
            continue;
        fn(name, fd);
    } while (::FindNextFileW(h, &fd));
    ::FindClose(h);
}

void clearReadOnly(const std::wstring& path, DWORD attrs)
{
    if (attrs & FILE_ATTRIBUTE_READONLY)
        ::SetFileAttributesW(win::toExtendedPath(path).c_str(), attrs & ~static_cast<DWORD>(FILE_ATTRIBUTE_READONLY));
}

} // namespace

Status cleanTempDirectory(const vault::JobLayout& layout)
{
    const std::wstring tmp = layout.tmp();
    if (!pathExists(tmp))
        return Status::success();
    Status result;
    forEachEntry(tmp, [&](const std::wstring& name, const WIN32_FIND_DATAW& fd) {
        const std::wstring full = win::joinPath(tmp, name);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (auto st = removeTree(full); !st && result.ok())
                result = st;
            return;
        }
        clearReadOnly(full, fd.dwFileAttributes);
        if (!::DeleteFileW(win::toExtendedPath(full).c_str()) && result.ok())
            result = win::lastError("limpar arquivo temporário");
    });
    return result;
}

Status removeTree(const std::wstring& path, std::uint64_t* bytesFreed)
{
    Status result;
    forEachEntry(path, [&](const std::wstring& name, const WIN32_FIND_DATAW& fd) {
        const std::wstring full = win::joinPath(path, name);
        const bool isLink = (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !isLink) {
            if (auto st = removeTree(full, bytesFreed); !st && result.ok())
                result = st;
            return;
        }
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            ::RemoveDirectoryW(win::toExtendedPath(full).c_str()); // junção: remove só o link
            return;
        }
        clearReadOnly(full, fd.dwFileAttributes);
        if (::DeleteFileW(win::toExtendedPath(full).c_str())) {
            if (bytesFreed)
                *bytesFreed += (static_cast<std::uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        } else if (result.ok()) {
            result = win::lastError("remover arquivo");
        }
    });
    if (!::RemoveDirectoryW(win::toExtendedPath(path).c_str()) && result.ok()) {
        const DWORD err = ::GetLastError();
        if (err != ERROR_FILE_NOT_FOUND && err != ERROR_PATH_NOT_FOUND)
            result = win::errorFromWin32(err, "remover diretório");
    }
    return result;
}

void removeEmptyDirectories(const std::wstring& root, bool includeRoot)
{
    forEachEntry(root, [&](const std::wstring& name, const WIN32_FIND_DATAW& fd) {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
            removeEmptyDirectories(win::joinPath(root, name), true);
    });
    if (includeRoot)
        ::RemoveDirectoryW(win::toExtendedPath(root).c_str()); // falha silenciosa se não estiver vazio
}

std::vector<std::wstring> listSubdirectories(const std::wstring& path)
{
    std::vector<std::wstring> out;
    forEachEntry(path, [&](const std::wstring& name, const WIN32_FIND_DATAW& fd) {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            out.push_back(name);
    });
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::wstring> listFiles(const std::wstring& path, const std::wstring& suffix)
{
    std::vector<std::wstring> out;
    forEachEntry(path, [&](const std::wstring& name, const WIN32_FIND_DATAW& fd) {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            return;
        if (name.size() >= suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0)
            out.push_back(name);
    });
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace synkro::ops
