#include "catalog/Manifest.h"

#include "platform/windows/Win32.h"

#include <QJsonDocument>

namespace synkro {

namespace {
constexpr char kFormat[] = "synkro-manifest";
constexpr int kFormatVersion = 1;
} // namespace

const char* nameOf(ManifestOp op) noexcept
{
    switch (op) {
    case ManifestOp::Added:    return "added";
    case ManifestOp::Modified: return "modified";
    case ManifestOp::Deleted:  return "deleted";
    case ManifestOp::Failed:   return "failed";
    case ManifestOp::Renamed:  return "renamed";
    }
    return "unknown";
}

static std::optional<ManifestOp> parseOp(const QString& s)
{
    if (s == QLatin1String("renamed"))  return ManifestOp::Renamed;
    if (s == QLatin1String("added"))    return ManifestOp::Added;
    if (s == QLatin1String("modified")) return ManifestOp::Modified;
    if (s == QLatin1String("deleted"))  return ManifestOp::Deleted;
    if (s == QLatin1String("failed"))   return ManifestOp::Failed;
    return std::nullopt;
}

QJsonObject toJson(const ManifestRecord& r)
{
    QJsonObject o;
    o.insert(QStringLiteral("op"), QString::fromLatin1(nameOf(r.op)));
    o.insert(QStringLiteral("path"), QString::fromStdWString(r.path));
    if (r.entry)
        o.insert(QStringLiteral("entry"), toJson(*r.entry));
    if (r.previous)
        o.insert(QStringLiteral("previous"), toJson(*r.previous));
    if (!r.error.empty())
        o.insert(QStringLiteral("error"), QString::fromStdString(r.error));
    return o;
}

Result<ManifestRecord> manifestRecordFromJson(const QJsonObject& o)
{
    ManifestRecord r;
    const auto op = parseOp(o.value(QStringLiteral("op")).toString());
    if (!op)
        return Error::make(ErrorCode::Corrupted, "operação desconhecida no manifest");
    r.op = *op;
    r.path = o.value(QStringLiteral("path")).toString().toStdWString();
    if (o.contains(QStringLiteral("entry"))) {
        auto e = catalogEntryFromJson(o.value(QStringLiteral("entry")).toObject());
        if (!e)
            return e.error();
        r.entry = std::move(e).value();
    }
    if (o.contains(QStringLiteral("previous"))) {
        auto p = catalogEntryFromJson(o.value(QStringLiteral("previous")).toObject());
        if (!p)
            return p.error();
        r.previous = std::move(p).value();
    }
    r.error = o.value(QStringLiteral("error")).toString().toStdString();
    return r;
}

QByteArray Manifest::serialize() const
{
    QJsonObject header;
    header.insert(QStringLiteral("format"), QString::fromLatin1(kFormat));
    header.insert(QStringLiteral("format_version"), kFormatVersion);
    header.insert(QStringLiteral("run_id"), QString::fromStdWString(runId));
    header.insert(QStringLiteral("count"), static_cast<qint64>(records.size()));

    QByteArray out = QJsonDocument(header).toJson(QJsonDocument::Compact);
    out.append('\n');
    for (const auto& r : records) {
        out.append(QJsonDocument(toJson(r)).toJson(QJsonDocument::Compact));
        out.append('\n');
    }
    return out;
}

Result<Manifest> Manifest::parse(const QByteArray& data)
{
    Manifest m;
    bool headerSeen = false;
    qint64 expected = -1;
    int lineNo = 0;
    for (const QByteArray& raw : data.split('\n')) {
        ++lineNo;
        const QByteArray line = raw.trimmed();
        if (line.isEmpty())
            continue;
        QJsonParseError err{};
        const QJsonDocument doc = QJsonDocument::fromJson(line, &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject())
            return Error::make(ErrorCode::Corrupted, "manifest inválido na linha " + std::to_string(lineNo));
        const QJsonObject o = doc.object();
        if (!headerSeen) {
            if (o.value(QStringLiteral("format")).toString() != QLatin1String(kFormat))
                return Error::make(ErrorCode::Corrupted, "arquivo não é um manifest do Synkro");
            m.runId = o.value(QStringLiteral("run_id")).toString().toStdWString();
            expected = o.value(QStringLiteral("count")).toInteger(-1);
            headerSeen = true;
            continue;
        }
        auto r = manifestRecordFromJson(o);
        if (!r)
            return r.error();
        m.records.push_back(std::move(r).value());
    }
    if (!headerSeen)
        return Error::make(ErrorCode::Corrupted, "manifest vazio");
    if (expected >= 0 && expected != static_cast<qint64>(m.records.size()))
        return Error::make(ErrorCode::Corrupted, "manifest truncado");
    return m;
}

Status Manifest::save(const std::wstring& path) const
{
    const QByteArray data = serialize();
    return win::writeFileAtomically(path, std::string_view(data.constData(), static_cast<size_t>(data.size())));
}

Result<Manifest> Manifest::load(const std::wstring& path)
{
    auto data = win::readWholeFile(path, std::size_t(4) << 30);
    if (!data)
        return data.error();
    return parse(QByteArray::fromStdString(data.value()));
}

} // namespace synkro
