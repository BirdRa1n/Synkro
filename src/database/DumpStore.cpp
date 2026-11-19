#include "database/DumpStore.h"

#include "catalog/Catalog.h"
#include "core/RunId.h"
#include "engine/VaultOps.h"
#include "platform/windows/Win32.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <set>

namespace synkro::db {

namespace {

QJsonArray toArray(const std::vector<std::string>& v)
{
    QJsonArray a;
    for (const auto& s : v)
        a.append(QString::fromStdString(s));
    return a;
}

std::vector<std::string> fromArray(const QJsonValue& v)
{
    std::vector<std::string> out;
    for (const auto& x : v.toArray())
        out.push_back(x.toString().toStdString());
    return out;
}

} // namespace

QByteArray DumpRecord::serialize() const
{
    QJsonObject o;
    o.insert(QStringLiteral("format"), QStringLiteral("synkro-dump"));
    o.insert(QStringLiteral("format_version"), 1);
    o.insert(QStringLiteral("job_id"), QString::fromStdWString(jobId));
    o.insert(QStringLiteral("run_id"), QString::fromStdWString(runId));
    o.insert(QStringLiteral("engine"), QString::fromStdString(engine));
    o.insert(QStringLiteral("database"), QString::fromStdString(database));
    o.insert(QStringLiteral("path"), QString::fromStdWString(relativePath));
    o.insert(QStringLiteral("dump_format"), QString::fromStdString(dumpFormat));
    o.insert(QStringLiteral("compression"), QString::fromStdString(compression));
    o.insert(QStringLiteral("size_bytes"), QString::number(size));
    o.insert(QStringLiteral("sha256"), QString::fromStdString(toHex(sha256)));
    o.insert(QStringLiteral("started_at"), startedAt.toUTC().toString(Qt::ISODate));
    o.insert(QStringLiteral("finished_at"), finishedAt.toUTC().toString(Qt::ISODate));
    if (!host.empty())
        o.insert(QStringLiteral("host"), QString::fromStdString(host));
    o.insert(QStringLiteral("tool"), QString::fromStdString(toolPath));
    o.insert(QStringLiteral("tool_version"), QString::fromStdString(toolVersion));
    o.insert(QStringLiteral("command"), QString::fromStdString(command));
    QJsonObject validation;
    validation.insert(QStringLiteral("method"), QString::fromStdString(validationMethod));
    validation.insert(QStringLiteral("result"), QString::fromStdString(validationResult));
    if (tablesKnown)
        validation.insert(QStringLiteral("table_count"), static_cast<qint64>(tables.size()));
    o.insert(QStringLiteral("validation"), validation);
    if (tablesKnown)
        o.insert(QStringLiteral("tables"), toArray(tables));
    if (!warnings.empty())
        o.insert(QStringLiteral("warnings"), toArray(warnings));
    o.insert(QStringLiteral("suspicious"), suspicious);
    o.insert(QStringLiteral("restore_hint"), QString::fromStdString(restoreHint));
    o.insert(QStringLiteral("created_by_host"), QString::fromStdString(createdByHost));
    if (restoreTestedAt.isValid()) {
        QJsonObject t;
        t.insert(QStringLiteral("at"), restoreTestedAt.toUTC().toString(Qt::ISODate));
        t.insert(QStringLiteral("result"), QString::fromStdString(restoreTestResult));
        t.insert(QStringLiteral("detail"), QString::fromStdString(restoreTestDetail));
        o.insert(QStringLiteral("restore_test"), t);
        o.insert(QStringLiteral("restore_tested_at"), restoreTestedAt.toUTC().toString(Qt::ISODate));
    }
    return QJsonDocument(o).toJson(QJsonDocument::Indented);
}

Result<DumpRecord> DumpRecord::parse(const QByteArray& data)
{
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(data, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()
        || doc.object().value(QStringLiteral("format")).toString() != QLatin1String("synkro-dump"))
        return Error::make(ErrorCode::Corrupted, "metadados de dump inválidos");
    const QJsonObject o = doc.object();
    DumpRecord r;
    r.jobId = o.value(QStringLiteral("job_id")).toString().toStdWString();
    r.runId = o.value(QStringLiteral("run_id")).toString().toStdWString();
    r.engine = o.value(QStringLiteral("engine")).toString().toStdString();
    r.database = o.value(QStringLiteral("database")).toString().toStdString();
    r.relativePath = o.value(QStringLiteral("path")).toString().toStdWString();
    r.dumpFormat = o.value(QStringLiteral("dump_format")).toString().toStdString();
    r.compression = o.value(QStringLiteral("compression")).toString().toStdString();
    r.size = o.value(QStringLiteral("size_bytes")).toString().toULongLong();
    auto sha = parseSha256Hex(o.value(QStringLiteral("sha256")).toString());
    if (!sha)
        return Error::make(ErrorCode::Corrupted, "sha256 inválido nos metadados do dump");
    r.sha256 = *sha;
    r.startedAt = QDateTime::fromString(o.value(QStringLiteral("started_at")).toString(), Qt::ISODate);
    r.finishedAt = QDateTime::fromString(o.value(QStringLiteral("finished_at")).toString(), Qt::ISODate);
    r.host = o.value(QStringLiteral("host")).toString().toStdString();
    r.toolPath = o.value(QStringLiteral("tool")).toString().toStdString();
    r.toolVersion = o.value(QStringLiteral("tool_version")).toString().toStdString();
    r.command = o.value(QStringLiteral("command")).toString().toStdString();
    const QJsonObject v = o.value(QStringLiteral("validation")).toObject();
    r.validationMethod = v.value(QStringLiteral("method")).toString().toStdString();
    r.validationResult = v.value(QStringLiteral("result")).toString().toStdString();
    r.tablesKnown = o.contains(QStringLiteral("tables"));
    r.tables = fromArray(o.value(QStringLiteral("tables")));
    r.warnings = fromArray(o.value(QStringLiteral("warnings")));
    r.suspicious = o.value(QStringLiteral("suspicious")).toBool();
    r.restoreHint = o.value(QStringLiteral("restore_hint")).toString().toStdString();
    r.createdByHost = o.value(QStringLiteral("created_by_host")).toString().toStdString();
    const QJsonObject t = o.value(QStringLiteral("restore_test")).toObject();
    if (!t.isEmpty()) {
        r.restoreTestedAt = QDateTime::fromString(t.value(QStringLiteral("at")).toString(), Qt::ISODate);
        r.restoreTestResult = t.value(QStringLiteral("result")).toString().toStdString();
        r.restoreTestDetail = t.value(QStringLiteral("detail")).toString().toStdString();
    }
    return r;
}

std::wstring dumpRelativePath(const std::string& database, const QDateTime& startedUtc, const std::wstring& runId,
                              const std::string& extension)
{
    const std::wstring stamp = startedUtc.toUTC().toString(QStringLiteral("yyyy-MM-ddTHHmmssZ")).toStdWString();
    return L"dumps/" + win::fromUtf8(safeName(database)) + L"/" + stamp + L"_" + runId + win::fromUtf8(extension);
}

std::wstring metadataPathFor(const std::wstring& dumpPath)
{
    const size_t slash = dumpPath.find_last_of(L"\\/");
    const size_t nameStart = slash == std::wstring::npos ? 0 : slash + 1;
    const size_t dot = dumpPath.find(L'.', nameStart);
    return (dot == std::wstring::npos ? dumpPath : dumpPath.substr(0, dot)) + L".dump.json";
}

std::vector<DumpRecord> listDumps(const vault::JobLayout& layout, const std::string& database)
{
    std::vector<DumpRecord> out;
    const std::wstring dumpsDir = win::joinPath(layout.root(), L"dumps");
    for (const auto& dbDir : ops::listSubdirectories(dumpsDir)) {
        if (!database.empty() && dbDir != win::fromUtf8(safeName(database)))
            continue;
        const std::wstring dir = win::joinPath(dumpsDir, dbDir);
        for (const auto& meta : ops::listFiles(dir, L".dump.json")) {
            auto data = win::readWholeFile(win::joinPath(dir, meta), 16u << 20);
            if (!data)
                continue;
            auto rec = DumpRecord::parse(QByteArray::fromStdString(data.value()));
            if (rec)
                out.push_back(std::move(rec).value());
        }
    }
    std::sort(out.begin(), out.end(), [](const DumpRecord& a, const DumpRecord& b) {
        return a.startedAt != b.startedAt ? a.startedAt > b.startedAt : a.runId > b.runId;
    });
    return out;
}

Status rewriteDumpIndex(const vault::JobLayout& layout)
{
    const auto dumps = listDumps(layout);
    QJsonObject header;
    header.insert(QStringLiteral("format"), QStringLiteral("synkro-dump-index"));
    header.insert(QStringLiteral("format_version"), 1);
    header.insert(QStringLiteral("count"), static_cast<qint64>(dumps.size()));
    QByteArray out = QJsonDocument(header).toJson(QJsonDocument::Compact) + '\n';
    for (const auto& d : dumps) {
        QJsonObject o;
        o.insert(QStringLiteral("database"), QString::fromStdString(d.database));
        o.insert(QStringLiteral("run_id"), QString::fromStdWString(d.runId));
        o.insert(QStringLiteral("path"), QString::fromStdWString(d.relativePath));
        o.insert(QStringLiteral("size_bytes"), QString::number(d.size));
        o.insert(QStringLiteral("sha256"), QString::fromStdString(toHex(d.sha256)));
        o.insert(QStringLiteral("started_at"), d.startedAt.toUTC().toString(Qt::ISODate));
        o.insert(QStringLiteral("suspicious"), d.suspicious);
        out += QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n';
    }
    return win::writeFileAtomically(win::joinPath(layout.root(), L"dumps.jsonl"),
                                    std::string_view(out.constData(), static_cast<size_t>(out.size())));
}

std::vector<std::size_t> selectGfs(const std::vector<QDateTime>& newestFirst, const GfsPolicy& policy,
                                   const QDateTime& nowLocal)
{
    std::set<std::size_t> keep;
    if (newestFirst.empty())
        return {};
    keep.insert(0); // o mais recente, sempre

    for (std::size_t i = 0; i < newestFirst.size() && i < static_cast<std::size_t>(std::max(0, policy.keepLast)); ++i)
        keep.insert(i);

    const QDate today = nowLocal.date();
    auto bucket = [&](int count, auto keyOf, auto inWindow) {
        std::set<long long> seen;
        for (std::size_t i = 0; i < newestFirst.size(); ++i) {
            const QDate d = newestFirst[i].toLocalTime().date();
            if (!inWindow(d))
                continue;
            if (seen.insert(keyOf(d)).second && static_cast<int>(seen.size()) <= count)
                keep.insert(i);
        }
    };
    if (policy.keepDaily > 0)
        bucket(policy.keepDaily, [](const QDate& d) { return d.toJulianDay(); },
               [&](const QDate& d) { return d > today.addDays(-policy.keepDaily); });
    if (policy.keepWeekly > 0)
        bucket(policy.keepWeekly,
               [](const QDate& d) {
                   int year = 0;
                   const int week = d.weekNumber(&year);
                   return static_cast<long long>(year) * 100 + week;
               },
               [&](const QDate& d) { return d > today.addDays(-7 * policy.keepWeekly); });
    if (policy.keepMonthly > 0)
        bucket(policy.keepMonthly, [](const QDate& d) { return static_cast<long long>(d.year()) * 12 + d.month(); },
               [&](const QDate& d) { return d > today.addMonths(-policy.keepMonthly); });

    return std::vector<std::size_t>(keep.begin(), keep.end());
}

Result<DumpRetentionResult> applyDumpRetention(const vault::JobLayout& layout, const std::string& database,
                                               const GfsPolicy& policy, const QDateTime& nowLocal)
{
    DumpRetentionResult result;
    std::vector<DumpRecord> all = listDumps(layout, database);
    // Só dumps válidos contam; um dump suspeito no topo congela a retenção.
    std::vector<DumpRecord> valid;
    for (auto& d : all) {
        if (d.validationResult == "ok")
            valid.push_back(std::move(d));
    }
    if (valid.empty() || valid.front().suspicious)
        return result;

    std::vector<QDateTime> times;
    for (const auto& d : valid)
        times.push_back(d.startedAt);
    const auto keepIdx = selectGfs(times, policy, nowLocal);
    std::set<std::size_t> keep(keepIdx.begin(), keepIdx.end());

    for (std::size_t i = 0; i < valid.size(); ++i) {
        if (keep.count(i))
            continue;
        const std::wstring file = win::joinPath(layout.root(), win::fromCatalogPath(valid[i].relativePath));
        if (::DeleteFileW(win::toExtendedPath(file).c_str()) || ::GetLastError() == ERROR_FILE_NOT_FOUND) {
            ::DeleteFileW(win::toExtendedPath(metadataPathFor(file)).c_str());
            ++result.removed;
            result.bytesFreed += valid[i].size;
        }
    }
    return result;
}

} // namespace synkro::db
