#include "core/Paths.h"

#include "platform/windows/Win32.h"

namespace synkro {

std::wstring dataDirectory()
{
    wchar_t buf[MAX_PATH] = {};
    if (::GetEnvironmentVariableW(L"SYNKRO_DATA_DIR", buf, MAX_PATH) > 0)
        return buf;
    if (::GetEnvironmentVariableW(L"ProgramData", buf, MAX_PATH) > 0)
        return win::joinPath(buf, L"Synkro");
    return L"C:\\ProgramData\\Synkro";
}

std::wstring stagingDirectory()
{
    return win::joinPath(dataDirectory(), L"staging");
}

std::wstring secretsFile()
{
    return win::joinPath(dataDirectory(), L"secrets.dat");
}

std::wstring stateDatabaseFile()
{
    // Pasta própria: o desktop roda como usuário comum e precisa gravar no
    // estado (o WAL do SQLite cria arquivos ao lado do banco), sem ganhar
    // escrita na configuração nem nos segredos.
    return win::joinPath(win::joinPath(dataDirectory(), L"state"), L"synkro.db");
}

} // namespace synkro
