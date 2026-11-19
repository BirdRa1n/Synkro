// Ferramenta falsa para os testes de backup de bancos. O mesmo executável é
// copiado com os nomes pg_dump.exe, pg_restore.exe, pg_dumpall.exe, psql.exe,
// createdb.exe, mysqldump.exe e mysql.exe e se comporta conforme o próprio
// nome e as variáveis FAKE_*:
//
//   FAKE_TABLES            tabelas do "banco" (separadas por vírgula)
//   FAKE_DATABASES         bancos listados por psql/mysql
//   FAKE_EXPECT_PASSWORD   senha que precisa chegar pelo arquivo de credenciais
//   FAKE_FAIL              nome da ferramenta que deve falhar
//   FAKE_TRUNCATE          mysqldump omite a linha final "-- Dump completed"
//   FAKE_MYISAM            mysqldump declara tabelas MyISAM
//   FAKE_PADDING           bytes extras no dump (para testar tamanho)
//   FAKE_LOG               arquivo onde cada chamada é registrada
//   FAKE_RESTORE_OUT       arquivo que recebe a entrada padrão do mysql
//   FAKE_RESTORED_TABLES   tabelas "encontradas" no banco restaurado (padrão: FAKE_TABLES)
//   FAKE_CHECK_RESULT      resposta das consultas de verificação (padrão: t / 1)
//
// A trava no servidor (pg_try_advisory_lock / GET_LOCK) é simulada com um
// mutex nomeado do Windows: exclusiva entre processos e liberada quando o
// processo que a segura termina — como a sessão no servidor real.

#include <windows.h>

#include <fcntl.h>
#include <io.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::string env(const char* name)
{
    const char* v = std::getenv(name);
    return v ? v : "";
}

std::string narrow(const wchar_t* s)
{
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), n, nullptr, nullptr);
    return out;
}

std::vector<std::string> split(const std::string& s)
{
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ','))
        if (!item.empty())
            out.push_back(item);
    return out;
}

