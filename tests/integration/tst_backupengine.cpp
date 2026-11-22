#include "catalog/Catalog.h"
#include "catalog/Manifest.h"
#include "core/RunId.h"
#include "engine/BackupEngine.h"
#include "engine/Journal.h"
#include "engine/Recovery.h"
#include "engine/Restore.h"
#include "engine/Retention.h"
#include "engine/VaultOps.h"
#include "engine/Verify.h"
#include "platform/windows/Locks.h"
#include "vault/JobLayout.h"
#include "vault/Vault.h"

#include "TestFs.h"

#include <QTemporaryDir>
#include <QTest>
#include <QThread>

#include <atomic>
#include <thread>

using namespace synkro;
using namespace synkro::test;

namespace {

// Ambiente de teste: uma origem e um ou mais cofres em pastas temporárias.
struct Env {
    QTemporaryDir dir;
    std::wstring root = nativePath(dir.path());
    std::wstring source = win::joinPath(root, L"origem");
    std::wstring vaultA = win::joinPath(root, L"discoA");
    std::wstring vaultB = win::joinPath(root, L"discoB");

    Env()
    {
        (void)win::createDirectories(source);
        (void)win::createDirectories(vaultA);
        (void)win::createDirectories(vaultB);
        (void)vault::initialize(vaultA, QStringLiteral("A"));
        (void)vault::initialize(vaultB, QStringLiteral("B"));
    }

    void put(const std::wstring& rel, const QByteArray& content) const
    {
        QVERIFY(writeFile(win::joinPath(source, rel), content));
    }

    void touchLater(const std::wstring& rel) const
    {
        // Garante mtime diferente mesmo em sequências rápidas.
        win::UniqueHandle h(::CreateFileW(win::toExtendedPath(win::joinPath(source, rel)).c_str(),
                                          FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr));
        FILETIME ft{};
        ::GetSystemTimeAsFileTime(&ft);
        ULARGE_INTEGER u{{ft.dwLowDateTime, ft.dwHighDateTime}};
        u.QuadPart += 10'000'000ull * 60; // +1 min
        ft.dwLowDateTime = u.LowPart;
        ft.dwHighDateTime = u.HighPart;
        ::SetFileTime(h.get(), nullptr, nullptr, &ft);
    }

    BackupRequest request(std::vector<std::wstring> vaults = {}) const
    {
        BackupRequest r;
        r.jobId = L"teste";
        r.sourceRoot = source;
        r.mainAlias = L"origem";
        if (vaults.empty())
            vaults = {vaultA};
        for (auto& v : vaults)
            r.vaults.push_back({v, {}, {}});
        r.applyRetention = false;
        return r;
    }

    vault::JobLayout layout(const std::wstring& v) const { return vault::JobLayout(v, L"teste"); }
};

const char* err(const BackupResult& r)
{
    static std::string s;
    s = r.error;
    for (const auto& v : r.vaults)
        s += " | " + v.error;
    return s.c_str();
}

} // namespace

class TstBackupEngine : public QObject {
    Q_OBJECT

private slots:
    void firstBackupThenNoChanges()
    {
        Env env;
        env.put(L"XML\\a.xml", "<a/>");
        env.put(L"XML\\b.xml", "<b/>");
        env.put(L"Config\\pdv.ini", "x=1");

        auto r1 = runBackup(env.request());
        QVERIFY2(r1.status == RunStatus::Success, err(r1));
        QCOMPARE(r1.vaults[0].added, 3ull);

        const auto L = env.layout(env.vaultA);
        QCOMPARE(readFile(L.currentFile(L"XML/a.xml")), QByteArray("<a/>"));
        auto cat = Catalog::load(L.catalogFile());
        QVERIFY(cat.ok());
        QCOMPARE(cat.value().entries.size(), size_t(3));
        QCOMPARE(cat.value().runId, r1.runId);
        QVERIFY(!ops::pathExists(L.journalFile(r1.runId)));  // diário removido no sucesso
        QVERIFY(ops::pathExists(L.manifestFile(r1.runId)));
        QVERIFY(ops::pathExists(L.summaryFile(r1.runId)));

        auto r2 = runBackup(env.request());
        QVERIFY2(r2.status == RunStatus::Success, err(r2));
        QCOMPARE(r2.vaults[0].added + r2.vaults[0].modified + r2.vaults[0].deleted, 0ull);
        QCOMPARE(r2.vaults[0].unchanged, 3ull);
    }

