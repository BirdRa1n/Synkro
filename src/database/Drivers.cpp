#include "database/Drivers.h"

#include "database/Sqlite.h"
#include "database/Tools.h"
#include "database/Zstd.h"
#include "engine/Hooks.h"
#include "engine/VaultOps.h"
#include "platform/windows/Win32.h"
#include "process/ProcessRunner.h"

#include <algorithm>
#include <optional>
#include <set>

namespace synkro::db {

namespace {

std::wstring w(const std::string& s)
{
    return win::fromUtf8(s);
}

std::string opt(const config::DatabaseJob& job, const char* key, const std::string& def)
{
    auto it = job.options.find(key);
    return it == job.options.end() ? def : it->second;
}

std::string tail(const std::string& s, size_t n = 600)
{
    std::string t = s;
    while (!t.empty() && (t.back() == '\n' || t.back() == '\r'))
        t.pop_back();
    return t.size() > n ? t.substr(t.size() - n) : t;
}

// O que pode ir para metadados/logs: nada de caminhos de credencial.
std::string sanitizedCommand(const std::vector<std::wstring>& args)
{
    std::vector<std::wstring> clean;
    for (const auto& a : args) {
        if (a.rfind(L"--defaults-extra-file=", 0) == 0)
            continue;
        clean.push_back(a);
    }
    return win::toUtf8(buildCommandLine(clean));
}

Status finalize(DumpArtifact& a)
{
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (!::GetFileAttributesExW(win::toExtendedPath(a.stagedFile).c_str(), GetFileExInfoStandard, &info))
        return win::lastError("ler dump gerado");
    a.size = (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    auto digest = Sha256::ofFile(a.stagedFile);
    if (!digest)
        return digest.error();
    a.sha256 = digest.value();
    return Status::success();
}

void removeIfExists(const std::wstring& p)
{
    ::DeleteFileW(win::toExtendedPath(p).c_str());
}

// Executa com novas tentativas (rede instável, servidor reiniciando).
template <typename Fn>
auto withRetries(const DumpContext& ctx, Fn&& attempt) -> decltype(attempt())
{
    auto result = attempt();
    for (int i = 0; i < ctx.retries && !result.ok(); ++i) {
        if (ctx.retryDelaySeconds > 0)
            ::Sleep(static_cast<DWORD>(ctx.retryDelaySeconds) * 1000);
        result = attempt();
    }
    return result;
}

} // namespace

std::vector<std::pair<std::wstring, std::wstring>> pgEnvironment(const Connection& c, const std::wstring& passFile)
{
    std::vector<std::pair<std::wstring, std::wstring>> env = {
        {L"PGPASSFILE", passFile},
        {L"PGCONNECT_TIMEOUT", std::to_wstring(c.connectTimeoutSeconds)},
        {L"PGAPPNAME", L"synkro"},
    };
    if (!c.sslMode.empty())
        env.emplace_back(L"PGSSLMODE", w(c.sslMode));
    if (!c.sslCa.empty())
        env.emplace_back(L"PGSSLROOTCERT", c.sslCa);
    return env;
}

namespace {

std::vector<std::string> parsePgRestoreList(const std::string& listing)
{
    std::vector<std::string> tables;
    size_t start = 0;
    while (start < listing.size()) {
        size_t end = listing.find('\n', start);
        if (end == std::string::npos)
            end = listing.size();
        std::string line = listing.substr(start, end - start);
        start = end + 1;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty() || line[0] == ';')
            continue;
        const size_t pos = line.find(" TABLE DATA ");
        if (pos == std::string::npos)
            continue;
        const std::string rest = line.substr(pos + 12);
        const size_t sp1 = rest.find(' ');
        if (sp1 == std::string::npos)
            continue;
        const size_t sp2 = rest.find(' ', sp1 + 1);
        tables.push_back(rest.substr(0, sp1) + "." + rest.substr(sp1 + 1, sp2 == std::string::npos ? std::string::npos : sp2 - sp1 - 1));
    }
    return tables;
}

// Leitura incremental de um dump SQL em texto: tabelas, MyISAM e o final.
struct SqlStreamInspector {
    std::string carry;
    std::string lastBytes;
    std::vector<std::string> tables;
    int myisam = 0;

