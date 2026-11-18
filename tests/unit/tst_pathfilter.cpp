#include "engine/Guards.h"
#include "scan/PathFilter.h"

#include <QTest>

using namespace synkro;

class TstPathFilter : public QObject {
    Q_OBJECT

private slots:
    void glob_data()
    {
        QTest::addColumn<QString>("pattern");
        QTest::addColumn<QString>("path");
        QTest::addColumn<bool>("match");
        QTest::newRow("estrela")          << "*.xml"        << "a.xml"            << true;
        QTest::newRow("estrela sem barra") << "*.xml"       << "NFe/a.xml"        << false;
        QTest::newRow("duplo")            << "**/*.xml"     << "NFe/2026/a.xml"   << true;
        QTest::newRow("duplo raiz")       << "**/*.xml"     << "a.xml"            << true;
        QTest::newRow("pasta/**")         << "Logs/**"      << "Logs/app.log"     << true;
        QTest::newRow("pasta/** fundo")   << "Logs/**"      << "Logs/a/b/c.log"   << true;
        QTest::newRow("pasta/** outra")   << "Logs/**"      << "XLogs/app.log"    << false;
        QTest::newRow("interrogação")     << "a?.txt"       << "ab.txt"           << true;
        QTest::newRow("interrog. barra")  << "a?b"          << "a/b"              << false;
        QTest::newRow("case")             << "*.TMP"        << "x.tmp"            << true;
        QTest::newRow("meio")             << "a/**/z.txt"   << "a/z.txt"          << true;
        QTest::newRow("meio fundo")       << "a/**/z.txt"   << "a/b/c/z.txt"      << true;
        QTest::newRow("prefixo ~$")       << "~$*"          << "~$doc.docx"       << true;
    }

    void glob()
    {
        QFETCH(QString, pattern);
        QFETCH(QString, path);
        QFETCH(bool, match);
        QCOMPARE(matchGlob(pattern.toStdWString(), path.toStdWString()), match);
    }

    void filterGitignoreSemantics()
    {
        PathFilter f({}, {L"*.tmp", L"Logs/**", L"~$*"});
        QVERIFY(!f.accepts(L"XML/a.tmp", false));      // sem '/': qualquer nível
        QVERIFY(!f.accepts(L"Logs", true));            // diretório podado
        QVERIFY(f.accepts(L"XML", true));
        QVERIFY(f.accepts(L"XML/a.xml", false));
        QVERIFY(!f.accepts(L"Docs/~$rel.docx", false));
    }

    void filterIncludeOnlyFiles()
    {
        PathFilter f({L"XML/**", L"NFe/**"}, {L"**/*.bak"});
        QVERIFY(f.accepts(L"Config", true));           // inclusão não poda diretórios
        QVERIFY(!f.accepts(L"Config/pdv.ini", false));
        QVERIFY(f.accepts(L"XML/a.xml", false));
        QVERIFY(f.accepts(L"NFe/2026/b.xml", false));
        QVERIFY(!f.accepts(L"NFe/2026/b.bak", false));
    }

    void anchoredPattern()
    {
        PathFilter f({}, {L"/temp.txt"});
        QVERIFY(!f.accepts(L"temp.txt", false));
        QVERIFY(f.accepts(L"sub/temp.txt", false));
    }

    void guardsMassDeletion()
    {
        GuardPolicy p;
        ChangeSet cs;
        cs.previousCount = 100;
        cs.deleted.resize(21, Change{ChangeKind::Deleted, L"x"});
        auto v = evaluateChangeGuards(p, cs, 79);
        QCOMPARE(v.code, GuardCode::MassDeletion);

        cs.deleted.resize(20, Change{ChangeKind::Deleted, L"x"});
        QVERIFY(!evaluateChangeGuards(p, cs, 80).blocked());
    }

    void guardsSmallSetsIgnoreRatios()
    {
        GuardPolicy p;
        ChangeSet cs;
        cs.previousCount = 5;
        cs.deleted.resize(4, Change{ChangeKind::Deleted, L"x"});
        QVERIFY(!evaluateChangeGuards(p, cs, 1).blocked());
    }

    void guardsEmptySourceAlwaysBlocks()
    {
        GuardPolicy p;
        ChangeSet cs;
        cs.previousCount = 3;
        cs.deleted.resize(3, Change{ChangeKind::Deleted, L"x"});
        QCOMPARE(evaluateChangeGuards(p, cs, 0).code, GuardCode::EmptySource);
    }

    void guardsMassModification()
    {
        GuardPolicy p;
        ChangeSet cs;
        cs.previousCount = 40;
        cs.modified.resize(21, Change{ChangeKind::Modified, L"x"});
        QCOMPARE(evaluateChangeGuards(p, cs, 40).code, GuardCode::MassModification);
    }

    void sentinel()
    {
        GuardPolicy p;
        p.sentinelFile = L"Config\\pdv.ini";
        ScanResult scan;
        scan.files.push_back(FileEntry{L"XML/a.xml"});
        QCOMPARE(evaluateSourceGuards(p, scan).code, GuardCode::SentinelMissing);
        scan.files.push_back(FileEntry{L"config/PDV.ini"});
        QVERIFY(!evaluateSourceGuards(p, scan).blocked());
    }
};

QTEST_GUILESS_MAIN(TstPathFilter)
#include "tst_pathfilter.moc"
