#include "hash/Sha256.h"

#include <QFile>
#include <QTemporaryDir>
#include <QTest>

using namespace synkro;

class TstSha256 : public QObject {
    Q_OBJECT

private slots:
    void knownVectors_data()
    {
        QTest::addColumn<QByteArray>("input");
        QTest::addColumn<QString>("expected");
        QTest::newRow("vazio") << QByteArray()
            << "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
        QTest::newRow("abc") << QByteArray("abc")
            << "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
        QTest::newRow("nist-448bits") << QByteArray("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")
            << "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1";
    }

    void knownVectors()
    {
        QFETCH(QByteArray, input);
        QFETCH(QString, expected);
        auto digest = Sha256::ofBytes(std::string_view(input.constData(), static_cast<size_t>(input.size())));
        QVERIFY(digest.ok());
        QCOMPARE(QString::fromStdString(toHex(digest.value())), expected);
    }

    void incrementalEqualsOneShot()
    {
        Sha256 h;
        QVERIFY(h.valid());
        QVERIFY(h.update("abcdbcdecdefdefgefghfghighijhijk").ok());
        QVERIFY(h.update("ijkljklmklmnlmnomnopnopq").ok());
        auto incremental = h.finish();
        QVERIFY(incremental.ok());

        auto oneShot = Sha256::ofBytes("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq");
        QCOMPARE(incremental.value(), oneShot.value());
    }

    void reusableAfterFinish()
    {
        Sha256 h;
        QVERIFY(h.update("abc").ok());
        auto first = h.finish();
        QVERIFY(h.update("abc").ok());
        auto second = h.finish();
        QCOMPARE(first.value(), second.value());
    }

    void fileHashWithAndWithoutCache_data()
    {
        QTest::addColumn<int>("size");
        QTest::newRow("0 bytes") << 0;
        QTest::newRow("1 byte") << 1;
        QTest::newRow("setor + 1") << 4097;
        QTest::newRow("3 MiB + 123") << (3 * 1024 * 1024 + 123);
    }

    void fileHashWithAndWithoutCache()
    {
        QFETCH(int, size);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        QByteArray content(size, Qt::Uninitialized);
        for (int i = 0; i < size; ++i)
            content[i] = static_cast<char>((i * 31 + 7) & 0xFF);

        const QString path = dir.filePath(QStringLiteral("dados.bin"));
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        QCOMPARE(f.write(content), static_cast<qint64>(size));
        f.close();

        auto expected = Sha256::ofBytes(std::string_view(content.constData(), static_cast<size_t>(size)));
        auto cached = Sha256::ofFile(QDir::toNativeSeparators(path).toStdWString(), false);
        auto uncached = Sha256::ofFile(QDir::toNativeSeparators(path).toStdWString(), true);
        QVERIFY2(cached.ok(), cached.ok() ? "" : cached.error().message.c_str());
        QVERIFY2(uncached.ok(), uncached.ok() ? "" : uncached.error().message.c_str());
        QCOMPARE(cached.value(), expected.value());
        QCOMPARE(uncached.value(), expected.value());
    }

    void missingFile()
    {
        auto r = Sha256::ofFile(L"C:\\caminho\\que\\nao\\existe\\x.bin");
        QVERIFY(!r.ok());
        QCOMPARE(r.error().code, ErrorCode::NotFound);
    }
};

QTEST_GUILESS_MAIN(TstSha256)
#include "tst_sha256.moc"
