#include "platform/windows/Win32.h"

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

} // namespace synkro::win
