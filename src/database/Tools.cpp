#include "database/Tools.h"

#include "engine/VaultOps.h"
#include "platform/windows/Win32.h"
#include "process/ProcessRunner.h"

#include <sddl.h>

#include <algorithm>

namespace synkro::db {

namespace {

// Ordena "16", "9.6", "10" numericamente.
bool versionLess(const std::wstring& a, const std::wstring& b)
{
    auto parts = [](const std::wstring& s) {
        std::vector<int> v;
        int cur = 0;
        bool any = false;
        for (wchar_t c : s) {
            if (c >= L'0' && c <= L'9') {
                cur = cur * 10 + (c - L'0');
                any = true;
            } else if (any) {
                v.push_back(cur);
                cur = 0;
                any = false;
            }
        }
        if (any)
            v.push_back(cur);
        return v;
    };
    return parts(a) < parts(b);
}

std::vector<std::wstring> candidateDirs(const std::string& tool)
{
    std::vector<std::wstring> dirs;
    wchar_t pf[MAX_PATH] = {};
    if (::GetEnvironmentVariableW(L"ProgramFiles", pf, MAX_PATH) == 0)
        return dirs;
    const std::wstring programFiles = pf;

    auto versioned = [&](const std::wstring& parent, const std::wstring& prefix) {
        std::vector<std::wstring> names;
        for (const auto& n : ops::listSubdirectories(parent)) {
            if (prefix.empty() || n.rfind(prefix, 0) == 0)
                names.push_back(n);
        }
        std::sort(names.begin(), names.end(), versionLess);
        for (auto it = names.rbegin(); it != names.rend(); ++it) // mais nova primeiro
            dirs.push_back(win::joinPath(win::joinPath(parent, *it), L"bin"));
    };

    if (tool.rfind("pg_", 0) == 0 || tool == "psql" || tool == "createdb") {
        versioned(win::joinPath(programFiles, L"PostgreSQL"), L"");
    } else if (tool == "mysqldump" || tool == "mysql" || tool == "mariadb-dump" || tool == "mariadb") {
        versioned(win::joinPath(programFiles, L"MySQL"), L"MySQL Server");
        versioned(programFiles, L"MariaDB");
    }
    return dirs;
}

} // namespace

Result<std::wstring> locateTool(const std::string& tool, const std::map<std::string, std::wstring>& explicitPaths)
{
    if (auto it = explicitPaths.find(tool); it != explicitPaths.end() && !it->second.empty()) {
        if (ops::pathExists(it->second))
            return it->second;
        return Error::make(ErrorCode::NotFound, tool + " não encontrado em " + win::toUtf8(it->second));
    }
    const std::wstring exe = win::fromUtf8(tool) + L".exe";
    for (const auto& dir : candidateDirs(tool)) {
        const std::wstring p = win::joinPath(dir, exe);
        if (ops::pathExists(p))
            return p;
    }
    wchar_t found[MAX_PATH] = {};
    if (::SearchPathW(nullptr, exe.c_str(), nullptr, MAX_PATH, found, nullptr) > 0)
        return std::wstring(found);
    return Error::make(ErrorCode::NotFound,
                       tool + " não encontrado nesta máquina; instale as ferramentas cliente ou informe tools." + tool);
}

std::string toolVersion(const std::wstring& toolPath)
{
    ProcessSpec spec;
    spec.args = {toolPath, L"--version"};
    spec.timeoutMs = 15000;
    auto r = runProcess(spec);
    if (!r)
        return {};
    std::string out = r.value().stdoutText;
    const size_t nl = out.find_first_of("\r\n");
    return nl == std::string::npos ? out : out.substr(0, nl);
}

Result<PrivateDirectory> PrivateDirectory::create(const std::wstring& path)
{
    const std::wstring parent = win::parentPath(path);
    if (!parent.empty()) {
        if (auto st = win::createDirectories(parent); !st)
            return st.error();
    }
    // Herança desligada: dono (OW) e SYSTEM, somente.
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;OICI;FA;;;OW)(A;OICI;FA;;;SY)",
                                                               SDDL_REVISION_1, &sd, nullptr))
        return win::lastError("montar ACL da pasta temporária");
    SECURITY_ATTRIBUTES sa{sizeof(sa), sd, FALSE};
    const BOOL ok = ::CreateDirectoryW(win::toExtendedPath(path).c_str(), &sa);
    const DWORD err = ::GetLastError();
    ::LocalFree(sd);
    if (!ok)
        return win::errorFromWin32(err, "criar pasta temporária da execução");
    PrivateDirectory dir;
    dir.m_path = path;
    return dir;
}

PrivateDirectory::PrivateDirectory(PrivateDirectory&& other) noexcept : m_path(std::move(other.m_path))
{
    other.m_path.clear();
}

PrivateDirectory::~PrivateDirectory()
{
    if (!m_path.empty())
        (void)ops::removeTree(m_path);
}

Result<std::wstring> PrivateDirectory::writeFile(const std::wstring& name, const std::string& content)
{
    const std::wstring p = win::joinPath(m_path, name);
    if (auto st = win::writeFileAtomically(p, content); !st)
        return st.error();
    return p;
}

std::string pgpassLine(const std::string& host, int port, const std::string& user, const std::string& password)
{
    auto esc = [](const std::string& s) {
        std::string o;
        for (char c : s) {
            if (c == ':' || c == '\\')
                o += '\\';
            o += c;
        }
        return o;
    };
    return esc(host) + ":" + std::to_string(port) + ":*:" + esc(user) + ":" + esc(password) + "\n";
}

} // namespace synkro::db