    // Tarefa antiga (uma pasta, na raiz) que ganha uma segunda pasta: o cofre
    // é reorganizado — cada pasta na sua subpasta — sem recopiar nada, e a
    // restauração de antes da mudança continua funcionando.
    void addingSourceReorganizesWithoutCopying()
    {
        Env env;
        env.put(L"XML\\a.xml", "<a v='1'/>");
        auto r1 = runBackup(env.request());
        QVERIFY2(r1.status == RunStatus::Success, err(r1));
        const auto L = env.layout(env.vaultA);
        QCOMPARE(readFile(L.currentFile(L"XML/a.xml")), QByteArray("<a v='1'/>")); // pasta única: raiz

        const std::wstring fiscal = win::joinPath(env.root, L"fiscal");
        QVERIFY(writeFile(win::joinPath(fiscal, L"2026\\nota.xml"), "<nota/>"));
        auto req = env.request();
        req.moreSources.push_back({fiscal, L"Fiscal"});
        QThread::msleep(1100);
        auto r2 = runBackup(req);
        QVERIFY2(r2.status == RunStatus::Success, err(r2));
        QCOMPARE(r2.vaults[0].reorganized, size_t(1));
        QCOMPARE(r2.vaults[0].added, 1ull);      // só a pasta nova é copiada
        QCOMPARE(r2.vaults[0].deleted, 0ull);
        QCOMPARE(r2.vaults[0].modified, 0ull);
        QCOMPARE(r2.vaults[0].unchanged, 1ull);
        QCOMPARE(readFile(L.currentFile(L"origem/XML/a.xml")), QByteArray("<a v='1'/>"));
        QCOMPARE(readFile(L.currentFile(L"Fiscal/2026/nota.xml")), QByteArray("<nota/>"));
        QVERIFY(!ops::pathExists(L.currentFile(L"XML/a.xml")));
        QVERIFY(!ops::pathExists(win::joinPath(L.root(), L"layout-migration.json")));

        // Mudança depois da reorganização: a versão anterior vai para versions/.
        env.put(L"XML\\a.xml", "<a v='2' maior='sim'/>");
        env.touchLater(L"XML\\a.xml");
        QThread::msleep(1100);
        auto r3 = runBackup(req);
        QVERIFY2(r3.status == RunStatus::Success, err(r3));
        QCOMPARE(r3.vaults[0].modified, 1ull);
        QCOMPARE(readFile(L.versionFile(r3.runId, L"origem/XML/a.xml")), QByteArray("<a v='1'/>"));

        RestoreRequest rq;
        rq.vaultLocation = env.vaultA;
        rq.jobId = L"teste";
        rq.targetDirectory = win::joinPath(env.root, L"restaurado");
        auto latest = restore(rq);
        QVERIFY2(latest.ok(), latest.ok() ? "" : latest.error().message.c_str());
        QCOMPARE(latest.value().restored, size_t(2));
        QCOMPARE(readFile(win::joinPath(rq.targetDirectory, L"origem\\XML\\a.xml")), QByteArray("<a v='2' maior='sim'/>"));
        QCOMPARE(readFile(win::joinPath(rq.targetDirectory, L"Fiscal\\2026\\nota.xml")), QByteArray("<nota/>"));

        // Execução de antes da reorganização: volta como era (pasta única).
        rq.runId = r1.runId;
        rq.targetDirectory = win::joinPath(env.root, L"restaurado-r1");
        auto old = restore(rq);
        QVERIFY2(old.ok(), old.ok() ? "" : old.error().message.c_str());
        QVERIFY2(old.value().failures.empty(),
                 old.value().failures.empty() ? "" : old.value().failures.front().second.c_str());
        QCOMPARE(old.value().restored, size_t(1));
        QCOMPARE(readFile(win::joinPath(rq.targetDirectory, L"XML\\a.xml")), QByteArray("<a v='1'/>"));
    }

