#include "config/ConfigLoader.h"
#include "crypto/VaultKey.h"
#include "database/DatabaseBackup.h"
#include "database/DatabaseRestore.h"
#include "database/Drivers.h"
#include "database/DumpStore.h"
#include "database/RestoreTest.h"
#include "database/SqlClient.h"
#include "database/Sqlite.h"
#include "database/Tools.h"
#include "database/Zstd.h"
#include "process/ProcessRunner.h"
#include "engine/VaultOps.h"
#include "secrets/SecretStore.h"
#include "vault/Vault.h"

#include "TestFs.h"

#include <QTemporaryDir>
#include <QTest>
#include <QThread>

using namespace synkro;
using namespace synkro::db;
using namespace synkro::test;

namespace {

constexpr char kPassword[] = "S3nh@-Ultra-Secreta";

struct Env {
    QTemporaryDir dir;
    std::wstring root = nativePath(dir.path());
    std::wstring tools = win::joinPath(root, L"tools");
    std::wstring vaultA = win::joinPath(root, L"discoA");
    std::wstring vaultB = win::joinPath(root, L"discoB");
    std::wstring staging = win::joinPath(root, L"staging");
    std::wstring secrets = win::joinPath(root, L"secrets.dat");
    std::wstring log = win::joinPath(root, L"fake.log");

    Env()
    {
        (void)win::createDirectories(tools);
        for (const wchar_t* name : {L"pg_dump", L"pg_restore", L"pg_dumpall", L"psql", L"createdb", L"mysqldump", L"mysql", L"gbak", L"isql", L"sqlcmd"})
            ::CopyFileW(QString::fromUtf8(FAKE_DBTOOL).replace('/', '\\').toStdWString().c_str(),
                        win::joinPath(tools, std::wstring(name) + L".exe").c_str(), FALSE);
        for (const auto& v : {vaultA, vaultB}) {
            (void)win::createDirectories(v);
            (void)vault::initialize(v, QStringLiteral("cofre"));
        }
        (void)SecretStore(secrets).set("db", kPassword);
        qputenv("FAKE_EXPECT_PASSWORD", kPassword);
        qputenv("FAKE_LOG", QString::fromStdWString(log).toUtf8());
        qputenv("FAKE_TABLES", "clientes,vendas");
        for (const char* v : {"FAKE_FAIL", "FAKE_TRUNCATE", "FAKE_MYISAM", "FAKE_PADDING", "FAKE_DATABASES",
                              "FAKE_RESTORED_TABLES", "FAKE_CHECK_RESULT", "FAKE_RESTORE_OUT", "FAKE_MSSQL_DIR"})
            qunsetenv(v);
    }

    config::JobConfig job(const std::string& yaml) const
    {
        config::GlobalConfig g;
        std::vector<config::Diagnostic> d;
        auto j = config::parseJobDocument(yaml, L"jobs\\t.yml", g, d);
        for (const auto& x : d) {
            if (x.severity == config::Severity::Error && x.key != "vaults")
                qWarning("%s", config::formatDiagnostic(x).c_str());
        }
        return j;
    }

    DatabaseBackupRequest request(const config::JobConfig& j, std::vector<std::wstring> vaults = {}) const
    {
        DatabaseBackupRequest r;
        r.jobId = win::fromUtf8(j.id);
        r.job = j.database;
        for (const wchar_t* t : {L"pg_dump", L"pg_restore", L"pg_dumpall", L"psql", L"createdb", L"mysqldump", L"mysql", L"gbak", L"isql", L"sqlcmd"})
            r.job.tools[win::toUtf8(t)] = win::joinPath(tools, std::wstring(t) + L".exe");
        if (vaults.empty())
            vaults = {vaultA};
        for (auto& v : vaults)
            r.vaults.push_back({v, {}, {}});
        r.secretsFile = secrets;
        r.stagingRoot = staging;
        r.retryDelaySeconds = 0;
        return r;
    }

    std::string pgJob(const char* extra = "") const
    {
        return std::string("version: 1\nid: pg\nname: pg\ntype: database\nengine: postgresql\n"
                           "connection: { host: localhost, port: 5432, user: synkro_backup, password: { secret: db } }\n"
                           "databases: [pdv_vendas]\n")
               + extra;
    }
};

QByteArray describe(const BackupResult& r)
{
    QByteArray s = QByteArray::fromStdString(r.error);
    for (const auto& v : r.vaults) {
        s += " | " + QByteArray(nameOf(v.status)) + " " + QByteArray::fromStdString(v.error);
        for (const auto& [p, e] : v.fileErrors)
            s += " [" + QString::fromStdWString(p).toUtf8() + ": " + QByteArray::fromStdString(e) + "]";
    }
    return s;
}

QByteArray decompress(const std::wstring& path, const std::wstring& tmp)
{
    ::DeleteFileW(tmp.c_str());
    if (!decompressFile(path, tmp))
        return {};
    return readFile(tmp);
}

} // namespace

