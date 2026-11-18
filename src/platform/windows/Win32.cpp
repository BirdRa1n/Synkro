#include "platform/windows/Win32.h"

#include <algorithm>
#include <vector>

namespace synkro::win {

std::string toUtf8(std::wstring_view text)
{
    if (text.empty())
        return {};
    const int size = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(size), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                          out.data(), size, nullptr, nullptr);
    return out;
}

std::wstring fromUtf8(std::string_view text)
{
    if (text.empty())
        return {};
    const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0);
    std::wstring out(static_cast<size_t>(size), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

std::string systemMessage(DWORD code)
{
    wchar_t* buffer = nullptr;
    const DWORD len = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    if (len == 0 || buffer == nullptr)
        return "erro do sistema " + std::to_string(code);

    std::wstring msg(buffer, len);
    ::LocalFree(buffer);
    while (!msg.empty() && (msg.back() == L'\r' || msg.back() == L'\n' || msg.back() == L' ' || msg.back() == L'.'))
        msg.pop_back();
    return toUtf8(msg);
}

static ErrorCode classify(DWORD code)
{
    switch (code) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_DRIVE:
    case ERROR_BAD_NETPATH:
        return ErrorCode::NotFound;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
    case ERROR_PRIVILEGE_NOT_HELD:
        return ErrorCode::AccessDenied;
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS:
        return ErrorCode::AlreadyExists;
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL:
        return ErrorCode::DiskFull;
    case ERROR_NOT_READY:
    case ERROR_DEVICE_NOT_CONNECTED:
    case ERROR_DEV_NOT_EXIST:
    case ERROR_DEVICE_REMOVED:
        return ErrorCode::DeviceNotReady;
    case ERROR_INVALID_PARAMETER:
    case ERROR_INVALID_NAME:
        return ErrorCode::InvalidArgument;
    case ERROR_NOT_SUPPORTED:
        return ErrorCode::Unsupported;
    default:
        return ErrorCode::IoError;
    }
}

Error errorFromWin32(DWORD code, std::string_view context)
{
    std::string message(context);
    message += ": ";
    message += systemMessage(code);
    return Error::make(classify(code), std::move(message), code);
}

Error lastError(std::string_view context)
{
    return errorFromWin32(::GetLastError(), context);
}

std::wstring toExtendedPath(std::wstring_view path)
{
    std::wstring p(path);
    for (auto& ch : p) {
        if (ch == L'/')
            ch = L'\\';
    }
    if (p.rfind(L"\\\\?\\", 0) == 0 || p.rfind(L"\\\\.\\", 0) == 0)
        return p;
    if (p.rfind(L"\\\\", 0) == 0)
        return L"\\\\?\\UNC\\" + p.substr(2);
    return L"\\\\?\\" + p;
}

std::wstring joinPath(std::wstring_view base, std::wstring_view relative)
{
    std::wstring out(base);
    while (!out.empty() && (out.back() == L'\\' || out.back() == L'/'))
        out.pop_back();
    size_t start = 0;
    while (start < relative.size() && (relative[start] == L'\\' || relative[start] == L'/'))
        ++start;
    if (start == relative.size())
        return out;
    out += L'\\';
    out.append(relative.substr(start));
    return out;
}

std::wstring toCatalogPath(std::wstring_view relative)
{
    std::wstring out(relative);
    for (auto& ch : out) {
        if (ch == L'\\')
            ch = L'/';
    }
    size_t start = 0;
    while (start < out.size() && out[start] == L'/')
        ++start;
    return out.substr(start);
}

std::wstring fromCatalogPath(std::wstring_view catalogPath)
{
    std::wstring out(catalogPath);
    for (auto& ch : out) {
        if (ch == L'/')
            ch = L'\\';
    }
    return out;
}

std::wstring foldCase(std::wstring_view path)
{
    std::wstring out(path);
    if (!out.empty())
        ::CharUpperBuffW(out.data(), static_cast<DWORD>(out.size()));
    return out;
}

std::wstring parentPath(std::wstring_view path)
{
    std::wstring_view p = path;
    while (!p.empty() && (p.back() == L'\\' || p.back() == L'/'))
        p.remove_suffix(1);
    const size_t pos = p.find_last_of(L"\\/");
    if (pos == std::wstring_view::npos)
        return {};
    return std::wstring(p.substr(0, pos));
}