    // Cofre da 0.9.0-beta.6: pasta principal na raiz e a adicional em <alias>/
    // ao lado. A reorganização põe a principal na subpasta e deixa a outra.
    void beta6LayoutIsReorganized()
    {
        Env env;
        env.put(L"XML\\a.xml", "<a/>");
        env.put(L"Fiscal\\nota.xml", "<nota/>"); // simula "Fiscal/" gravado pela pasta adicional
        auto r1 = runBackup(env.request());
        QVERIFY2(r1.status == RunStatus::Success, err(r1));

        // A pasta adicional é a mesma (movida, mantém identidade e datas).
        const std::wstring fiscal = win::joinPath(env.root, L"fiscal");
        QVERIFY(::MoveFileExW(win::toExtendedPath(win::joinPath(env.source, L"Fiscal")).c_str(),
                              win::toExtendedPath(fiscal).c_str(), 0));
        auto req = env.request();
        req.moreSources.push_back({fiscal, L"Fiscal"});
        QThread::msleep(1100);
        auto r2 = runBackup(req);
        QVERIFY2(r2.status == RunStatus::Success, err(r2));
        QCOMPARE(r2.vaults[0].reorganized, size_t(1)); // só o arquivo da principal mudou de lugar
        QCOMPARE(r2.vaults[0].added + r2.vaults[0].modified + r2.vaults[0].deleted, 0ull);
        QCOMPARE(r2.vaults[0].unchanged, 2ull);
        const auto L = env.layout(env.vaultA);
        QCOMPARE(readFile(L.currentFile(L"origem/XML/a.xml")), QByteArray("<a/>"));
        QCOMPARE(readFile(L.currentFile(L"Fiscal/nota.xml")), QByteArray("<nota/>"));
    }

    // Queda de energia no meio da reorganização: a próxima execução termina.
    void interruptedReorganizationResumes()
    {
        Env env;
        env.put(L"XML\\a.xml", "<a/>");
        auto r1 = runBackup(env.request());
        QVERIFY2(r1.status == RunStatus::Success, err(r1));
        const auto L = env.layout(env.vaultA);

        // Reproduz o passo 1 já feito: marcador gravado e current\ movido para
        // current.layout\origem, mas current.layout ainda não renomeado.
        const QByteArray marker = "{\"format\":\"synkro-layout-migration\",\"main\":\"origem\",\"extras\":[\"Fiscal\"],"
                                  "\"run_id\":\"" + QString::fromStdWString(newRunId()).toUtf8() + "\",\"token\":\"abc123\"}";
        QVERIFY(writeFile(win::joinPath(L.root(), L"layout-migration.json"), marker));
        QVERIFY(writeFile(win::joinPath(L.current(), L".synkro-layout-abc123"), ""));
        const std::wstring staging = win::joinPath(L.root(), L"current.layout");
        QVERIFY(win::createDirectories(staging).ok());
        QVERIFY(::MoveFileExW(win::toExtendedPath(L.current()).c_str(),
                              win::toExtendedPath(win::joinPath(staging, L"origem")).c_str(), 0));

        const std::wstring fiscal = win::joinPath(env.root, L"fiscal");
        QVERIFY(writeFile(win::joinPath(fiscal, L"nota.xml"), "<nota/>"));
        auto req = env.request();
        req.moreSources.push_back({fiscal, L"Fiscal"});
        QThread::msleep(1100);
        auto r2 = runBackup(req);
        QVERIFY2(r2.status == RunStatus::Success, err(r2));
        QCOMPARE(r2.vaults[0].unchanged, 1ull);
        QCOMPARE(r2.vaults[0].added, 1ull);
        QCOMPARE(readFile(L.currentFile(L"origem/XML/a.xml")), QByteArray("<a/>"));
        QVERIFY(!ops::pathExists(staging));
        QVERIFY(!ops::pathExists(win::joinPath(L.root(), L"layout-migration.json")));
        QVERIFY(!ops::pathExists(L.currentFile(L"origem/.synkro-layout-abc123")));
    }