class TstDatabase : public QObject {
    Q_OBJECT

private slots:
    void argumentBuilders()
    {
        Connection c{"srv", 5433, "u", kPassword, "require", {}, 15};
        config::DatabaseJob j;
        j.options["format"] = "custom";
        j.options["compress"] = "6";
        j.extraArgs = {"--no-comments"};
        const auto pg = pgDumpArgs(L"pg_dump.exe", c, "vendas", L"C:\\out.dump", j, {"public.log_*"});
        const QString pgLine = QString::fromStdWString(buildCommandLine(pg));
        QVERIFY(pgLine.contains("--format=custom") && pgLine.contains("--exclude-table=public.log_*"));
        QVERIFY(pgLine.contains("--no-password") && pgLine.endsWith("vendas"));
        QVERIFY(!pgLine.contains(kPassword));

        config::DatabaseJob m;
        const auto my = mysqldumpArgs(L"mysqldump.exe", L"C:\\cred\\my.cnf", "mysql", "retaguarda", m);
        QCOMPARE(QString::fromStdWString(my[1]), QStringLiteral("--defaults-extra-file=C:\\cred\\my.cnf"));
        const QString myLine = QString::fromStdWString(buildCommandLine(my));
        QVERIFY(myLine.contains("--single-transaction") && myLine.contains("--set-gtid-purged=OFF"));
        QVERIFY(!myLine.contains("--databases"));
        QVERIFY(!QString::fromStdWString(buildCommandLine(mysqldumpArgs(L"x", L"y", "mariadb", "db", m))).contains("gtid"));

        m.options["myisam_strategy"] = "lock";
        const QString lockLine = QString::fromStdWString(buildCommandLine(mysqldumpArgs(L"x", L"y", "mysql", "db", m)));
        QVERIFY(lockLine.contains("--lock-tables") && !lockLine.contains("--single-transaction"));

        const std::string cnf = mysqlDefaultsFile(c, "mysql");
        QVERIFY(cnf.find("ssl-mode=REQUIRE") != std::string::npos);
        QCOMPARE(pgpassLine("h", 5432, "u", "a:b\\c"), std::string("h:5432:*:u:a\\:b\\\\c\n"));
    }

    void gfsSelection()
    {
        const QDateTime now(QDate(2026, 11, 18), QTime(23, 0));
        std::vector<QDateTime> times;
        for (int i = 0; i < 400; ++i)
            times.push_back(now.addDays(-i)); // um dump por dia, do mais novo ao mais antigo
        GfsPolicy p{7, 14, 8, 12};
        const auto keep = selectGfs(times, p, now);
        // 14 diários (inclui os 7 últimos) + semanais e mensais além da janela diária.
        QVERIFY(keep.size() >= 14 && keep.size() <= 14 + 8 + 12);
        QCOMPARE(keep.front(), size_t(0));
        QVERIFY(std::find(keep.begin(), keep.end(), size_t(399)) == keep.end()); // além de 12 meses
        QVERIFY(std::find(keep.begin(), keep.end(), size_t(13)) != keep.end());  // ainda na janela diária
    }

    void zstdRoundTrip()
    {
        QTemporaryDir dir;
        const std::wstring root = nativePath(dir.path());
        QByteArray data;
        for (int i = 0; i < 50000; ++i)
            data += "INSERT INTO vendas VALUES (" + QByteArray::number(i) + ");\n";
        QVERIFY(writeFile(win::joinPath(root, L"a.sql"), data));
        QVERIFY(compressFile(win::joinPath(root, L"a.sql"), win::joinPath(root, L"a.sql.zst")).ok());
        QVERIFY(readFile(win::joinPath(root, L"a.sql.zst")).size() < data.size() / 5);
        QCOMPARE(decompress(win::joinPath(root, L"a.sql.zst"), win::joinPath(root, L"b.sql")), data);

        // Arquivo truncado é detectado.
        QByteArray zst = readFile(win::joinPath(root, L"a.sql.zst"));
        zst.chop(10);
        QVERIFY(writeFile(win::joinPath(root, L"t.zst"), zst));
        QVERIFY(!decompressFile(win::joinPath(root, L"t.zst"), win::joinPath(root, L"t.sql")).ok());
    }

    void postgresBackupToTwoVaults()
    {
        Env env;
        auto j = env.job(env.pgJob("postgresql: { include_globals: true }\n"));
        auto r = runDatabaseBackup(env.request(j, {env.vaultA, env.vaultB}));
        QVERIFY2(r.status == RunStatus::Success, describe(r).constData());
        QCOMPARE(r.vaults[0].added, 2ull); // pdv_vendas + _globals

        for (const auto& v : {env.vaultA, env.vaultB}) {
            const vault::JobLayout L(v, L"pg");
            const auto dumps = listDumps(L, "pdv_vendas");
            QCOMPARE(dumps.size(), size_t(1));
            const DumpRecord& d = dumps.front();
            QCOMPARE(d.validationResult, std::string("ok"));
            QCOMPARE(d.tables, (std::vector<std::string>{"public.clientes", "public.vendas"}));
            const std::wstring file = win::joinPath(L.root(), win::fromCatalogPath(d.relativePath));
            QCOMPARE(Sha256::ofFile(file).value(), d.sha256);
            QVERIFY(ops::pathExists(metadataPathFor(file)));
            QVERIFY(ops::pathExists(win::joinPath(L.root(), L"dumps.jsonl")));
            QCOMPARE(listDumps(L, "_globals").size(), size_t(1));
        }

        // Nenhum segredo vaza para os metadados, resumos ou logs; o preparo foi apagado.
        const vault::JobLayout L(env.vaultA, L"pg");
        for (const auto& d : listDumps(L))
            QVERIFY(!readFile(metadataPathFor(win::joinPath(L.root(), win::fromCatalogPath(d.relativePath)))).contains(kPassword));
        QVERIFY(!readFile(L.summaryFile(r.runId)).contains(kPassword));
        QVERIFY(!readFile(env.log).contains(kPassword));
        QVERIFY(ops::listSubdirectories(env.staging).empty());
    }

