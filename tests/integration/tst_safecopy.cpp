#include "transfer/SafeCopy.h"

#include "TestFs.h"

#include <QTemporaryDir>
#include <QTest>

#include <atomic>
#include <chrono>
#include <thread>

using namespace synkro;
using namespace synkro::test;

namespace {

QByteArray pattern(int size)
{
    QByteArray data(size, Qt::Uninitialized);
    for (int i = 0; i < size; ++i)
        data[i] = static_cast<char>((i * 131 + 17) & 0xFF);
    return data;
}

std::uint64_t lastWrite(const std::wstring& path)
{
    WIN32_FILE_ATTRIBUTE_DATA d{};
    ::GetFileAttributesExW(win::toExtendedPath(path).c_str(), GetFileExInfoStandard, &d);
    return win::toTicks(d.ftLastWriteTime);
}

int countFiles(const std::wstring& dir)
{
    int n = 0;
    WIN32_FIND_DATAW fd{};
    HANDLE h = ::FindFirstFileW(win::toExtendedPath(win::joinPath(dir, L"*")).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
            ++n;
    } while (::FindNextFileW(h, &fd));
    ::FindClose(h);
    return n;
}

} // namespace

class TstSafeCopy : public QObject {
    Q_OBJECT

private slots:
    void copiesContentHashAndTimes_data()
    {
        QTest::addColumn<int>("size");
        QTest::addColumn<bool>("uncached");
        QTest::newRow("vazio")              << 0 << false;
        QTest::newRow("pequeno")            << 10 << false;
        QTest::newRow("2,5 MiB")            << (5 * 512 * 1024) << false;
        QTest::newRow("2,5 MiB sem cache")  << (5 * 512 * 1024) << true;
        QTest::newRow("não alinhado sem cache") << 12345 << true;
    }

    void copiesContentHashAndTimes()
    {
        QFETCH(int, size);
        QFETCH(bool, uncached);

        QTemporaryDir dir;
        const std::wstring root = nativePath(dir.path());
        const std::wstring src = win::joinPath(root, L"origem\\dados.bin");
        const std::wstring dst = win::joinPath(root, L"cofre\\current\\sub\\dados.bin");
        const QByteArray content = pattern(size);
        QVERIFY(writeFile(src, content));

        CopyOptions opt;
        opt.verify = uncached ? VerifyMode::RereadUncached : VerifyMode::HashOnCopy;
        opt.tempDirectory = win::joinPath(root, L"cofre\\.tmp");

        auto r = copyFileVerified(src, dst, opt);
        QVERIFY2(r.ok(), r.ok() ? "" : r.error().message.c_str());
        QCOMPARE(r.value().bytes, static_cast<std::uint64_t>(size));
        QCOMPARE(readFile(dst), content);

        auto expected = Sha256::ofBytes(std::string_view(content.constData(), static_cast<size_t>(size)));
        QCOMPARE(r.value().sha256, expected.value());
        QCOMPARE(lastWrite(dst), lastWrite(src));
        QCOMPARE(countFiles(opt.tempDirectory), 0); // nenhum temporário esquecido
    }

    void replacesExistingReadOnlyDestination()
    {
        QTemporaryDir dir;
        const std::wstring root = nativePath(dir.path());
        const std::wstring src = win::joinPath(root, L"a.txt");
        const std::wstring dst = win::joinPath(root, L"dest\\a.txt");
        QVERIFY(writeFile(dst, "versão antiga"));
        QVERIFY(::SetFileAttributesW(win::toExtendedPath(dst).c_str(), FILE_ATTRIBUTE_READONLY));
        QVERIFY(writeFile(src, "versão nova"));

        auto r = copyFileVerified(src, dst);
        QVERIFY2(r.ok(), r.ok() ? "" : r.error().message.c_str());
        QCOMPARE(readFile(dst), QByteArray("versão nova"));
        ::SetFileAttributesW(win::toExtendedPath(dst).c_str(), FILE_ATTRIBUTE_NORMAL);
    }

