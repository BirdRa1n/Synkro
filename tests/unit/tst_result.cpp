#include "core/Result.h"

#include <QTest>

using namespace synkro;

class TstResult : public QObject {
    Q_OBJECT

private slots:
    void valueResult()
    {
        Result<int> r = 42;
        QVERIFY(r.ok());
        QCOMPARE(r.value(), 42);
    }

    void errorResult()
    {
        Result<int> r = Error::make(ErrorCode::NotFound, "sem arquivo", 2);
        QVERIFY(!r.ok());
        QCOMPARE(r.error().code, ErrorCode::NotFound);
        QCOMPARE(r.error().systemCode, 2ul);
        QCOMPARE(QString::fromUtf8(nameOf(r.error().code)), QStringLiteral("not_found"));
    }

    void voidStatus()
    {
        Status ok = Status::success();
        QVERIFY(ok.ok());

        Status fail = Error::make(ErrorCode::DiskFull, "disco cheio");
        QVERIFY(!fail.ok());
        QCOMPARE(fail.error().code, ErrorCode::DiskFull);
    }
};

QTEST_GUILESS_MAIN(TstResult)
#include "tst_result.moc"