    void wrongPasswordFailsWithoutLeaking()
    {
        Env env;
        (void)SecretStore(env.secrets).set("db", "senha-errada");
        auto r = runDatabaseBackup(env.request(env.job(env.pgJob())));
        QCOMPARE(r.status, RunStatus::Failed);
        QVERIFY(describe(r).contains("password authentication failed"));
        QVERIFY(!describe(r).contains("senha-errada"));
        QVERIFY(listDumps(vault::JobLayout(env.vaultA, L"pg")).empty());
    }

    void missingSecretFailsEarly()
    {
        Env env;
        auto j = env.job(env.pgJob());
        j.database.password.name = "nao-cadastrado";
        auto r = runDatabaseBackup(env.request(j));
        QCOMPARE(r.status, RunStatus::Failed);
        QVERIFY(r.error.find("nao-cadastrado") != std::string::npos);
    }

    void emptyDatabaseIsBlockedAndOldDumpKept()
    {
        Env env;
        auto j = env.job(env.pgJob());
        QVERIFY(runDatabaseBackup(env.request(j)).status == RunStatus::Success);
        QThread::msleep(1100);
        qputenv("FAKE_TABLES", "");
        auto r = runDatabaseBackup(env.request(j));
        QCOMPARE(r.status, RunStatus::Blocked);
        QCOMPARE(r.vaults[0].guard.code, GuardCode::EmptySource);
        QCOMPARE(listDumps(vault::JobLayout(env.vaultA, L"pg"), "pdv_vendas").size(), size_t(1));
    }

    void missingTablesAndSizeDropWarn()
    {
        Env env;
        auto j = env.job(env.pgJob());
        qputenv("FAKE_PADDING", "100000");
        QVERIFY(runDatabaseBackup(env.request(j)).status == RunStatus::Success);
        QThread::msleep(1100);
        qputenv("FAKE_PADDING", "0");
        qputenv("FAKE_TABLES", "clientes");
        auto r = runDatabaseBackup(env.request(j));
        QCOMPARE(r.status, RunStatus::Success); // on_missing_tables: warn (padrão)
        bool missing = false, smaller = false;
        for (const auto& w : r.warnings) {
            missing = missing || w.find("sumiram") != std::string::npos;
            smaller = smaller || w.find("nenhum dump antigo será apagado") != std::string::npos;
        }
        QVERIFY(missing);
        QVERIFY(smaller);
        const auto dumps = listDumps(vault::JobLayout(env.vaultA, L"pg"), "pdv_vendas");
        QCOMPARE(dumps.size(), size_t(2));
        QVERIFY(dumps.front().suspicious);
    }

    void serverDownRetriesThenFails()
    {
        Env env;
        qputenv("FAKE_FAIL", "pg_dump");
        auto j = env.job(env.pgJob("dump_retries: 2\n"));
        auto r = runDatabaseBackup(env.request(j));
        QCOMPARE(r.status, RunStatus::Failed);
        QCOMPARE(readFile(env.log).count("pg_dump | --host="), 3); // 1 + 2 novas tentativas
    }

    void allDatabasesListedFromServer()
    {
        Env env;
        qputenv("FAKE_DATABASES", "a,b,c");
        auto j = env.job("version: 1\nid: pg\nname: pg\ntype: database\nengine: postgresql\n"
                         "connection: { host: localhost, user: u, password: { secret: db } }\ndatabases: all\n");
        auto r = runDatabaseBackup(env.request(j));
        QVERIFY2(r.status == RunStatus::Success, describe(r).constData());
        QCOMPARE(r.vaults[0].added, 3ull);
    }

    void mysqlBackupStreamsCompressed()
    {
        Env env;
        auto j = env.job("version: 1\nid: my\nname: my\ntype: database\nengine: mysql\n"
                         "connection: { host: 127.0.0.1, user: u, password: { secret: db } }\ndatabases: [retaguarda]\n");
        auto r = runDatabaseBackup(env.request(j));
        QVERIFY2(r.status == RunStatus::Success, describe(r).constData());
        const vault::JobLayout L(env.vaultA, L"my");
        const auto dumps = listDumps(L, "retaguarda");
        QCOMPARE(dumps.size(), size_t(1));
        QCOMPARE(dumps.front().compression, std::string("zstd"));
        QCOMPARE(dumps.front().tables.size(), size_t(2));
        const QByteArray sql = decompress(win::joinPath(L.root(), win::fromCatalogPath(dumps.front().relativePath)),
                                          win::joinPath(env.root, L"out.sql"));
        QVERIFY(sql.contains("CREATE TABLE `vendas`") && sql.contains("-- Dump completed"));
    }

    void mysqlTruncatedDumpRejected()
    {
        Env env;
        qputenv("FAKE_TRUNCATE", "1");
        auto j = env.job("version: 1\nid: my\nname: my\ntype: database\nengine: mysql\n"
                         "connection: { host: 127.0.0.1, user: u, password: { secret: db } }\ndatabases: [x]\n");
        auto r = runDatabaseBackup(env.request(j));
        QCOMPARE(r.status, RunStatus::Failed);
        QVERIFY(describe(r).contains("Dump completed"));
    }