// Tamanho do prefixo que não deve ser criado: "C:\" ou "\\servidor\share\".
static size_t rootLength(const std::wstring& p)
{
    if (p.size() >= 2 && p[1] == L':')
        return p.size() >= 3 ? 3 : 2;
    if (p.rfind(L"\\\\", 0) == 0) {
        const size_t server = p.find(L'\\', 2);
        if (server == std::wstring::npos)
            return p.size();
        const size_t share = p.find(L'\\', server + 1);
        return share == std::wstring::npos ? p.size() : share + 1;
    }
    return 0;
}

Status createDirectories(std::wstring_view path)
{
    std::wstring p(path);
    for (auto& ch : p) {
        if (ch == L'/')
            ch = L'\\';
    }
    if (p.rfind(L"\\\\?\\UNC\\", 0) == 0)
        p = L"\\\\" + p.substr(8);
    else if (p.rfind(L"\\\\?\\", 0) == 0)
        p = p.substr(4);
    while (!p.empty() && p.back() == L'\\')
        p.pop_back();
    if (p.empty())
        return Error::make(ErrorCode::InvalidArgument, "caminho de diretório vazio");

    size_t pos = rootLength(p);
    while (pos < p.size()) {
        size_t next = p.find(L'\\', pos);
        if (next == std::wstring::npos)
            next = p.size();
        const std::wstring partial = p.substr(0, next);
        if (!::CreateDirectoryW(toExtendedPath(partial).c_str(), nullptr)) {
            const DWORD err = ::GetLastError();
            if (err != ERROR_ALREADY_EXISTS)
                return errorFromWin32(err, "criar diretório");
            const DWORD attrs = ::GetFileAttributesW(toExtendedPath(partial).c_str());
            if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0)
                return Error::make(ErrorCode::AlreadyExists, "já existe um arquivo com o nome do diretório");
        }
        pos = next + 1;
    }
    return Status::success();
}

Status writeFileAtomically(std::wstring_view path, std::string_view data)
{
    const std::wstring target(path);
    const std::wstring temp = target + L".tmp-" + std::to_wstring(::GetCurrentProcessId()) + L"-"
                              + std::to_wstring(::GetTickCount64());

    {
        UniqueHandle h(::CreateFileW(toExtendedPath(temp).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                     FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!h)
            return lastError("criar arquivo temporário");

        size_t offset = 0;
        while (offset < data.size()) {
            const DWORD chunk = static_cast<DWORD>(std::min<size_t>(data.size() - offset, 1u << 20));
            DWORD written = 0;
            if (!::WriteFile(h.get(), data.data() + offset, chunk, &written, nullptr) || written == 0) {
                Error e = lastError("gravar arquivo temporário");
                h.reset();
                ::DeleteFileW(toExtendedPath(temp).c_str());
                return e;
            }
            offset += written;
        }
        if (!::FlushFileBuffers(h.get())) {
            Error e = lastError("sincronizar arquivo temporário");
            h.reset();
            ::DeleteFileW(toExtendedPath(temp).c_str());
            return e;
        }
    }

    if (!::MoveFileExW(toExtendedPath(temp).c_str(), toExtendedPath(target).c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        Error e = lastError("substituir arquivo");
        ::DeleteFileW(toExtendedPath(temp).c_str());
        return e;
    }
    return Status::success();
}

Result<std::string> readWholeFile(std::wstring_view path, std::size_t maxBytes)
{
    UniqueHandle h(::CreateFileW(toExtendedPath(path).c_str(), GENERIC_READ,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                 FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!h)
        return lastError("abrir arquivo");

    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(h.get(), &size))
        return lastError("obter tamanho do arquivo");
    if (static_cast<unsigned long long>(size.QuadPart) > maxBytes)
        return Error::make(ErrorCode::InvalidArgument, "arquivo maior que o limite permitido");

    std::string out(static_cast<size_t>(size.QuadPart), '\0');
    size_t offset = 0;
    while (offset < out.size()) {
        DWORD read = 0;
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(out.size() - offset, 1u << 20));
        if (!::ReadFile(h.get(), out.data() + offset, chunk, &read, nullptr))
            return lastError("ler arquivo");
        if (read == 0)
            break;
        offset += read;
    }
    out.resize(offset);
    return out;
}

} // namespace synkro::win
