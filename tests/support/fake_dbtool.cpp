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
    return fail("ferramenta falsa desconhecida: " + tool);
}