    // Volta a ter uma pasta só: o cofre já organizado continua em subpastas.
    void folderLayoutIsKeptWithSingleSource()
    {
        Env env;
        env.put(L"a.txt", "a");
        const std::wstring extra = win::joinPath(env.root, L"extra");
        QVERIFY(writeFile(win::joinPath(extra, L"b.txt"), "b"));
        auto req = env.request();
        req.moreSources.push_back({extra, L"extra"});
        auto r1 = runBackup(req);
        QVERIFY2(r1.status == RunStatus::Success, err(r1));
        QThread::msleep(1100);
        auto r2 = runBackup(env.request()); // só a principal
        QCOMPARE(int(r2.status), int(RunStatus::Success));
        QCOMPARE(r2.vaults[0].unchanged, 1ull);  // origem/a.txt continua no lugar
        QCOMPARE(r2.vaults[0].deleted, 1ull);    // extra/b.txt sai (vira versão)
        QVERIFY(ops::pathExists(env.layout(env.vaultA).currentFile(L"origem/a.txt")));
    }

    void emptyExtraSourceIsBlocked()
    {
        Env env;
        for (int i = 0; i < 20; ++i)
            env.put(L"main" + std::to_wstring(i) + L".txt", "m");
        const std::wstring extra = win::joinPath(env.root, L"pendrive");
        QVERIFY(writeFile(win::joinPath(extra, L"x.txt"), "x"));
        auto req = env.request();
        req.moreSources.push_back({extra, L"pendrive"});
        auto r1 = runBackup(req);
        QVERIFY2(r1.status == RunStatus::Success, err(r1));

        // A pasta adicional esvaziou (pendrive que não montou): 1 de 21 arquivos
        // passaria pela guarda de exclusão em massa, mas a de pasta vazia pega.
        QVERIFY(::DeleteFileW(win::toExtendedPath(win::joinPath(extra, L"x.txt")).c_str()));
        auto r2 = runBackup(req);
        QCOMPARE(int(r2.status), int(RunStatus::Blocked));
        QVERIFY(ops::pathExists(env.layout(env.vaultA).currentFile(L"pendrive/x.txt")));
    }

    void modificationKeepsPreviousVersionAndPointInTimeRestore()
    {
        Env env;
        env.put(L"nota.xml", "versao-1");
        auto r1 = runBackup(env.request());
        QVERIFY2(r1.status == RunStatus::Success, err(r1));

        env.put(L"nota.xml", "versao-2-maior");
        env.touchLater(L"nota.xml");
        QThread::msleep(1100); // run IDs com segundos diferentes
        auto r2 = runBackup(env.request());
        QVERIFY2(r2.status == RunStatus::Success, err(r2));
        QCOMPARE(r2.vaults[0].modified, 1ull);

        const auto L = env.layout(env.vaultA);
        QCOMPARE(readFile(L.currentFile(L"nota.xml")), QByteArray("versao-2-maior"));
        QCOMPARE(readFile(L.versionFile(r2.runId, L"nota.xml")), QByteArray("versao-1"));

        // Restaura o estado atual e o da primeira execução.
        RestoreRequest rq;
        rq.vaultLocation = env.vaultA;
        rq.jobId = L"teste";
        rq.targetDirectory = win::joinPath(env.root, L"restaurado-atual");
        auto latest = restore(rq);
        QVERIFY2(latest.ok(), latest.ok() ? "" : latest.error().message.c_str());
        QCOMPARE(latest.value().restored, size_t(1));
        QCOMPARE(readFile(win::joinPath(rq.targetDirectory, L"nota.xml")), QByteArray("versao-2-maior"));

        rq.runId = r1.runId;
        rq.targetDirectory = win::joinPath(env.root, L"restaurado-r1");
        auto old = restore(rq);
        QVERIFY2(old.ok(), old.ok() ? "" : old.error().message.c_str());
        QVERIFY(old.value().failures.empty());
        QCOMPARE(readFile(win::joinPath(rq.targetDirectory, L"nota.xml")), QByteArray("versao-1"));
    }

