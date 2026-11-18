#include "scan/FileScanner.h"

#include "TestFs.h"

#include <QTemporaryDir>
#include <QTest>

using namespace synkro;
using namespace synkro::test;

class TstFileScanner : public QObject {
    Q_OBJECT

private:
    static QStringList paths(const ScanResult& r)
    {
        QStringList out;
        for (const auto& f : r.files)
            out << QString::fromStdWString(f.relativePath);
        return out;
    }

private slots:
    void scansNestedTreeSorted()
    {
        QTemporaryDir dir;
        const std::wstring root = nativePath(dir.path());
        QVERIFY(writeFile(win::joinPath(root, L"b.txt"), "bb"));
        QVERIFY(writeFile(win::joinPath(root, L"NFe\\2026\\11\\nota.xml"), "<nfe/>"));
        QVERIFY(writeFile(win::joinPath(root, L"a\\vazio.bin"), QByteArray()));
        QVERIFY(makeDirs(win::joinPath(root, L"pasta-vazia")));

        auto r = scanDirectory(root);
        QVERIFY2(r.ok(), r.ok() ? "" : r.error().message.c_str());
        QCOMPARE(paths(r.value()), (QStringList{"NFe/2026/11/nota.xml", "a/vazio.bin", "b.txt"}));
        QCOMPARE(r.value().totalBytes, 8ull);
        QVERIFY(r.value().issues.empty());
    }

    void reportsSizeAndTimes()
    {
        QTemporaryDir dir;
        const std::wstring root = nativePath(dir.path());
        QVERIFY(writeFile(win::joinPath(root, L"x.dat"), QByteArray(1234, 'x')));

        auto r = scanDirectory(root);
        QVERIFY(r.ok());
        QCOMPARE(r.value().files.size(), size_t(1));
        const FileEntry& f = r.value().files.front();
        QCOMPARE(f.size, 1234ull);
        QVERIFY(f.lastWriteTime > 0);
        QVERIFY(!f.hasExtendedInfo);
    }

    void unicodeAndLongPaths()
    {
        QTemporaryDir dir;
        const std::wstring root = nativePath(dir.path());

        std::wstring deep;
        for (int i = 0; i < 12; ++i)
            deep += L"diretorio-com-nome-bem-comprido-" + std::to_wstring(i) + L"\\";
        const std::wstring longFile = deep + L"configuração-fiscal-ção.xml";
        QVERIFY(win::joinPath(root, longFile).size() > 300);
        QVERIFY(writeFile(win::joinPath(root, longFile), "ok"));

        auto r = scanDirectory(root);
        QVERIFY2(r.ok(), r.ok() ? "" : r.error().message.c_str());
        QCOMPARE(r.value().files.size(), size_t(1));
        QCOMPARE(r.value().files.front().relativePath, win::toCatalogPath(longFile));
    }

    void filterSkipsDirectoriesAndFiles()
    {
        QTemporaryDir dir;
        const std::wstring root = nativePath(dir.path());
        QVERIFY(writeFile(win::joinPath(root, L"Logs\\app.log"), "log"));
        QVERIFY(writeFile(win::joinPath(root, L"XML\\a.xml"), "a"));
        QVERIFY(writeFile(win::joinPath(root, L"XML\\a.tmp"), "t"));

        ScanOptions opt;
        int logsVisits = 0;
        opt.filter = [&](std::wstring_view rel, bool isDir) {
            if (isDir && rel == L"Logs") {
                ++logsVisits;
                return false;
            }
            return !(rel.size() > 4 && rel.substr(rel.size() - 4) == L".tmp");
        };

        auto r = scanDirectory(root, opt);
        QVERIFY(r.ok());
        QCOMPARE(paths(r.value()), QStringList{"XML/a.xml"});
        QCOMPARE(logsVisits, 1);
    }

    void readsFileIds()
    {
        QTemporaryDir dir;
        const std::wstring root = nativePath(dir.path());
        QVERIFY(writeFile(win::joinPath(root, L"1.txt"), "1"));
        QVERIFY(writeFile(win::joinPath(root, L"2.txt"), "2"));

        ScanOptions opt;
        opt.readFileIds = true;
        auto r = scanDirectory(root, opt);
        QVERIFY(r.ok());
        QCOMPARE(r.value().files.size(), size_t(2));
        const auto& a = r.value().files[0];
        const auto& b = r.value().files[1];
        QVERIFY(a.hasExtendedInfo && b.hasExtendedInfo);
        QVERIFY(a.changeTime > 0);
        QVERIFY(a.fileId != b.fileId);
        QCOMPARE(a.volumeSerial, b.volumeSerial);
    }

    void skipsJunctions()
    {
        QTemporaryDir dir;
        const std::wstring root = nativePath(dir.path());
        QVERIFY(writeFile(win::joinPath(root, L"real\\f.txt"), "f"));

        // Junções não exigem privilégio de administrador (ao contrário de symlinks).
        const QString cmd = QStringLiteral("cmd /c mklink /J \"%1\" \"%2\" >nul")
                                .arg(QString::fromStdWString(win::joinPath(root, L"juncao")),
                                     QString::fromStdWString(win::joinPath(root, L"real")));
        if (_wsystem(cmd.toStdWString().c_str()) != 0)
            QSKIP("não foi possível criar junção neste ambiente");

        auto r = scanDirectory(root);
        QVERIFY(r.ok());
        QCOMPARE(paths(r.value()), QStringList{"real/f.txt"});
        QCOMPARE(r.value().skippedLinks.size(), size_t(1));
        QCOMPARE(QString::fromStdWString(r.value().skippedLinks.front()), QStringLiteral("juncao"));
    }

    void missingRootFails()
    {
        auto r = scanDirectory(L"C:\\pasta\\que\\nao\\existe\\synkro");
        QVERIFY(!r.ok());
        QCOMPARE(r.error().code, ErrorCode::NotFound);
    }

    void fileAsRootFails()
    {
        QTemporaryDir dir;
        const std::wstring file = win::joinPath(nativePath(dir.path()), L"f.txt");
        QVERIFY(writeFile(file, "x"));
        auto r = scanDirectory(file);
        QVERIFY(!r.ok());
        QCOMPARE(r.error().code, ErrorCode::InvalidArgument);
    }
};

QTEST_GUILESS_MAIN(TstFileScanner)
#include "tst_filescanner.moc"