    void mysqlMyisamWarns()
    {
        Env env;
        qputenv("FAKE_MYISAM", "1");
        auto j = env.job("version: 1\nid: my\nname: my\ntype: database\nengine: mysql\n"
                         "connection: { host: 127.0.0.1, user: u, password: { secret: db } }\ndatabases: [x]\n");
        auto r = runDatabaseBackup(env.request(j));
        QCOMPARE(r.status, RunStatus::Success);
        QVERIFY(!r.warnings.empty() && r.warnings.front().find("MyISAM") != std::string::npos);
    }

    void sqliteIntoEncryptedVault()
    {
        Env env;
        crypto::forgetRememberedKeys();
        QVERIFY(crypto::enableEncryption(env.vaultA, env.secrets));
        crypto::forgetRememberedKeys(); // o backup usa a chave guardada (secrets.dat do teste)
        const std::wstring dbFile = win::joinPath(env.root, L"pdv\\caixa.db");
        QVERIFY(win::createDirectories(win::parentPath(dbFile)).ok());
        {
            auto db = SqliteDb::open(dbFile, SqliteDb::Mode::ReadWriteCreate);
            QVERIFY(db.ok());
            QVERIFY(db.value().exec("CREATE TABLE clientes(cpf TEXT); INSERT INTO clientes VALUES ('111.222.333-44');").ok());
        }
        std::string yml = "version: 1\nid: lite\nname: lite\ntype: database\nengine: sqlite\nsqlite:\n  files: [\""
                          + win::toUtf8(dbFile) + "\"]\n";
        for (auto& ch : yml)
            if (ch == '\\')
                ch = '/';
        auto r = runDatabaseBackup(env.request(env.job(yml)));
        QVERIFY2(r.status == RunStatus::Success, describe(r).constData());

        const vault::JobLayout L(env.vaultA, L"lite");
        const auto dumps = listDumps(L, "caixa");
        QCOMPARE(dumps.size(), size_t(1));
        const std::wstring stored = win::joinPath(L.root(), win::fromCatalogPath(dumps.front().relativePath));
        QVERIFY(crypto::looksEncrypted(stored));
        QVERIFY(!readFile(stored).contains("111.222.333-44"));

        DatabaseRestoreRequest rr;
        rr.vaultLocation = env.vaultA;
        rr.jobId = L"lite";
        rr.database = "caixa";
        rr.targetPath = win::joinPath(env.root, L"restaurado.db");
        rr.stagingRoot = env.staging;
        rr.secretsFile = env.secrets;
        auto restored = restoreDatabase(rr);
        QVERIFY2(restored.ok(), restored.ok() ? "" : restored.error().message.c_str());
        auto db = SqliteDb::open(rr.targetPath, SqliteDb::Mode::ReadOnly);
        QVERIFY(db.ok());
        QCOMPARE(listTables(db.value()), std::vector<std::string>{"clientes"});
    }

    void sqliteBackupAndRestore()
    {
        Env env;
        const std::wstring dbFile = win::joinPath(env.root, L"pdv\\caixa.db");
        QVERIFY(win::createDirectories(win::parentPath(dbFile)).ok());
        {
            auto db = SqliteDb::open(dbFile, SqliteDb::Mode::ReadWriteCreate);
            QVERIFY(db.ok());
            QVERIFY(db.value().exec("PRAGMA journal_mode=WAL; CREATE TABLE vendas(id INTEGER PRIMARY KEY, total REAL);"
                                    "INSERT INTO vendas(total) VALUES (10.5),(20.0);").ok());
            // Conexão aberta e em WAL durante o backup, como no PDV.
            std::string yml = "version: 1\nid: lite\nname: lite\ntype: database\nengine: sqlite\nsqlite:\n  files: [\""
                              + win::toUtf8(dbFile) + "\"]\n";
            for (auto& ch : yml)
                if (ch == '\\')
                    ch = '/';
            auto r = runDatabaseBackup(env.request(env.job(yml)));
            QVERIFY2(r.status == RunStatus::Success, describe(r).constData());
        }
        const vault::JobLayout L(env.vaultA, L"lite");
        const auto dumps = listDumps(L, "caixa");
        QCOMPARE(dumps.size(), size_t(1));
        QCOMPARE(dumps.front().tables, std::vector<std::string>{"vendas"});

        DatabaseRestoreRequest rr;
        rr.vaultLocation = env.vaultA;
        rr.jobId = L"lite";
        rr.database = "caixa";
        rr.targetPath = win::joinPath(env.root, L"restaurado.db");
        rr.stagingRoot = env.staging;
        auto restored = restoreDatabase(rr);
        QVERIFY2(restored.ok(), restored.ok() ? "" : restored.error().message.c_str());
        auto db = SqliteDb::open(rr.targetPath, SqliteDb::Mode::ReadOnly);
        QVERIFY(db.ok());
        QCOMPARE(listTables(db.value()), std::vector<std::string>{"vendas"});

        // Nunca sobrescreve.
        auto again = restoreDatabase(rr);
        QVERIFY(!again.ok());
        QCOMPARE(again.error().code, ErrorCode::AlreadyExists);
    }