    void deletionMovesToVersionsAndIsRestorable()
    {
        Env env;
        env.put(L"a.txt", "A");
        env.put(L"b.txt", "B");
        auto r1 = runBackup(env.request());
        QVERIFY(r1.status == RunStatus::Success);

        QVERIFY(::DeleteFileW(win::toExtendedPath(win::joinPath(env.source, L"b.txt")).c_str()));
        QThread::msleep(1100);
        auto r2 = runBackup(env.request());
        QVERIFY2(r2.status == RunStatus::Success, err(r2));
        QCOMPARE(r2.vaults[0].deleted, 1ull);

        const auto L = env.layout(env.vaultA);
        QVERIFY(!ops::pathExists(L.currentFile(L"b.txt")));
        QCOMPARE(readFile(L.versionFile(r2.runId, L"b.txt")), QByteArray("B")); // nada é apagado

        RestoreRequest rq;
        rq.vaultLocation = env.vaultA;
        rq.jobId = L"teste";
        rq.runId = r1.runId;
        rq.targetDirectory = win::joinPath(env.root, L"r");
        auto res = restore(rq);
        QVERIFY(res.ok());
        QCOMPARE(res.value().restored, size_t(2));
        QCOMPARE(readFile(win::joinPath(rq.targetDirectory, L"b.txt")), QByteArray("B"));
    }

    void massDeletionBlocksAndLeavesVaultUntouched()
    {
        Env env;
        for (int i = 0; i < 30; ++i)
            env.put(L"f" + std::to_wstring(i) + L".txt", QByteArray::number(i));
        auto r1 = runBackup(env.request());
        QVERIFY(r1.status == RunStatus::Success);

        for (int i = 0; i < 10; ++i) // 33% > 20%
            ::DeleteFileW(win::toExtendedPath(win::joinPath(env.source, L"f" + std::to_wstring(i) + L".txt")).c_str());

        auto r2 = runBackup(env.request());
        QCOMPARE(r2.status, RunStatus::Blocked);
        QCOMPARE(r2.vaults[0].guard.code, GuardCode::MassDeletion);

        const auto L = env.layout(env.vaultA);
        QVERIFY(ops::pathExists(L.currentFile(L"f0.txt")));
        QCOMPARE(Catalog::load(L.catalogFile()).value().entries.size(), size_t(30));

        // Com confirmação explícita, a exclusão é aplicada (e preservada em versions/).
        auto req = env.request();
        req.overrideGuards = true;
        QThread::msleep(1100);
        auto r3 = runBackup(req);
        QVERIFY2(r3.status == RunStatus::Success, err(r3));
        QCOMPARE(r3.vaults[0].deleted, 10ull);
    }

    void emptySourceIsBlocked()
    {
        Env env;
        env.put(L"a.txt", "A");
        QVERIFY(runBackup(env.request()).status == RunStatus::Success);
        ::DeleteFileW(win::toExtendedPath(win::joinPath(env.source, L"a.txt")).c_str());
        auto r = runBackup(env.request());
        QCOMPARE(r.status, RunStatus::Blocked);
        QCOMPARE(r.vaults[0].guard.code, GuardCode::EmptySource);
    }

