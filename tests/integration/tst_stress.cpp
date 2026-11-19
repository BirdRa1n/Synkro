// Teste de estresse do motor de backup (Fase 4). Não roda por padrão:
//
//   set SYNKRO_STRESS=1
//   set SYNKRO_STRESS_FILES=100000      (padrão 100000)
//   set SYNKRO_STRESS_DIR=E:\stress     (opcional; padrão: pasta temporária)
//   ctest --preset mingw-release -R tst_stress --output-on-failure
//
// Mede e confere: backup completo, incremental com 1% alterado / 0,5%
// apagado / 1% novo, execução sem mudanças, verificação por amostragem e
// restauração completa conferida por SHA-256 numa amostra.

#include "engine/BackupEngine.h"
#include "engine/Restore.h"
#include "engine/VaultOps.h"
#include "engine/Verify.h"
#include "hash/Sha256.h"
#include "vault/JobLayout.h"
#include "vault/Vault.h"

#include "TestFs.h"

#include <QDir>
#include <QElapsedTimer>
#include <QRandomGenerator>
#include <QTemporaryDir>
#include <QTest>

#include <set>

using namespace synkro;
using namespace synkro::test;

namespace {

int envInt(const char* name, int def)
{
    bool ok = false;
    const int v = qEnvironmentVariableIntValue(name, &ok);
    return ok && v > 0 ? v : def;
}

std::wstring relPath(int i)
{
    // ~500 arquivos por pasta, dois níveis, nomes com acento e espaço.
    return L"lote " + std::to_wstring(i / 5000) + L"\\pasta-" + std::to_wstring((i / 500) % 10) + L"\\nota fiscal "
           + std::to_wstring(i) + L" ção.xml";
}

QByteArray contentFor(int i, int generation)
{
    QByteArray b = "<nfe numero=\"" + QByteArray::number(i) + "\" geracao=\"" + QByteArray::number(generation) + "\">";
    b += QByteArray(64 + (i * 37) % 900, char('a' + i % 26));
    return b + "</nfe>\n";
}

void bumpMtime(const std::wstring& path)
{
    win::UniqueHandle h(::CreateFileW(win::toExtendedPath(path).c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, nullptr,
                                      OPEN_EXISTING, 0, nullptr));
    FILETIME ft{};
    ::GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u{{ft.dwLowDateTime, ft.dwHighDateTime}};
    u.QuadPart += 10'000'000ull * 120;
    ft.dwLowDateTime = u.LowPart;
    ft.dwHighDateTime = u.HighPart;
    ::SetFileTime(h.get(), nullptr, nullptr, &ft);
}

QString rate(std::size_t files, qint64 ms)
{
    return QStringLiteral("%1 arquivos em %2 s (%3 arq/s)")
        .arg(files)
        .arg(ms / 1000.0, 0, 'f', 1)
        .arg(ms > 0 ? files * 1000.0 / ms : 0.0, 0, 'f', 0);
}

} // namespace

