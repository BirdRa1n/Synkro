#pragma once

#include "catalog/Manifest.h"

#include <QDateTime>
#include <QString>

#include <cstdint>
#include <string>

namespace synkro {

// runs/<run>.summary.json — totais e status de uma execução num cofre.
struct RunSummary {
    std::wstring runId;
    std::wstring jobId;
    QString vaultId;
    QString source;
    QString status;          // success | partial | failed | blocked | interrupted
    QDateTime startedAt;
    QDateTime finishedAt;
    std::uint64_t filesScanned = 0;
    std::uint64_t added = 0;
    std::uint64_t modified = 0;
    std::uint64_t deleted = 0;
    std::uint64_t failed = 0;
    std::uint64_t unchanged = 0;
    std::uint64_t bytesWritten = 0;
    QString note;

    void count(const ManifestRecord& r);

    QByteArray serialize() const;
    static Result<RunSummary> parse(const QByteArray& data);
    Status save(const std::wstring& path) const;
    static Result<RunSummary> load(const std::wstring& path);
};

} // namespace synkro