    void missingSourceFailsWithoutTouchingVault()
    {
        Env env;
        env.put(L"a.txt", "A");
        QVERIFY(runBackup(env.request()).status == RunStatus::Success);

        auto req = env.request();
        req.sourceRoot = win::joinPath(env.root, L"origem-que-sumiu");
        auto r = runBackup(req);
        QCOMPARE(r.status, RunStatus::Failed);
        QVERIFY(r.vaults.empty());
        QVERIFY(ops::pathExists(env.layout(env.vaultA).currentFile(L"a.txt")));
    }

    void uninitializedVaultFails()
    {
        Env env;
        env.put(L"a.txt", "A");
        const std::wstring notVault = win::joinPath(env.root, L"disco-sem-cofre");
        QVERIFY(win::createDirectories(notVault).ok());
        auto r = runBackup(env.request({notVault}));
        QCOMPARE(r.status, RunStatus::Failed);
        QCOMPARE(r.vaults[0].status, VaultRunStatus::Failed);
        QVERIFY(!ops::pathExists(win::joinPath(notVault, L"SynkroVault")));
    }

    void redundancyAcrossTwoVaults()
    {
        Env env;
        env.put(L"a.txt", "A");
        auto r = runBackup(env.request({env.vaultA, env.vaultB}));
        QVERIFY2(r.status == RunStatus::Success, err(r));
        QCOMPARE(readFile(env.layout(env.vaultB).currentFile(L"a.txt")), QByteArray("A"));

        // Um cofre ausente: Degradado com min_vaults_ok = todos; Sucesso com 1.
        const std::wstring missing = win::joinPath(env.root, L"disco-desconectado");
        auto d = runBackup(env.request({env.vaultA, missing}));
        QCOMPARE(d.status, RunStatus::Degraded);

        auto req = env.request({env.vaultA, missing});
        req.minVaultsOk = 1;
        QCOMPARE(runBackup(req).status, RunStatus::Success);
    }

    void wrongVaultIdIsRejected()
    {
        Env env;
        env.put(L"a.txt", "A");
        auto req = env.request();
        req.vaults[0].expectedVaultId = QStringLiteral("00000000-0000-0000-0000-000000000001");
        auto r = runBackup(req);
        QCOMPARE(r.status, RunStatus::Failed);
        QVERIFY(!ops::pathExists(env.layout(env.vaultA).current()));
    }

    void filterExcludesFiles()
    {
        Env env;
        env.put(L"XML\\a.xml", "a");
        env.put(L"Logs\\app.log", "log");
        env.put(L"x.tmp", "t");
        auto req = env.request();
        req.filter = PathFilter({}, {L"Logs/**", L"*.tmp"});
        auto r = runBackup(req);
        QVERIFY(r.status == RunStatus::Success);
        QCOMPARE(r.vaults[0].added, 1ull);
    }

    void verifyDetectsCorruption()
    {
        Env env;
        env.put(L"a.txt", "conteudo original");
        env.put(L"b.txt", "B");
        QVERIFY(runBackup(env.request()).status == RunStatus::Success);

        const auto L = env.layout(env.vaultA);
        auto ok = verifyVault(L);
        QVERIFY(ok.ok());
        QVERIFY(ok.value().healthy());
        QCOMPARE(ok.value().ok, size_t(2));

        QVERIFY(writeFile(L.currentFile(L"a.txt"), "conteudo adulterado"));
        QVERIFY(::DeleteFileW(win::toExtendedPath(L.currentFile(L"b.txt")).c_str()));
        auto bad = verifyVault(L);
        QVERIFY(bad.ok());
        QCOMPARE(bad.value().corrupted.size(), size_t(1));
        QCOMPARE(bad.value().missing.size(), size_t(1));
    }

