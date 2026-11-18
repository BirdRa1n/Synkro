#pragma once

#include "core/Result.h"
#include "platform/windows/Win32.h"

#include <string>

namespace synkro::win {

// Trava de tarefa entre processos (mutex nomeado Global\Synkro.Job.<id>).
// Garante que uma execução agendada e uma manual da mesma tarefa nunca
// rodem ao mesmo tempo. Se o dono anterior morreu, a trava é assumida.
class JobLock {
public:
    // AlreadyExists se outra execução detém a trava.
    static Result<JobLock> tryAcquire(const std::wstring& jobId);

    JobLock(JobLock&& other) noexcept : m_mutex(std::move(other.m_mutex)) {}
    JobLock& operator=(JobLock&&) = delete;
    ~JobLock();

private:
    JobLock() = default;
    UniqueHandle m_mutex;
};

// Trava exclusiva baseada em arquivo (job.lock dentro do cofre). Protege o
// cofre mesmo contra outra máquina que tenha o disco montado. É liberada
// automaticamente pelo sistema se o processo morrer.
class FileLock {
public:
    static Result<FileLock> tryAcquire(const std::wstring& path);

    FileLock(FileLock&&) noexcept = default;
    FileLock& operator=(FileLock&&) noexcept = default;

private:
    FileLock() = default;
    UniqueHandle m_file;
};

} // namespace synkro::win
