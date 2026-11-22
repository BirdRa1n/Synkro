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

Status scheduleShutdown(unsigned long countdownSeconds, const std::wstring& message, bool forceApps)
{
    if (shutdownDisabledByEnvironment())
        return Error::make(ErrorCode::Unsupported, "desligamento desativado por SYNKRO_NO_SHUTDOWN=1");
    if (countdownSeconds < 30)
        countdownSeconds = 30; // sempre há tempo para cancelar
    if (auto st = enableShutdownPrivilege(); !st)
        return st;
    std::wstring msg = message;
    // Sem SHUTDOWN_FORCE_OTHERS, um programa aberto com algo por salvar (ou que
    // recuse fechar) segura o desligamento na tela "programas impedindo o
    // desligamento" — num PDV sem ninguém, a noite toda.
    DWORD flags = SHUTDOWN_POWEROFF;
    if (forceApps)
        flags |= SHUTDOWN_FORCE_OTHERS;
    const DWORD rc = ::InitiateShutdownW(nullptr, msg.data(), countdownSeconds, flags,
                                         SHTDN_REASON_MAJOR_APPLICATION | SHTDN_REASON_MINOR_MAINTENANCE
                                             | SHTDN_REASON_FLAG_PLANNED);
    if (rc == ERROR_SHUTDOWN_IN_PROGRESS || rc == ERROR_SHUTDOWN_IS_SCHEDULED)
        return Status::success(); // outra tarefa já agendou
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
