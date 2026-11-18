#include "engine/Journal.h"

#include <QJsonDocument>
#include <QJsonObject>

namespace synkro {

Result<Journal> Journal::create(const std::wstring& path, const std::wstring& runId, int flushEvery)
{
    Journal j;
    j.m_flushEvery = flushEvery < 1 ? 1 : flushEvery;
    j.m_file.reset(::CreateFileW(win::toExtendedPath(path).c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                 CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!j.m_file)
        return win::lastError("criar diário da execução");

    QJsonObject start;
    start.insert(QStringLiteral("t"), QStringLiteral("start"));
    start.insert(QStringLiteral("run_id"), QString::fromStdWString(runId));
    if (auto st = j.appendLine(QJsonDocument(start).toJson(QJsonDocument::Compact)); !st)
        return st.error();
    if (auto st = j.flush(); !st)
        return st.error();
    return j;
}

Journal::~Journal()
{
    close();
}

void Journal::close()
{
    if (m_file) {
        (void)flush();
        m_file.reset();
    }
}

Status Journal::appendLine(const QByteArray& line)
{
    m_pending.append(line);
    m_pending.append('\n');
    ++m_pendingCount;
    return Status::success();
}

Status Journal::flush()
{
    if (!m_file || m_pending.isEmpty())
        return Status::success();
    const char* data = m_pending.constData();
    DWORD remaining = static_cast<DWORD>(m_pending.size());
    while (remaining > 0) {
        DWORD written = 0;
        if (!::WriteFile(m_file.get(), data, remaining, &written, nullptr) || written == 0)
            return win::lastError("gravar diário da execução");
        data += written;
        remaining -= written;
    }
    if (!::FlushFileBuffers(m_file.get()))
        return win::lastError("sincronizar diário da execução");
    m_pending.clear();
    m_pendingCount = 0;
    return Status::success();
}

Status Journal::recordDone(const ManifestRecord& record)
{
    QJsonObject o;
    o.insert(QStringLiteral("t"), QStringLiteral("done"));
    o.insert(QStringLiteral("rec"), toJson(record));
    if (auto st = appendLine(QJsonDocument(o).toJson(QJsonDocument::Compact)); !st)
        return st;
    return m_pendingCount >= m_flushEvery ? flush() : Status::success();
}

Status Journal::recordEnd(const std::string& status)
{
    QJsonObject o;
    o.insert(QStringLiteral("t"), QStringLiteral("end"));
    o.insert(QStringLiteral("status"), QString::fromStdString(status));
    if (auto st = appendLine(QJsonDocument(o).toJson(QJsonDocument::Compact)); !st)
        return st;
    return flush();
}

Result<JournalContents> readJournal(const std::wstring& path)
{
    auto data = win::readWholeFile(path, std::size_t(4) << 30);
    if (!data)
        return data.error();

    JournalContents c;
    const QList<QByteArray> lines = QByteArray::fromStdString(data.value()).split('\n');
    for (qsizetype i = 0; i < lines.size(); ++i) {
        const QByteArray line = lines[i].trimmed();
        if (line.isEmpty())
            continue;
        QJsonParseError err{};
        const QJsonDocument doc = QJsonDocument::fromJson(line, &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject()) {
            // Só a última linha pode estar incompleta; qualquer outra é corrupção.
            bool isLast = true;
            for (qsizetype k = i + 1; k < lines.size(); ++k) {
                if (!lines[k].trimmed().isEmpty()) {
                    isLast = false;
                    break;
                }
            }
            if (!isLast)
                return Error::make(ErrorCode::Corrupted, "diário corrompido no meio do arquivo");
            c.truncatedTail = true;
            break;
        }
        const QJsonObject o = doc.object();
        const QString t = o.value(QStringLiteral("t")).toString();
        if (t == QLatin1String("start")) {
            c.runId = o.value(QStringLiteral("run_id")).toString().toStdWString();
        } else if (t == QLatin1String("done")) {
            auto rec = manifestRecordFromJson(o.value(QStringLiteral("rec")).toObject());
            if (!rec)
                return rec.error();
            c.done.push_back(std::move(rec).value());
        } else if (t == QLatin1String("end")) {
            c.ended = true;
            c.endStatus = o.value(QStringLiteral("status")).toString().toStdString();
        }
    }
    return c;
}

} // namespace synkro
