#include "platform/windows/NetworkShare.h"

#include "platform/windows/Win32.h"

#include <QDateTime>
#include <QHostAddress>
#include <QNetworkInterface>
#include <QTcpSocket>

#include <winnetwk.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>

namespace synkro::win {

namespace {

std::mutex g_mutex;
std::map<std::wstring, qint64> g_unreachable; // servidor (minúsculas) -> momento da falha (ms)
std::map<std::wstring, std::wstring> g_connected; // raiz do compartilhamento -> usuário
std::atomic<int> g_probeTimeoutMs{3000};

std::string describeNetError(DWORD code, const std::wstring& root)
{
    const std::string share = toUtf8(root);
    switch (code) {
    case ERROR_LOGON_FAILURE:
    case ERROR_INVALID_PASSWORD:
        return "usuário ou senha recusados pelo servidor";
    case ERROR_BAD_NETPATH:
        return "servidor não encontrado na rede";
    case ERROR_BAD_NET_NAME:
        return "o compartilhamento " + share + " não existe no servidor";
    case ERROR_ACCESS_DENIED:
        return "acesso negado a " + share;
    case ERROR_ACCOUNT_LOCKED_OUT:
        return "a conta está bloqueada no servidor";
    case ERROR_ACCOUNT_DISABLED:
        return "a conta está desativada no servidor";
    case ERROR_PASSWORD_EXPIRED:
    case ERROR_PASSWORD_MUST_CHANGE:
        return "a senha da conta expirou no servidor";
    case ERROR_SESSION_CREDENTIAL_CONFLICT:
        return "já existe uma conexão com este servidor usando outro usuário";
    case ERROR_NO_NETWORK:
    case ERROR_NETWORK_UNREACHABLE:
        return "rede indisponível";
    default:
        return "falha ao conectar em " + share + " (erro " + std::to_string(code) + ")";
    }
}

} // namespace

bool isUncPath(const std::wstring& p)
{
    if (p.size() < 5)
        return false;
    const bool lead = (p[0] == L'\\' || p[0] == L'/') && (p[1] == L'\\' || p[1] == L'/');
    if (!lead || p[2] == L'?' || p[2] == L'.' || p[2] == L'\\' || p[2] == L'/')
        return false;
    const std::wstring n = normalizeUnc(p);
    const auto sep = n.find(L'\\', 2);
    return sep != std::wstring::npos && sep > 2 && sep + 1 < n.size();
}

std::wstring normalizeUnc(const std::wstring& path)
{
    std::wstring p = path;
    for (auto& c : p)
        if (c == L'/')
            c = L'\\';
    while (p.size() > 2 && p.back() == L'\\')
        p.pop_back();
    return p;
}

std::wstring uncServer(const std::wstring& path)
{
    const std::wstring n = normalizeUnc(path);
    if (n.size() < 3)
        return {};
    const auto sep = n.find(L'\\', 2);
    return n.substr(2, sep == std::wstring::npos ? std::wstring::npos : sep - 2);
}

std::wstring uncShareRoot(const std::wstring& path)
{
    const std::wstring n = normalizeUnc(path);
    const auto first = n.find(L'\\', 2);
    if (first == std::wstring::npos)
        return n;
    const auto second = n.find(L'\\', first + 1);
    return second == std::wstring::npos ? n : n.substr(0, second);
}

bool isThisComputer(const std::wstring& server)
{
    const std::wstring s = foldCase(server); // foldCase: maiúsculas
    if (s.empty() || s == foldCase(L"localhost") || s == L"." || s == L"127.0.0.1" || s == L"::1" || s == L"[::1]")
        return true;
    for (auto fmt : {ComputerNameNetBIOS, ComputerNameDnsHostname, ComputerNameDnsFullyQualified}) {
        wchar_t name[256] = {};
        DWORD size = 256;
        if (::GetComputerNameExW(fmt, name, &size) && size > 0 && foldCase(std::wstring(name, size)) == s)
            return true;
    }
    QHostAddress addr;
    if (!addr.setAddress(QString::fromStdWString(server)))
        return false;
    if (addr.isLoopback())
        return true;
    const auto mine = QNetworkInterface::allAddresses();
    return std::any_of(mine.begin(), mine.end(), [&](const QHostAddress& a) { return a.isEqual(addr); });
}

void setSmbProbeTimeout(int timeoutMs)
{
    g_probeTimeoutMs = std::clamp(timeoutMs, 100, 30000);
}

Status probeSmbServer(const std::wstring& server, int timeoutMs)
{
    if (timeoutMs < 0)
        timeoutMs = g_probeTimeoutMs;
    if (server.empty())
        return Error::make(ErrorCode::InvalidArgument, "servidor não informado");
    if (server.find(L'@') != std::wstring::npos)
        return Status::success(); // WebDAV (\\srv@SSL\...): outra porta, sem sondagem
    const std::wstring key = foldCase(server);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    {
        std::lock_guard<std::mutex> g(g_mutex);
        auto it = g_unreachable.find(key);
        if (it != g_unreachable.end() && now - it->second < 60000)
            return Error::make(ErrorCode::DeviceNotReady, "o servidor " + toUtf8(server) + " não respondeu há pouco (porta 445)");
    }
    QTcpSocket s;
    s.connectToHost(QString::fromStdWString(server), 445);
    if (s.waitForConnected(timeoutMs)) {
        s.abort();
        std::lock_guard<std::mutex> g(g_mutex);
        g_unreachable.erase(key);
        return Status::success();
    }
    const bool notFound = s.error() == QAbstractSocket::HostNotFoundError;
    s.abort();
    {
        std::lock_guard<std::mutex> g(g_mutex);
        g_unreachable[key] = now;
    }
    return Error::make(ErrorCode::DeviceNotReady,
                       notFound ? "o servidor " + toUtf8(server) + " não foi encontrado na rede (DNS)"
                                : "o servidor " + toUtf8(server) + " não responde (porta 445 do compartilhamento de arquivos)");
}

Status connectShare(const std::wstring& uncPath, const std::wstring& user, const std::string& passwordUtf8)
{
    if (user.empty())
        return Status::success();
    const std::wstring root = uncShareRoot(uncPath);
    {
        std::lock_guard<std::mutex> g(g_mutex);
        auto it = g_connected.find(foldCase(root));
        if (it != g_connected.end() && it->second == user)
            return Status::success();
    }

    std::wstring password = fromUtf8(passwordUtf8);
    NETRESOURCEW nr{};
    nr.dwType = RESOURCETYPE_DISK;
    nr.lpRemoteName = const_cast<wchar_t*>(root.c_str());
    // Sem letra de unidade e sem CONNECT_UPDATE_PROFILE: nada fica gravado no perfil.
    DWORD rc = ::WNetAddConnection2W(&nr, password.c_str(), user.c_str(), CONNECT_TEMPORARY);
    if (rc == ERROR_SESSION_CREDENTIAL_CONFLICT) {
        // Conexão antiga com outra credencial (ex.: senha trocada): refaz.
        ::WNetCancelConnection2W(root.c_str(), 0, FALSE);
        rc = ::WNetAddConnection2W(&nr, password.c_str(), user.c_str(), CONNECT_TEMPORARY);
    }
    ::SecureZeroMemory(password.data(), password.size() * sizeof(wchar_t));
    if (rc == NO_ERROR || rc == ERROR_ALREADY_ASSIGNED || rc == ERROR_DEVICE_ALREADY_REMEMBERED) {
        std::lock_guard<std::mutex> g(g_mutex);
        g_connected[foldCase(root)] = user;
        return Status::success();
    }
    return Error::make(rc == ERROR_ACCESS_DENIED || rc == ERROR_LOGON_FAILURE || rc == ERROR_INVALID_PASSWORD
                           ? ErrorCode::AccessDenied
                           : ErrorCode::DeviceNotReady,
                       describeNetError(rc, root), rc);
}

} // namespace synkro::win
