#include "config/ConfigLoader.h"
#include "config/JobRequest.h"
#include "engine/BackupEngine.h"
#include "vault/Vault.h"

#include "TestFs.h"

#include <QTemporaryDir>
#include <QTest>

using namespace synkro;
using namespace synkro::config;
using namespace synkro::test;

class TstConfigSet : public QObject {
    Q_OBJECT

private:
    static QString dump(const ConfigSet& s)
    {
        QString out;
        for (const auto& d : s.diagnostics)
            out += QString::fromStdString(formatDiagnostic(d)) + "\n";
        return out;
    }

private slots:
    void cycleAndMissingDependency()
    {
        QTemporaryDir dir;
        const std::wstring root = nativePath(dir.path());
        QVERIFY(writeFile(win::joinPath(root, L"synkro.yml"),
                          "version: 1\nvaults:\n  - id: a\n    vault_id: 7f1c9a2e-3b0d-4c7e-9a51-0d2b6e1f4a10\n"
                          "defaults:\n  vaults: [a]\n"));
        QVERIFY(writeFile(win::joinPath(root, L"jobs\\x.yml"),
                          "version: 1\nid: x\nname: x\ntype: files\nsource: \"C:/x\"\ndepends_on: [y]\n"));
        QVERIFY(writeFile(win::joinPath(root, L"jobs\\y.yml"),
                          "version: 1\nid: y\nname: y\ntype: files\nsource: \"C:/y\"\ndepends_on: [x, z]\n"));

        LoadOptions opt;
        opt.checkPhysicalDisks = false;
        const ConfigSet set = loadConfig(root, opt);
        bool cycle = false, missing = false;
        for (const auto& d : set.diagnostics) {
            cycle = cycle || d.message.find("circular") != std::string::npos;
            missing = missing || d.message.find("\"z\" não existe") != std::string::npos;
        }
        QVERIFY2(cycle, qPrintable(dump(set)));
        QVERIFY2(missing, qPrintable(dump(set)));
    }

    void endToEndBackupFromYaml()
    {
        QTemporaryDir dir;
        const std::wstring root = nativePath(dir.path());
        const std::wstring source = win::joinPath(root, L"origem");
        const std::wstring disk = win::joinPath(root, L"disco");
        QVERIFY(writeFile(win::joinPath(source, L"XML\\a.xml"), "<a/>"));
        QVERIFY(writeFile(win::joinPath(source, L"Logs\\x.log"), "log"));
        QVERIFY(win::createDirectories(disk).ok());
        auto id = vault::initialize(disk, QStringLiteral("Disco de teste"));
        QVERIFY(id.ok());

        const std::string yml = "version: 1\nvaults:\n  - id: teste\n    vault_id: " + id.value().vaultId.toStdString()
                                + "\n    label: \"Disco de teste\"\n    path: \"" + win::toUtf8(disk) + "\"\n";
        std::string ymlEscaped;
        for (char c : yml)
            ymlEscaped += (c == '\\') ? std::string("/") : std::string(1, c);
        QVERIFY(writeFile(win::joinPath(root, L"cfg\\synkro.yml"), QByteArray::fromStdString(ymlEscaped)));

        std::string job = "version: 1\nid: tarefa\nname: Tarefa\ntype: files\nsource: \"" + win::toUtf8(source)
                          + "\"\nvaults: [teste]\nexclude: [\"Logs/**\"]\n";
        for (auto& c : job) {
            if (c == '\\')
                c = '/';
        }
        QVERIFY(writeFile(win::joinPath(root, L"cfg\\jobs\\tarefa.yml"), QByteArray::fromStdString(job)));

        LoadOptions opt;
        opt.checkPhysicalDisks = false;
        const ConfigSet set = loadConfig(win::joinPath(root, L"cfg"), opt);
        QVERIFY2(!set.hasErrors(), qPrintable(dump(set)));

        auto req = makeBackupRequest(set, *set.findJob("tarefa"));
        QVERIFY2(req.ok(), req.ok() ? "" : req.error().message.c_str());
        QCOMPARE(req.value().vaults.size(), size_t(1));
        QVERIFY(!req.value().vaults[0].location.empty()); // localizado pelo vault_id

        auto result = runBackup(req.value());
        QCOMPARE(result.status, RunStatus::Success);
        QCOMPARE(result.vaults[0].added, 1ull); // Logs/** excluído
    }

    void disconnectedVaultMakesRunFail()
    {
        QTemporaryDir dir;
        const std::wstring root = nativePath(dir.path());
        QVERIFY(writeFile(win::joinPath(root, L"origem\\a.txt"), "a"));
        QVERIFY(writeFile(win::joinPath(root, L"synkro.yml"),
                          "version: 1\nvaults:\n  - id: ausente\n    vault_id: 00000000-0000-0000-0000-0000000000aa\n"
                          "    label: \"Disco ausente\"\n"));
        std::string job = "version: 1\nid: t\nname: t\ntype: files\nsource: \"" + win::toUtf8(win::joinPath(root, L"origem"))
                          + "\"\nvaults: [ausente]\n";
        for (auto& c : job) {
            if (c == '\\')
                c = '/';
        }
        QVERIFY(writeFile(win::joinPath(root, L"jobs\\t.yml"), QByteArray::fromStdString(job)));

        LoadOptions opt;
        opt.checkPhysicalDisks = false;
        const ConfigSet set = loadConfig(root, opt);
        QVERIFY2(!set.hasErrors(), qPrintable(dump(set)));
        auto req = makeBackupRequest(set, *set.findJob("t"));
        QVERIFY(req.ok());
        QVERIFY(req.value().vaults[0].location.empty());

        auto result = runBackup(req.value());
        QCOMPARE(result.status, RunStatus::Failed);
        QVERIFY(result.vaults[0].error.find("não está conectado") != std::string::npos);
    }
};

QTEST_GUILESS_MAIN(TstConfigSet)
#include "tst_configset.moc"
