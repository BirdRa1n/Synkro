#include "core/RunId.h"

#include <QRandomGenerator>
#include <QTimeZone>

#include <cwctype>

namespace synkro {

std::wstring newRunId(const QDateTime& utcNow)
{
    const QString stamp = utcNow.toUTC().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    const QString suffix = QStringLiteral("%1").arg(QRandomGenerator::system()->bounded(0x10000), 4, 16, QLatin1Char('0'));
    return (QStringLiteral("r-") + stamp + QLatin1Char('-') + suffix).toStdWString();
}

bool isValidRunId(const std::wstring& runId)
{
    // r-YYYYMMDD-HHMMSS-xxxx
    if (runId.size() != 22 || runId.compare(0, 2, L"r-") != 0 || runId[10] != L'-' || runId[17] != L'-')
        return false;
    for (size_t i = 2; i < 22; ++i) {
        if (i == 10 || i == 17)
            continue;
        const wchar_t c = runId[i];
        if (i < 17 ? !std::iswdigit(c) : !std::iswxdigit(c))
            return false;
    }
    return true;
}

QDateTime runIdTimestamp(const std::wstring& runId)
{
    if (!isValidRunId(runId))
        return {};
    const QString stamp = QString::fromStdWString(runId.substr(2, 15));
    QDateTime dt = QDateTime::fromString(stamp, QStringLiteral("yyyyMMdd-HHmmss"));
    dt.setTimeZone(QTimeZone::UTC);
    return dt;
}

bool isValidJobId(const std::wstring& id)
{
    if (id.empty() || id.size() > 64 || id.front() == L'-' || id.back() == L'-')
        return false;
    for (wchar_t c : id) {
        const bool ok = (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') || c == L'-';
        if (!ok)
            return false;
    }
    return true;
}

} // namespace synkro
