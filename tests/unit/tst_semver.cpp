#include "core/SemVer.h"

#include <QTest>

using namespace synkro;

class TestSemVer : public QObject {
    Q_OBJECT

private slots:
    void parses()
    {
        auto v = parseSemVer("v0.9.0-beta.1+build.7");
        QVERIFY(v.valid);
        QCOMPARE(v.major, 0);
        QCOMPARE(v.minor, 9);
        QCOMPARE(v.patch, 0);
        QCOMPARE(v.prerelease, (std::vector<std::string>{"beta", "1"}));
        QVERIFY(parseSemVer("1.2.3").valid);
        for (const char* bad : {"", "1.2", "1.2.3.4", "01.2.3", "1.2.x", "1.2.3-", "1.2.3-beta..1", "1.2.3-be ta", "beta"})
            QVERIFY2(!parseSemVer(bad).valid, bad);
    }

    void precedence()
    {
        // Ordem da especificação do semver 2.0, mais os casos do Synkro.
        const char* ordered[] = {"0.8.9",        "0.9.0-alpha",      "0.9.0-alpha.1", "0.9.0-alpha.beta", "0.9.0-beta",
                                 "0.9.0-beta.2", "0.9.0-beta.11",    "0.9.0-rc.1",    "0.9.0",            "0.9.1-beta.1",
                                 "0.9.1",        "0.10.0",           "1.0.0"};
        const int n = static_cast<int>(sizeof(ordered) / sizeof(*ordered));
        for (int i = 0; i < n; ++i)
            for (int j = 0; j < n; ++j) {
                const int c = compareSemVer(parseSemVer(ordered[i]), parseSemVer(ordered[j]));
                QVERIFY2((i < j && c < 0) || (i == j && c == 0) || (i > j && c > 0),
                         qPrintable(QStringLiteral("%1 x %2").arg(ordered[i], ordered[j])));
            }
        QVERIFY(isNewerVersion("v0.9.0-beta.2", "0.9.0-beta.1"));
        QVERIFY(isNewerVersion("0.9.0", "0.9.0-beta.9"));
        QVERIFY(!isNewerVersion("0.9.0-beta.1", "0.9.0"));
        QVERIFY(!isNewerVersion("0.9.0", "0.9.0"));
        QVERIFY(!isNewerVersion("lixo", "0.9.0"));
    }
};

QTEST_GUILESS_MAIN(TestSemVer)
#include "tst_semver.moc"
