#include "config/ConfigLoader.h"
#include "config/PlanDescriber.h"
#include "config/Values.h"

#include <QTest>

using namespace synkro::config;

namespace {

GlobalConfig sampleGlobal()
{
    std::vector<Diagnostic> d;
    return parseGlobalDocument(R"(
version: 1
vaults:
  - id: a
    vault_id: 7f1c9a2e-3b0d-4c7e-9a51-0d2b6e1f4a10
    label: "Disco A"
  - id: b
    vault_id: 2b8e5d71-9c44-4f3a-8e0b-6a7c1d9f3e22
defaults:
  vaults: [a, b]
)",
                               L"synkro.yml", d);
}

std::vector<Diagnostic> diagnose(const std::string& yaml)
{
    std::vector<Diagnostic> d;
    (void)parseJobDocument(yaml, L"jobs\\t.yml", sampleGlobal(), d);
    return d;
}

bool hasError(const std::vector<Diagnostic>& d, const std::string& key, const std::string& fragment = {})
{
    for (const auto& x : d) {
        if (x.severity == Severity::Error && x.key == key
            && (fragment.empty() || x.message.find(fragment) != std::string::npos))
            return true;
    }
    return false;
}

QString dump(const std::vector<Diagnostic>& d)
{
    QString s;
    for (const auto& x : d)
        s += QString::fromStdString(formatDiagnostic(x)) + "\n";
    return s;
}

const char* kMinimalFiles = R"(
version: 1
id: pdv-arquivos
name: "Arquivos"
type: files
source: "C:/PDV"
schedule:
  - daily: "22:30"
)";

} // namespace

class TstConfig : public QObject {
    Q_OBJECT

private slots:
    void values()
    {
        QCOMPARE(parseDuration("30s").value(), 30LL);
        QCOMPARE(parseDuration("10m").value(), 600LL);
        QCOMPARE(parseDuration("2h").value(), 7200LL);
        QCOMPARE(parseDuration("90d").value(), 90LL * 86400);
        QVERIFY(!parseDuration("10 minutos"));
        QCOMPARE(parseSize("500MB").value(), 500ull << 20);
        QCOMPARE(parseSize("1,5 GB").value(), 3ull << 29);
        QCOMPARE(parseRatio("20%").value(), 0.2);
        QCOMPARE(parseRatio("0.5").value(), 0.5);
        QVERIFY(!parseRatio("150%"));
        QCOMPARE(parseTimeOfDay("22:00").value(), 22 * 60);
        QVERIFY(!parseTimeOfDay("24:00"));
        QCOMPARE(parseWeekday("SUN").value(), 7);
        QCOMPARE(describeWeekdays({1, 2, 3, 4, 5}), std::string("de segunda a sexta"));
    }

    void minimalFilesJobIsValidWithDefaults()
    {
        std::vector<Diagnostic> d;
        const JobConfig job = parseJobDocument(kMinimalFiles, L"jobs\\pdv-arquivos.yml", sampleGlobal(), d);
        QVERIFY2(d.empty(), qPrintable(dump(d)));
        QCOMPARE(job.vaults, (std::vector<std::string>{"a", "b"})); // de defaults
        QCOMPARE(job.files.source, std::wstring(L"C:\\PDV"));
        QCOMPARE(job.files.maxDeleted, 0.20);
        QCOMPARE(job.schedule.size(), size_t(1));
        QCOMPARE(job.schedule[0].kind, TriggerKind::Daily);
        QCOMPARE(job.schedule[0].atMinutes, 22 * 60 + 30);
    }