    void customCommand()
    {
        Env env;
        auto j = env.job("version: 1\nid: erp\nname: erp\ntype: database\nengine: custom\n"
                         "custom:\n  command: [\"C:/Windows/System32/cmd.exe\", \"/d\", \"/s\", \"/c\", \"echo backup-do-erp> {output}\"]\n"
                         "  output: file\n  output_name: erp.bak\n  validate:\n    command: [\"C:/Windows/System32/cmd.exe\", \"/d\", \"/c\", \"exit 0\"]\n");
        auto r = runDatabaseBackup(env.request(j));
        QVERIFY2(r.status == RunStatus::Success, describe(r).constData());
        const vault::JobLayout L(env.vaultA, L"erp");
        const auto dumps = listDumps(L, "erp");
        QCOMPARE(dumps.size(), size_t(1));
        const QByteArray content = decompress(win::joinPath(L.root(), win::fromCatalogPath(dumps.front().relativePath)),
                                              win::joinPath(env.root, L"erp.out"));
        QVERIFY(content.contains("backup-do-erp"));
    }

    void restorePostgresNeverOverwrites()
    {
        Env env;
        auto j = env.job(env.pgJob());
        QVERIFY(runDatabaseBackup(env.request(j)).status == RunStatus::Success);

        DatabaseRestoreRequest rr;
        rr.vaultLocation = env.vaultA;
        rr.jobId = L"pg";
        rr.database = "pdv_vendas";
        rr.job = env.request(j).job;
        rr.secretsFile = env.secrets;
        rr.stagingRoot = env.staging;
        rr.targetDatabase = "pdv_vendas_restaurado";
        auto ok = restoreDatabase(rr);
        QVERIFY2(ok.ok(), ok.ok() ? "" : ok.error().message.c_str());
        const QByteArray log = readFile(env.log);
        QVERIFY(log.contains("createdb |") && log.contains("pdv_vendas_restaurado"));
        QVERIFY(log.contains("pg_restore |") && log.contains("--dbname=pdv_vendas_restaurado"));

        rr.targetDatabase = "existe";
        auto exists = restoreDatabase(rr);
        QVERIFY(!exists.ok());
        QVERIFY(exists.error().message.find("já existe") != std::string::npos);

        rr.targetDatabase = "nome; DROP DATABASE x";
        QVERIFY(!restoreDatabase(rr).ok());
    }

    void restoreMysqlPipesDump()
    {
        Env env;
        auto j = env.job("version: 1\nid: my\nname: my\ntype: database\nengine: mysql\n"
                         "connection: { host: 127.0.0.1, user: u, password: { secret: db } }\ndatabases: [retaguarda]\n");
        QVERIFY(runDatabaseBackup(env.request(j)).status == RunStatus::Success);
        const std::wstring out = win::joinPath(env.root, L"restore-in.sql");
        qputenv("FAKE_RESTORE_OUT", QString::fromStdWString(out).toUtf8());

        DatabaseRestoreRequest rr;
        rr.vaultLocation = env.vaultA;
        rr.jobId = L"my";
        rr.database = "retaguarda";
        rr.job = env.request(j).job;
        rr.secretsFile = env.secrets;
        rr.stagingRoot = env.staging;
        rr.targetDatabase = "retaguarda_teste";
        auto ok = restoreDatabase(rr);
        QVERIFY2(ok.ok(), ok.ok() ? "" : ok.error().message.c_str());
        QVERIFY(readFile(out).contains("CREATE TABLE `clientes`"));
    }

    void dumpRetentionKeepsNewest()
    {
        Env env;
        auto j = env.job(env.pgJob("retention: { keep_last: 2, keep_daily: 0, keep_weekly: 0, keep_monthly: 0 }\n"));
        for (int i = 0; i < 4; ++i) {
            QVERIFY(runDatabaseBackup(env.request(j)).status == RunStatus::Success);
            QThread::msleep(1100);
        }
        QCOMPARE(listDumps(vault::JobLayout(env.vaultA, L"pg"), "pdv_vendas").size(), size_t(2));
    }

    void serverLockIsExclusiveAndReleased()
    {
        Env env;
        auto j = env.job(env.pgJob("remote: { server_lock: true }\n"));
        QVERIFY(j.database.serverLock);
        const auto req = env.request(j);
        SqlTarget target{"postgresql", req.job.tools, Connection{"localhost", 5432, "synkro_backup", kPassword, {}, {}, 15}};

        {
            // "Outra máquina" segura a trava: o dump é ignorado, nada vai ao cofre.
            auto other = ServerLock::acquire(target, "pdv_vendas", env.root, 120);
            QVERIFY2(other.ok(), other.ok() ? "" : other.error().message.c_str());
            auto second = ServerLock::acquire(target, "pdv_vendas", env.root, 120);
            QVERIFY(!second.ok());
            QCOMPARE(second.error().code, ErrorCode::AlreadyExists);

            auto r = runDatabaseBackup(req);
            QCOMPARE(r.status, RunStatus::AlreadyRunning);
            QVERIFY(r.error.find("outra máquina") != std::string::npos);
            QVERIFY(listDumps(vault::JobLayout(env.vaultA, L"pg")).empty());
        }

        // Trava solta: o dump acontece e a trava é liberada no fim.
        auto r = runDatabaseBackup(req);
        QVERIFY2(r.status == RunStatus::Success, describe(r).constData());
        QVERIFY(readFile(env.log).contains("pg_try_advisory_lock(hashtext('synkro:pdv_vendas'))"));
        auto after = ServerLock::acquire(target, "pdv_vendas", env.root, 120);
        QVERIFY2(after.ok(), after.ok() ? "" : after.error().message.c_str());
        QVERIFY(!readFile(env.log).contains(kPassword));
    }

