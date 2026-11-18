#include "config/ConfigLoader.h"
#include "secrets/SecretStore.h"

#include "TestFs.h"

#include <QTemporaryDir>
#include <QTest>

using namespace synkro;
using namespace synkro::test;

class TstSecrets : public QObject {
    Q_OBJECT

private slots:
    void setGetRemove()
    {
        QTemporaryDir dir;
        SecretStore store(win::joinPath(nativePath(dir.path()), L"sub\\secrets.dat"));
        QVERIFY(store.set("pg-pdv", "s3nh@ com espaço e ç").ok());
        QVERIFY(store.set("outro", "x").ok());

        auto v = store.get("pg-pdv");
        QVERIFY2(v.ok(), v.ok() ? "" : v.error().message.c_str());
        QCOMPARE(v.value(), std::string("s3nh@ com espaço e ç"));
        QVERIFY(store.contains("outro"));
        QCOMPARE(store.names().value().size(), size_t(2));

        QVERIFY(store.remove("outro").ok());
        QVERIFY(!store.contains("outro"));
        QCOMPARE(store.get("outro").error().code, ErrorCode::NotFound);
    }

    void valueNeverStoredInPlaintext()
    {
        QTemporaryDir dir;
        const std::wstring path = win::joinPath(nativePath(dir.path()), L"secrets.dat");
        SecretStore store(path);
        QVERIFY(store.set("banco", "SenhaMuitoSecreta123").ok());
        const QByteArray raw = readFile(path);
        QVERIFY(!raw.isEmpty());
        QVERIFY(!raw.contains("SenhaMuitoSecreta123"));
        QVERIFY(raw.contains("banco")); // o nome fica visível, o valor não
    }

    void invalidNamesAndEmptyValues()
    {
        QTemporaryDir dir;
        SecretStore store(win::joinPath(nativePath(dir.path()), L"secrets.dat"));
        QVERIFY(!store.set("Com Espaço", "x").ok());
        QVERIFY(!store.set("ok", "").ok());
        QCOMPARE(store.get("inexistente").error().code, ErrorCode::NotFound);
    }

    void tamperedCipherFailsToDecrypt()
    {
        QTemporaryDir dir;
        const std::wstring path = win::joinPath(nativePath(dir.path()), L"secrets.dat");
        SecretStore store(path);
        QVERIFY(store.set("x", "valor").ok());
        QByteArray raw = readFile(path);
        const int pos = raw.indexOf("\"x\": \"") + 10;
        raw[pos] = raw[pos] == 'A' ? 'B' : 'A';
        QVERIFY(writeFile(path, raw));
        QVERIFY(!store.get("x").ok());
    }

    void configWarnsAboutMissingSecrets()
    {
        QTemporaryDir dir;
        const std::wstring root = nativePath(dir.path());
        QVERIFY(writeFile(win::joinPath(root, L"synkro.yml"),
                          "version: 1\nvaults:\n  - id: a\n    vault_id: 7f1c9a2e-3b0d-4c7e-9a51-0d2b6e1f4a10\n"
                          "defaults:\n  vaults: [a]\n"));
        QVERIFY(writeFile(win::joinPath(root, L"jobs\\pg.yml"),
                          "version: 1\nid: pg\nname: pg\ntype: database\nengine: postgresql\n"
                          "connection: { host: localhost, user: u, password: { secret: pg-falta } }\ndatabases: [x]\n"));

        config::LoadOptions opt;
        opt.checkPhysicalDisks = false;
        opt.secretsFile = win::joinPath(root, L"secrets.dat");
        auto set = config::loadConfig(root, opt);
        bool warned = false;
        for (const auto& d : set.diagnostics)
            warned = warned || (d.severity == config::Severity::Warning && d.message.find("pg-falta") != std::string::npos);
        QVERIFY(warned);

        SecretStore(opt.secretsFile).set("pg-falta", "x").ok();
        set = config::loadConfig(root, opt);
        QCOMPARE(set.diagnostics.size(), size_t(0));
    }
};

QTEST_GUILESS_MAIN(TstSecrets)
#include "tst_secrets.moc"
