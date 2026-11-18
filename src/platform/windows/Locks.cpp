#include "platform/windows/Locks.h"

namespace synkro::win {

Result<JobLock> JobLock::tryAcquire(const std::wstring& jobId)
{
    return acquire(jobId, 0);
}

Result<JobLock> JobLock::acquire(const std::wstring& jobId, unsigned long timeoutMs)
{
    JobLock lock;
    const std::wstring suffix = L"Synkro.Job." + jobId;

    HANDLE h = ::CreateMutexW(nullptr, FALSE, (L"Global\\" + suffix).c_str());
    if (!h && ::GetLastError() == ERROR_ACCESS_DENIED)
        h = ::CreateMutexW(nullptr, FALSE, (L"Local\\" + suffix).c_str());
    if (!h)
        return lastError("criar trava da tarefa");
    lock.m_mutex.reset(h);

    const DWORD wait = ::WaitForSingleObject(h, timeoutMs);
    if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED)
        return lock;
    if (wait == WAIT_TIMEOUT)
        return Error::make(ErrorCode::AlreadyExists, "a tarefa já está em execução");
    return lastError("obter trava da tarefa");
}

JobLock::~JobLock()
{
    if (m_mutex)
        ::ReleaseMutex(m_mutex.get());
}

Result<FileLock> FileLock::tryAcquire(const std::wstring& path)
{
    FileLock lock;
    lock.m_file.reset(::CreateFileW(toExtendedPath(path).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                    OPEN_ALWAYS, FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_DELETE_ON_CLOSE, nullptr));
    if (!lock.m_file) {
        const DWORD err = ::GetLastError();
        if (err == ERROR_SHARING_VIOLATION || err == ERROR_LOCK_VIOLATION)
            return Error::make(ErrorCode::AlreadyExists, "o cofre está em uso por outra execução", err);
        return errorFromWin32(err, "travar o cofre");
    }
    return lock;
}

} // namespace synkro::win
