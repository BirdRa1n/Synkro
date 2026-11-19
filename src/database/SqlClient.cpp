#include "database/SqlClient.h"

#include "core/RunId.h"
#include "database/Tools.h"
#include "platform/windows/Win32.h"
#include "process/ProcessRunner.h"

#include <QSysInfo>

#include <chrono>

namespace synkro::db {

namespace {

std::wstring w(const std::string& s)
{
    return win::fromUtf8(s);
}

std::string tail(const std::string& s, size_t n = 600)
{
    std::string t = s;
    while (!t.empty() && (t.back() == '\n' || t.back() == '\r'))
        t.pop_back();
    return t.size() > n ? t.substr(t.size() - n) : t;
}

bool isMysql(const std::string& engine)
{
    return engine == "mysql" || engine == "mariadb";
}

Result<std::wstring> clientTool(const SqlTarget& t)
{
    if (t.engine == "postgresql")
        return locateTool("psql", t.tools);
    Result<std::wstring> tool = locateTool(t.engine == "mariadb" ? "mariadb" : "mysql", t.tools);
    if (!tool && t.engine == "mariadb")
        tool = locateTool("mysql", t.tools);
    return tool;
}

// Monta o processo do cliente com as credenciais gravadas em `cred`.
Result<ProcessSpec> clientSpec(const SqlTarget& t, PrivateDirectory& cred, const std::string& database,
                               const std::vector<std::wstring>& statements, const std::wstring& appName)
{
    auto tool = clientTool(t);
    if (!tool)
        return tool.error();
    const Connection& c = t.connection;
    ProcessSpec spec;
    if (t.engine == "postgresql") {
        auto pass = cred.writeFile(L"pgpass", pgpassLine(c.host, c.port, c.user, c.password));
        if (!pass)
            return pass.error();
        spec.args = {tool.value(), L"--host=" + w(c.host), L"--port=" + std::to_wstring(c.port), L"--username=" + w(c.user),
                     L"--no-password", L"--dbname=" + w(database), L"-At", L"--set=ON_ERROR_STOP=1"};
        for (const auto& s : statements) {
            spec.args.push_back(L"-c");
            spec.args.push_back(s);
        }
        spec.environment = pgEnvironment(c, pass.value());
        for (auto& [k, v] : spec.environment) {
            if (k == L"PGAPPNAME" && !appName.empty())
                v = appName;
        }
    } else if (isMysql(t.engine)) {
        auto defaults = cred.writeFile(L"my.cnf", mysqlDefaultsFile(c, t.engine));
        if (!defaults)
            return defaults.error();
        std::wstring joined;
        for (const auto& s : statements)
            joined += s + L";";
        // --defaults-extra-file precisa ser a primeira opção; --unbuffered
        // entrega cada resultado assim que fica pronto (trava no servidor).
        spec.args = {tool.value(), L"--defaults-extra-file=" + defaults.value(), L"-N", L"-B", L"--unbuffered"};
        if (!database.empty())
            spec.args.push_back(L"--database=" + w(database));
        spec.args.push_back(L"-e");
        spec.args.push_back(joined);
    } else {
        return Error::make(ErrorCode::Unsupported, "cliente SQL não suportado para o motor " + t.engine);
    }
    return spec;
}

} // namespace

std::string quoteIdentifier(const std::string& engine, const std::string& name)
{
    const char q = isMysql(engine) ? '`' : '"';
    std::string out(1, q);
    for (char ch : name) {
        if (ch == q)
            out += q;
        out += ch;
    }
    return out + q;
}

std::vector<std::string> outputLines(const std::string& output)
{
    std::vector<std::string> out;
    size_t start = 0;
    while (start < output.size()) {
        size_t end = output.find('\n', start);
        if (end == std::string::npos)
            end = output.size();
        std::string line = output.substr(start, end - start);
        start = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        if (!line.empty())
            out.push_back(line);
    }
    return out;
}

Result<std::string> runSql(const SqlTarget& target, const std::string& database, const std::string& sql,
                           const std::wstring& workDir, std::int64_t timeoutSeconds)
{
    auto cred = PrivateDirectory::create(win::joinPath(workDir, L"cred-sql-" + newRunId()));
    if (!cred)
        return cred.error();
    auto spec = clientSpec(target, cred.value(), database, {w(sql)}, {});
    if (!spec)
        return spec.error();
    spec.value().timeoutMs = timeoutSeconds * 1000;
    auto r = runProcess(spec.value());
    if (!r)
        return r.error();
    if (r.value().timedOut)
        return Error::make(ErrorCode::IoError, "a consulta excedeu o tempo limite");
    if (r.value().exitCode != 0)
        return Error::make(ErrorCode::IoError, "a consulta falhou: " + tail(r.value().stderrText));
    return r.value().stdoutText;
}

// ---- Trava no servidor ---------------------------------------------------------

std::string ServerLock::lockName(const std::string& database)
{
    std::string name = "synkro:" + database;
    if (name.size() > 64)
        name.resize(64);
    return name;
}

Result<std::unique_ptr<ServerLock>> ServerLock::acquire(const SqlTarget& target, const std::string& database,
                                                        const std::wstring& workDir, std::int64_t holdSeconds)
{
    std::unique_ptr<ServerLock> lock(new ServerLock());
    auto cred = PrivateDirectory::create(win::joinPath(workDir, L"cred-lock-" + newRunId()));
    if (!cred)
        return cred.error();
    lock->m_cred = std::make_unique<PrivateDirectory>(std::move(cred).value());

    // O literal vai entre aspas simples; o nome do banco já foi validado,
    // mas as aspas são duplicadas por garantia.
    std::string literal = "'";
    for (char ch : lockName(database)) {
        if (ch == '\'')
            literal += '\'';
        literal += ch;
    }
    literal += "'";

    const std::int64_t hold = holdSeconds > 0 ? holdSeconds : 86400;
    std::vector<std::wstring> statements;
    if (target.engine == "postgresql") {
        // A trava de sessão fica com o processo; pg_sleep só mantém a sessão viva.
        statements = {w("SELECT pg_try_advisory_lock(hashtext(" + literal + "))"),
                      w("SELECT pg_sleep(" + std::to_string(hold) + ")")};
    } else {
        statements = {w("SELECT GET_LOCK(" + literal + ", 0)"), w("SELECT SLEEP(" + std::to_string(hold) + ")")};
    }
    const std::wstring appName = L"synkro-lock@" + QSysInfo::machineHostName().toStdWString();
    // A trava é por banco: no PostgreSQL ela só vale dentro do banco conectado.
    auto spec = clientSpec(target, *lock->m_cred, target.engine == "postgresql" ? database : std::string(), statements, appName);
    if (!spec)
        return spec.error();

    lock->m_cancel = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!lock->m_cancel)
        return win::lastError("criar evento da trava");

