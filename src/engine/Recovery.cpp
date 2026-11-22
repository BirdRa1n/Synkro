#include "engine/Recovery.h"

#include "catalog/Manifest.h"
#include "engine/Journal.h"
#include "engine/RunSummary.h"
#include "engine/VaultOps.h"
#include "platform/windows/Win32.h"
#include "scan/FileScanner.h"

#include <unordered_set>

namespace synkro {

static void applyRecord(Catalog& catalog, const ManifestRecord& r)
{
    switch (r.op) {
    case ManifestOp::Added:
    case ManifestOp::Modified:
        if (r.entry)
            catalog.upsert(*r.entry);
        break;
    case ManifestOp::Deleted:
        catalog.remove(r.path);
        break;
    case ManifestOp::Failed:
        break;
    case ManifestOp::Renamed:
        if (r.previous)
            catalog.remove(r.previous->meta.relativePath);
        if (r.entry)
            catalog.upsert(*r.entry);
        break;
    }
}

Result<std::vector<RecoveredRun>> recoverInterruptedRuns(const vault::JobLayout& layout, Catalog& catalog)
{
    std::vector<RecoveredRun> recovered;

    (void)ops::cleanTempDirectory(layout);

    const auto journals = ops::listFiles(layout.runs(), L".journal.jsonl");
    for (const auto& name : journals) {
        const std::wstring runId = name.substr(0, name.size() - std::wstring(L".journal.jsonl").size());
        const std::wstring journalPath = win::joinPath(layout.runs(), name);

        auto contents = readJournal(journalPath);
        if (!contents)
            return Error::make(contents.error().code,
                               "não foi possível recuperar a execução " + win::toUtf8(runId) + ": "
                                   + contents.error().message);

        RecoveredRun rr;
        rr.runId = runId;
        rr.journalTruncated = contents.value().truncatedTail;

        std::unordered_set<std::wstring> confirmedDeletes;
        for (const auto& r : contents.value().done) {
            if (r.op == ManifestOp::Deleted)
                confirmedDeletes.insert(win::foldCase(r.path));
        }

        // 1. Trocas interrompidas: a versão anterior já foi para versions/ mas
        //    a nova não chegou a current/. Devolve a anterior.
        const std::wstring versionsDir = layout.versions(runId);
        if (ops::pathExists(versionsDir)) {
            auto moved = scanDirectory(versionsDir);
            if (moved) {
                for (const auto& f : moved.value().files) {
                    if (confirmedDeletes.count(win::foldCase(f.relativePath)))
                        continue;
                    if (ops::pathExists(layout.currentFile(f.relativePath)))
                        continue;
                    if (auto st = ops::moveVersionBackToCurrent(layout, runId, f.relativePath); st)
                        ++rr.versionsMovedBack;
                }
                ops::removeEmptyDirectories(versionsDir, true);
            }
        }

        // 2. Operações confirmadas entram no catálogo.
        Manifest manifest;
        manifest.runId = runId;
        for (const auto& r : contents.value().done) {
            applyRecord(catalog, r);
            manifest.records.push_back(r);
            ++rr.recordsApplied;
        }

        // 3. Manifest e resumo da execução interrompida.
        if (auto st = manifest.save(layout.manifestFile(runId)); !st)
            return st.error();

        RunSummary summary;
        summary.runId = runId;
        summary.jobId = layout.jobId();
        summary.status = QStringLiteral("interrupted");
        summary.finishedAt = QDateTime::currentDateTimeUtc();
        for (const auto& r : manifest.records)
            summary.count(r);
        summary.note = QStringLiteral("execução interrompida; recuperada na execução seguinte");
        if (auto st = summary.save(layout.summaryFile(runId)); !st)
            return st.error();

        // 4. Catálogo consistente com current/ e diário descartado.
        catalog.runId = runId;
        if (auto st = catalog.save(layout.catalogFile()); !st)
            return st.error();
        ::DeleteFileW(win::toExtendedPath(journalPath).c_str());

        recovered.push_back(rr);
    }
    return recovered;
}

} // namespace synkro
