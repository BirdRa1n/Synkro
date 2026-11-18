#include "platform/windows/Win32.h"

#include <QTest>

using namespace synkro;

class TstWin32 : public QObject {
    Q_OBJECT

private slots:
    void extendedPath_data()
    {
        QTest::addColumn<QString>("input");
        QTest::addColumn<QString>("expected");
        QTest::newRow("local")      << "C:\\PDV\\XML"          << "\\\\?\\C:\\PDV\\XML";
        QTest::newRow("barras")     << "C:/PDV/XML"            << "\\\\?\\C:\\PDV\\XML";
        QTest::newRow("unc")        << "\\\\srv\\share\\a"     << "\\\\?\\UNC\\srv\\share\\a";
        QTest::newRow("ja-estendido") << "\\\\?\\C:\\x"        << "\\\\?\\C:\\x";
        QTest::newRow("volume-guid") << "\\\\?\\Volume{abc}\\" << "\\\\?\\Volume{abc}\\";
    }

    void extendedPath()
    {
        QFETCH(QString, input);
        QFETCH(QString, expected);
        QCOMPARE(QString::fromStdWString(win::toExtendedPath(input.toStdWString())), expected);
    }

    void joinPath()
    {
        QCOMPARE(QString::fromStdWString(win::joinPath(L"C:\\a\\", L"/b/c")), QStringLiteral("C:\\a\\b/c"));
        QCOMPARE(QString::fromStdWString(win::joinPath(L"C:\\a", L"b")), QStringLiteral("C:\\a\\b"));
        QCOMPARE(QString::fromStdWString(win::joinPath(L"C:\\a", L"")), QStringLiteral("C:\\a"));
    }

    void catalogPath()
    {
        QCOMPARE(QString::fromStdWString(win::toCatalogPath(L"\\NFe\\2026\\a.xml")), QStringLiteral("NFe/2026/a.xml"));
        QCOMPARE(QString::fromStdWString(win::fromCatalogPath(L"NFe/2026/a.xml")), QStringLiteral("NFe\\2026\\a.xml"));
    }

    void foldCaseIgnoresCase()
    {
        QCOMPARE(win::foldCase(L"NFe/Ação.XML"), win::foldCase(L"nfe/AÇÃO.xml"));
        QVERIFY(win::foldCase(L"a.xml") != win::foldCase(L"b.xml"));
    }

    void utf8RoundTrip()
    {
        const std::wstring original = L"Configuração fiscal — nº 1";
        QCOMPARE(win::fromUtf8(win::toUtf8(original)), original);
    }

    void errorMapping()
    {
        const Error e = win::errorFromWin32(ERROR_FILE_NOT_FOUND, "abrir");
        QCOMPARE(e.code, ErrorCode::NotFound);
        QCOMPARE(e.systemCode, static_cast<unsigned long>(ERROR_FILE_NOT_FOUND));
        QVERIFY(e.message.rfind("abrir: ", 0) == 0);

        QCOMPARE(win::errorFromWin32(ERROR_DISK_FULL, "x").code, ErrorCode::DiskFull);
        QCOMPARE(win::errorFromWin32(ERROR_DEVICE_NOT_CONNECTED, "x").code, ErrorCode::DeviceNotReady);
        QCOMPARE(win::errorFromWin32(ERROR_SHARING_VIOLATION, "x").code, ErrorCode::AccessDenied);
    }
};

QTEST_GUILESS_MAIN(TstWin32)
#include "tst_win32.moc"