    void restoreRefusesNonEmptyTarget()
    {
        Env env;
        env.put(L"a.txt", "A");
        QVERIFY(runBackup(env.request()).status == RunStatus::Success);
        const std::wstring target = win::joinPath(env.root, L"alvo");
        QVERIFY(writeFile(win::joinPath(target, L"ja-existe.txt"), "x"));

        RestoreRequest rq;
        rq.vaultLocation = env.vaultA;
        rq.jobId = L"teste";
        rq.targetDirectory = target;
        auto r = restore(rq);
        QVERIFY(!r.ok());
        QCOMPARE(r.error().code, ErrorCode::AlreadyExists);
        QCOMPARE(readFile(win::joinPath(target, L"ja-existe.txt")), QByteArray("x"));
    }

    void recoveryMovesInterruptedSwapBackAndAppliesJournal()
    {
        Env env;
        env.put(L"a.txt", "A");
        QVERIFY(runBackup(env.request()).status == RunStatus::Success);
        const auto L = env.layout(env.vaultA);

        // Simula uma queda: a versão de a.txt foi para versions/ mas a nova
        // não chegou; b.txt foi copiado e confirmado no diário.
        const std::wstring run = newRunId();
        QVERIFY(ops::moveCurrentToVersions(L, run, L"a.txt").ok());
        QVERIFY(writeFile(L.currentFile(L"b.txt"), "B"));
        QVERIFY(writeFile(win::joinPath(L.tmp(), L"synkro-lixo.part"), "lixo"));
        {
            auto j = Journal::create(L.journalFile(run), run);
            QVERIFY(j.ok());
            ManifestRecord rec;
            rec.op = ManifestOp::Added;
            rec.path = L"b.txt";
            CatalogEntry e;
            e.meta.relativePath = L"b.txt";
            e.meta.size = 1;
            e.sha256 = Sha256::ofBytes("B").value();
            e.sinceRun = run;
            rec.entry = e;
            QVERIFY(j.value().recordDone(rec).ok());
            QVERIFY(j.value().flush().ok());
        }

        Catalog cat = Catalog::load(L.catalogFile()).value();
        auto rec = recoverInterruptedRuns(L, cat);
        QVERIFY2(rec.ok(), rec.ok() ? "" : rec.error().message.c_str());
        QCOMPARE(rec.value().size(), size_t(1));
        QCOMPARE(rec.value()[0].versionsMovedBack, size_t(1));
        QCOMPARE(rec.value()[0].recordsApplied, size_t(1));

        QCOMPARE(readFile(L.currentFile(L"a.txt")), QByteArray("A"));
        QVERIFY(!ops::pathExists(L.journalFile(run)));
        QVERIFY(ops::pathExists(L.manifestFile(run)));
        QVERIFY(ops::listFiles(L.tmp(), L"").empty());
        auto reloaded = Catalog::load(L.catalogFile());
        QVERIFY(reloaded.ok());
        QVERIFY(reloaded.value().find(L"b.txt") != nullptr);
        QVERIFY(reloaded.value().find(L"a.txt") != nullptr);

        // A próxima execução segue normalmente e o cofre fica íntegro.
        env.put(L"b.txt", "B");
        auto next = runBackup(env.request());
        QVERIFY2(next.status == RunStatus::Success, err(next));
        QVERIFY(verifyVault(L).value().healthy());
    }

