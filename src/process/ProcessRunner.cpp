#include "process/ProcessRunner.h"

#include "platform/windows/Win32.h"

#include <algorithm>
#include <atomic>
#include <map>
#include <thread>

namespace synkro {

std::wstring buildCommandLine(const std::vector<std::wstring>& args)
{
    std::wstring cmd;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i > 0)
            cmd += L' ';
        const std::wstring& a = args[i];
        const bool needsQuotes = a.empty() || a.find_first_of(L" \t\n\v\"") != std::wstring::npos;
        if (!needsQuotes) {
            cmd += a;
            continue;
        }
        cmd += L'"';
        size_t backslashes = 0;
        for (wchar_t c : a) {
            if (c == L'\\') {
                ++backslashes;
            } else if (c == L'"') {
                cmd.append(backslashes * 2 + 1, L'\\');
                cmd += L'"';
                backslashes = 0;
            } else {
                cmd.append(backslashes, L'\\');
                cmd += c;
                backslashes = 0;
            }
        }
        cmd.append(backslashes * 2, L'\\');
        cmd += L'"';
    }
    return cmd;
}

namespace {

struct CaseInsensitive {
    bool operator()(const std::wstring& a, const std::wstring& b) const { return win::foldCase(a) < win::foldCase(b); }
};

std::wstring buildEnvironmentBlock(const std::vector<std::pair<std::wstring, std::wstring>>& extra)
{
    std::map<std::wstring, std::wstring, CaseInsensitive> vars;
    if (wchar_t* block = ::GetEnvironmentStringsW()) {
        for (const wchar_t* p = block; *p; p += wcslen(p) + 1) {
            const std::wstring entry(p);
            const size_t eq = entry.find(L'=', 1); // entradas "=C:=C:\..." começam com '='
            if (eq == std::wstring::npos)
                continue;
            vars[entry.substr(0, eq)] = entry.substr(eq + 1);
        }
        ::FreeEnvironmentStringsW(block);
    }
    for (const auto& [k, v] : extra)
        vars[k] = v;

    std::wstring out;
    for (const auto& [k, v] : vars) {
        out += k;
        out += L'=';
        out += v;
        out += L'\0';
    }
    out += L'\0';
    return out;
}

void appendLimited(std::string& target, const char* data, size_t size, size_t limit)
{
    target.append(data, size);
    if (target.size() > limit)
        target.erase(0, target.size() - limit); // mantém o final, onde estão os erros
}

} // namespace

Result<ProcessResult> runProcess(const ProcessSpec& spec)
{
    if (spec.args.empty())
        return Error::make(ErrorCode::InvalidArgument, "comando vazio");

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE outRead = nullptr, outWrite = nullptr, errRead = nullptr, errWrite = nullptr;
    if (!::CreatePipe(&outRead, &outWrite, &sa, 0))
        return win::lastError("criar pipe de saída");
    win::UniqueHandle hOutRead(outRead), hOutWrite(outWrite);
    if (!::CreatePipe(&errRead, &errWrite, &sa, 0))
        return win::lastError("criar pipe de erro");
    win::UniqueHandle hErrRead(errRead), hErrWrite(errWrite);
    // Só as pontas de escrita são herdadas pelo filho.
    ::SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
    ::SetHandleInformation(errRead, HANDLE_FLAG_INHERIT, 0);

    HANDLE nulIn = spec.stdinFile.empty()
                       ? ::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr)
                       : ::CreateFileW(win::toExtendedPath(spec.stdinFile).c_str(), GENERIC_READ, FILE_SHARE_READ, &sa,
                                       OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (nulIn == INVALID_HANDLE_VALUE)
        return win::lastError("abrir entrada padrão do processo");
    win::UniqueHandle hNul(nulIn);

    win::UniqueHandle job(::CreateJobObjectW(nullptr, nullptr));
    if (!job)
        return win::lastError("criar Job Object");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    ::SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits));

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nulIn;
    si.hStdOutput = outWrite;
    si.hStdError = errWrite;

    // O YAML aceita "C:/Programa/x.exe"; o cmd.exe, porém, interpreta as
    // barras do próprio caminho como opções. O executável sempre usa '\'.
    std::vector<std::wstring> args = spec.args;
    std::replace(args.front().begin(), args.front().end(), L'/', L'\\');
    std::wstring cmdLine = buildCommandLine(args);
    std::wstring env = buildEnvironmentBlock(spec.environment);

    PROCESS_INFORMATION pi{};
    if (!::CreateProcessW(nullptr, cmdLine.data(), nullptr, nullptr, TRUE,
                          CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, env.data(),
                          spec.workingDirectory.empty() ? nullptr : spec.workingDirectory.c_str(), &si, &pi))
        return win::lastError("iniciar " + win::toUtf8(spec.args.front()));

    win::UniqueHandle process(pi.hProcess), thread(pi.hThread);
    ::AssignProcessToJobObject(job.get(), pi.hProcess);
    ::ResumeThread(pi.hThread);

    // Pontas de escrita fechadas aqui: o EOF chega quando o filho termina.
    hOutWrite.reset();
    hErrWrite.reset();

    ProcessResult result;
    std::atomic<bool> finished{false};
    std::atomic<bool> timedOut{false};

    std::thread watchdog([&] {
        const DWORD wait = spec.timeoutMs > 0 ? static_cast<DWORD>(std::min<std::int64_t>(spec.timeoutMs, INFINITE - 1))
                                              : INFINITE;
        if (::WaitForSingleObject(process.get(), wait) == WAIT_TIMEOUT && !finished) {
            timedOut = true;
            ::TerminateJobObject(job.get(), 1);
        }
    });

    std::thread errReader([&] {
        char buf[4096];
        DWORD n = 0;
        while (::ReadFile(hErrRead.get(), buf, sizeof(buf), &n, nullptr) && n > 0)
            appendLimited(result.stderrText, buf, n, spec.maxCapturedStderr);
    });

    Error callbackError;
    bool callbackFailed = false;
    {
        std::vector<char> buf(64 * 1024);
        DWORD n = 0;
        while (::ReadFile(hOutRead.get(), buf.data(), static_cast<DWORD>(buf.size()), &n, nullptr) && n > 0) {
            if (spec.onStdout) {
                if (callbackFailed)
                    continue; // drena para o filho não travar até ser encerrado
                if (auto st = spec.onStdout(buf.data(), n); !st) {
                    callbackFailed = true;
                    callbackError = st.error();
                    ::TerminateJobObject(job.get(), 1);
                }
            } else {
                appendLimited(result.stdoutText, buf.data(), n, spec.maxCapturedStdout);
            }
        }
    }

    ::WaitForSingleObject(process.get(), INFINITE);
    finished = true;
    errReader.join();
    watchdog.join();

    DWORD code = 0;
    ::GetExitCodeProcess(process.get(), &code);
    result.exitCode = code;
    result.timedOut = timedOut;

    if (callbackFailed)
        return callbackError;
    return result;
}

} // namespace synkro
