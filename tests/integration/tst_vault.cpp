#include "platform/windows/Volume.h"
#include "vault/Vault.h"
#include "vault/VaultCandidateCheck.h"

#include "TestFs.h"

#include <QTemporaryDir>
#include <QTest>
#include <QUuid>

using namespace synkro;
using namespace synkro::test;

class TstVault : public QObject {
    Q_OBJECT

private:
    static const vault::CheckFinding* find(const vault::CandidateReport& r, vault::CheckId id)
    {
        for (const auto& f : r.findings) {
            if (f.id == id)
                return &f;
        }
        return nullptr;
    }

private slots:
    void initializeCreatesLayoutAndIdentity()
    {
        QTemporaryDir dir;
        const std::wstring location = nativePath(dir.path());

        auto created = vault::initialize(location, QStringLiteral("HD Externo 1"));
        QVERIFY2(created.ok(), created.ok() ? "" : created.error().message.c_str());
        QVERIFY(!QUuid::fromString(created.value().vaultId).isNull());
        QVERIFY(exists(win::joinPath(location, L"SynkroVault\\jobs")));

        auto read = vault::readIdentity(location);
        QVERIFY(read.ok());
        QCOMPARE(read.value().vaultId, created.value().vaultId);
        QCOMPARE(read.value().label, QStringLiteral("HD Externo 1"));
        QCOMPARE(read.value().formatVersion, 1);
        QVERIFY(read.value().createdAt.isValid());
    }

    void initializeRefusesExistingVault()
    {
        QTemporaryDir dir;
        const std::wstring location = nativePath(dir.path());
        auto first = vault::initialize(location, QStringLiteral("A"));
        QVERIFY(first.ok());

        auto second = vault::initialize(location, QStringLiteral("B"));
        QVERIFY(!second.ok());
        QCOMPARE(second.error().code, ErrorCode::AlreadyExists);
        QCOMPARE(vault::readIdentity(location).value().vaultId, first.value().vaultId);
    }

    void parseRejectsInvalidDocuments_data()
    {
        QTest::addColumn<QByteArray>("json");
        QTest::addColumn<int>("code");
        const int corrupted = static_cast<int>(ErrorCode::Corrupted);
        QTest::newRow("não é json") << QByteArray("isto não é json") << corrupted;
        QTest::newRow("formato errado") << QByteArray(R"({"format":"outro","format_version":1})") << corrupted;
        QTest::newRow("sem versão") << QByteArray(R"({"format":"synkro-vault","vault_id":"7f1c9a2e-3b0d-4c7e-9a51-0d2b6e1f4a10"})") << corrupted;
        QTest::newRow("uuid inválido") << QByteArray(R"({"format":"synkro-vault","format_version":1,"vault_id":"abc"})") << corrupted;
        QTest::newRow("versão futura") << QByteArray(R"({"format":"synkro-vault","format_version":99,"vault_id":"7f1c9a2e-3b0d-4c7e-9a51-0d2b6e1f4a10"})")
                                       << static_cast<int>(ErrorCode::Unsupported);
    }

    void parseRejectsInvalidDocuments()
    {
        QFETCH(QByteArray, json);
        QFETCH(int, code);
        auto r = vault::parse(json);
        QVERIFY(!r.ok());
        QCOMPARE(static_cast<int>(r.error().code), code);
    }

    void serializeRoundTrip()
    {
        vault::VaultIdentity id;
        id.vaultId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        id.label = QStringLiteral("Cofre ção");
        id.createdAt = QDateTime(QDate(2026, 11, 18), QTime(22, 0), QTimeZone::UTC);
        id.createdByHost = QStringLiteral("PDV-MATRIZ");
        id.synkroVersion = QStringLiteral("0.1.0");
        id.formatVersion = 1;

        auto back = vault::parse(vault::serialize(id));
        QVERIFY(back.ok());
        QCOMPARE(back.value().vaultId, id.vaultId);
        QCOMPARE(back.value().label, id.label);
        QCOMPARE(back.value().createdAt, id.createdAt);
    }

    void queryVolumeOfTempDir()
    {
        QTemporaryDir dir;
        auto v = win::queryVolume(nativePath(dir.path()));
        QVERIFY2(v.ok(), v.ok() ? "" : v.error().message.c_str());
        QVERIFY(!v.value().mountPoint.empty());
        QVERIFY(!v.value().fileSystem.empty());
        QVERIFY(v.value().totalBytes > 0);
    }

    void candidateOnSameDiskAsSourceIsBlocked()
    {
        // Origem e candidato na mesma pasta temporária: obrigatoriamente o
        // mesmo disco físico, portanto o cofre deve ser recusado.
        QTemporaryDir dir;
        const std::wstring root = nativePath(dir.path());

        vault::CandidateRequest req;
        req.candidatePath = root;
        req.sourcePaths = {root};
        req.runWriteTest = false;

        const auto report = vault::checkCandidate(req);
        QVERIFY(!report.accepted());

        const auto* same = find(report, vault::CheckId::DifferentDiskFromSources);
        QVERIFY(same != nullptr);
        if (report.volume.physicalDisks.empty())
            QCOMPARE(same->severity, vault::CheckSeverity::Warning);
        else
            QCOMPARE(same->severity, vault::CheckSeverity::Blocking);
    }

    void systemVolumeIsBlocked()
    {
        vault::CandidateRequest req;
        req.candidatePath = L"C:\\";
        req.runWriteTest = false;
        const auto report = vault::checkCandidate(req);
        if (!report.volume.isSystemVolume)
            QSKIP("o Windows não está em C: neste ambiente");
        const auto* sys = find(report, vault::CheckId::NotSystemVolume);
        QVERIFY(sys != nullptr);
        QCOMPARE(sys->severity, vault::CheckSeverity::Blocking);
        QVERIFY(!report.accepted());
    }

    void freeSpaceWarning()
    {
        QTemporaryDir dir;
        vault::CandidateRequest req;
        req.candidatePath = nativePath(dir.path());
        req.estimatedSourceBytes = std::uint64_t(1) << 60; // 1 EiB: nunca cabe
        req.runWriteTest = false;
        const auto report = vault::checkCandidate(req);
        const auto* space = find(report, vault::CheckId::FreeSpace);
        QVERIFY(space != nullptr);
        QCOMPARE(space->severity, vault::CheckSeverity::Warning);
    }
};

QTEST_GUILESS_MAIN(TstVault)
#include "tst_vault.moc"
