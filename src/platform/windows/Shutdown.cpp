#include "platform/windows/Shutdown.h"

#include "platform/windows/Win32.h"

#include <reason.h>

namespace synkro::win {

namespace {

Status enableShutdownPrivilege()
{
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
        return lastError("abrir token do processo");
    UniqueHandle t(token);
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (!::LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &tp.Privileges[0].Luid))
        return lastError("localizar privilégio de desligamento");
    if (!::AdjustTokenPrivileges(token, FALSE, &tp, 0, nullptr, nullptr) || ::GetLastError() == ERROR_NOT_ALL_ASSIGNED)
        return Error::make(ErrorCode::AccessDenied, "o processo não tem permissão para desligar o computador");
    return Status::success();
}

} // namespace

bool shutdownDisabledByEnvironment()
{
    wchar_t buf[8] = {};
    return ::GetEnvironmentVariableW(L"SYNKRO_NO_SHUTDOWN", buf, 8) > 0 && buf[0] == L'1';
}

Status scheduleShutdown(unsigned long countdownSeconds, const std::wstring& message)
{
    if (shutdownDisabledByEnvironment())
        return Error::make(ErrorCode::Unsupported, "desligamento desativado por SYNKRO_NO_SHUTDOWN=1");
    if (countdownSeconds < 30)
        countdownSeconds = 30; // sempre há tempo para cancelar
    if (auto st = enableShutdownPrivilege(); !st)
        return st;
    std::wstring msg = message;
    const DWORD rc = ::InitiateShutdownW(nullptr, msg.data(), countdownSeconds, SHUTDOWN_POWEROFF,
                                         SHTDN_REASON_MAJOR_APPLICATION | SHTDN_REASON_MINOR_MAINTENANCE
                                             | SHTDN_REASON_FLAG_PLANNED);
    if (rc != ERROR_SUCCESS)
        return errorFromWin32(rc, "agendar desligamento");
    return Status::success();
}

Status cancelShutdown()
{
    if (auto st = enableShutdownPrivilege(); !st)
        return st;
    if (!::AbortSystemShutdownW(nullptr))
        return lastError("cancelar desligamento");
    return Status::success();
}

} // namespace synkro::win
