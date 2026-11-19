#include "config/ConfigApply.h"
#include "config/ConfigLoader.h"
#include "config/JobRequest.h"
#include "config/ShutdownPolicy.h"
#include "engine/BackupEngine.h"
#include "engine/Scrub.h"
#include "engine/Verify.h"
#include "platform/windows/TaskScheduler.h"
#include "state/Alerts.h"
#include "state/StateStore.h"
#include "vault/JobLayout.h"
#include "vault/Vault.h"

#include "TestFs.h"

#include <QTemporaryDir>
#include <QTest>

using namespace synkro;
using namespace synkro::config;
using namespace synkro::test;

namespace {

std::string slashes(std::string s)
{
    for (auto& c : s)
        if (c == '\\')
            c = '/';
    return s;
}

// Configuração em pasta temporária com dois cofres-pasta e uma tarefa de arquivos.
struct Setup {
    QTemporaryDir dir;
    std::wstring root = nativePath(dir.path());
    std::wstring cfg = win::joinPath(root, L"cfg");
    std::wstring source = win::joinPath(root, L"origem");
    std::wstring a = win::joinPath(root, L"discoA");
    std::wstring b = win::joinPath(root, L"discoB");
    QString idA, idB;

    explicit Setup(const std::string& jobExtra = "", const std::string& schedule = "schedule:\n  - daily: \"22:30\"\n")
    {
        (void)writeFile(win::joinPath(source, L"x.txt"), "conteudo x");
        (void)writeFile(win::joinPath(source, L"y.txt"), "conteudo y");
        for (const auto& v : {a, b})
            (void)win::createDirectories(v);
        idA = vault::initialize(a, QStringLiteral("A")).value().vaultId;
        idB = vault::initialize(b, QStringLiteral("B")).value().vaultId;
        const std::string global = "version: 1\nvaults:\n"
                                   "  - { id: a, vault_id: " + idA.toStdString() + ", label: \"Disco A\", path: \"" + slashes(win::toUtf8(a)) + "\" }\n"
                                   "  - { id: b, vault_id: " + idB.toStdString() + ", label: \"Disco B\", path: \"" + slashes(win::toUtf8(b)) + "\", rotation: true }\n"
                                   "defaults:\n  vaults: [a, b]\n"
                                   "maintenance:\n  scrub: { schedule: { weekly: { days: [sun], at: \"03:00\" } }, sample: 100% }\n";
        (void)writeFile(win::joinPath(cfg, L"synkro.yml"), QByteArray::fromStdString(global));
        const std::string job = "version: 1\nid: arquivos\nname: \"Arquivos\"\ntype: files\nsource: \"" + slashes(win::toUtf8(source))
                                + "\"\n" + schedule + "consistency: { snapshot: none }\n" + jobExtra;
        (void)writeFile(win::joinPath(cfg, L"jobs\\arquivos.yml"), QByteArray::fromStdString(job));
    }

    ConfigSet load() const
    {
        LoadOptions opt;
        opt.checkPhysicalDisks = false;
        opt.checkSecrets = false;
        return loadConfig(cfg, opt);
    }
};

QString dump(const ConfigSet& s)
{
    QString out;
    for (const auto& d : s.diagnostics)
        out += QString::fromStdString(formatDiagnostic(d)) + "\n";
    return out;
}

bool hasAlert(const std::vector<AlertRow>& v, const std::string& code, const std::string& subject)
{
    for (const auto& a : v)
        if (a.code == code && a.subject == subject)
            return true;
    return false;
}

} // namespace