class TstStress : public QObject {
    Q_OBJECT

private slots:
    void manyFiles()
    {
        if (qEnvironmentVariable("SYNKRO_STRESS") != QLatin1String("1"))
            QSKIP("defina SYNKRO_STRESS=1 para rodar o teste de estresse");
        const int n = envInt("SYNKRO_STRESS_FILES", 100000);

        QTemporaryDir tmp(qEnvironmentVariable("SYNKRO_STRESS_DIR").isEmpty()
                              ? QDir::tempPath() + QStringLiteral("/synkro-stress-XXXXXX")
                              : qEnvironmentVariable("SYNKRO_STRESS_DIR") + QStringLiteral("/synkro-stress-XXXXXX"));
        QVERIFY(tmp.isValid());
        const std::wstring root = nativePath(tmp.path());
        const std::wstring source = win::joinPath(root, L"origem");
        const std::wstring vaultDir = win::joinPath(root, L"cofre");
        QVERIFY(win::createDirectories(vaultDir).ok());
        QVERIFY(vault::initialize(vaultDir, QStringLiteral("estresse")).ok());

        QElapsedTimer t;
        t.start();
        for (int i = 0; i < n; ++i)
            QVERIFY(writeFile(win::joinPath(source, relPath(i)), contentFor(i, 0)));
        qInfo().noquote() << "origem criada:" << rate(n, t.elapsed());

        BackupRequest req;
        req.jobId = L"estresse";
        req.sourceRoot = source;
        req.vaults.push_back({vaultDir, {}, {}});
        req.applyRetention = false;

        // 1. Completo.
        t.restart();
        auto full = runBackup(req);
        const qint64 fullMs = t.elapsed();
        QVERIFY2(full.status == RunStatus::Success, full.error.c_str());
        QCOMPARE(full.vaults[0].added, std::uint64_t(n));
        qInfo().noquote() << "backup completo:" << rate(n, fullMs);

        // 2. Incremental: 1% alterado, 0,5% apagado, 1% novo (abaixo das guardas).
        const int changed = n / 100, removed = n / 200, added = n / 100;
        std::set<int> modifiedSet;
        for (int i = 0; static_cast<int>(modifiedSet.size()) < changed; ++i) {
            const int k = static_cast<int>((static_cast<long long>(i) * 97) % n);
            if (!modifiedSet.insert(k).second)
                continue;
            const std::wstring p = win::joinPath(source, relPath(k));
            QVERIFY(writeFile(p, contentFor(k, 1)));
            bumpMtime(p);
        }
        std::set<int> gone;
        for (int i = 0; static_cast<int>(gone.size()) < removed; ++i) {
            const int k = static_cast<int>((static_cast<long long>(i) * 131 + 7) % n);
            if (modifiedSet.count(k)) // não apagar um alterado
                continue;
            if (gone.insert(k).second)
                QVERIFY(::DeleteFileW(win::toExtendedPath(win::joinPath(source, relPath(k))).c_str()));
        }
        for (int i = n; i < n + added; ++i)
            QVERIFY(writeFile(win::joinPath(source, relPath(i)), contentFor(i, 0)));

        t.restart();
        auto inc = runBackup(req);
        const qint64 incMs = t.elapsed();
        QVERIFY2(inc.status == RunStatus::Success, inc.error.c_str());
        QCOMPARE(inc.vaults[0].added, std::uint64_t(added));
        QCOMPARE(inc.vaults[0].deleted, std::uint64_t(removed));
        QCOMPARE(inc.vaults[0].modified, std::uint64_t(changed));
        qInfo().noquote() << "incremental:" << rate(n + added - removed, incMs) << "— novos" << inc.vaults[0].added
                          << "alterados" << inc.vaults[0].modified << "apagados" << inc.vaults[0].deleted;

        // 3. Sem mudanças: nada é copiado.
        t.restart();
        auto same = runBackup(req);
        const qint64 sameMs = t.elapsed();
        QVERIFY(same.status == RunStatus::Success);
        QCOMPARE(same.vaults[0].added + same.vaults[0].modified + same.vaults[0].deleted, std::uint64_t(0));
        qInfo().noquote() << "sem mudanças:" << rate(n + added - removed, sameMs);
        QVERIFY2(sameMs < fullMs, "a execução sem mudanças deveria ser mais rápida que a completa");

        // 4. Verificação por amostragem (releitura sem cache).
        t.restart();
        auto verified = verifyVault(vault::JobLayout(vaultDir, L"estresse"), 5.0);
        QVERIFY(verified.ok());
        QVERIFY(verified.value().healthy());
        qInfo().noquote() << "verificação de 5%:" << rate(verified.value().checked, t.elapsed());

        // 5. Restauração completa para pasta nova, conferida numa amostra.
        RestoreRequest rr;
        rr.vaultLocation = vaultDir;
        rr.jobId = L"estresse";
        rr.targetDirectory = win::joinPath(root, L"restaurado");
        t.restart();
        auto restored = restore(rr);
        QVERIFY2(restored.ok(), restored.ok() ? "" : restored.error().message.c_str());
        QCOMPARE(restored.value().restored, size_t(n + added - removed));
        QVERIFY(restored.value().failures.empty());
        qInfo().noquote() << "restauração:" << rate(restored.value().restored, t.elapsed());
        for (int i = 0; i < n + added; i += 997) {
            if (gone.count(i))
                continue;
            const auto a = Sha256::ofFile(win::joinPath(source, relPath(i)));
            const auto b = Sha256::ofFile(win::joinPath(rr.targetDirectory, relPath(i)));
            QVERIFY(a.ok() && b.ok());
            QCOMPARE(a.value(), b.value());
        }
    }
};

QTEST_GUILESS_MAIN(TstStress)
#include "tst_stress.moc"
