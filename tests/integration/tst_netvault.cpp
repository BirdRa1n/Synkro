// Cofres em compartilhamento de rede (UNC).
//
// O caminho de rede real usado aqui é o compartilhamento administrativo da
// própria máquina por loopback (\\localhost\C$\...). Ele exercita o
// redirecionador SMB de verdade (gravação, troca atômica, trava, releitura
// sem cache); quando não está acessível (política da máquina), esses casos
// são pulados.

#include "config/ConfigLoader.h"
#include "config/ConfigWriter.h"
#include "config/VaultAccess.h"
#include "engine/BackupEngine.h"
#include "engine/Restore.h"
#include "hash/Sha256.h"
#include "platform/windows/NetworkShare.h"
#include "state/Alerts.h"
#include "vault/JobLayout.h"
#include "vault/Vault.h"
#include "vault/VaultCandidateCheck.h"

#include "TestFs.h"

#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QTest>

using namespace synkro;
using namespace synkro::test;

namespace {

bool hasErrorAt(const std::vector<config::Diagnostic>& all, const std::string& key)
{
    for (const auto& d : all)
        if (d.severity == config::Severity::Error && d.key == key)
            return true;
    return false;
}

std::size_t errorCount(const std::vector<config::Diagnostic>& all)
{
    return static_cast<std::size_t>(
        std::count_if(all.begin(), all.end(), [](const config::Diagnostic& d) { return d.severity == config::Severity::Error; }));
}

// "C:\Users\x\Temp\abc" -> "\\localhost\C$\Users\x\Temp\abc"
std::wstring loopbackUnc(const std::wstring& local)
{
    return L"\\\\localhost\\" + local.substr(0, 1) + L"$" + local.substr(2);
}

bool reachable(const std::wstring& unc)
{
    return ::GetFileAttributesW(win::toExtendedPath(unc).c_str()) != INVALID_FILE_ATTRIBUTES;
}

const char* kVaultYaml =
    "version: 1\n"
    "vaults:\n"
    "  - id: nas\n"
    "    vault_id: 7f1c9a2e-3b0d-4c7e-9a51-0d2b6e1f4a10\n"
    "    label: \"NAS do escritório\"\n"
    "    path: \"//nas01/backup/pdv01\"\n"
    "    credentials:\n"
    "      user: \"nas01\\\\synkro\"\n"
    "      password: { secret: nas-synkro }\n";

} // namespace

class TestNetVault : public QObject {
    Q_OBJECT

private slots:
    void uncHelpers()
    {
        QVERIFY(win::isUncPath(L"\\\\nas\\backup"));
        QVERIFY(win::isUncPath(L"//nas/backup/pdv01/"));
        QVERIFY(!win::isUncPath(L"\\\\nas"));
        QVERIFY(!win::isUncPath(L"\\\\?\\C:\\x"));
        QVERIFY(!win::isUncPath(L"\\\\.\\PhysicalDrive0"));
        QVERIFY(!win::isUncPath(L"E:\\"));
        QCOMPARE(win::normalizeUnc(L"//nas/backup/pdv01/"), std::wstring(L"\\\\nas\\backup\\pdv01"));
        QCOMPARE(win::uncServer(L"\\\\nas01\\backup\\pdv01"), std::wstring(L"nas01"));
        QCOMPARE(win::uncShareRoot(L"\\\\nas01\\backup\\pdv01\\x"), std::wstring(L"\\\\nas01\\backup"));
        QCOMPARE(win::uncShareRoot(L"\\\\nas01\\backup"), std::wstring(L"\\\\nas01\\backup"));

        QVERIFY(win::isThisComputer(L"localhost"));
        QVERIFY(win::isThisComputer(L"127.0.0.1"));
        wchar_t name[256] = {};
        DWORD size = 256;
        QVERIFY(::GetComputerNameW(name, &size));
        QVERIFY(win::isThisComputer(std::wstring(name, size)));
        QVERIFY(!win::isThisComputer(L"nas01-que-nao-existe"));
        QVERIFY(!win::isThisComputer(L"192.0.2.10"));
    }