class TstOperations : public QObject {
    Q_OBJECT

private slots:
    void stateStoreRunsAndAlerts()
    {
        QTemporaryDir dir;
        auto st = StateStore::open(win::joinPath(nativePath(dir.path()), L"estado\\synkro.db"));
        QVERIFY2(st.ok(), st.ok() ? "" : st.error().message.c_str());
        StateStore& s = st.value();

        BackupResult ok;
        ok.runId = L"r-20261118-220000-aaaa";
        ok.status = RunStatus::Success;
        ok.vaults.push_back(VaultRunResult{});
        const QDateTime t0 = QDateTime::currentDateTimeUtc().addSecs(-3600);
        QVERIFY(s.recordRun(L"job", "files", "schedule", ok, t0, t0.addSecs(60)).ok());
        BackupResult bad = ok;
        bad.runId = L"r-20261118-230000-bbbb";
        bad.status = RunStatus::Failed;
        bad.error = "origem inacessível";
        QVERIFY(s.recordRun(L"job", "files", "manual", bad, t0.addSecs(120), t0.addSecs(180)).ok());

        QCOMPARE(s.lastRun(L"job")->status, std::string("failed"));
        QCOMPARE(s.lastSuccess(L"job")->runId, ok.runId);
        QCOMPARE(s.recentRuns(10).size(), size_t(2));

        AlertRow a{"no_success", "job", AlertSeverity::Critical, "msg", {}, {}};
        QVERIFY(s.raiseAlert(a).value());     // novo
        QVERIFY(!s.raiseAlert(a).value());    // já aberto
        QCOMPARE(s.openAlerts().size(), size_t(1));
        QVERIFY(s.resolveAlert("no_success", "job").ok());
        QVERIFY(s.openAlerts().empty());
        QVERIFY(s.raiseAlert(a).value());     // reabre depois de resolvido

        QVERIFY(s.setValue("config_hash", "abc").ok());
        QCOMPARE(s.getValue("config_hash"), std::string("abc"));
    }

    void alertsFromHistoryAndVaults()
    {
        Setup env;
        const ConfigSet set = env.load();
        QVERIFY2(!set.hasErrors(), qPrintable(dump(set)));
        QTemporaryDir sd;
        auto st = StateStore::open(win::joinPath(nativePath(sd.path()), L"s.db"));
        QVERIFY(st.ok());
        StateStore& s = st.value();
        const QDateTime now = QDateTime::currentDateTimeUtc();

        // Nada executado, cofre B nunca visto.
        std::vector<VaultObservation> vaults = {{"a", env.idA, true, env.a, 50, 100, -1, {}}, {"b", env.idB, false, {}, 0, 0, -1, {}}};
        auto alerts = computeAlerts(set, s, vaults, now);
        QVERIFY(hasAlert(alerts, "never_run", "arquivos"));
        QVERIFY(hasAlert(alerts, "vault_absent", "b"));
        QVERIFY(!hasAlert(alerts, "low_space", "a"));

        // Disco do cofre com defeito (SMART).
        auto sick = vaults;
        sick[0].health = 2;
        sick[0].healthText = "ST3500 — Com defeito";
        const auto sickAlerts = computeAlerts(set, s, sick, now);
        QVERIFY(hasAlert(sickAlerts, "disk_health", "a"));

        // Sucesso antigo (> 26 h) + falha na última execução + pouco espaço em A.
        BackupResult ok;
        ok.runId = L"r-20261110-220000-aaaa";
        ok.status = RunStatus::Success;
        QVERIFY(s.recordRun(L"arquivos", "files", "schedule", ok, now.addDays(-3), now.addDays(-3)).ok());
        BackupResult bad;
        bad.runId = L"r-20261117-220000-bbbb";
        bad.status = RunStatus::Failed;
        QVERIFY(s.recordRun(L"arquivos", "files", "schedule", bad, now.addDays(-1), now.addDays(-1)).ok());
        vaults[0].freeBytes = 5;
        alerts = computeAlerts(set, s, vaults, now);
        QVERIFY(hasAlert(alerts, "no_success", "arquivos"));
        QVERIFY(hasAlert(alerts, "last_run_failed", "arquivos"));
        QVERIFY(hasAlert(alerts, "low_space", "a"));

        auto ev = reconcileAlerts(s, alerts);
        QVERIFY(ev.ok());
        QCOMPARE(ev.value().opened.size(), alerts.size());

        // Tudo volta ao normal: alertas resolvidos.
        BackupResult good;
        good.runId = L"r-20261118-220000-cccc";
        good.status = RunStatus::Success;
        QVERIFY(s.recordRun(L"arquivos", "files", "schedule", good, now, now).ok());
        QVERIFY(s.recordVaultSeen(env.idB, env.b, 50, 100, true).ok());
        vaults = {{"a", env.idA, true, env.a, 50, 100, -1, {}}, {"b", env.idB, false, {}, 0, 0, -1, {}}};
        auto ev2 = reconcileAlerts(s, computeAlerts(set, s, vaults, now));
        QVERIFY(ev2.ok());
        QVERIFY(ev2.value().active.empty());
        QCOMPARE(ev2.value().resolved.size(), alerts.size());
    }