    void multipleSources()
    {
        const char* yaml = R"(
version: 1
id: pdv-arquivos
name: "Arquivos"
type: files
sources:
  - "C:/PDV"
  - "D:/Fiscal/"
  - { path: "E:/Exportacao/Notas", name: notas-exportadas }
schedule:
  - weekly: { days: [mon, tue, wed, thu, fri, sat], at: "22:00" }
  - weekly: { days: [sun], at: "12:00" }
)";
        std::vector<Diagnostic> d;
        const JobConfig job = parseJobDocument(yaml, L"jobs\\pdv-arquivos.yml", sampleGlobal(), d);
        QVERIFY2(d.empty(), qPrintable(dump(d)));
        QCOMPARE(job.files.source, std::wstring(L"C:\\PDV"));
        QCOMPARE(job.files.moreSources.size(), size_t(2));
        QCOMPARE(job.files.moreSources[0].path, std::wstring(L"D:\\Fiscal"));
        QCOMPARE(job.files.moreSources[0].alias, std::wstring(L"Fiscal"));
        QVERIFY(job.files.moreSources[0].name.empty());
        QCOMPARE(job.files.moreSources[1].alias, std::wstring(L"notas-exportadas"));
        QCOMPARE(job.schedule.size(), size_t(2));
        QCOMPARE(job.schedule[1].days, std::vector<int>{7});
        QCOMPARE(job.schedule[1].atMinutes, 12 * 60);
        QVERIFY(describeJob({}, job).find("Ler 3 pastas") != std::string::npos);
    }

    void invalidSources()
    {
        auto job = [](const std::string& sources) {
            return "version: 1\nid: t\nname: t\ntype: files\n" + sources + "schedule:\n  - daily: \"22:00\"\n";
        };
        QVERIFY(hasError(diagnose(job("source: \"C:/A\"\nsources: [\"C:/B\"]\n")), "sources", "não os dois"));
        QVERIFY(hasError(diagnose(job("sources: [\"C:/A\", \"C:/a/\"]\n")), "sources[1]", "repetida"));
        QVERIFY(hasError(diagnose(job("sources: [\"C:/A\", \"C:/A/B\"]\n")), "sources[1]", "dentro de outra"));
        QVERIFY(hasError(diagnose(job("sources: [\"C:/A\", \"D:/x/Notas\", \"E:/y/notas\"]\n")), "sources[2].name", "já usa"));
        QVERIFY(hasError(diagnose(job("sources: [{ path: \"C:/A\", name: a }]\n")), "sources[0].name", "raiz"));
        QVERIFY(hasError(diagnose(job("sources: [\"C:/A\", \"relativo/x\"]\n")), "sources[1]", "caminho completo"));
        QVERIFY(hasError(diagnose(job("sources: []\n")), "sources", "lista"));
    }

    void plaintextPasswordRejected()
    {
        const auto d = diagnose(R"(
version: 1
id: pg
name: pg
type: database
engine: postgresql
connection:
  host: localhost
  user: u
  password: "abc"
databases: [x]
)");
        QVERIFY2(hasError(d, "connection.password", "texto puro"), qPrintable(dump(d)));
        // Linha do erro aponta para o campo.
        for (const auto& x : d) {
            if (x.key == "connection.password")
                QCOMPARE(x.line, 10);
        }
    }

    void unknownFieldsAndWrongType()
    {
        const auto d = diagnose(std::string(kMinimalFiles) + "\nsorce: \"C:/x\"\nengine: postgresql\n");
        QVERIFY(hasError(d, "sorce", "desconhecido"));
        QVERIFY(hasError(d, "engine", "não se aplica"));
    }

    void invalidValues()
    {
        const auto d = diagnose(R"(
version: 1
id: Job_Invalido
name: x
type: files
source: "relativo/pasta"
verify: sempre
retention:
  keep_versions: 90 dias
guards:
  max_deleted: 150%
schedule:
  - daily: "25:00"
  - weekly: { days: [seg], at: "22:00" }
  - every: 1m
)");
        QVERIFY(hasError(d, "id"));
        QVERIFY(hasError(d, "source"));
        QVERIFY(hasError(d, "verify"));
        QVERIFY(hasError(d, "retention.keep_versions"));
        QVERIFY(hasError(d, "guards.max_deleted"));
        QVERIFY(hasError(d, "schedule[0].daily"));
        QVERIFY(hasError(d, "schedule[1].weekly.days"));
        QVERIFY(hasError(d, "schedule[2].every"));
    }

    void duplicateKeysRejected()
    {
        const auto d = diagnose(std::string(kMinimalFiles) + "schedule:\n  - daily: \"03:00\"\n");
        QVERIFY2(hasError(d, "schedule", "repetido"), qPrintable(dump(d)));
        const auto nested = diagnose(std::string(kMinimalFiles) + "guards:\n  max_deleted: 10%\n  max_deleted: 90%\n");
        QVERIFY(hasError(nested, "guards.max_deleted", "repetido"));
    }

    void undeclaredVault()
    {
        const auto d = diagnose(std::string(kMinimalFiles) + "vaults: [a, zzz]\n");
        QVERIFY(hasError(d, "vaults", "zzz"));
    }

    void forbiddenExtraArgs()
    {
        const auto d = diagnose(R"(
version: 1
id: pg
name: pg
type: database
engine: postgresql
connection: { host: localhost, user: u, password: { secret: s } }
databases: [x]
postgresql:
  extra_args: ["--no-comments", "--file=C:/x.dump"]
)");
        QVERIFY2(hasError(d, "postgresql.extra_args", "--file"), qPrintable(dump(d)));
    }

    void remoteWithoutTlsRejected()
    {
        const auto d = diagnose(R"(
version: 1
id: pg
name: pg
type: database
engine: postgresql
connection: { host: 192.168.0.10, user: u, password: { secret: s }, sslmode: disable }
databases: all
)");
        QVERIFY2(hasError(d, "connection", "criptografia"), qPrintable(dump(d)));
    }

    void remoteDefaultsToRequire()
    {
        std::vector<Diagnostic> d;
        const auto job = parseJobDocument(R"(
version: 1
id: pg
name: pg
type: database
engine: postgresql
connection: { host: srv-banco, user: u, password: { secret: s } }
databases: all
)",
                                          L"jobs\\pg.yml", sampleGlobal(), d);
        QVERIFY2(d.empty(), qPrintable(dump(d)));
        QCOMPARE(job.database.sslMode, std::string("require"));
        QVERIFY(job.database.remote);
        QVERIFY(job.database.serverLock);
    }

    void sqliteOnNetworkShareRejected()
    {
        const auto d = diagnose(R"(
version: 1
id: lite
name: lite
type: database
engine: sqlite
sqlite:
  files: ["\\\\servidor\\pdv\\caixa.db"]
)");
        QVERIFY2(hasError(d, "sqlite.files", "rede"), qPrintable(dump(d)));
    }

    void firebirdAndSqlServerRules()
    {
        // Firebird não lista bancos e não tem trava no servidor.
        auto d = diagnose(R"(
version: 1
id: fb
name: fb
type: database
engine: firebird
connection: { host: localhost, user: SYSDBA, password: { secret: fb } }
databases: all
remote: { server_lock: true }
)");
        QVERIFY2(hasError(d, "databases", "caminho"), qPrintable(dump(d)));
        QVERIFY2(hasError(d, "remote.server_lock", "não é suportada"), qPrintable(dump(d)));

        // SQL Server remoto precisa de compartilhamento e criptografia.
        d = diagnose(R"(
version: 1
id: ms
name: ms
type: database
engine: sqlserver
connection: { host: srv-erp, user: synkro, password: { secret: ms }, encrypt: optional }
databases: [ERP]
)");
        QVERIFY2(hasError(d, "sqlserver.backup_dir", "compartilhada"), qPrintable(dump(d)));
        QVERIFY2(hasError(d, "connection", "encrypt"), qPrintable(dump(d)));

        d = diagnose("version: 1\nid: ms\nname: ms\ntype: database\nengine: sqlserver\n"
                     "connection: { host: srv-erp, auth: windows, user: x }\n"
                     "databases: [\"a]b\"]\nsqlserver: { backup_dir: \"D:/backup\" }\n");
        QVERIFY2(hasError(d, "connection", "auth: windows"), qPrintable(dump(d)));
        QVERIFY2(hasError(d, "databases[0]", "inválido"), qPrintable(dump(d)));
        QVERIFY2(hasError(d, "sqlserver.backup_dir", "compartilhamento"), qPrintable(dump(d)));
    }

    void firebirdAndSqlServerParse()
    {
        GlobalConfig g;
        std::vector<Diagnostic> d;
        auto fb = parseJobDocument(R"(
version: 1
id: fb
name: fb
type: database
engine: firebird
connection: { host: localhost, user: SYSDBA, password: { secret: fb } }
databases: ["C:/PDV/dados/PDV.FDB", { name: estoque, path: "estoque-alias" }]
firebird: { garbage_collect: true }
)",
                                           L"jobs\\fb.yml", g, d);
        QCOMPARE(fb.database.port, 3050);
        QCOMPARE(fb.database.databases.size(), size_t(2));
        QCOMPARE(fb.database.databases[0].name, std::string("PDV"));
        QCOMPARE(fb.database.databases[0].path, std::string("C:/PDV/dados/PDV.FDB"));
        QCOMPARE(fb.database.databases[1].path, std::string("estoque-alias"));
        QVERIFY(!fb.database.serverLock);

        auto ms = parseJobDocument("version: 1\nid: ms\nname: ms\ntype: database\nengine: sqlserver\n"
                                   "connection: { host: '.\\SQLEXPRESS', auth: windows }\ndatabases: all\n",
                                   L"jobs\\ms.yml", g, d);
        QVERIFY(ms.database.windowsAuth);
        QCOMPARE(ms.database.port, 0); // instância nomeada: sem porta fixa
        QVERIFY(!ms.database.remote);
        QCOMPARE(ms.database.sslMode, std::string("optional"));
    }

    void brokenYamlReportsLine()
    {
        const auto d = diagnose("version: 1\nid: x\n  name: [quebrado\n");
        QVERIFY(!d.empty());
        QVERIFY(d.front().message.find("YAML inválido") != std::string::npos);
        QVERIFY(d.front().line >= 2);
    }

    void examplesAreValid()
    {
        LoadOptions opt;
        opt.checkPhysicalDisks = false;
        opt.checkSecrets = false;
        const ConfigSet set = loadConfig(QStringLiteral(SYNKRO_SOURCE_DIR "/config/examples").replace('/', '\\').toStdWString(), opt);
        QVERIFY2(!set.hasErrors(), qPrintable(dump(set.diagnostics)));
        QCOMPARE(set.warningCount(), size_t(0));
        QCOMPARE(set.jobs.size(), size_t(8));
        QCOMPARE(set.global.vaults.size(), size_t(2));

        const JobConfig* files = set.findJob("pdv-arquivos");
        QVERIFY(files);
        QCOMPARE(files->minVaultsOk, 1);
        QCOMPARE(files->hooks.before.size(), size_t(1));
        QCOMPARE(files->schedule.size(), size_t(2));
        QCOMPARE(files->schedule[1].kind, TriggerKind::OnStartup);
        QCOMPARE(files->schedule[1].delaySeconds, 600LL);

        const JobConfig* lite = set.findJob("caixa-sqlite");
        QVERIFY(lite);
        QCOMPARE(lite->schedule[0].kind, TriggerKind::Every);
        QCOMPARE(lite->schedule[0].betweenStart, 8 * 60);
    }

    void planDescribesJobInPortuguese()
    {
        LoadOptions opt;
        opt.checkPhysicalDisks = false;
        const ConfigSet set = loadConfig(QStringLiteral(SYNKRO_SOURCE_DIR "/config/examples").replace('/', '\\').toStdWString(), opt);
        const JobConfig* files = set.findJob("pdv-arquivos");
        QVERIFY(files);
        const QString plan = QString::fromStdString(describeJob(set, *files));
        QVERIFY2(plan.contains(QStringLiteral("De segunda a sábado, às 22:30.")), qPrintable(plan));
        QVERIFY(plan.contains(QStringLiteral("Ao ligar o computador, 10 minutos depois.")));
        QVERIFY(plan.contains(QStringLiteral("Sucesso exige 1 de 2 cofres.")));
        QVERIFY(plan.contains(QStringLiteral("versions/")));
        QVERIFY(plan.contains(QStringLiteral("Config/pdv.ini")));
        QVERIFY(plan.contains(QStringLiteral("snapshot VSS")));
        QVERIFY(plan.contains(QStringLiteral("fechar-caixa.exe")));

        const JobConfig* pg = set.findJob("pdv-postgres");
        const QString pgPlan = QString::fromStdString(describeJob(set, *pg));
        QVERIFY2(pgPlan.contains(QStringLiteral("Todos os dias às 22:00.")), qPrintable(pgPlan));
        QVERIFY(pgPlan.contains(QStringLiteral("segredo \"pg-pdv\"")));
        QVERIFY(pgPlan.contains(QStringLiteral("A tarefa \"Arquivos do PDV (XML fiscais e configuração)\" começa em seguida")));
    }
};

QTEST_GUILESS_MAIN(TstConfig)
#include "tst_config.moc"
