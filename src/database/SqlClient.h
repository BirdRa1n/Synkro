#pragma once

// Cliente de linha de comando (psql / mysql) para consultas curtas e para a
// trava no servidor. As credenciais vão sempre por arquivo numa pasta
// privada (PGPASSFILE / --defaults-extra-file), nunca pelos argumentos.

#include "config/Config.h"
#include "core/Result.h"
#include "database/Drivers.h"

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace synkro::db {

class PrivateDirectory;

struct SqlTarget {
    std::string engine; // postgresql | mysql | mariadb
    std::map<std::string, std::wstring> tools;
    Connection connection;
};

// Executa `sql` no banco `database` e devolve a saída sem cabeçalhos
// (psql -At / mysql -N -B), uma linha por registro.
Result<std::string> runSql(const SqlTarget& target, const std::string& database, const std::string& sql,
                           const std::wstring& workDir, std::int64_t timeoutSeconds = 300);

// "host/porta:caminho" do Firebird (o caminho é como o servidor o enxerga).
std::string firebirdConnectionString(const Connection& c, const std::string& database);

// Argumentos comuns do sqlcmd (servidor, autenticação, saída sem cabeçalho).
// A senha nunca entra aqui: vai em SQLCMDPASSWORD no ambiente do filho.
std::vector<std::wstring> sqlcmdArgs(const std::wstring& tool, const Connection& c, const std::string& database);

// Linhas não vazias de uma saída de runSql.
std::vector<std::string> outputLines(const std::string& output);

// Identificador citado para o motor ("x" no PostgreSQL, `x` no MySQL).
std::string quoteIdentifier(const std::string& engine, const std::string& name);

// Trava no servidor de banco (database-backup.md §4A.3): impede que duas
// máquinas façam o dump do mesmo banco ao mesmo tempo. Uma sessão do cliente
// fica aberta segurando pg_try_advisory_lock / GET_LOCK; ao destruir o
// objeto o processo é encerrado e o próprio servidor solta a trava (também
// se o worker morrer: o Job Object encerra o cliente junto).
class ServerLock {
public:
    // Erro AlreadyExists = outra máquina está com a trava.
    static Result<std::unique_ptr<ServerLock>> acquire(const SqlTarget& target, const std::string& database,
                                                       const std::wstring& workDir, std::int64_t holdSeconds);

    // Nome da trava: "synkro:<banco>" (até 64 caracteres, limite do MySQL).
    static std::string lockName(const std::string& database);

    ServerLock(const ServerLock&) = delete;
    ServerLock& operator=(const ServerLock&) = delete;
    ~ServerLock();

private:
    ServerLock() = default;
    void release();

    void* m_cancel = nullptr;
    std::thread m_thread;
    std::unique_ptr<PrivateDirectory> m_cred;

    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::string m_stdout;
    bool m_gotLine = false;
    bool m_exited = false;
    std::string m_stderr;
    std::string m_startError;
};

} // namespace synkro::db