    void preservesReadOnlyAttribute()
    {
        QTemporaryDir dir;
        const std::wstring root = nativePath(dir.path());
        const std::wstring src = win::joinPath(root, L"ro.txt");
        const std::wstring dst = win::joinPath(root, L"out\\ro.txt");
        QVERIFY(writeFile(src, "x"));
        QVERIFY(::SetFileAttributesW(win::toExtendedPath(src).c_str(), FILE_ATTRIBUTE_READONLY));

        auto r = copyFileVerified(src, dst);
        QVERIFY(r.ok());
        QVERIFY(::GetFileAttributesW(win::toExtendedPath(dst).c_str()) & FILE_ATTRIBUTE_READONLY);

        ::SetFileAttributesW(win::toExtendedPath(src).c_str(), FILE_ATTRIBUTE_NORMAL);
        ::SetFileAttributesW(win::toExtendedPath(dst).c_str(), FILE_ATTRIBUTE_NORMAL);
    }

    void missingSourceLeavesDestinationIntact()
    {
        QTemporaryDir dir;
        const std::wstring root = nativePath(dir.path());
        const std::wstring dst = win::joinPath(root, L"d.txt");
        QVERIFY(writeFile(dst, "intacto"));

        auto r = copyFileVerified(win::joinPath(root, L"nao-existe.txt"), dst);
        QVERIFY(!r.ok());
        QCOMPARE(r.error().code, ErrorCode::NotFound);
        QCOMPARE(readFile(dst), QByteArray("intacto"));
    }

    void sourceModifiedDuringCopyIsRejected()
    {
        QTemporaryDir dir;
        const std::wstring root = nativePath(dir.path());
        const std::wstring src = win::joinPath(root, L"crescendo.log");
        const std::wstring dst = win::joinPath(root, L"out\\crescendo.log");
        const std::wstring tmpDir = win::joinPath(root, L"out\\.tmp");
        QVERIFY(writeFile(dst, "anterior"));
        QVERIFY(writeFile(src, pattern(64 * 1024 * 1024))); // 64 MiB: dá tempo de alterar durante a cópia

        std::atomic<bool> stop{false};
        std::atomic<bool> started{false};
        std::thread writer([&] {
            win::UniqueHandle h(::CreateFileW(win::toExtendedPath(src).c_str(), FILE_APPEND_DATA,
                                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
            started = true;
            while (!stop.load()) {
                DWORD w = 0;
                ::WriteFile(h.get(), "linha\n", 6, &w, nullptr);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
        while (!started.load())
            std::this_thread::yield();

        CopyOptions opt;
        opt.tempDirectory = tmpDir;
        opt.bufferSize = 64 * 1024; // leitura mais lenta para garantir concorrência
        auto r = copyFileVerified(src, dst, opt);
        stop = true;
        writer.join();

        QVERIFY(!r.ok());
        QCOMPARE(r.error().code, ErrorCode::SourceChangedDuringCopy);
        QCOMPARE(readFile(dst), QByteArray("anterior"));
        QCOMPARE(countFiles(tmpDir), 0);
    }

    void longUnicodePaths()
    {
        QTemporaryDir dir;
        const std::wstring root = nativePath(dir.path());
        std::wstring deep;
        for (int i = 0; i < 10; ++i)
            deep += L"pasta-de-teste-com-nome-longo-" + std::to_wstring(i) + L"\\";
        const std::wstring src = win::joinPath(root, L"origem\\" + deep + L"nota-fiscal-ção.xml");
        const std::wstring dst = win::joinPath(root, L"destino\\" + deep + L"nota-fiscal-ção.xml");
        QVERIFY(dst.size() > 300);
        QVERIFY(writeFile(src, "<nfe/>"));

        auto r = copyFileVerified(src, dst);
        QVERIFY2(r.ok(), r.ok() ? "" : r.error().message.c_str());
        QCOMPARE(readFile(dst), QByteArray("<nfe/>"));
    }
};

QTEST_GUILESS_MAIN(TstSafeCopy)
#include "tst_safecopy.moc"
