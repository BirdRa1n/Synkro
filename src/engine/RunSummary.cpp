#include "engine/RunSummary.h"

#include "core/Version.h"
#include "platform/windows/Win32.h"

#include <QJsonDocument>
#include <QJsonObject>

namespace synkro {

void RunSummary::count(const ManifestRecord& r)
{
    switch (r.op) {
    case ManifestOp::Added:
        ++added;
        if (r.entry)
            bytesWritten += r.entry->meta.size;
        break;
    case ManifestOp::Modified:
        ++modified;
        if (r.entry)
            bytesWritten += r.entry->meta.size;
        break;
    case ManifestOp::Deleted:
        ++deleted;
        break;
    case ManifestOp::Failed:
        ++failed;
        break;
    case ManifestOp::Renamed:
        break; // reorganização: nada foi copiado
    }
}

QByteArray RunSummary::serialize() const
{
    QJsonObject o;
    o.insert(QStringLiteral("format"), QStringLiteral("synkro-run-summary"));
    o.insert(QStringLiteral("format_version"), 1);
    o.insert(QStringLiteral("run_id"), QString::fromStdWString(runId));
    o.insert(QStringLiteral("job_id"), QString::fromStdWString(jobId));
    o.insert(QStringLiteral("vault_id"), vaultId);
    o.insert(QStringLiteral("source"), source);
    o.insert(QStringLiteral("status"), status);
    o.insert(QStringLiteral("started_at"), startedAt.toUTC().toString(Qt::ISODateWithMs));
    o.insert(QStringLiteral("finished_at"), finishedAt.toUTC().toString(Qt::ISODateWithMs));
    o.insert(QStringLiteral("files_scanned"), QString::number(filesScanned));
    o.insert(QStringLiteral("added"), QString::number(added));
    o.insert(QStringLiteral("modified"), QString::number(modified));
    o.insert(QStringLiteral("deleted"), QString::number(deleted));
    o.insert(QStringLiteral("failed"), QString::number(failed));
    o.insert(QStringLiteral("unchanged"), QString::number(unchanged));
    o.insert(QStringLiteral("bytes_written"), QString::number(bytesWritten));
    o.insert(QStringLiteral("synkro_version"), QString::fromLatin1(versionString()));
    if (!note.isEmpty())
        o.insert(QStringLiteral("note"), note);
    return QJsonDocument(o).toJson(QJsonDocument::Indented);
}

Result<RunSummary> RunSummary::parse(const QByteArray& data)
{
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(data, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return Error::make(ErrorCode::Corrupted, "resumo de execução inválido");
    const QJsonObject o = doc.object();
    RunSummary s;
    s.runId = o.value(QStringLiteral("run_id")).toString().toStdWString();
    s.jobId = o.value(QStringLiteral("job_id")).toString().toStdWString();
    s.vaultId = o.value(QStringLiteral("vault_id")).toString();
    s.source = o.value(QStringLiteral("source")).toString();
    s.status = o.value(QStringLiteral("status")).toString();
    s.startedAt = QDateTime::fromString(o.value(QStringLiteral("started_at")).toString(), Qt::ISODateWithMs);
    s.finishedAt = QDateTime::fromString(o.value(QStringLiteral("finished_at")).toString(), Qt::ISODateWithMs);
    s.filesScanned = o.value(QStringLiteral("files_scanned")).toString().toULongLong();
    s.added = o.value(QStringLiteral("added")).toString().toULongLong();
    s.modified = o.value(QStringLiteral("modified")).toString().toULongLong();
    s.deleted = o.value(QStringLiteral("deleted")).toString().toULongLong();
    s.failed = o.value(QStringLiteral("failed")).toString().toULongLong();
    s.unchanged = o.value(QStringLiteral("unchanged")).toString().toULongLong();
    s.bytesWritten = o.value(QStringLiteral("bytes_written")).toString().toULongLong();
    s.note = o.value(QStringLiteral("note")).toString();
    return s;
}

Status RunSummary::save(const std::wstring& path) const
{
    const QByteArray data = serialize();
    return win::writeFileAtomically(path, std::string_view(data.constData(), static_cast<size_t>(data.size())));
}

Result<RunSummary> RunSummary::load(const std::wstring& path)
{
    auto data = win::readWholeFile(path, 1u << 20);
    if (!data)
        return data.error();
    return parse(QByteArray::fromStdString(data.value()));
}

} // namespace synkro