    void shutdownDecision()
    {
        Setup env("shutdown_after: true\ngroup: noite\n");
        ConfigSet set = env.load();
        const JobConfig& job = *set.findJob("arquivos");
        QVERIFY(decideShutdown(set, job, RunStatus::Success, false).shutdown);
        QVERIFY(!decideShutdown(set, job, RunStatus::Degraded, false).shutdown);
        QVERIFY(!decideShutdown(set, job, RunStatus::Success, true).shutdown);

        set.global.shutdownAllowedGroups = {"dia"};
        QVERIFY(!decideShutdown(set, job, RunStatus::Success, false).shutdown);
        set.global.shutdownAllowedGroups = {"noite"};
        QVERIFY(decideShutdown(set, job, RunStatus::Success, false).shutdown);

        // Outra tarefa depende desta: quem desliga é a última da cadeia.
        JobConfig dependent;
        dependent.id = "depois";
        dependent.name = "Depois";
        dependent.dependsOn = {"arquivos"};
        set.jobs.push_back(dependent);
        QVERIFY(!decideShutdown(set, job, RunStatus::Success, false).shutdown);
    }

    void taskXml()
    {
        QCOMPARE(win::isoDuration(600), std::wstring(L"PT10M"));
        QCOMPARE(win::isoDuration(5400), std::wstring(L"PT1H30M"));
        QCOMPARE(win::isoDuration(86400), std::wstring(L"P1D"));

        Setup env("", "schedule:\n  - weekly: { days: [mon, fri], at: \"22:30\" }\n  - on_startup: { delay: 10m }\n"
                      "  - every: 2h\n    between: [\"08:00\", \"22:00\"]\n");
        const ConfigSet set = env.load();
        QVERIFY2(!set.hasErrors(), qPrintable(dump(set)));
        const auto tasks = desiredTasks(set, L"C:\\Program Files\\Synkro\\synkro-worker.exe");
        QCOMPARE(tasks.size(), size_t(3)); // job-arquivos, Maintenance, Scrub
        QCOMPARE(tasks[0].name, std::wstring(L"job-arquivos"));
        QCOMPARE(tasks[0].arguments, std::wstring(L"backup --job arquivos --trigger schedule"));

        const QString xml = QString::fromStdWString(win::buildTaskXml(tasks[0]));
        QVERIFY2(xml.contains("<ScheduleByWeek><WeeksInterval>1</WeeksInterval><DaysOfWeek><Monday/><Friday/></DaysOfWeek>"), qPrintable(xml));
        QVERIFY(xml.contains("<StartBoundary>2026-01-01T22:30:00</StartBoundary>"));
        QVERIFY(xml.contains("<BootTrigger><Delay>PT10M</Delay></BootTrigger>"));
        QVERIFY(xml.contains("<Interval>PT2H</Interval><Duration>PT14H</Duration>"));
        QVERIFY(xml.contains("<UserId>S-1-5-18</UserId><RunLevel>HighestAvailable</RunLevel>"));
        QVERIFY(xml.contains("<StartWhenAvailable>true</StartWhenAvailable>"));
        QVERIFY(xml.contains("<MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>"));

        // Teste de restauração agendado à parte (padrão: domingo 04:00).
        QVERIFY(writeFile(win::joinPath(env.cfg, L"jobs\\banco.yml"),
                          "version: 1\nid: banco\nname: Banco\ntype: database\nengine: sqlite\n"
                          "sqlite: { files: [\"C:/PDV/caixa.db\"] }\nrestore_test: { enabled: true }\n"));
        const ConfigSet withDb = env.load();
        QVERIFY2(!withDb.hasErrors(), qPrintable(dump(withDb)));
        const auto all = desiredTasks(withDb, L"w.exe");
        auto rt = std::find_if(all.begin(), all.end(), [](const auto& t) { return t.name == L"restoretest-banco"; });
        QVERIFY(rt != all.end());
        QCOMPARE(rt->arguments, std::wstring(L"restore-test --job banco"));
        QCOMPARE(rt->triggers.size(), size_t(1));
        QCOMPARE(rt->triggers[0].kind, TriggerKind::Weekly);
        QCOMPARE(rt->triggers[0].days, std::vector<int>{7});
        QCOMPARE(rt->triggers[0].atMinutes, 240);
        QCOMPARE(tasks[1].name, std::wstring(L"Maintenance"));
        QCOMPARE(tasks[2].arguments, std::wstring(L"maintenance --scrub"));
    }