    void parsesNetworkVault()
    {
        std::vector<config::Diagnostic> d;
        const auto g = config::parseGlobalDocument(kVaultYaml, L"synkro.yml", d);
        QCOMPARE(errorCount(d), std::size_t(0));
        const auto* v = g.findVault("nas");
        QVERIFY(v);
        QVERIFY(v->network);
        QCOMPARE(v->path, std::wstring(L"\\\\nas01\\backup\\pdv01"));
        QCOMPARE(v->netUser, std::string("nas01\\synkro"));
        QCOMPARE(v->netPassword.name, std::string("nas-synkro"));
        QCOMPARE(v->maxAbsenceSeconds, std::int64_t(86400));
        QCOMPARE(g.secrets.size(), std::size_t(1));

        // Ida e volta pelo gerador do desktop.
        std::vector<config::Diagnostic> d2;
        const auto again = config::parseGlobalDocument(config::emitGlobal(g), L"synkro.yml", d2);
        QCOMPARE(errorCount(d2), std::size_t(0));
        const auto* w = again.findVault("nas");
        QVERIFY(w && w->network);
        QCOMPARE(w->path, v->path);
        QCOMPARE(w->netUser, v->netUser);
        QCOMPARE(w->netPassword.name, v->netPassword.name);
    }

    void rejectsUselessNetworkVaults()
    {
        wchar_t name[256] = {};
        DWORD size = 256;
        ::GetComputerNameW(name, &size);
        const std::string self = win::toUtf8(std::wstring(name, size));
        const std::string yaml =
            "version: 1\n"
            "vaults:\n"
            "  - { id: a, vault_id: 11111111-1111-4111-8111-111111111111, path: \"//localhost/backup\" }\n"
            "  - { id: b, vault_id: 22222222-2222-4222-8222-222222222222, path: \"//127.0.0.1/backup\" }\n"
            "  - { id: c, vault_id: 33333333-3333-4333-8333-333333333333, path: \"//" + self + "/backup\" }\n"
            "  - { id: d, vault_id: 44444444-4444-4444-8444-444444444444, path: \"E:/\",\n"
            "      credentials: { user: x, password: { secret: y } } }\n"
            "  - { id: e, vault_id: 55555555-5555-4555-8555-555555555555, path: \"//nas01/b\", rotation: true }\n"
            "  - { id: f, vault_id: 66666666-6666-4666-8666-666666666666, path: \"//nas01\" }\n"
            "  - { id: g, vault_id: 77777777-7777-4777-8777-777777777777, path: \"//nas01/b\",\n"
            "      credentials: { user: x, password: \"texto-puro\" } }\n";
        std::vector<config::Diagnostic> d;
        (void)config::parseGlobalDocument(yaml, L"synkro.yml", d);
        QVERIFY(hasErrorAt(d, "vaults[0].path"));
        QVERIFY(hasErrorAt(d, "vaults[1].path"));
        QVERIFY(hasErrorAt(d, "vaults[2].path"));
        QVERIFY(hasErrorAt(d, "vaults[3].credentials"));
        QVERIFY(hasErrorAt(d, "vaults[4].rotation"));
        QVERIFY(hasErrorAt(d, "vaults[5].path"));
        QVERIFY(hasErrorAt(d, "vaults[6].credentials.password"));
    }

