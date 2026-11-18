#include "catalog/Catalog.h"

#include "platform/windows/Win32.h"

#include <QJsonDocument>

#include <algorithm>

namespace synkro {

namespace {

constexpr char kFormat[] = "synkro-catalog";
constexpr int kFormatVersion = 1;

QString hex64(std::uint64_t v)
{
    return QString::number(v);
}

std::uint64_t num64(const QJsonValue& v)
{
    // Inteiros de 64 bits são gravados como string para não perder precisão
    // (JSON/double só representa 53 bits com exatidão).
    if (v.isString())
        return v.toString().toULongLong();
    return static_cast<std::uint64_t>(v.toDouble());
}

} // namespace

std::string toHexId(const FileId& id)
{
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(32);
    for (auto b : id) {
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 0x0F]);
    }
    return out;
}

static bool parseHexBytes(const QString& hex, std::uint8_t* out, size_t n)
{
    if (static_cast<size_t>(hex.size()) != n * 2)
        return false;
    for (size_t i = 0; i < n; ++i) {
        bool ok = false;
        const uint v = hex.mid(static_cast<qsizetype>(i * 2), 2).toUInt(&ok, 16);
        if (!ok)
            return false;
        out[i] = static_cast<std::uint8_t>(v);
    }
    return true;
}

std::optional<Sha256Digest> parseSha256Hex(const QString& hex)
{
    Sha256Digest d{};
    if (!parseHexBytes(hex, d.data(), d.size()))
        return std::nullopt;
    return d;
}

QJsonObject toJson(const CatalogEntry& e)
{
    QJsonObject o;
    o.insert(QStringLiteral("path"), QString::fromStdWString(e.meta.relativePath));
    o.insert(QStringLiteral("size"), hex64(e.meta.size));
    o.insert(QStringLiteral("mtime"), hex64(e.meta.lastWriteTime));
    o.insert(QStringLiteral("ctime"), hex64(e.meta.creationTime));
    o.insert(QStringLiteral("attrs"), static_cast<qint64>(e.meta.attributes));
    if (e.meta.hasExtendedInfo) {
        o.insert(QStringLiteral("change_time"), hex64(e.meta.changeTime));
        o.insert(QStringLiteral("volume"), hex64(e.meta.volumeSerial));
        o.insert(QStringLiteral("file_id"), QString::fromStdString(toHexId(e.meta.fileId)));
    }
    o.insert(QStringLiteral("sha256"), QString::fromStdString(toHex(e.sha256)));
    o.insert(QStringLiteral("since_run"), QString::fromStdWString(e.sinceRun));
    return o;
}

Result<CatalogEntry> catalogEntryFromJson(const QJsonObject& o)
{
    CatalogEntry e;
    e.meta.relativePath = o.value(QStringLiteral("path")).toString().toStdWString();
    if (e.meta.relativePath.empty())
        return Error::make(ErrorCode::Corrupted, "entrada sem caminho");
    e.meta.size = num64(o.value(QStringLiteral("size")));
    e.meta.lastWriteTime = num64(o.value(QStringLiteral("mtime")));
    e.meta.creationTime = num64(o.value(QStringLiteral("ctime")));
    e.meta.attributes = static_cast<std::uint32_t>(o.value(QStringLiteral("attrs")).toInteger());
    if (o.contains(QStringLiteral("file_id"))) {
        e.meta.hasExtendedInfo = true;
        e.meta.changeTime = num64(o.value(QStringLiteral("change_time")));
        e.meta.volumeSerial = num64(o.value(QStringLiteral("volume")));
        if (!parseHexBytes(o.value(QStringLiteral("file_id")).toString(), e.meta.fileId.data(), e.meta.fileId.size()))
            return Error::make(ErrorCode::Corrupted, "file_id inválido");
    }
    auto sha = parseSha256Hex(o.value(QStringLiteral("sha256")).toString());
    if (!sha)
        return Error::make(ErrorCode::Corrupted, "sha256 inválido");
    e.sha256 = *sha;
    e.sinceRun = o.value(QStringLiteral("since_run")).toString().toStdWString();
    return e;
}

void Catalog::reindex()
{
    m_index.clear();
    m_index.reserve(entries.size());
    for (size_t i = 0; i < entries.size(); ++i)
        m_index[win::foldCase(entries[i].meta.relativePath)] = i;
}

const CatalogEntry* Catalog::find(const std::wstring& catalogPath) const
{
    const auto it = m_index.find(win::foldCase(catalogPath));
    return it == m_index.end() ? nullptr : &entries[it->second];
}

CatalogEntry* Catalog::find(const std::wstring& catalogPath)
{
    const auto it = m_index.find(win::foldCase(catalogPath));
    return it == m_index.end() ? nullptr : &entries[it->second];
}

void Catalog::upsert(CatalogEntry entry)
{
    const std::wstring key = win::foldCase(entry.meta.relativePath);
    const auto it = m_index.find(key);
    if (it != m_index.end()) {
        entries[it->second] = std::move(entry);
        return;
    }
    m_index.emplace(key, entries.size());
    entries.push_back(std::move(entry));
}