    void applyWithoutTasksCopiesConfig()
    {
        Setup env;
        const ConfigSet set = env.load();
        QVERIFY2(!set.hasErrors(), qPrintable(dump(set)));
        ApplyOptions opt;
        opt.registerTasks = false; // nunca mexer no Agendador real durante os testes
        auto r = applyConfig(set, opt);
        QVERIFY2(r.ok(), r.ok() ? "" : r.error().message.c_str());
        QVERIFY(exists(win::joinPath(env.cfg, L".lastgood\\synkro.yml")));
        QVERIFY(exists(win::joinPath(env.cfg, L".lastgood\\jobs\\arquivos.yml")));
        for (const auto& v : {env.a, env.b})
            QVERIFY(readFile(vault::JobLayout(v, L"arquivos").jobConfigFile()).contains("id: arquivos"));

        const std::string h1 = configHash(env.cfg);
        QVERIFY(writeFile(win::joinPath(env.cfg, L"jobs\\arquivos.yml"),
                          readFile(win::joinPath(env.cfg, L"jobs\\arquivos.yml")) + "# comentário\n"));
        QVERIFY(configHash(env.cfg) != h1);
    }

    void applyRefusesInvalidConfig()
    {
        Setup env;
        QVERIFY(writeFile(win::joinPath(env.cfg, L"jobs\\ruim.yml"), "version: 1\nid: ruim\nname: x\ntype: files\n"));
        const ConfigSet set = env.load();
        QVERIFY(set.hasErrors());
        ApplyOptions opt;
        opt.registerTasks = false;
        QVERIFY(!applyConfig(set, opt).ok());
        QVERIFY(!exists(win::joinPath(env.cfg, L".lastgood")));
    }

    void scrubRepairsFromOtherVault()
    {
        Setup env;
        const ConfigSet set = env.load();
        auto req = makeBackupRequest(set, *set.findJob("arquivos"));
        QVERIFY(req.ok());
        QCOMPARE(runBackup(req.value()).status, RunStatus::Success);

        const vault::JobLayout A(env.a, L"arquivos");
        QVERIFY(writeFile(A.currentFile(L"x.txt"), "corrompido!"));
        QVERIFY(::DeleteFileW(win::toExtendedPath(A.currentFile(L"y.txt")).c_str()));

        auto noRepair = runScrub(set, 1.0, false);
        QCOMPARE(noRepair.problems.size(), size_t(2));

        auto report = runScrub(set, 1.0, true);
        QCOMPARE(report.repaired, size_t(2));
        QVERIFY(report.problems.empty());
        QCOMPARE(readFile(A.currentFile(L"x.txt")), QByteArray("conteudo x"));
        QVERIFY(verifyVault(A).value().healthy());
    }

    void registerRealTaskIsManual()
    {
        QSKIP("registrar tarefas altera o Agendador do Windows desta máquina; validar manualmente com "
              "synkro-worker config apply em ambiente de teste (como administrador)");
    }
};

QTEST_GUILESS_MAIN(TstOperations)
#include "tst_operations.moc"