    void mysqlServerLock()
    {
        Env env;
        auto j = env.job("version: 1\nid: my\nname: my\ntype: database\nengine: mysql\n"
                         "connection: { host: 127.0.0.1, user: u, password: { secret: db } }\ndatabases: [retaguarda]\n"
                         "remote: { server_lock: true }\n");
        auto r = runDatabaseBackup(env.request(j));
        QVERIFY2(r.status == RunStatus::Success, describe(r).constData());
        QVERIFY(readFile(env.log).contains("GET_LOCK('synkro:retaguarda', 0)"));
    }

    void restoreTestPostgres()
    {
        Env env;
        auto j = env.job(env.pgJob("restore_test:\n  enabled: true\n  checks:\n"
                                   "    - { name: tem vendas, sql: \"SELECT count(*) > 0 FROM vendas\" }\n"));
        QVERIFY(j.database.restoreTest);
        QCOMPARE(j.database.restoreTestConfig.checks.size(), size_t(1));
        QVERIFY(runDatabaseBackup(env.request(j)).status == RunStatus::Success);

        RestoreTestRequest t;
        t.vaultLocation = env.vaultA;
        t.jobId = L"pg";
        t.job = env.request(j).job;
        t.secretsFile = env.secrets;
        t.stagingRoot = env.staging;
        auto ok = runRestoreTest(t);
        QVERIFY2(ok.ok(), ok.ok() ? "" : ok.error().message.c_str());
        QVERIFY2(ok.value().ok(), ok.value().items.front().detail.c_str());
        QCOMPARE(ok.value().items.front().tablesFound, size_t(2));
        QCOMPARE(ok.value().items.front().checks.size(), size_t(1));
        const QByteArray log = readFile(env.log);
        QVERIFY(log.contains("createdb |") && log.contains("synkro_verify_"));
        QVERIFY(log.contains("DROP DATABASE IF EXISTS \"synkro_verify_"));
        auto dumps = listDumps(vault::JobLayout(env.vaultA, L"pg"), "pdv_vendas");
        QVERIFY(dumps.front().restoreTestedAt.isValid());
        QCOMPARE(dumps.front().restoreTestResult, std::string("ok"));

        // Tabela que não voltou e verificação falsa: falha, e o temporário é apagado igual.
        qputenv("FAKE_RESTORED_TABLES", "public.clientes");
        qputenv("FAKE_CHECK_RESULT", "f");
        auto bad = runRestoreTest(t);
        QVERIFY(bad.ok());
        QVERIFY(!bad.value().ok());
        const auto& item = bad.value().items.front();
        QCOMPARE(item.missingTables, std::vector<std::string>{"public.vendas"});
        QVERIFY(!item.checks.front().ok);
        QVERIFY(item.detail.find("tem vendas") != std::string::npos);
        QCOMPARE(readFile(env.log).count("DROP DATABASE IF EXISTS"), 2);
        QCOMPARE(listDumps(vault::JobLayout(env.vaultA, L"pg"), "pdv_vendas").front().restoreTestResult, std::string("failed"));
        QVERIFY(ops::listSubdirectories(env.staging).empty());
    }

    void restoreTestSeparateServer()
    {
        Env env;
        (void)SecretStore(env.secrets).set("teste", kPassword);
        auto j = env.job(env.pgJob("restore_test:\n  enabled: true\n"
                                   "  connection: { host: srv-teste, port: 5444, user: verificador, password: { secret: teste } }\n"));
        QVERIFY(!j.database.restoreTestConfig.sameConnection);
        QVERIFY(runDatabaseBackup(env.request(j)).status == RunStatus::Success);
        RestoreTestRequest t;
        t.vaultLocation = env.vaultA;
        t.jobId = L"pg";
        t.job = env.request(j).job;
        t.secretsFile = env.secrets;
        t.stagingRoot = env.staging;
        auto r = runRestoreTest(t);
        QVERIFY2(r.ok() && r.value().ok(), r.ok() ? r.value().items.front().detail.c_str() : r.error().message.c_str());
        const QByteArray log = readFile(env.log);
        QVERIFY(log.contains("createdb | --host=srv-teste | --port=5444 | --username=verificador"));
    }