    void feed(const char* data, size_t size)
    {
        lastBytes.append(data, size);
        if (lastBytes.size() > 1024)
            lastBytes.erase(0, lastBytes.size() - 1024);
        carry.append(data, size);
        size_t start = 0;
        for (;;) {
            const size_t nl = carry.find('\n', start);
            if (nl == std::string::npos)
                break;
            line(carry.substr(start, nl - start));
            start = nl + 1;
        }
        carry.erase(0, start);
        if (carry.size() > 1u << 20) // linhas gigantes (INSERT): não é preciso guardar
            carry.clear();
    }

    void line(const std::string& l)
    {
        static const std::string mysqlCreate = "CREATE TABLE `";
        static const std::string pgCreate = "CREATE TABLE ";
        if (l.rfind(mysqlCreate, 0) == 0) {
            const size_t end = l.find('`', mysqlCreate.size());
            if (end != std::string::npos)
                tables.push_back(l.substr(mysqlCreate.size(), end - mysqlCreate.size()));
        } else if (l.rfind(pgCreate, 0) == 0) {
            const size_t end = l.find_first_of(" (", pgCreate.size());
            tables.push_back(l.substr(pgCreate.size(), end == std::string::npos ? std::string::npos : end - pgCreate.size()));
        }
        if (l.find("ENGINE=MyISAM") != std::string::npos || l.find("ENGINE=Aria") != std::string::npos)
            ++myisam;
    }
};

Status scanTextFile(const std::wstring& path, SqlStreamInspector& inspector)
{
    win::UniqueHandle h(::CreateFileW(win::toExtendedPath(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                      OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!h)
        return win::lastError("ler dump");
    std::vector<char> buf(1u << 20);
    for (;;) {
        DWORD n = 0;
        if (!::ReadFile(h.get(), buf.data(), static_cast<DWORD>(buf.size()), &n, nullptr))
            return win::lastError("ler dump");
        if (n == 0)
            break;
        inspector.feed(buf.data(), n);
    }
    return Status::success();
}

} // namespace

std::string safeName(const std::string& name)
{
    std::string out;
    for (char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-'
                        || c == '_' || c == '.';
        out += ok ? c : '_';
    }
    if (out.empty() || out == "." || out == "..")
        out = "_" + out;
    return out;
}

// ---- Argumentos --------------------------------------------------------------

std::vector<std::wstring> pgDumpArgs(const std::wstring& tool, const Connection& c, const std::string& database,
                                     const std::wstring& outFile, const config::DatabaseJob& job,
                                     const std::vector<std::string>& excludeTables)
{
    std::vector<std::wstring> a = {tool,
                                   L"--host=" + w(c.host),
                                   L"--port=" + std::to_wstring(c.port),
                                   L"--username=" + w(c.user),
                                   L"--no-password"};
    const std::string format = opt(job, "format", "custom");
    if (format == "plain") {
        a.push_back(L"--format=plain");
    } else {
        a.push_back(L"--format=custom");
        a.push_back(L"--compress=" + w(opt(job, "compress", "6")));
    }
    for (const auto& t : excludeTables)
        a.push_back(L"--exclude-table=" + w(t));
    for (const auto& e : job.extraArgs)
        a.push_back(w(e));
    a.push_back(L"--file=" + outFile);
    a.push_back(w(database));
    return a;
}

std::string mysqlDefaultsFile(const Connection& c, const std::string& engine)
{
    auto quote = [](const std::string& s) {
        std::string o = "\"";
        for (char ch : s) {
            if (ch == '"' || ch == '\\')
                o += '\\';
            o += ch;
        }
        return o + "\"";
    };
    std::string f = "[client]\n";
    f += "user=" + quote(c.user) + "\n";
    f += "password=" + quote(c.password) + "\n";
    f += "host=" + c.host + "\n";
    f += "port=" + std::to_string(c.port) + "\n";
    f += "connect-timeout=" + std::to_string(c.connectTimeoutSeconds) + "\n";
    if (engine == "mariadb") {
        if (c.sslMode == "disabled")
            f += "skip-ssl\n";
        else if (c.sslMode == "required")
            f += "ssl\n";
        else if (c.sslMode == "verify_ca" || c.sslMode == "verify_identity")
            f += "ssl\nssl-verify-server-cert\n";
    } else if (!c.sslMode.empty()) {
        std::string mode = c.sslMode;
        std::transform(mode.begin(), mode.end(), mode.begin(), [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
        f += "ssl-mode=" + mode + "\n";
    }
    if (!c.sslCa.empty())
        f += "ssl-ca=" + win::toUtf8(c.sslCa) + "\n";
    return f;
}

std::vector<std::wstring> mysqldumpArgs(const std::wstring& tool, const std::wstring& defaultsFile, const std::string& engine,
                                        const std::string& database, const config::DatabaseJob& job)
{
    // --defaults-extra-file precisa ser a primeira opção.
    std::vector<std::wstring> a = {tool, L"--defaults-extra-file=" + defaultsFile};
    const bool lock = opt(job, "myisam_strategy", "warn") == "lock";
    if (lock)
        a.push_back(L"--lock-tables");
    else if (opt(job, "single_transaction", "true") == "true")
        a.push_back(L"--single-transaction");
    a.push_back(L"--quick");
    if (opt(job, "routines", "true") == "true")
        a.push_back(L"--routines");
    a.push_back(opt(job, "triggers", "true") == "true" ? L"--triggers" : L"--skip-triggers");
    if (opt(job, "events", "true") == "true")
        a.push_back(L"--events");
    a.push_back(L"--hex-blob");
    a.push_back(L"--default-character-set=utf8mb4");
    a.push_back(L"--no-tablespaces");
    if (engine == "mysql")
        a.push_back(L"--set-gtid-purged=OFF");
    for (const auto& e : job.extraArgs)
        a.push_back(w(e));
    a.push_back(w(database)); // sem --databases: restaurável com outro nome
    return a;
}

// ---- Listagem de bancos -------------------------------------------------------

Result<std::vector<std::string>> listServerDatabases(const config::DatabaseJob& job, const Connection& c,
                                                     const DumpContext& ctx)
{
    ProcessSpec spec;
    spec.timeoutMs = 120000;
    auto credDir = PrivateDirectory::create(win::joinPath(ctx.workDir, L"cred-list"));
    if (!credDir)
        return credDir.error();

    std::set<std::string> system;
    if (job.engine == "postgresql") {
        auto psql = locateTool("psql", job.tools);
        if (!psql)
            return psql.error();
        auto pass = credDir.value().writeFile(L"pgpass", pgpassLine(c.host, c.port, c.user, c.password));
        if (!pass)
            return pass.error();
        spec.args = {psql.value(), L"--host=" + w(c.host), L"--port=" + std::to_wstring(c.port), L"--username=" + w(c.user),
                     L"--no-password", L"--dbname=postgres", L"-At", L"-c",
                     L"SELECT datname FROM pg_database WHERE datallowconn AND NOT datistemplate ORDER BY 1"};
        spec.environment = pgEnvironment(c, pass.value());
    } else {
        auto mysql = locateTool(job.engine == "mariadb" ? "mariadb" : "mysql", job.tools);
        if (!mysql && job.engine == "mariadb")
            mysql = locateTool("mysql", job.tools);
        if (!mysql)
            return mysql.error();
        auto defaults = credDir.value().writeFile(L"my.cnf", mysqlDefaultsFile(c, job.engine));
        if (!defaults)
            return defaults.error();
        spec.args = {mysql.value(), L"--defaults-extra-file=" + defaults.value(), L"-N", L"-B", L"-e", L"SHOW DATABASES"};
        system = {"information_schema", "performance_schema", "mysql", "sys"};
    }

    auto r = withRetries(ctx, [&] { return runProcess(spec); });
    if (!r)
        return r.error();
    if (r.value().exitCode != 0)
        return Error::make(ErrorCode::IoError, "não foi possível listar os bancos do servidor: " + tail(r.value().stderrText));

    std::vector<std::string> out;
    std::string s = r.value().stdoutText;
    size_t start = 0;
    while (start < s.size()) {
        size_t end = s.find('\n', start);
        if (end == std::string::npos)
            end = s.size();
        std::string name = s.substr(start, end - start);
        start = end + 1;
        while (!name.empty() && (name.back() == '\r' || name.back() == ' '))
            name.pop_back();
        if (!name.empty() && !system.count(name))
            out.push_back(name);
    }
    return out;
}

// ---- PostgreSQL ---------------------------------------------------------------

Result<DumpArtifact> dumpPostgres(const config::DatabaseJob& job, const Connection& c, const config::DatabaseEntry& db,
                                  const DumpContext& ctx)
{
    auto tool = locateTool("pg_dump", job.tools);
    if (!tool)
        return tool.error();

    DumpArtifact a;
    a.database = db.name;
    a.toolPath = win::toUtf8(tool.value());
    a.toolVersion = toolVersion(tool.value());
    const bool plain = opt(job, "format", "custom") == "plain";
    a.dumpFormat = plain ? "plain" : "custom";

    auto cred = PrivateDirectory::create(win::joinPath(ctx.workDir, L"cred-" + w(safeName(db.name))));
    if (!cred)
        return cred.error();
    auto pass = cred.value().writeFile(L"pgpass", pgpassLine(c.host, c.port, c.user, c.password));
    if (!pass)
        return pass.error();

    const std::wstring raw = win::joinPath(ctx.workDir, w(safeName(db.name)) + (plain ? L".sql" : L".dump"));
    ProcessSpec spec;
    spec.args = pgDumpArgs(tool.value(), c, db.name, raw, job, db.excludeTables);
    spec.environment = pgEnvironment(c, pass.value());
    spec.timeoutMs = ctx.timeoutSeconds * 1000;
    a.command = sanitizedCommand(spec.args);

    auto run = withRetries(ctx, [&]() -> Result<ProcessResult> {
        removeIfExists(raw);
        auto r = runProcess(spec);
        if (r && (r.value().exitCode != 0 || r.value().timedOut))
            return Error::make(ErrorCode::IoError, r.value().timedOut ? "pg_dump excedeu o tempo limite"
                                                                     : "pg_dump falhou: " + tail(r.value().stderrText));
        return r;
    });
    if (!run) {
        removeIfExists(raw);
        return run.error();
    }

    if (plain) {
        SqlStreamInspector insp;
        if (auto st = scanTextFile(raw, insp); !st)
            return st.error();
        a.validationMethod = "marca final \"PostgreSQL database dump complete\"";
        if (insp.lastBytes.find("PostgreSQL database dump complete") == std::string::npos)
            return Error::make(ErrorCode::Corrupted, "dump do PostgreSQL incompleto (sem a marca de término)");
        a.tables = insp.tables;
        a.tablesKnown = true;
        a.stagedFile = raw + L".zst";
        a.extension = ".sql.zst";
        a.compression = "zstd";
        if (auto st = compressFile(raw, a.stagedFile, std::stoi(opt(job, "compression_level", "3"))); !st)
            return st.error();
        removeIfExists(raw);
        a.restoreHint = "zstd -d <arquivo> e depois psql --dbname=<novo_banco> -f <arquivo.sql>";
    } else {
        auto restoreTool = locateTool("pg_restore", job.tools);
        if (!restoreTool)
            return Error::make(ErrorCode::NotFound, "pg_restore é necessário para validar o dump: " + restoreTool.error().message);
        ProcessSpec list;
        list.args = {restoreTool.value(), L"--list", raw};
        list.timeoutMs = ctx.timeoutSeconds * 1000;
        auto r = runProcess(list);
        a.validationMethod = "pg_restore --list";
        if (!r)
            return r.error();
        if (r.value().exitCode != 0)
            return Error::make(ErrorCode::Corrupted, "pg_restore --list rejeitou o dump: " + tail(r.value().stderrText));
        a.tables = parsePgRestoreList(r.value().stdoutText);
        a.tablesKnown = true;
        a.stagedFile = raw;
        a.extension = ".dump";
        a.compression = "pg-internal";
        a.restoreHint = "pg_restore --dbname=<novo_banco> --no-owner <arquivo>";
    }
    a.validationResult = "ok";
    if (auto st = finalize(a); !st)
        return st.error();
    return a;
}

Result<DumpArtifact> dumpPostgresGlobals(const config::DatabaseJob& job, const Connection& c, const DumpContext& ctx)
{
    auto tool = locateTool("pg_dumpall", job.tools);
    if (!tool)
        return tool.error();
    DumpArtifact a;
    a.database = "_globals";
    a.dumpFormat = "plain";
    a.toolPath = win::toUtf8(tool.value());
    a.toolVersion = toolVersion(tool.value());

    auto cred = PrivateDirectory::create(win::joinPath(ctx.workDir, L"cred-globals"));
    if (!cred)
        return cred.error();
    auto pass = cred.value().writeFile(L"pgpass", pgpassLine(c.host, c.port, c.user, c.password));
    if (!pass)
        return pass.error();

    const std::wstring raw = win::joinPath(ctx.workDir, L"_globals.sql");
    ProcessSpec spec;
    spec.args = {tool.value(), L"--host=" + w(c.host), L"--port=" + std::to_wstring(c.port), L"--username=" + w(c.user),
                 L"--no-password", L"--globals-only", L"--no-role-passwords", L"--file=" + raw};
    spec.environment = pgEnvironment(c, pass.value());
    spec.timeoutMs = ctx.timeoutSeconds * 1000;
    a.command = sanitizedCommand(spec.args);
    auto r = withRetries(ctx, [&] { return runProcess(spec); });
    if (!r)
        return r.error();
    if (r.value().exitCode != 0)
        return Error::make(ErrorCode::IoError, "pg_dumpall --globals-only falhou: " + tail(r.value().stderrText));

    a.validationMethod = "código de saída";
    a.validationResult = "ok";
    a.stagedFile = raw + L".zst";
    a.extension = ".sql.zst";
    a.compression = "zstd";
    if (auto st = compressFile(raw, a.stagedFile); !st)
        return st.error();
    removeIfExists(raw);
    a.restoreHint = "zstd -d <arquivo> e depois psql -f <arquivo.sql> (papéis e tablespaces)";
    if (auto st = finalize(a); !st)
        return st.error();
    return a;
}

// ---- MySQL / MariaDB -------------------------------------------------------------

Result<DumpArtifact> dumpMysql(const config::DatabaseJob& job, const Connection& c, const std::string& database,
                               const DumpContext& ctx)
{
    Result<std::wstring> tool = job.engine == "mariadb" ? locateTool("mariadb-dump", job.tools)
                                                        : locateTool("mysqldump", job.tools);
    if (!tool && job.engine == "mariadb")
        tool = locateTool("mysqldump", job.tools);
    if (!tool)
        return tool.error();

    DumpArtifact a;
    a.database = database;
    a.dumpFormat = "plain";
    a.toolPath = win::toUtf8(tool.value());
    a.toolVersion = toolVersion(tool.value());

    auto cred = PrivateDirectory::create(win::joinPath(ctx.workDir, L"cred-" + w(safeName(database))));
    if (!cred)
        return cred.error();
    auto defaults = cred.value().writeFile(L"my.cnf", mysqlDefaultsFile(c, job.engine));
    if (!defaults)
        return defaults.error();

    const bool zstd = opt(job, "compression", "zstd") == "zstd";
    const int level = std::stoi(opt(job, "compression_level", "3"));
    a.compression = zstd ? "zstd" : "none";
    a.extension = zstd ? ".sql.zst" : ".sql";
    a.stagedFile = win::joinPath(ctx.workDir, w(safeName(database)) + w(a.extension));

    ProcessSpec spec;
    spec.args = mysqldumpArgs(tool.value(), defaults.value(), job.engine, database, job);
    spec.timeoutMs = ctx.timeoutSeconds * 1000;
    a.command = sanitizedCommand(spec.args);

    SqlStreamInspector insp;
    auto attempt = [&]() -> Result<ProcessResult> {
        removeIfExists(a.stagedFile);
        insp = SqlStreamInspector{};
        std::optional<ZstdFileWriter> zw;
        win::UniqueHandle rawFile;
        if (zstd) {
            auto created = ZstdFileWriter::create(a.stagedFile, level);
            if (!created)
                return created.error();
            zw.emplace(std::move(created).value());
        } else {
            rawFile.reset(::CreateFileW(win::toExtendedPath(a.stagedFile).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                        FILE_ATTRIBUTE_NORMAL, nullptr));
            if (!rawFile)
                return win::lastError("criar arquivo do dump");
        }
        spec.onStdout = [&](const char* data, std::size_t n) -> Status {
            insp.feed(data, n);
            if (zw)
                return zw->write(data, n);
            DWORD written = 0;
            if (!::WriteFile(rawFile.get(), data, static_cast<DWORD>(n), &written, nullptr) || written != n)
                return win::lastError("gravar dump");
            return Status::success();
        };
        auto r = runProcess(spec);
        if (!r)
            return r;
        if (r.value().exitCode != 0 || r.value().timedOut)
            return Error::make(ErrorCode::IoError, r.value().timedOut ? "mysqldump excedeu o tempo limite"
                                                                     : "mysqldump falhou: " + tail(r.value().stderrText));
        if (zw) {
            if (auto st = zw->finish(); !st)
                return st.error();
        } else if (!::FlushFileBuffers(rawFile.get())) {
            return win::lastError("sincronizar dump");
        }
        return r;
    };
    auto run = withRetries(ctx, attempt);
    if (!run) {
        removeIfExists(a.stagedFile);
        return run.error();
    }

    a.validationMethod = "código de saída e marca \"-- Dump completed\"";
    if (insp.lastBytes.find("-- Dump completed") == std::string::npos) {
        removeIfExists(a.stagedFile);
        return Error::make(ErrorCode::Corrupted, "dump do MySQL incompleto (sem a linha \"-- Dump completed\")");
    }
    a.validationResult = "ok";
    a.tables = insp.tables;
    a.tablesKnown = true;
    if (insp.myisam > 0)
        a.warnings.push_back(std::to_string(insp.myisam)
                             + " tabela(s) MyISAM/Aria: --single-transaction não garante um instante único para elas"
                               " (use mysql.myisam_strategy: lock)");
    a.restoreHint = zstd ? "zstd -d <arquivo> e depois mysql <novo_banco> < <arquivo.sql>" : "mysql <novo_banco> < <arquivo>";
    if (auto st = finalize(a); !st)
        return st.error();
    return a;
}

// ---- SQLite ---------------------------------------------------------------------

std::vector<std::wstring> expandSqliteFiles(const std::vector<std::wstring>& patterns)
{
    std::vector<std::wstring> out;
    for (const auto& p : patterns) {
        if (p.find_first_of(L"*?") == std::wstring::npos) {
            out.push_back(p);
            continue;
        }
        const std::wstring dir = win::parentPath(p);
        WIN32_FIND_DATAW fd{};
        HANDLE h = ::FindFirstFileW(win::toExtendedPath(p).c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE)
            continue;
        std::vector<std::wstring> found;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                found.push_back(win::joinPath(dir, fd.cFileName));
        } while (::FindNextFileW(h, &fd));
        ::FindClose(h);
        std::sort(found.begin(), found.end());
        out.insert(out.end(), found.begin(), found.end());
    }
    return out;
}

Result<DumpArtifact> dumpSqlite(const config::DatabaseJob& job, const std::wstring& file, const std::string& logicalName,
                                const DumpContext& ctx)
{
    DumpArtifact a;
    a.database = logicalName;
    a.dumpFormat = "sqlite";
    a.toolPath = "SQLite Online Backup API (embutida)";
    a.command = "backup " + win::toUtf8(file);

    const std::wstring raw = win::joinPath(ctx.workDir, w(safeName(logicalName)) + L".db");
    int busy = 30000;
    if (auto it = job.options.find("busy_timeout"); it != job.options.end())
        busy = std::stoi(it->second) * 1000;

    auto info = withRetries(ctx, [&]() -> Result<SqliteBackupInfo> {
        removeIfExists(raw);
        return backupDatabase(file, raw, busy);
    });
    if (!info)
        return info.error();
    a.tables = info.value().tables;
    a.tablesKnown = true;

    if (opt(job, "integrity_check", "true") == "true") {
        a.validationMethod = "PRAGMA integrity_check";
        auto check = integrityCheck(raw);
        if (!check)
            return check.error();
        if (check.value() != "ok") {
            removeIfExists(raw);
            return Error::make(ErrorCode::Corrupted, "integrity_check da cópia falhou: " + check.value());
        }
    } else {
        a.validationMethod = "backup API concluída";
    }
    a.validationResult = "ok";

    if (opt(job, "compression", "zstd") == "zstd") {
        a.stagedFile = raw + L".zst";
        a.extension = ".db.zst";
        a.compression = "zstd";
        if (auto st = compressFile(raw, a.stagedFile); !st)
            return st.error();
        removeIfExists(raw);
        a.restoreHint = "zstd -d <arquivo> -o <novo.db>";
    } else {
        a.stagedFile = raw;
        a.extension = ".db";
        a.compression = "none";
        a.restoreHint = "copie o arquivo para o nome desejado";
    }
    if (auto st = finalize(a); !st)
        return st.error();
    return a;
}

// ---- Comando personalizado -------------------------------------------------------

Result<DumpArtifact> dumpCustom(const config::DatabaseJob& job, const std::map<std::wstring, std::wstring>& env,
                                const DumpContext& ctx)
{
    DumpArtifact a;
    const std::string outName = opt(job, "output_name", "custom.out");
    a.database = safeName(outName.substr(0, outName.find('.')));
    a.dumpFormat = "command";
    a.toolPath = job.customCommand.empty() ? "" : job.customCommand.front();

    const std::wstring raw = win::joinPath(ctx.workDir, w(safeName(outName)));
    auto vars = ctx.vars;
    vars[L"output"] = raw;
    vars[L"staging"] = ctx.workDir;

    ProcessSpec spec;
    for (const auto& arg : job.customCommand)
        spec.args.push_back(expandVariables(w(arg), vars));
    for (const auto& [k, v] : env)
        spec.environment.emplace_back(k, v);
    spec.timeoutMs = ctx.timeoutSeconds * 1000;
    a.command = sanitizedCommand(spec.args);

    const bool toStdout = opt(job, "output", "file") == "stdout";
    auto run = withRetries(ctx, [&]() -> Result<ProcessResult> {
        removeIfExists(raw);
        win::UniqueHandle outFile;
        if (toStdout) {
            outFile.reset(::CreateFileW(win::toExtendedPath(raw).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                        FILE_ATTRIBUTE_NORMAL, nullptr));
            if (!outFile)
                return win::lastError("criar arquivo de saída");
            spec.onStdout = [&](const char* data, std::size_t n) -> Status {
                DWORD written = 0;
                if (!::WriteFile(outFile.get(), data, static_cast<DWORD>(n), &written, nullptr) || written != n)
                    return win::lastError("gravar saída do comando");
                return Status::success();
            };
        }
        auto r = runProcess(spec);
        if (!r)
            return r;
        const auto& codes = job.successExitCodes;
        if (r.value().timedOut)
            return Error::make(ErrorCode::IoError, "o comando excedeu o tempo limite");
        if (std::find(codes.begin(), codes.end(), static_cast<int>(r.value().exitCode)) == codes.end())
            return Error::make(ErrorCode::IoError, "o comando terminou com código " + std::to_string(r.value().exitCode)
                                                       + ": " + tail(r.value().stderrText));
        if (outFile)
            ::FlushFileBuffers(outFile.get());
        return r;
    });
    if (!run)
        return run.error();
    if (!ops::pathExists(raw))
        return Error::make(ErrorCode::NotFound, "o comando terminou sem gerar o arquivo esperado (" + win::toUtf8(raw) + ")");

    if (!job.customValidate.empty()) {
        ProcessSpec v;
        for (const auto& arg : job.customValidate)
            v.args.push_back(expandVariables(w(arg), vars));
        for (const auto& [k, val] : env)
            v.environment.emplace_back(k, val);
        v.timeoutMs = ctx.timeoutSeconds * 1000;
        auto r = runProcess(v);
        a.validationMethod = "comando de validação";
        if (!r)
            return r.error();
        if (r.value().exitCode != 0)
            return Error::make(ErrorCode::Corrupted, "o comando de validação rejeitou a saída: " + tail(r.value().stderrText));
    } else {
        a.validationMethod = "código de saída";
    }
    a.validationResult = "ok";

    if (opt(job, "compression", "zstd") == "zstd") {
        a.stagedFile = raw + L".zst";
        a.extension = "." + outName + ".zst";
        a.compression = "zstd";
        if (auto st = compressFile(raw, a.stagedFile); !st)
            return st.error();
        removeIfExists(raw);
    } else {
        a.stagedFile = raw;
        a.extension = "." + outName;
        a.compression = "none";
    }
    a.restoreHint = job.customRestore.empty() ? "use o procedimento de restauração do fornecedor"
                                              : "synkro-worker restore-db (executa custom.restore.command)";
    if (auto st = finalize(a); !st)
        return st.error();
    return a;
}

} // namespace synkro::db