    void unreachableServerFailsFast()
    {
        // 192.0.2.0/24 é reservado para documentação: nunca responde.
        QElapsedTimer t;
        t.start();
        auto st = win::probeSmbServer(L"192.0.2.77", 800);
        QVERIFY(!st);
        QVERIFY(st.error().message.find("192.0.2.77") != std::string::npos);
        // Resultado negativo guardado: a segunda consulta é imediata.
        QElapsedTimer t2;
        t2.start();
        QVERIFY(!win::probeSmbServer(L"192.0.2.77", 800));
        QVERIFY(t2.elapsed() < 200);
        QVERIFY(t.elapsed() < 5000);

        config::VaultConfig v;
        v.id = "nas";
        v.label = "NAS";
        v.vaultId = "7f1c9a2e-3b0d-4c7e-9a51-0d2b6e1f4a10";
        v.path = L"\\\\192.0.2.77\\backup\\pdv01";
        v.network = true;
        v.maxAbsenceSeconds = 86400; // padrão do loader para cofre em rede
        std::string why;
        QVERIFY(!config::locateVault(v, &why));
        QVERIFY(why.find("não responde") != std::string::npos || why.find("não respondeu") != std::string::npos);

        // Alerta de cofre em rede inacessível traz o motivo e é crítico (não é rodízio).
        QTemporaryDir dir;
        auto state = StateStore::open(nativePath(dir.path() + "/state.db"));
        QVERIFY(state);
        config::ConfigSet set;
        set.global.vaults = {v};
        auto obs = observeVaults(set, state.value());
        QCOMPARE(obs.size(), std::size_t(1));
        QVERIFY(!obs[0].connected && obs[0].network && !obs[0].unavailableReason.empty());
        (void)state.value().recordVaultSeen(QString::fromStdString(v.vaultId), v.path, 0, 0, true);
        const auto alerts = computeAlerts(set, state.value(), obs, QDateTime::currentDateTimeUtc().addDays(3));
        const auto it = std::find_if(alerts.begin(), alerts.end(), [](const AlertRow& a) { return a.code == "vault_absent"; });
        QVERIFY(it != alerts.end());
        QVERIFY(it->severity == AlertSeverity::Critical);
        QVERIFY(it->message.find("em rede") != std::string::npos);
    }

    // Releitura sem cache pelo SMB: o ponteiro desalinhado depois de uma
    // leitura curta fazia a leitura seguinte falhar ("parâmetro incorreto").
    void uncachedReadOverSmb()
    {
        QTemporaryDir dir;
        const std::wstring local = nativePath(dir.path());
        const std::wstring unc = loopbackUnc(local);
        if (!reachable(unc))
            QSKIP("compartilhamento administrativo por loopback indisponível nesta máquina");
        for (int size : {0, 12, 4096, 5000, 300000, 1 << 20, (1 << 20) + 7, 3 * (1 << 20) + 100}) {
            QByteArray data(size, '\0');
            for (int i = 0; i < size; ++i)
                data[i] = static_cast<char>(i * 31);
            const std::wstring name = L"f" + std::to_wstring(size) + L".bin";
            QVERIFY(win::writeFileAtomically(win::joinPath(unc, name), data.toStdString()));
            const auto expected = Sha256::ofBytes(data.toStdString());
            const auto viaLocal = Sha256::ofFile(win::joinPath(local, name), true);
            const auto viaUnc = Sha256::ofFile(win::joinPath(unc, name), true);
            const std::string msg = "tamanho " + std::to_string(size) + ": "
                                    + (viaUnc ? std::string("ok") : viaUnc.error().message);
            QVERIFY2(viaLocal && viaLocal.value() == expected.value(), msg.c_str());
            QVERIFY2(viaUnc && viaUnc.value() == expected.value(), msg.c_str());
        }
    }

