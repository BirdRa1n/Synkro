#include "platform/windows/Win32.h"

#include "TestFs.h"

#include <QTemporaryDir>
#include <QTest>

using namespace synkro;
using namespace synkro::test;

class TstFileOps : public QObject {
    Q_OBJECT

private slots:
    void createDirectoriesNestedAndIdempotent()
    {
        QTemporaryDir dir;
        const std::wstring deep = win::joinPath(nativePath(dir.path()), L"a\\b\\c\\d");
        QVERIFY(win::createDirectories(deep).ok());
        QVERIFY(exists(deep));
        QVERIFY(win::createDirectories(deep).ok());
        QVERIFY(win::createDirectories(L"C:/Windows").ok()); // já existe, barras normais
    }

    void createDirectoriesFailsOverFile()
    {
        QTemporaryDir dir;
        const std::wstring file = win::joinPath(nativePath(dir.path()), L"arquivo");
        QVERIFY(writeFile(file, "x"));
        auto st = win::createDirectories(win::joinPath(file, L"sub"));
        QVERIFY(!st.ok());
    }

    void parentPath()
    {
        QCOMPARE(win::parentPath(L"C:\\a\\b.txt"), std::wstring(L"C:\\a"));
        QCOMPARE(win::parentPath(L"C:\\a\\b\\"), std::wstring(L"C:\\a"));
        QCOMPARE(win::parentPath(L"b.txt"), std::wstring());
    }

    void atomicWriteCreatesAndReplaces()
    {
        QTemporaryDir dir;
        const std::wstring path = win::joinPath(nativePath(dir.path()), L"vault.json");

        QVERIFY(win::writeFileAtomically(path, "{\"v\":1}").ok());
        auto first = win::readWholeFile(path);
        QVERIFY(first.ok());
        QCOMPARE(first.value(), std::string("{\"v\":1}"));

        QVERIFY(win::writeFileAtomically(path, "{\"v\":2}").ok());
        QCOMPARE(win::readWholeFile(path).value(), std::string("{\"v\":2}"));

        // Nenhum temporário deve sobrar ao lado do arquivo.
        QDir d(dir.path());
        QCOMPARE(d.entryList(QDir::Files), QStringList{"vault.json"});
    }

    void readWholeFileRespectsLimit()
    {
        QTemporaryDir dir;
        const std::wstring path = win::joinPath(nativePath(dir.path()), L"grande.bin");
        QVERIFY(writeFile(path, QByteArray(1000, 'x')));
        auto r = win::readWholeFile(path, 10);
        QVERIFY(!r.ok());
        QCOMPARE(r.error().code, ErrorCode::InvalidArgument);
    }
};

QTEST_GUILESS_MAIN(TstFileOps)
#include "tst_fileops.moc"