bool Catalog::remove(const std::wstring& catalogPath)
{
    const auto it = m_index.find(win::foldCase(catalogPath));
    if (it == m_index.end())
        return false;
    const size_t idx = it->second;
    m_index.erase(it);
    if (idx != entries.size() - 1) {
        entries[idx] = std::move(entries.back());
        m_index[win::foldCase(entries[idx].meta.relativePath)] = idx;
    }
    entries.pop_back();
    return true;
}

void Catalog::sort()
{
    std::sort(entries.begin(), entries.end(), [](const CatalogEntry& a, const CatalogEntry& b) {
        return a.meta.relativePath < b.meta.relativePath;
    });
    reindex();
}

QByteArray Catalog::serialize() const
{
    std::vector<const CatalogEntry*> ordered;
    ordered.reserve(entries.size());
    for (const auto& e : entries)
        ordered.push_back(&e);
    std::sort(ordered.begin(), ordered.end(), [](const CatalogEntry* a, const CatalogEntry* b) {
        return a->meta.relativePath < b->meta.relativePath;
    });

    QJsonObject header;
    header.insert(QStringLiteral("format"), QString::fromLatin1(kFormat));
    header.insert(QStringLiteral("format_version"), kFormatVersion);
    header.insert(QStringLiteral("run_id"), QString::fromStdWString(runId));
    header.insert(QStringLiteral("count"), static_cast<qint64>(entries.size()));

    QByteArray out = QJsonDocument(header).toJson(QJsonDocument::Compact);
    out.append('\n');
    for (const auto* e : ordered) {
        out.append(QJsonDocument(toJson(*e)).toJson(QJsonDocument::Compact));
        out.append('\n');
    }
    return out;
}

Result<Catalog> Catalog::parse(const QByteArray& data)
{
    Catalog cat;
    const QList<QByteArray> lines = data.split('\n');
    bool headerSeen = false;
    qint64 expected = -1;
    int lineNo = 0;
    for (const QByteArray& raw : lines) {
        ++lineNo;
        const QByteArray line = raw.trimmed();
        if (line.isEmpty())
            continue;
        QJsonParseError err{};
        const QJsonDocument doc = QJsonDocument::fromJson(line, &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject())
            return Error::make(ErrorCode::Corrupted, "catálogo inválido na linha " + std::to_string(lineNo));
        const QJsonObject o = doc.object();
        if (!headerSeen) {
            if (o.value(QStringLiteral("format")).toString() != QLatin1String(kFormat))
                return Error::make(ErrorCode::Corrupted, "arquivo não é um catálogo do Synkro");
            if (o.value(QStringLiteral("format_version")).toInt() > kFormatVersion)
                return Error::make(ErrorCode::Unsupported, "catálogo de uma versão mais nova do Synkro");
            cat.runId = o.value(QStringLiteral("run_id")).toString().toStdWString();
            expected = o.value(QStringLiteral("count")).toInteger(-1);
            headerSeen = true;
            continue;
        }
        auto entry = catalogEntryFromJson(o);
        if (!entry)
            return Error::make(ErrorCode::Corrupted,
                               "catálogo inválido na linha " + std::to_string(lineNo) + ": " + entry.error().message);
        cat.entries.push_back(std::move(entry).value());
    }
    if (!headerSeen)
        return Error::make(ErrorCode::Corrupted, "catálogo vazio");
    if (expected >= 0 && expected != static_cast<qint64>(cat.entries.size()))
        return Error::make(ErrorCode::Corrupted, "catálogo truncado (quantidade de entradas não confere)");
    cat.reindex();
    return cat;
}

Status Catalog::save(const std::wstring& path) const
{
    const QByteArray data = serialize();
    auto digest = Sha256::ofBytes(std::string_view(data.constData(), static_cast<size_t>(data.size())));
    if (!digest)
        return digest.error();

    if (auto st = win::writeFileAtomically(path, std::string_view(data.constData(), static_cast<size_t>(data.size())));
        !st)
        return st;
    const std::string sidecar = toHex(digest.value()) + "  catalog.jsonl\n";
    return win::writeFileAtomically(path + L".sha256", sidecar);
}

Result<Catalog> Catalog::load(const std::wstring& path)
{
    auto data = win::readWholeFile(path, std::size_t(4) << 30);
    if (!data)
        return data.error();

    auto sidecar = win::readWholeFile(path + L".sha256", 4096);
    if (!sidecar)
        return Error::make(ErrorCode::Corrupted, "catalog.jsonl.sha256 ausente ou ilegível");
    const QString expectedHex = QString::fromStdString(sidecar.value()).section(QLatin1Char(' '), 0, 0).trimmed();
    auto actual = Sha256::ofBytes(data.value());
    if (!actual)
        return actual.error();
    if (QString::fromStdString(toHex(actual.value())) != expectedHex)
        return Error::make(ErrorCode::Corrupted, "o hash do catálogo não confere (arquivo corrompido)");

    return parse(QByteArray::fromStdString(data.value()));
}

} // namespace synkro