    void backupAndRestoreOverSmb()
    {
        QTemporaryDir dir;
        const std::wstring local = nativePath(dir.path());
        const std::wstring source = win::joinPath(local, L"origem");
        const std::wstring vaultLocal = win::joinPath(local, L"nas");
        QVERIFY(win::createDirectories(source));
        QVERIFY(win::createDirectories(vaultLocal));
        const std::wstring unc = loopbackUnc(vaultLocal);
        if (!reachable(unc))
            QSKIP("compartilhamento administrativo por loopback indisponível nesta máquina");

        QVERIFY(writeFile(win::joinPath(source, L"NFe\\2026\\nota1.xml"), "<nfe>1</nfe>"));
        QVERIFY(writeFile(win::joinPath(source, L"pdv.ini"), QByteArray(300000, 'x')));

        // Cofre inicializado pelo caminho de rede.
        auto id = vault::initialize(unc, QStringLiteral("NAS"));
        QVERIFY2(id, id ? "" : id.error().message.c_str());

        // Localização pela configuração (sem varrer volumes).
        config::VaultConfig vc;
        vc.id = "nas";
        vc.label = "NAS";
        vc.vaultId = id.value().vaultId.toStdString();
        vc.path = unc;
        vc.network = true;
        std::string why;
        auto located = config::locateVault(vc, &why);
        QVERIFY2(located, why.c_str());
        QCOMPARE(located->location, unc);

        BackupRequest req;
        req.jobId = L"pdv-arquivos";
        req.sourceRoot = source;
        req.applyRetention = false;
        req.verify = VerifyMode::RereadUncached; // releitura sem cache pelo SMB
        req.vaults.push_back({unc, id.value().vaultId, QStringLiteral("NAS")});
        auto r1 = runBackup(req);
        std::string detail = std::string(nameOf(r1.status)) + ": " + r1.error;
        for (const auto& v : r1.vaults) {
            detail += " | " + v.error + " falhas=" + std::to_string(v.failed);
            for (const auto& [file, e] : v.fileErrors)
                detail += " [" + win::toUtf8(file) + ": " + e + "]";
        }
        QVERIFY2(r1.status == RunStatus::Success, detail.c_str());
        QCOMPARE(r1.vaults[0].added, 2ull);

        QVERIFY(writeFile(win::joinPath(source, L"pdv.ini"), "y=2"));
        auto r2 = runBackup(req);
        QVERIFY(r2.status == RunStatus::Success);
        QCOMPARE(r2.vaults[0].modified, 1ull);

        // Os dados estão de fato na pasta do "servidor".
        const vault::JobLayout layout(vaultLocal, L"pdv-arquivos");
        QCOMPARE(readFile(layout.currentFile(L"NFe/2026/nota1.xml")), QByteArray("<nfe>1</nfe>"));

        RestoreRequest rr;
        rr.vaultLocation = unc;
        rr.jobId = L"pdv-arquivos";
        rr.targetDirectory = win::joinPath(local, L"restaurado");
        auto restored = restore(rr);
        QVERIFY2(restored, restored ? "" : restored.error().message.c_str());
        QCOMPARE(restored.value().restored, std::size_t(2));
        QVERIFY(restored.value().failures.empty());
        QCOMPARE(readFile(win::joinPath(rr.targetDirectory, L"pdv.ini")), QByteArray("y=2"));
    }

    void candidateCheckOnShare()
    {
        QTemporaryDir dir;
        const std::wstring local = nativePath(dir.path());
        const std::wstring unc = loopbackUnc(local);
        if (!reachable(unc))
            QSKIP("compartilhamento administrativo por loopback indisponível nesta máquina");

        vault::CandidateRequest req;
        req.candidatePath = unc;
        const auto report = vault::checkCandidate(req);
        // Compartilhamento desta própria máquina: bloqueado, mas o resto é examinado.
        const auto blocked = std::find_if(report.findings.begin(), report.findings.end(), [](const vault::CheckFinding& f) {
            return f.id == vault::CheckId::DifferentDiskFromSources && f.severity == vault::CheckSeverity::Blocking;
        });
        QVERIFY(blocked != report.findings.end());
        QVERIFY(blocked->title.find("própria máquina") != std::string::npos);
        const auto write = std::find_if(report.findings.begin(), report.findings.end(),
                                        [](const vault::CheckFinding& f) { return f.id == vault::CheckId::WriteTest; });
        QVERIFY(write != report.findings.end());
        QVERIFY2(write->severity == vault::CheckSeverity::Ok, write->detail.c_str());
        QVERIFY(!report.accepted());
    }
};

QTEST_GUILESS_MAIN(TestNetVault)
#include "tst_netvault.moc"