    void restoreTestSqlite()
    {
        Env env;
        const std::wstring dbFile = win::joinPath(env.root, L"pdv\\caixa.db");
        QVERIFY(win::createDirectories(win::parentPath(dbFile)).ok());
        {
            auto db = SqliteDb::open(dbFile, SqliteDb::Mode::ReadWriteCreate);
            QVERIFY(db.ok());
            QVERIFY(db.value().exec("CREATE TABLE vendas(id INTEGER PRIMARY KEY, total REAL);"
                                    "INSERT INTO vendas(total) VALUES (10.5),(20.0);").ok());
        }
        std::string yml = "version: 1\nid: lite\nname: lite\ntype: database\nengine: sqlite\nsqlite:\n  files: [\""
                          + win::toUtf8(dbFile) + "\"]\nrestore_test:\n  enabled: true\n  checks:\n"
                          "    - { name: duas vendas, sql: \"SELECT count(*) = 2 FROM vendas\" }\n"
                          "    - { name: total certo, sql: \"SELECT sum(total) = 99 FROM vendas\" }\n";
        for (auto& ch : yml)
            if (ch == '\\')
                ch = '/';
        auto j = env.job(yml);
        QVERIFY(runDatabaseBackup(env.request(j)).status == RunStatus::Success);

        RestoreTestRequest t;
        t.vaultLocation = env.vaultA;
        t.jobId = L"lite";
        t.job = j.database;
        t.stagingRoot = env.staging;
        auto r = runRestoreTest(t);
        QVERIFY2(r.ok(), r.ok() ? "" : r.error().message.c_str());
        const auto& item = r.value().items.front();
        QCOMPARE(item.tablesFound, size_t(1));
        QVERIFY(item.checks[0].ok);
        QVERIFY(!item.checks[1].ok); // soma é 30.5
        QVERIFY(!r.value().ok());
        QVERIFY(ops::listSubdirectories(env.staging).empty());
    }

    void restoreTestMysql()
    {
        Env env;
        auto j = env.job("version: 1\nid: my\nname: my\ntype: database\nengine: mysql\n"
                         "connection: { host: 127.0.0.1, user: u, password: { secret: db } }\ndatabases: [retaguarda]\n"
                         "restore_test: { enabled: true, checks: [ { name: ok, sql: \"SELECT 1\" } ] }\n");
        QVERIFY(runDatabaseBackup(env.request(j)).status == RunStatus::Success);
        qputenv("FAKE_RESTORE_OUT", QString::fromStdWString(win::joinPath(env.root, L"in.sql")).toUtf8());
        RestoreTestRequest t;
        t.vaultLocation = env.vaultA;
        t.jobId = L"my";
        t.job = env.request(j).job;
        t.secretsFile = env.secrets;
        t.stagingRoot = env.staging;
        auto r = runRestoreTest(t);
        QVERIFY2(r.ok() && r.value().ok(), r.ok() ? r.value().items.front().detail.c_str() : r.error().message.c_str());
        QVERIFY(readFile(env.log).contains("DROP DATABASE IF EXISTS `synkro_verify_"));
    }

    void verifyNames()
    {
        QCOMPARE(verifyDatabaseName(L"r-20261118-220000-8f3a"), std::string("synkro_verify_20261118_220000_8f3a"));
        QVERIFY(isTruthy("t") && isTruthy(" TRUE\n") && isTruthy("1") && !isTruthy("f") && !isTruthy("0") && !isTruthy(""));
        QCOMPARE(quoteIdentifier("mysql", "a`b"), std::string("`a``b`"));
        QCOMPARE(quoteIdentifier("postgresql", "a\"b"), std::string("\"a\"\"b\""));
        QCOMPARE(quoteIdentifier("sqlserver", "a]b"), std::string("[a]]b]"));
        QVERIFY(isTruthy("      <true>"));
        QCOMPARE(parseGbakTables("gbak:    writing data for table \"CLIENTES\"\ngbak:writing data for table VENDAS \n"),
                 (std::vector<std::string>{"CLIENTES", "VENDAS"}));
        Connection c{"srv", 3051, "SYSDBA", "x", {}, {}, 15};
        QCOMPARE(firebirdConnectionString(c, "C:/PDV/PDV.FDB"), std::string("srv/3051:C:/PDV/PDV.FDB"));
        const QString ms = QString::fromStdWString(buildCommandLine(sqlcmdArgs(L"sqlcmd", Connection{"PDV\\SQLEXPRESS", 0, "", "", "mandatory", {}, 15}, "ERP")));
        QVERIFY2(ms.contains("-S PDV\\SQLEXPRESS -E -d ERP") && ms.contains("-N") && !ms.contains("-P"), qPrintable(ms));
    }

