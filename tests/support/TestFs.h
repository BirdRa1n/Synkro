#pragma once

// Auxiliares de sistema de arquivos para os testes (usam a API Win32 com
// caminhos estendidos, para poder criar caminhos acima de 260 caracteres).

#include "platform/windows/Win32.h"

#include <QByteArray>
#include <QDir>
#include <QString>

#include <string>

namespace synkro::test {

inline std::wstring nativePath(const QString& path)
{
    return QDir::toNativeSeparators(path).toStdWString();
}

inline bool makeDirs(const std::wstring& path)
{
    std::wstring partial;
    size_t pos = 0;
    // Pula a unidade ("C:\").
    if (path.size() >= 3 && path[1] == L':')
        pos = 3;
    partial = path.substr(0, pos);
    while (pos <= path.size()) {
        const size_t next = path.find(L'\\', pos);
        const size_t end = next == std::wstring::npos ? path.size() : next;
        partial = path.substr(0, end);
        if (!partial.empty() && partial.back() != L':') {
            if (!::CreateDirectoryW(win::toExtendedPath(partial).c_str(), nullptr)
                && ::GetLastError() != ERROR_ALREADY_EXISTS)
                return false;
        }
        if (next == std::wstring::npos)
            break;
        pos = next + 1;
    }
    return true;
}

inline bool writeFile(const std::wstring& path, const QByteArray& content)
{
    const size_t slash = path.find_last_of(L'\\');
    if (slash != std::wstring::npos && !makeDirs(path.substr(0, slash)))
        return false;

    win::UniqueHandle h(::CreateFileW(win::toExtendedPath(path).c_str(), GENERIC_WRITE, 0, nullptr,
                                      CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!h)
        return false;
    DWORD written = 0;
    if (!content.isEmpty()
        && !::WriteFile(h.get(), content.constData(), static_cast<DWORD>(content.size()), &written, nullptr))
        return false;
    return written == static_cast<DWORD>(content.size());
}

inline QByteArray readFile(const std::wstring& path)
{
    win::UniqueHandle h(::CreateFileW(win::toExtendedPath(path).c_str(), GENERIC_READ, FILE_SHARE_READ,
                                      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!h)
        return {};
    LARGE_INTEGER size{};
    ::GetFileSizeEx(h.get(), &size);
    QByteArray out(static_cast<qsizetype>(size.QuadPart), Qt::Uninitialized);
    DWORD read = 0;
    if (!out.isEmpty())
        ::ReadFile(h.get(), out.data(), static_cast<DWORD>(out.size()), &read, nullptr);
    return out;
}

inline bool exists(const std::wstring& path)
{
    return ::GetFileAttributesW(win::toExtendedPath(path).c_str()) != INVALID_FILE_ATTRIBUTES;
}

} // namespace synkro::test