std::string readFile(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string value(const std::vector<std::string>& args, const std::string& prefix)
{
    for (const auto& a : args)
        if (a.rfind(prefix, 0) == 0)
            return a.substr(prefix.size());
    return {};
}

bool has(const std::vector<std::string>& args, const std::string& a)
{
    for (const auto& x : args)
        if (x == a)
            return true;
    return false;
}

void log(const std::string& tool, const std::vector<std::string>& args)
{
    const std::string path = env("FAKE_LOG");
    if (path.empty())
        return;
    std::ofstream f(path, std::ios::app | std::ios::binary);
    f << tool;
    for (const auto& a : args)
        f << " | " << a;
    f << "\n";
}

int fail(const std::string& msg, int code = 1)
{
    std::fprintf(stderr, "%s\n", msg.c_str());
    return code;
}

// A senha precisa ter chegado pelo arquivo de credenciais, nunca pelos argumentos.
bool passwordOk(const std::string& credentialFile, const std::vector<std::string>& args)
{
    const std::string expected = env("FAKE_EXPECT_PASSWORD");
    if (expected.empty())
        return true;
    for (const auto& a : args)
        if (a.find(expected) != std::string::npos)
            return false;
    return readFile(credentialFile).find(expected) != std::string::npos;
}

std::vector<std::string> values(const std::vector<std::string>& args, const std::string& flag)
{
    std::vector<std::string> out;
    for (size_t i = 0; i + 1 < args.size(); ++i)
        if (args[i] == flag)
            out.push_back(args[i + 1]);
    return out;
}

// Trava simulada: devolve true se obteve. O mutex fica com o processo.
bool takeLock(const std::string& sql)
{
    const size_t a = sql.find('\'');
    const size_t b = sql.find('\'', a + 1);
    std::string name = "fake-synkro-lock-" + (a == std::string::npos ? std::string("x") : sql.substr(a + 1, b - a - 1));
    for (char& c : name)
        if (c == ':' || c == '\\')
            c = '_';
    HANDLE m = ::CreateMutexA(nullptr, FALSE, ("Local\\" + name).c_str());
    return m && ::GetLastError() != ERROR_ALREADY_EXISTS;
}

void holdSession()
{
    std::fflush(stdout);
    ::Sleep(60000); // encerrado pelo Synkro antes disso
}

std::string quoted(const std::string& sql, const std::string& after)
{
    const size_t p = sql.find(after);
    if (p == std::string::npos)
        return {};
    const size_t a = sql.find('\'', p + after.size() - 1);
    const size_t b = sql.find('\'', a + 1);
    return sql.substr(a + 1, b - a - 1);
}

// "host/porta:C:\caminho.fdb" -> "C:\caminho.fdb"
std::string fbPath(const std::string& conn)
{
    const size_t slash = conn.find('/');
    const size_t colon = conn.find(':', slash == std::string::npos ? 0 : slash);
    return slash != std::string::npos && colon != std::string::npos ? conn.substr(colon + 1) : conn;
}

std::string tableLine(const std::string& content)
{
    const size_t pos = content.find("TABLES=");
    return pos == std::string::npos ? std::string() : content.substr(pos + 7, content.find('\n', pos) - pos - 7);
}

bool exists(const std::string& path)
{
    return ::GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::string restoredTables(const std::vector<std::string>& tables, const std::string& prefix)
{
    const std::string explicitList = env("FAKE_RESTORED_TABLES");
    std::string out;
    for (const auto& t : explicitList.empty() ? tables : split(explicitList))
        out += (explicitList.empty() ? prefix : std::string()) + t + "\n";
    return out;
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    _setmode(_fileno(stdout), _O_BINARY);
    wchar_t self[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::string tool = narrow(self);
    tool = tool.substr(tool.find_last_of('\\') + 1);
    tool = tool.substr(0, tool.rfind('.'));

    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i)
        args.push_back(narrow(argv[i]));
    log(tool, args);

    if (has(args, "--version")) {
        std::printf("%s (Fake) 16.4\n", tool.c_str());
        return 0;
    }
    if (tool == "gbak" && has(args, "-z")) {
        std::printf("gbak:gbak version WI-V3.0.10 Firebird 3.0 (Fake)\n");
        return 1; // como o real: sem arquivos, termina com erro
    }
    if (tool == "sqlcmd" && has(args, "-?")) {
        std::printf("Microsoft (R) SQL Server Command Line Tool\nVersion 15.0.4298.1 NT (Fake)\n");
        return 0;
    }
    if (env("FAKE_FAIL") == tool)
        return fail(tool + ": connection to server failed: FATAL: password authentication failed", 2);

    const auto tables = split(env("FAKE_TABLES"));

    if (tool == "pg_dump") {
        if (!passwordOk(env("PGPASSFILE"), args))
            return fail("pg_dump: error: password authentication failed", 2);
        const std::string out = value(args, "--file=");
        std::ofstream f(out, std::ios::binary);
        f << "PGDMP\nTABLES=";
        for (size_t i = 0; i < tables.size(); ++i)
            f << (i ? "," : "") << tables[i];
        f << "\n" << std::string(static_cast<size_t>(std::atoi(env("FAKE_PADDING").c_str())), 'x');
        return 0;
    }
    if (tool == "pg_restore") {
        if (has(args, "--list")) {
            const std::string content = readFile(args.back());
            if (content.rfind("PGDMP", 0) != 0)
                return fail("pg_restore: error: input file does not appear to be a valid archive");
            const size_t pos = content.find("TABLES=");
            const std::string list = content.substr(pos + 7, content.find('\n', pos) - pos - 7);
            int n = 100;
            for (const auto& t : split(list))
                std::printf("%d; 0 %d TABLE DATA public %s postgres\n", n, n + 1, t.c_str()), ++n;
            return 0;
        }
        return passwordOk(env("PGPASSFILE"), args) ? 0 : fail("pg_restore: password authentication failed");
    }
    if (tool == "pg_dumpall") {
        std::ofstream f(value(args, "--file="), std::ios::binary);
        f << "-- globals\nCREATE ROLE app;\n";
        return 0;
    }
    if (tool == "createdb") {
        if (args.back() == "existe")
            return fail("createdb: error: database creation failed: ERROR:  database \"existe\" already exists");
        return passwordOk(env("PGPASSFILE"), args) ? 0 : fail("createdb: password authentication failed");
    }
    if (tool == "psql") {
        if (!value(args, "--file=").empty())
            return 0;
        const auto cmds = values(args, "-c");
        if (!cmds.empty() && cmds[0].find("pg_try_advisory_lock") != std::string::npos) {
            if (!passwordOk(env("PGPASSFILE"), args))
                return fail("psql: error: password authentication failed", 2);
            std::printf("%s\n", takeLock(cmds[0]) ? "t" : "f");
            holdSession();
            return 0;
        }
        if (!cmds.empty() && cmds[0].find("information_schema.tables") != std::string::npos) {
            std::printf("%s", restoredTables(tables, "public.").c_str());
            return 0;
        }
        if (!cmds.empty() && cmds[0].rfind("DROP DATABASE", 0) == 0)
            return 0;
        if (!cmds.empty() && cmds[0].find("pg_database") == std::string::npos) {
            const std::string r = env("FAKE_CHECK_RESULT");
            std::printf("%s\n", r.empty() ? "t" : r.c_str());
            return 0;
        }
        for (const auto& d : split(env("FAKE_DATABASES")))
            std::printf("%s\n", d.c_str());
        return 0;
    }
    if (tool == "mysqldump" || tool == "mariadb-dump") {
        if (!passwordOk(value(args, "--defaults-extra-file="), args))
            return fail("mysqldump: Got error: 1045: Access denied for user", 2);
        if (args.size() < 1 || args[0].rfind("--defaults-extra-file=", 0) != 0)
            return fail("mysqldump: --defaults-extra-file precisa ser a primeira opção");
        std::printf("-- MySQL dump 10.13\n");
        for (const auto& t : tables) {
            std::printf("CREATE TABLE `%s` (\n  `id` int NOT NULL\n) ENGINE=%s;\n", t.c_str(),
                        env("FAKE_MYISAM").empty() ? "InnoDB" : "MyISAM");
            std::printf("INSERT INTO `%s` VALUES (1),(2);\n", t.c_str());
        }
        if (env("FAKE_TRUNCATE").empty())
            std::printf("-- Dump completed on 2026-11-18 22:00:00\n");
        return 0;
    }
    if (tool == "mysql" || tool == "mariadb") {
        const std::string e = has(args, "-e") ? args[static_cast<size_t>(std::find(args.begin(), args.end(), "-e") - args.begin()) + 1] : "";
        if (e == "SHOW DATABASES") {
            std::printf("information_schema\nmysql\n");
            for (const auto& d : split(env("FAKE_DATABASES")))
                std::printf("%s\n", d.c_str());
            std::printf("sys\n");
            return 0;
        }
        if (e.find("CREATE DATABASE `existe`") != std::string::npos)
            return fail("ERROR 1007 (HY000): Can't create database 'existe'; database exists");
        if (e.find("GET_LOCK") != std::string::npos) {
            std::printf("%s\n", takeLock(e) ? "1" : "0");
            holdSession();
            return 0;
        }
        if (e.rfind("SHOW TABLES", 0) == 0) {
            std::printf("%s", restoredTables(tables, "").c_str());
            return 0;
        }
        if (e.rfind("SELECT", 0) == 0 && !value(args, "--database=").empty()) {
            const std::string r = env("FAKE_CHECK_RESULT");
            std::printf("%s\n", r.empty() ? "1" : r.c_str());
            return 0;
        }
        if (!e.empty())
            return 0;
        // Restauração: grava a entrada padrão.
        std::ofstream out(env("FAKE_RESTORE_OUT"), std::ios::binary);
        char buf[4096];
        size_t n = 0;
        while ((n = std::fread(buf, 1, sizeof(buf), stdin)) > 0)
            out.write(buf, static_cast<std::streamsize>(n));
        return 0;
    }
    if (tool == "gbak") {
        const std::string passFile = values(args, "-fetch_password").empty() ? "" : values(args, "-fetch_password")[0];
        if (!passwordOk(passFile, args))
            return fail("gbak: ERROR:Your user name and password are not defined.", 1);
        const std::string src = args[args.size() - 2], dst = args.back();
        if (has(args, "-b")) {
            std::ofstream f(dst, std::ios::binary);
            f << "GBAK\nTABLES=";
            for (size_t i = 0; i < tables.size(); ++i)
                f << (i ? "," : "") << tables[i];
            f << "\n" << std::string(static_cast<size_t>(std::atoi(env("FAKE_PADDING").c_str())), 'x');
            const auto logs = values(args, "-y");
            if (!logs.empty()) {
                if (exists(logs[0]))
                    return fail("gbak: ERROR:cannot open status and error output file " + logs[0]);
                std::ofstream log(logs[0], std::ios::binary);
                log << "gbak:readied database " << fbPath(src) << " for backup\n";
                for (const auto& t : tables)
                    log << "gbak:    writing data for table " << t << "\ngbak:       2 records written\n";
                if (env("FAKE_TRUNCATE").empty())
                    log << "gbak:closing file, committing, and finishing. 1024 bytes written\n";
            }
            return 0;
        }
        if (has(args, "-c")) {
            const std::string content = readFile(src);
            if (content.rfind("GBAK", 0) != 0)
                return fail("gbak: ERROR:expected backup description record");
            const std::string target = fbPath(dst);
            if (exists(target))
                return fail("gbak: ERROR:database " + target + " already exists.  To replace it, use the -REP switch");
            std::ofstream f(target, std::ios::binary);
            f << "FDB\nTABLES=" << tableLine(content) << "\n";
            return 0;
        }
        return fail("gbak: ERROR:switches can't be used together");
    }
    if (tool == "isql") {
        const std::string expected = env("FAKE_EXPECT_PASSWORD");
        if (!expected.empty() && (env("ISC_PASSWORD") != expected || has(args, expected)))
            return fail("Statement failed, SQLSTATE = 28000\nYour user name and password are not defined.", 1);
        const auto scripts = values(args, "-i");
        const std::string script = scripts.empty() ? "" : readFile(scripts[0]);
        const std::string path = fbPath(args.back());
        if (!exists(path))
            return fail("Statement failed, SQLSTATE = 08001\nI/O error during \"CreateFile (open)\" operation for file \"" + path + "\"");
        if (script.find("RDB$RELATIONS") != std::string::npos) {
            const std::string list = env("FAKE_RESTORED_TABLES").empty() ? tableLine(readFile(path)) : env("FAKE_RESTORED_TABLES");
            for (const auto& t : split(list))
                std::printf("%-31s\n", t.c_str()); // isql completa com espaços
            return 0;
        }
        if (script.find("DROP DATABASE") != std::string::npos) {
            ::DeleteFileA(path.c_str());
            return 0;
        }
        const std::string r = env("FAKE_CHECK_RESULT");
        std::printf("%12s\n", r.empty() ? "<true>" : r.c_str());
        return 0;
    }
    if (tool == "sqlcmd") {
        const std::string expected = env("FAKE_EXPECT_PASSWORD");
        if (!has(args, "-E") && !expected.empty() && (env("SQLCMDPASSWORD") != expected || has(args, expected)))
            return fail("Sqlcmd: Error: Microsoft ODBC Driver 17 for SQL Server : Login failed for user.", 1);
        const auto qs = values(args, "-Q");
        const std::string q = qs.empty() ? "" : qs[0];
        const std::string dir = env("FAKE_MSSQL_DIR");
        if (q.find("InstanceDefaultBackupPath") != std::string::npos) {
            std::printf("%s\n", dir.empty() ? "NULL" : dir.c_str());
            return 0;
        }
        if (q.find("InstanceDefaultDataPath") != std::string::npos) {
            std::printf("%s|%s\n", dir.c_str(), dir.c_str());
            return 0;
        }
        if (q.find("sys.databases") != std::string::npos) {
            for (const auto& d : split(env("FAKE_DATABASES")))
                std::printf("%s\n", d.c_str());
            return 0;
        }
        if (q.find("sys.tables") != std::string::npos) {
            std::printf("%s", restoredTables(tables, "dbo.").c_str());
            return 0;
        }
        if (q.find("BACKUP DATABASE") != std::string::npos) {
            std::ofstream f(quoted(q, "DISK = N'"), std::ios::binary);
            if (!f)
                return fail("Msg 3201, Level 16: Cannot open backup device. Operating system error 5(Access is denied.).");
            f << "MSSQLBAK\nTABLES=";
            for (size_t i = 0; i < tables.size(); ++i)
                f << (i ? "," : "") << tables[i];
            f << "\n" << std::string(static_cast<size_t>(std::atoi(env("FAKE_PADDING").c_str())), 'x');
            return 0;
        }
        if (q.find("RESTORE VERIFYONLY") != std::string::npos) {
            if (readFile(quoted(q, "DISK = N'")).rfind("MSSQLBAK", 0) != 0)
                return fail("Msg 3241, Level 16: The media family on device is incorrectly formed.");
            std::printf("The backup set on file 1 is valid.\n");
            return 0;
        }
        if (q.find("RESTORE FILELISTONLY") != std::string::npos) {
            std::printf("pdv|C:\\dados\\pdv.mdf|D|PRIMARY|1048576\npdv_log|C:\\dados\\pdv_log.ldf|L|NULL|524288\n");
            return 0;
        }
        if (q.find("RESTORE DATABASE") != std::string::npos)
            return readFile(quoted(q, "DISK = N'")).rfind("MSSQLBAK", 0) == 0 ? 0 : fail("Msg 3241: incorrectly formed");
        if (q.find("DROP DATABASE") != std::string::npos)
            return 0;
        if (q.find("DB_ID(") != std::string::npos) {
            std::printf("%s\n", q.find("N'existe'") != std::string::npos ? "1" : "0");
            return 0;
        }
        const std::string r = env("FAKE_CHECK_RESULT");
        std::printf("%s\n", r.empty() ? "1" : r.c_str());
        return 0;
    }
    return fail("ferramenta falsa desconhecida: " + tool);
}
