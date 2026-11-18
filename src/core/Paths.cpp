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
    return win::joinPath(dataDirectory(), L"synkro.db");
}

} // namespace synkro
