#include "engine/BackupEngine.h"
#include "engine/Hooks.h"
#include "platform/windows/Snapshot.h"
#include "platform/windows/Volume.h"
#include "process/ProcessRunner.h"
#include "vault/Vault.h"

#include "TestFs.h"

#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QTest>

#include <shellapi.h>

using namespace synkro;
using namespace synkro::test;

namespace {

// /s: o cmd remove só as aspas externas e executa o resto como está.
std::vector<std::wstring> cmd(const std::wstring& script)
{
    return {L"C:\\Windows\\System32\\cmd.exe", L"/d", L"/s", L"/c", script};
}

QByteArray describe(const BackupResult& r)
{
    QByteArray s = QByteArray::fromStdString(r.error);
    for (const auto& h : r.hooks)
        s += " | " + QByteArray(h.phase.c_str()) + " ok=" + QByteArray::number(h.ok) + " " +
             QByteArray::fromStdString(h.message) + " [" + QString::fromStdWString(h.command).toUtf8() + "]";
    return s;
}

} // namespace

class TstProcess : public QObject {
    Q_OBJECT

private slots:
    void commandLineQuotingRoundTrip()
    {
        const std::vector<std::wstring> args = {L"C:\\Program Files\\x.exe", L"simples", L"com espaço",
                                                L"aspas \"dentro\"", L"barra\\final\\", L"", L"a\\\\\"b"};
        const std::wstring line = buildCommandLine(args);
        int argc = 0;
        LPWSTR* argv = ::CommandLineToArgvW(line.c_str(), &argc);
        QCOMPARE(argc, static_cast<int>(args.size()));
        for (int i = 0; i < argc; ++i)
            QCOMPARE(QString::fromWCharArray(argv[i]), QString::fromStdWString(args[static_cast<size_t>(i)]));
        ::LocalFree(argv);
    }

    void capturesOutputAndExitCode()
    {
        ProcessSpec spec;
        spec.args = cmd(L"echo saida& echo erro 1>&2& exit /b 7");
        auto r = runProcess(spec);
        QVERIFY(r.ok());
        QCOMPARE(r.value().exitCode, 7ul);
        QVERIFY(r.value().stdoutText.find("saida") != std::string::npos);
        QVERIFY(r.value().stderrText.find("erro") != std::string::npos);
        QVERIFY(!r.value().timedOut);
    }

    void environmentOnlyForChild()
    {
        ProcessSpec spec;
        spec.args = cmd(L"echo %SYNKRO_TESTE_SEGREDO%");
        spec.environment = {{L"SYNKRO_TESTE_SEGREDO", L"valor-filho"}};
        auto r = runProcess(spec);
        QVERIFY(r.ok());
        QVERIFY(r.value().stdoutText.find("valor-filho") != std::string::npos);
        QVERIFY(qEnvironmentVariableIsEmpty("SYNKRO_TESTE_SEGREDO"));
    }

    void streamsStdout()
    {
        ProcessSpec spec;
        spec.args = cmd(L"for /l %i in (1,1,2000) do @echo linha %i");
        std::size_t bytes = 0;
        spec.onStdout = [&](const char*, std::size_t n) {
            bytes += n;
            return Status::success();
        };
        auto r = runProcess(spec);
        QVERIFY(r.ok());
        QVERIFY(bytes > 2000 * 7);
        QVERIFY(r.value().stdoutText.empty());
    }

    void timeoutKillsProcessTree()
    {
        ProcessSpec spec;
        // cmd inicia um ping (processo neto) que duraria ~30 s.
        spec.args = cmd(L"ping -n 30 127.0.0.1 >nul");
        spec.timeoutMs = 800;
        QElapsedTimer t;
        t.start();
        auto r = runProcess(spec);
        QVERIFY(r.ok());
        QVERIFY(r.value().timedOut);
        QVERIFY2(t.elapsed() < 10000, "o processo neto não foi encerrado junto");
    }

    void missingExecutable()
    {
        ProcessSpec spec;
        spec.args = {L"C:\\nao\\existe\\programa.exe"};
        auto r = runProcess(spec);
        QVERIFY(!r.ok());
    }

    void expandVariables()
    {
        QCOMPARE(synkro::expandVariables(L"--saida={job}-{run_id}.txt {desconhecida}", {{L"job", L"j"}, {L"run_id", L"r1"}}),
                 std::wstring(L"--saida=j-r1.txt {desconhecida}"));
    }

    void beforeHookFailureSkipsBackup()
    {
        QTemporaryDir dir;
        const std::wstring root = nativePath(dir.path());
        const std::wstring src = win::joinPath(root, L"origem"), disk = win::joinPath(root, L"disco");
        QVERIFY(writeFile(win::joinPath(src, L"a.txt"), "a"));
        QVERIFY(win::createDirectories(disk).ok());
        QVERIFY(vault::initialize(disk, QStringLiteral("d")).ok());
        const std::wstring marker = win::joinPath(root, L"on_failure.txt");

        BackupRequest req;
        req.jobId = L"hooks";
        req.sourceRoot = src;
        req.vaults.push_back({disk, {}, {}});
        req.hooks.before.push_back({cmd(L"exit /b 3"), 30, true});
        req.hooks.onFailure.push_back({cmd(L"echo falhou> " + marker), 30, true});

        auto r = runBackup(req);
        QCOMPARE(r.status, RunStatus::Failed);
        QVERIFY(r.vaults.empty());
        QCOMPARE(r.hooks.size(), size_t(2));
        QCOMPARE(r.hooks[0].exitCode, 3ul);
        QVERIFY(exists(marker));
    }

