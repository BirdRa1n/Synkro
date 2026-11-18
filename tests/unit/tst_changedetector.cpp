#include "scan/ChangeDetector.h"

#include <QTest>

using namespace synkro;

namespace {

FileEntry entry(const wchar_t* path, std::uint64_t size, std::uint64_t mtime)
{
    FileEntry e;
    e.relativePath = path;
    e.size = size;
    e.lastWriteTime = mtime;
    return e;
}

KnownFile known(const wchar_t* path, std::uint64_t size, std::uint64_t mtime)
{
    return KnownFile{entry(path, size, mtime), std::nullopt};
}

} // namespace

class TstChangeDetector : public QObject {
    Q_OBJECT

private slots:
    void firstRunEverythingAdded()
    {
        const auto set = detectChanges({}, {entry(L"a", 1, 1), entry(L"b", 2, 2)});
        QCOMPARE(set.added.size(), size_t(2));
        QVERIFY(set.modified.empty());
        QVERIFY(set.deleted.empty());
        QCOMPARE(set.deletedRatio(), 0.0);
    }

    void unchangedWhenMetadataEqual()
    {
        const auto set = detectChanges({known(L"a", 1, 10)}, {entry(L"a", 1, 10)});
        QVERIFY(set.empty());
        QCOMPARE(set.unchanged, size_t(1));
    }

    void detectsAddModifyDelete()
    {
        const std::vector<KnownFile> prev{known(L"keep", 1, 1), known(L"size", 1, 1),
                                          known(L"time", 1, 1), known(L"gone", 1, 1)};
        const std::vector<FileEntry> cur{entry(L"keep", 1, 1), entry(L"size", 2, 1),
                                         entry(L"time", 1, 5), entry(L"new", 1, 1)};
        const auto set = detectChanges(prev, cur);

        QCOMPARE(set.unchanged, size_t(1));
        QCOMPARE(set.added.size(), size_t(1));
        QCOMPARE(set.added[0].relativePath, std::wstring(L"new"));
        QCOMPARE(set.deleted.size(), size_t(1));
        QCOMPARE(set.deleted[0].relativePath, std::wstring(L"gone"));
        QCOMPARE(set.modified.size(), size_t(2));
        QCOMPARE(set.modified[0].reasons, std::uint32_t(ReasonSize));
        QCOMPARE(set.modified[1].reasons, std::uint32_t(ReasonWriteTime));
        QCOMPARE(set.deletedRatio(), 0.25);
        QCOMPARE(set.modifiedRatio(), 0.5);
    }

    void caseInsensitiveMatching()
    {
        const auto set = detectChanges({known(L"NFe/Nota.XML", 3, 3)}, {entry(L"nfe/nota.xml", 3, 3)});
        QVERIFY(set.added.empty());
        QVERIFY(set.deleted.empty());
        QCOMPARE(set.modified.size(), size_t(1));
        QCOMPARE(set.modified[0].reasons, std::uint32_t(ReasonCaseRename));
    }

    void extendedInfoComparedOnlyWhenBothSidesHaveIt()
    {
        KnownFile old = known(L"db.fdb", 100, 50);
        old.meta.hasExtendedInfo = true;
        old.meta.changeTime = 50;
        old.meta.fileId[0] = 1;

        FileEntry same = entry(L"db.fdb", 100, 50);
        same.hasExtendedInfo = true;
        same.changeTime = 99;      // alterado sem mudar mtime
        same.fileId[0] = 2;        // substituído

        auto set = detectChanges({old}, {same});
        QCOMPARE(set.modified.size(), size_t(1));
        QCOMPARE(set.modified[0].reasons, std::uint32_t(ReasonChangeTime | ReasonFileId));

        DetectOptions noExtended;
        noExtended.compareChangeTime = false;
        noExtended.compareFileId = false;
        QVERIFY(detectChanges({old}, {same}, noExtended).empty());

        FileEntry withoutInfo = entry(L"db.fdb", 100, 50);
        QVERIFY(detectChanges({old}, {withoutInfo}).empty());

        // Lido de um snapshot VSS: outro número de série de volume, mesmo arquivo.
        FileEntry fromSnapshot = entry(L"db.fdb", 100, 50);
        fromSnapshot.hasExtendedInfo = true;
        fromSnapshot.changeTime = 50;
        fromSnapshot.fileId = old.meta.fileId;
        fromSnapshot.volumeSerial = 0x1234;
        QVERIFY(detectChanges({old}, {fromSnapshot}).empty());
    }

    void massDeletionRatio()
    {
        std::vector<KnownFile> prev;
        for (int i = 0; i < 10; ++i)
            prev.push_back(known((L"f" + std::to_wstring(i)).c_str(), 1, 1));
        const auto set = detectChanges(prev, {});
        QCOMPARE(set.deleted.size(), size_t(10));
        QCOMPARE(set.deletedRatio(), 1.0);
    }
};

QTEST_GUILESS_MAIN(TstChangeDetector)
#include "tst_changedetector.moc"