    ProcessSpec ps = std::move(spec).value();
    ps.cancelEvent = lock->m_cancel;
    ps.timeoutMs = (hold + 60) * 1000;
    ServerLock* self = lock.get();
    ps.onStdout = [self](const char* data, std::size_t n) -> Status {
        std::lock_guard g(self->m_mutex);
        if (!self->m_gotLine) {
            self->m_stdout.append(data, n);
            if (self->m_stdout.find('\n') != std::string::npos) {
                self->m_gotLine = true;
                self->m_cv.notify_all();
            }
        }
        return Status::success();
    };
    lock->m_thread = std::thread([self, ps = std::move(ps)] {
        auto r = runProcess(ps);
        std::lock_guard g(self->m_mutex);
        self->m_exited = true;
        if (r)
            self->m_stderr = r.value().stderrText;
        else
            self->m_startError = r.error().message;
        self->m_cv.notify_all();
    });

    const auto wait = std::chrono::seconds(target.connection.connectTimeoutSeconds + 30);
    std::string first;
    {
        std::unique_lock g(lock->m_mutex);
        const bool answered = lock->m_cv.wait_for(g, wait, [&] { return lock->m_gotLine || lock->m_exited; });
        if (!answered)
            return Error::make(ErrorCode::IoError, "o servidor não respondeu ao pedido da trava");
        if (!lock->m_gotLine) {
            if (!lock->m_startError.empty())
                return Error::make(ErrorCode::IoError, "não foi possível pedir a trava no servidor: " + lock->m_startError);
            return Error::make(ErrorCode::IoError, "não foi possível pedir a trava no servidor: " + tail(lock->m_stderr));
        }
        first = outputLines(lock->m_stdout).empty() ? std::string() : outputLines(lock->m_stdout).front();
    }
    if (first == "t" || first == "1")
        return lock;
    if (first == "f" || first == "0")
        return Error::make(ErrorCode::AlreadyExists,
                           "outra máquina está fazendo o dump de " + database + " neste servidor (trava " + lockName(database) + ")");
    return Error::make(ErrorCode::IoError, "resposta inesperada do servidor ao pedir a trava: " + first);
}

void ServerLock::release()
{
    if (m_cancel)
        ::SetEvent(static_cast<HANDLE>(m_cancel));
    if (m_thread.joinable())
        m_thread.join();
    if (m_cancel) {
        ::CloseHandle(static_cast<HANDLE>(m_cancel));
        m_cancel = nullptr;
    }
}

ServerLock::~ServerLock()
{
    release();
}

} // namespace synkro::db