    void hooksRunInOrderAndWarnDoesNotFail()
    {
        QTemporaryDir dir;
        const std::wstring root = nativePath(dir.path());
        const std::wstring src = win::joinPath(root, L"origem"), disk = win::joinPath(root, L"disco");
        QVERIFY(writeFile(win::joinPath(src, L"a.txt"), "a"));
        QVERIFY(win::createDirectories(disk).ok());
        QVERIFY(vault::initialize(disk, QStringLiteral("d")).ok());
        const std::wstring log = win::joinPath(root, L"ordem.txt");

        BackupRequest req;
        req.jobId = L"hooks2";
        req.sourceRoot = src;
        req.vaults.push_back({disk, {}, {}});
        req.hooks.before.push_back({cmd(L"echo before {job}>> " + log), 30, true});
        req.hooks.before.push_back({cmd(L"exit /b 1"), 30, false}); // on_error: warn
        req.hooks.after.push_back({cmd(L"echo after>> " + log), 30, true});
        req.hooks.onSuccess.push_back({cmd(L"echo success>> " + log), 30, true});

        auto r = runBackup(req);
        QVERIFY2(r.status == RunStatus::Success, describe(r).constData());
        const QByteArray content = readFile(log);
        QVERIFY2(content.contains("before hooks2") && content.indexOf("after") > content.indexOf("before")
                     && content.indexOf("success") > content.indexOf("after"),
                 content.constData());
    }

    void afterHookFailureDegradesRun()
    {
        QTemporaryDir dir;
        const std::wstring root = nativePath(dir.path());
        const std::wstring src = win::joinPath(root, L"origem"), disk = win::joinPath(root, L"disco");
        QVERIFY(writeFile(win::joinPath(src, L"a.txt"), "a"));
        QVERIFY(win::createDirectories(disk).ok());
        QVERIFY(vault::initialize(disk, QStringLiteral("d")).ok());

        BackupRequest req;
        req.jobId = L"hooks3";
        req.sourceRoot = src;
        req.vaults.push_back({disk, {}, {}});
        req.hooks.after.push_back({cmd(L"exit /b 2"), 30, true});
        auto r = runBackup(req);
        QCOMPARE(r.status, RunStatus::Degraded);
        QCOMPARE(r.vaults[0].status, VaultRunStatus::Success); // os dados foram copiados
    }

    void snapshotPathTranslation()
    {
        const std::wstring dev = L"\\\\?\\GLOBALROOT\\Device\\HarddiskVolumeShadowCopy7";
        QCOMPARE(win::translateToSnapshot(L"C:\\", dev, L"C:\\PDV\\XML"), dev + L"\\PDV\\XML");
        QCOMPARE(win::translateToSnapshot(L"C:\\", dev, L"c:/pdv"), dev + L"\\pdv");
        QCOMPARE(win::translateToSnapshot(L"C:\\", dev, L"C:\\"), dev + L"\\");
        QVERIFY(win::translateToSnapshot(L"C:\\", dev, L"D:\\x").empty());
    }

    void snapshotWithoutElevationFallsBack()
    {
        if (win::isProcessElevated())
            QSKIP("executando como administrador; o caminho sem privilégio não se aplica");
        QTemporaryDir dir;
        const std::wstring root = nativePath(dir.path());
        const std::wstring src = win::joinPath(root, L"origem"), disk = win::joinPath(root, L"disco");
        QVERIFY(writeFile(win::joinPath(src, L"a.txt"), "a"));
        QVERIFY(win::createDirectories(disk).ok());
        QVERIFY(vault::initialize(disk, QStringLiteral("d")).ok());

        BackupRequest req;
        req.jobId = L"vss";
        req.sourceRoot = src;
        req.vaults.push_back({disk, {}, {}});
        req.useSnapshot = true;
        req.continueWithoutSnapshot = true;
        auto ok = runBackup(req);
        QCOMPARE(ok.status, RunStatus::Success);
        QVERIFY(!ok.snapshotUsed);
        QVERIFY(ok.snapshotNote.find("administrador") != std::string::npos);

        req.continueWithoutSnapshot = false;
        QCOMPARE(runBackup(req).status, RunStatus::Failed);
    }

    void snapshotWhenElevated()
    {
        if (!win::isProcessElevated())
            QSKIP("exige administrador");
        QTemporaryDir dir;
        auto vol = win::queryVolume(nativePath(dir.path()));
        QVERIFY(vol.ok());
        QVERIFY(writeFile(win::joinPath(nativePath(dir.path()), L"congelado.txt"), "antes"));
        auto snap = win::VolumeSnapshot::create(vol.value().mountPoint);
        QVERIFY2(snap.ok(), snap.ok() ? "" : snap.error().message.c_str());
        QVERIFY(writeFile(win::joinPath(nativePath(dir.path()), L"congelado.txt"), "depois"));
        const std::wstring inSnap = snap.value().translate(win::joinPath(nativePath(dir.path()), L"congelado.txt"));
        QCOMPARE(readFile(inSnap), QByteArray("antes"));
    }
};

QTEST_GUILESS_MAIN(TstProcess)
#include "tst_process.moc"
