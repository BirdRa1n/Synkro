#pragma once

// Utilitários finos sobre a API Win32 usados pelo motor: conversão de texto,
// mapeamento de erros, caminhos longos e handles RAII.

#include "core/Result.h"

#include <windows.h>

#include <string>
#include <string_view>

namespace synkro::win {

// ---- Texto -------------------------------------------------------------------

std::string toUtf8(std::wstring_view text);
std::wstring fromUtf8(std::string_view text);

// ---- Erros -------------------------------------------------------------------

// Mensagem do sistema para um código Win32, em UTF-8 e sem quebra de linha final.
std::string systemMessage(DWORD code);

// Converte um código Win32 em Error com contexto ("abrir origem: <msg>").
Error errorFromWin32(DWORD code, std::string_view context);

// Atalho para errorFromWin32(GetLastError(), context).
Error lastError(std::string_view context);

// ---- Caminhos ----------------------------------------------------------------

// Converte um caminho absoluto para a forma estendida (\\?\C:\... ou
// \\?\UNC\servidor\...), que remove o limite de 260 caracteres. Barras '/'
// viram '\'. Caminhos já estendidos são devolvidos como estão.
std::wstring toExtendedPath(std::wstring_view path);

// Junta dois segmentos com um único '\'.
std::wstring joinPath(std::wstring_view base, std::wstring_view relative);

// Caminho relativo no formato canônico do catálogo: separador '/', sem '/'
// inicial. Ex.: "NFe\2026\a.xml" -> "NFe/2026/a.xml".
std::wstring toCatalogPath(std::wstring_view relative);

// Inverso de toCatalogPath: '/' -> '\'.
std::wstring fromCatalogPath(std::wstring_view catalogPath);

// Chave para comparação de caminhos sem diferenciar maiúsculas/minúsculas,
// equivalente à regra do NTFS (maiúsculas ordinais).
std::wstring foldCase(std::wstring_view path);

// ---- Handle RAII -------------------------------------------------------------

class UniqueHandle {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE h) noexcept : m_handle(h) {}
    ~UniqueHandle() { reset(); }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept : m_handle(other.release()) {}
    UniqueHandle& operator=(UniqueHandle&& other) noexcept
    {
        if (this != &other)
            reset(other.release());
        return *this;
    }

    HANDLE get() const noexcept { return m_handle; }
    bool valid() const noexcept { return m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE; }
    explicit operator bool() const noexcept { return valid(); }

    HANDLE release() noexcept
    {
        HANDLE h = m_handle;
        m_handle = INVALID_HANDLE_VALUE;
        return h;
    }

    void reset(HANDLE h = INVALID_HANDLE_VALUE) noexcept
    {
        if (valid())
            ::CloseHandle(m_handle);
        m_handle = h;
    }

private:
    HANDLE m_handle = INVALID_HANDLE_VALUE;
};

// Converte FILETIME em inteiro de 100 ns desde 1601-01-01 UTC.
inline unsigned long long toTicks(const FILETIME& ft) noexcept
{
    return (static_cast<unsigned long long>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

inline unsigned long long toTicks(const LARGE_INTEGER& li) noexcept
{
    return static_cast<unsigned long long>(li.QuadPart);
}

} // namespace synkro::win
