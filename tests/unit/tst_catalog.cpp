#include "catalog/Catalog.h"
#include "catalog/Manifest.h"
#include "core/RunId.h"

#include "TestFs.h"

#include <QTemporaryDir>
#include <QTest>

using namespace synkro;
using namespace synkro::test;

namespace {

CatalogEntry makeEntry(const wchar_t* path, std::uint64_t size, bool extended = false)
{
    CatalogEntry e;
    e.meta.relativePath = path;
    e.meta.size = size;
    e.meta.lastWriteTime = 133'000'000'000'000'000ull + size; // > 2^53: exige gravação como string
    e.meta.creationTime = 132'000'000'000'000'000ull;
    e.meta.attributes = FILE_ATTRIBUTE_ARCHIVE;
    if (extended) {
        e.meta.hasExtendedInfo = true;
        e.meta.changeTime = 133'000'000'000'000'123ull;
        e.meta.volumeSerial = 0xDEADBEEFCAFEull;
        for (size_t i = 0; i < e.meta.fileId.size(); ++i)
            e.meta.fileId[i] = static_cast<std::uint8_t>(i * 17);
    }
    e.sha256 = Sha256::ofBytes(std::to_string(size)).value();
    e.sinceRun = L"r-20261118-220000-abcd";
    return e;
}

} // namespace

class TstCatalog : public QObject {
    Q_OBJECT

private slots:
    void runIds()
    {
        const auto id = newRunId(QDateTime(QDate(2026, 11, 18), QTime(22, 0, 3), QTimeZone::UTC));
        QVERIFY(isValidRunId(id));
        QCOMPARE(QString::fromStdWString(id).left(18), QStringLiteral("r-20261118-220003-"));
        QCOMPARE(runIdTimestamp(id), QDateTime(QDate(2026, 11, 18), QTime(22, 0, 3), QTimeZone::UTC));
        QVERIFY(!isValidRunId(L"r-2026-xx"));

        const auto later = newRunId(QDateTime(QDate(2026, 11, 19), QTime(1, 0), QTimeZone::UTC));
        QVERIFY(id < later); // ordem lexicográfica == cronológica
    }

    void jobIds()
    {
        QVERIFY(isValidJobId(L"pdv-arquivos"));
        QVERIFY(isValidJobId(L"a1"));
        QVERIFY(!isValidJobId(L""));
        QVERIFY(!isValidJobId(L"-x"));
        QVERIFY(!isValidJobId(L"PDV"));
        QVERIFY(!isValidJobId(L"a/b"));
    }

    void entryRoundTripPreserves64BitValues()
    {
        const CatalogEntry e = makeEntry(L"NFe/2026/nota-ção.xml", 1234, true);
        auto back = catalogEntryFromJson(toJson(e));
        QVERIFY(back.ok());
        QCOMPARE(back.value().meta.relativePath, e.meta.relativePath);
        QCOMPARE(back.value().meta.lastWriteTime, e.meta.lastWriteTime);
        QCOMPARE(back.value().meta.changeTime, e.meta.changeTime);
        QCOMPARE(back.value().meta.volumeSerial, e.meta.volumeSerial);
        QCOMPARE(back.value().meta.fileId, e.meta.fileId);
        QCOMPARE(back.value().sha256, e.sha256);
        QCOMPARE(back.value().sinceRun, e.sinceRun);
    }

    void upsertFindRemoveCaseInsensitive()
    {
        Catalog c;
        c.upsert(makeEntry(L"A/x.xml", 1));
        c.upsert(makeEntry(L"b.txt", 2));
        QVERIFY(c.find(L"a/X.XML") != nullptr);

        c.upsert(makeEntry(L"a/x.xml", 99)); // mesmo arquivo, outro "case"
        QCOMPARE(c.entries.size(), size_t(2));
        QCOMPARE(c.find(L"A/x.xml")->meta.size, 99ull);

        QVERIFY(c.remove(L"A/X.xml"));
        QVERIFY(c.find(L"a/x.xml") == nullptr);
        QVERIFY(c.find(L"b.txt") != nullptr);
        QVERIFY(!c.remove(L"inexistente"));
    }

    void saveAndLoad()
    {
        QTemporaryDir dir;
        const std::wstring path = win::joinPath(nativePath(dir.path()), L"catalog.jsonl");

        Catalog c;
        c.runId = L"r-20261118-220000-abcd";
        c.upsert(makeEntry(L"z.txt", 3));
        c.upsert(makeEntry(L"a.txt", 1, true));
        QVERIFY(c.save(path).ok());

        auto loaded = Catalog::load(path);
        QVERIFY2(loaded.ok(), loaded.ok() ? "" : loaded.error().message.c_str());
        QCOMPARE(loaded.value().runId, c.runId);
        QCOMPARE(loaded.value().entries.size(), size_t(2));
        QCOMPARE(loaded.value().entries[0].meta.relativePath, std::wstring(L"a.txt")); // ordenado
        QVERIFY(loaded.value().find(L"Z.TXT") != nullptr);
    }

    void loadDetectsCorruption()
    {
        QTemporaryDir dir;
        const std::wstring path = win::joinPath(nativePath(dir.path()), L"catalog.jsonl");
        Catalog c;
        c.upsert(makeEntry(L"a.txt", 1));
        QVERIFY(c.save(path).ok());

        QByteArray data = readFile(path);
        data.replace("a.txt", "b.txt");
        QVERIFY(writeFile(path, data));

        auto loaded = Catalog::load(path);
        QVERIFY(!loaded.ok());
        QCOMPARE(loaded.error().code, ErrorCode::Corrupted);
    }

    void missingCatalogIsNotFound()
    {
        auto loaded = Catalog::load(L"C:\\nao\\existe\\catalog.jsonl");
        QVERIFY(!loaded.ok());
        QCOMPARE(loaded.error().code, ErrorCode::NotFound);
    }

    void truncatedCatalogRejected()
    {
        Catalog c;
        c.upsert(makeEntry(L"a.txt", 1));
        c.upsert(makeEntry(L"b.txt", 2));
        QByteArray data = c.serialize();
        data.truncate(data.lastIndexOf("{\"attrs\""));
        auto parsed = Catalog::parse(data);
        QVERIFY(!parsed.ok());
    }

    void manifestRoundTrip()
    {
        QTemporaryDir dir;
        const std::wstring path = win::joinPath(nativePath(dir.path()), L"r.manifest.jsonl");

        Manifest m;
        m.runId = L"r-20261118-220000-abcd";
        m.records.push_back({ManifestOp::Added, L"novo.xml", makeEntry(L"novo.xml", 1), std::nullopt, {}});
        m.records.push_back({ManifestOp::Modified, L"mod.xml", makeEntry(L"mod.xml", 2), makeEntry(L"mod.xml", 3), {}});
        m.records.push_back({ManifestOp::Deleted, L"del.xml", std::nullopt, makeEntry(L"del.xml", 4), {}});
        m.records.push_back({ManifestOp::Failed, L"bloq.db", std::nullopt, std::nullopt, "acesso negado"});
        QVERIFY(m.save(path).ok());

        auto back = Manifest::load(path);
        QVERIFY2(back.ok(), back.ok() ? "" : back.error().message.c_str());
        QCOMPARE(back.value().records.size(), size_t(4));
        QCOMPARE(back.value().records[1].previous->meta.size, 3ull);
        QCOMPARE(back.value().records[2].op, ManifestOp::Deleted);
        QCOMPARE(back.value().records[3].error, std::string("acesso negado"));
    }
};

QTEST_GUILESS_MAIN(TstCatalog)
#include "tst_catalog.moc"