    void retentionExpiresOldVersionsButKeepsRecentAndDeleted()
    {
        Env env;
        const auto L = env.layout(env.vaultA);
        const QDateTime now = QDateTime::currentDateTimeUtc();

        bool written = true;
        auto mk = [&](int daysAgo, const wchar_t* file) -> std::wstring {
            const std::wstring run = newRunId(now.addDays(-daysAgo));
            written = written && writeFile(L.versionFile(run, file), "v");
            return run;
        };
        const auto veryOld = mk(400, L"velho.txt");      // expira por completo
        const auto oldWithDelete = mk(120, L"mod.txt");  // > 90 dias, < 180
        QVERIFY(writeFile(L.versionFile(oldWithDelete, L"excluido.txt"), "e"));
        Manifest m;
        m.runId = oldWithDelete;
        m.records.push_back({ManifestOp::Deleted, L"excluido.txt", std::nullopt, std::nullopt, {}});
        QVERIFY(win::createDirectories(L.runs()).ok());
        QVERIFY(m.save(L.manifestFile(oldWithDelete)).ok());
        const auto r3 = mk(10, L"a.txt");
        const auto r4 = mk(5, L"b.txt");
        const auto r5 = mk(1, L"c.txt");
        QVERIFY(written);

        RetentionPolicy p; // 90 dias, mínimo 3 execuções, excluídos 180 dias
        auto res = applyRetention(L, p, now);
        QVERIFY(res.ok());
        QCOMPARE(res.value().runsExpired, size_t(1));
        QVERIFY(!ops::pathExists(L.versions(veryOld)));
        QVERIFY(!ops::pathExists(L.versionFile(oldWithDelete, L"mod.txt")));
        QVERIFY(ops::pathExists(L.versionFile(oldWithDelete, L"excluido.txt")));
        QCOMPARE(res.value().deletedFilesKept, size_t(1));
        for (const std::wstring& r : std::vector<std::wstring>{r3, r4, r5})
            QVERIFY(ops::pathExists(L.versions(r)));
    }

    void retentionNeverTouchesNewestRuns()
    {
        Env env;
        const auto L = env.layout(env.vaultA);
        const QDateTime now = QDateTime::currentDateTimeUtc();
        std::vector<std::wstring> runs;
        for (int i = 0; i < 3; ++i) {
            runs.push_back(newRunId(now.addDays(-500 + i)));
            QVERIFY(writeFile(L.versionFile(runs.back(), L"x.txt"), "v"));
        }
        auto res = applyRetention(L, RetentionPolicy{}, now);
        QVERIFY(res.ok());
        QCOMPARE(res.value().runsExpired, size_t(0)); // mesmo antigas, são as 3 mais recentes
    }

    void caseOnlyRenameUpdatesCurrent()
    {
        Env env;
        env.put(L"Nota.XML", "n");
        QVERIFY(runBackup(env.request()).status == RunStatus::Success);
        QVERIFY(::MoveFileW(win::toExtendedPath(win::joinPath(env.source, L"Nota.XML")).c_str(),
                            win::toExtendedPath(win::joinPath(env.source, L"nota.xml")).c_str()));
        QThread::msleep(1100);
        auto r = runBackup(env.request());
        QVERIFY2(r.status == RunStatus::Success, err(r));
        QCOMPARE(r.vaults[0].modified, 1ull);
        auto cat = Catalog::load(env.layout(env.vaultA).catalogFile()).value();
        QCOMPARE(cat.entries.size(), size_t(1));
        QCOMPARE(cat.entries[0].meta.relativePath, std::wstring(L"nota.xml"));
        QVERIFY(verifyVault(env.layout(env.vaultA)).value().healthy());
    }

    void jobLockPreventsConcurrentRuns()
    {
        std::atomic<bool> holding{false}, release{false};
        std::thread holder([&] {
            auto lock = win::JobLock::tryAcquire(L"trava-teste");
            QVERIFY(lock.ok());
            holding = true;
            while (!release)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
        });
        while (!holding)
            std::this_thread::yield();

        auto second = win::JobLock::tryAcquire(L"trava-teste");
        QVERIFY(!second.ok());
        QCOMPARE(second.error().code, ErrorCode::AlreadyExists);

        release = true;
        holder.join();
        QVERIFY(win::JobLock::tryAcquire(L"trava-teste").ok());
    }
};

QTEST_GUILESS_MAIN(TstBackupEngine)
#include "tst_backupengine.moc"