    void firebirdBackupRestoreAndTest()
    {
        Env env;
        const std::string yml = "version: 1\nid: fb\nname: fb\ntype: database\nengine: firebird\n"
                                "connection: { host: localhost, user: SYSDBA, password: { secret: db } }\n"
                                "databases: [\"C:/PDV/dados/PDV.FDB\"]\n"
                                "restore_test: { enabled: true, checks: [ { name: ok, sql: \"SELECT TRUE FROM RDB$DATABASE\" } ] }\n";
        auto j = env.job(yml);
        auto r = runDatabaseBackup(env.request(j, {env.vaultA, env.vaultB}));
        QVERIFY2(r.status == RunStatus::Success, describe(r).constData());
        const vault::JobLayout L(env.vaultA, L"fb");
        const auto dumps = listDumps(L, "PDV");
        QCOMPARE(dumps.size(), size_t(1));
        QCOMPARE(dumps.front().compression, std::string("zstd"));
        QVERIFY(dumps.front().relativePath.find(L".fbk.zst") != std::wstring::npos);
        QCOMPARE(dumps.front().tables, (std::vector<std::string>{"clientes", "vendas"}));
        QVERIFY(dumps.front().command.find("-fetch_password") == std::string::npos);
        const QByteArray log = readFile(env.log);
        QVERIFY(log.contains("gbak | -b | -g | -v") && log.contains("localhost/3050:C:/PDV/dados/PDV.FDB"));
        QVERIFY(!log.contains(kPassword));

        // restore-db: arquivo novo; nunca sobrescreve.
        DatabaseRestoreRequest rr;
        rr.vaultLocation = env.vaultA;
        rr.jobId = L"fb";
        rr.database = "PDV";
        rr.job = env.request(j).job;
        rr.secretsFile = env.secrets;
        rr.stagingRoot = env.staging;
        rr.targetPath = win::joinPath(env.root, L"restaurado.fdb");
        auto restored = restoreDatabase(rr);
        QVERIFY2(restored.ok(), restored.ok() ? "" : restored.error().message.c_str());
        QVERIFY(readFile(rr.targetPath).contains("clientes"));
        auto again = restoreDatabase(rr);
        QVERIFY(!again.ok());
        QCOMPARE(again.error().code, ErrorCode::AlreadyExists);

        // Teste de restauração: banco temporário criado, conferido e apagado.
        RestoreTestRequest t;
        t.vaultLocation = env.vaultA;
        t.jobId = L"fb";
        t.job = env.request(j).job;
        t.secretsFile = env.secrets;
        t.stagingRoot = env.staging;
        auto test = runRestoreTest(t);
        QVERIFY2(test.ok() && test.value().ok(), test.ok() ? test.value().items.front().detail.c_str() : test.error().message.c_str());
        QCOMPARE(test.value().items.front().tablesFound, size_t(2));
        QVERIFY(ops::listSubdirectories(env.staging).empty());

        // Log do gbak sem a linha de término: dump rejeitado.
        qputenv("FAKE_TRUNCATE", "1");
        auto bad = runDatabaseBackup(env.request(j));
        QCOMPARE(bad.status, RunStatus::Failed);
        QVERIFY(describe(bad).contains("incompleto"));
    }

    void sqlServerBackupRestoreAndTest()
    {
        Env env;
        const std::wstring msDir = win::joinPath(env.root, L"mssql-backup");
        QVERIFY(win::createDirectories(msDir).ok());
        qputenv("FAKE_MSSQL_DIR", QString::fromStdWString(msDir).toUtf8());
        auto j = env.job("version: 1\nid: ms\nname: ms\ntype: database\nengine: sqlserver\n"
                         "connection: { host: localhost, user: synkro, password: { secret: db } }\n"
                         "databases: [PDV]\nrestore_test: { enabled: true }\n");
        auto r = runDatabaseBackup(env.request(j));
        QVERIFY2(r.status == RunStatus::Success, describe(r).constData());
        const vault::JobLayout L(env.vaultA, L"ms");
        const auto dumps = listDumps(L, "PDV");
        QCOMPARE(dumps.size(), size_t(1));
        QCOMPARE(dumps.front().tables, (std::vector<std::string>{"dbo.clientes", "dbo.vendas"}));
        QCOMPARE(dumps.front().validationMethod, std::string("RESTORE VERIFYONLY WITH CHECKSUM"));
        QVERIFY(ops::listFiles(msDir, L".bak").empty()); // o .bak saiu da pasta do servidor
        const QByteArray log = readFile(env.log);
        QVERIFY(log.contains("WITH COPY_ONLY, CHECKSUM, INIT, FORMAT"));
        QVERIFY(log.contains("-U | synkro") && !log.contains(kPassword) && !log.contains("| -P |"));

        DatabaseRestoreRequest rr;
        rr.vaultLocation = env.vaultA;
        rr.jobId = L"ms";
        rr.database = "PDV";
        rr.job = env.request(j).job;
        rr.secretsFile = env.secrets;
        rr.stagingRoot = env.staging;
        rr.targetDatabase = "PDV_restaurado";
        auto ok = restoreDatabase(rr);
        QVERIFY2(ok.ok(), ok.ok() ? "" : ok.error().message.c_str());
        QVERIFY(readFile(env.log).contains("RESTORE DATABASE [PDV_restaurado]"));
        QVERIFY(readFile(env.log).contains("MOVE N'pdv_log' TO N'"));
        QVERIFY(ops::listFiles(msDir, L".bak").empty());
        rr.targetDatabase = "existe";
        auto exists = restoreDatabase(rr);
        QVERIFY(!exists.ok());
        QCOMPARE(exists.error().code, ErrorCode::AlreadyExists);

        RestoreTestRequest t;
        t.vaultLocation = env.vaultA;
        t.jobId = L"ms";
        t.job = env.request(j).job;
        t.secretsFile = env.secrets;
        t.stagingRoot = env.staging;
        auto test = runRestoreTest(t);
        QVERIFY2(test.ok() && test.value().ok(), test.ok() ? test.value().items.front().detail.c_str() : test.error().message.c_str());
        QVERIFY(readFile(env.log).contains("DROP DATABASE [synkro_verify_"));
    }

    void sqlServerWithoutBackupDirFailsClearly()
    {
        Env env; // FAKE_MSSQL_DIR ausente: a instância "não informa" a pasta padrão
        auto j = env.job("version: 1\nid: ms\nname: ms\ntype: database\nengine: sqlserver\n"
                         "connection: { host: localhost, user: synkro, password: { secret: db } }\ndatabases: [PDV]\n");
        auto r = runDatabaseBackup(env.request(j));
        QCOMPARE(r.status, RunStatus::Failed);
        QVERIFY2(describe(r).contains("sqlserver.backup_dir"), describe(r).constData());
    }
};

QTEST_GUILESS_MAIN(TstDatabase)
#include "tst_database.moc"
