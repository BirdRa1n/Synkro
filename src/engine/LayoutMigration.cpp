#include "engine/LayoutMigration.h"

#include "catalog/Manifest.h"
#include "engine/RunSummary.h"
#include "engine/VaultOps.h"
#include "platform/windows/Win32.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>

namespace synkro {

namespace {

std::wstring markerFile(const vault::JobLayout& L)
{
    return win::joinPath(L.root(), L"layout-migration.json");
}

std::wstring stagingDir(const vault::JobLayout& L)
{
    return win::joinPath(L.root(), L"current.layout");
}

Status renamePath(const std::wstring& from, const std::wstring& to)
{
    if (!::MoveFileExW(win::toExtendedPath(from).c_str(), win::toExtendedPath(to).c_str(), MOVEFILE_WRITE_THROUGH))
        return win::lastError("reorganizar o cofre (" + win::toUtf8(from) + " → " + win::toUtf8(to) + ")");
    return Status::success();
}

Status saveMarker(const std::wstring& path, const LayoutMigrationPlan& plan, const std::wstring& token)
{
    QJsonObject o;
    o.insert(QStringLiteral("format"), QStringLiteral("synkro-layout-migration"));
    o.insert(QStringLiteral("main"), QString::fromStdWString(plan.mainAlias));
    QJsonArray extras;
    for (const auto& e : plan.extraAliases)
        extras.append(QString::fromStdWString(e));
    o.insert(QStringLiteral("extras"), extras);
    o.insert(QStringLiteral("run_id"), QString::fromStdWString(plan.runId));
    o.insert(QStringLiteral("token"), QString::fromStdWString(token));
    return win::writeFileAtomically(path, QJsonDocument(o).toJson(QJsonDocument::Indented).toStdString());
}

Result<std::pair<LayoutMigrationPlan, std::wstring>> loadMarker(const std::wstring& path)
{
    auto text = win::readWholeFile(path, 1u << 20);
    if (!text)
        return text.error();
    const QJsonObject o = QJsonDocument::fromJson(QByteArray::fromStdString(text.value())).object();
    LayoutMigrationPlan plan;
    plan.mainAlias = o.value(QStringLiteral("main")).toString().toStdWString();
    for (const auto& e : o.value(QStringLiteral("extras")).toArray())
        plan.extraAliases.push_back(e.toString().toStdWString());
    plan.runId = o.value(QStringLiteral("run_id")).toString().toStdWString();
    const std::wstring token = o.value(QStringLiteral("token")).toString().toStdWString();
    if (plan.mainAlias.empty() || plan.runId.empty() || token.empty())
        return Error::make(ErrorCode::Corrupted, "marcador de reorganização do cofre ilegível: " + win::toUtf8(path));
    return std::pair{plan, token};
}

bool underPrefix(const std::wstring& path, const std::wstring& alias)
{
    const std::wstring prefix = win::foldCase(win::toCatalogPath(alias)) + L"/";
    const std::wstring folded = win::foldCase(path);
    return folded.size() > prefix.size() && folded.compare(0, prefix.size(), prefix) == 0;
}

} // namespace

bool layoutMigrationPending(const vault::JobLayout& layout)
{
    return ops::pathExists(markerFile(layout));
}

Result<LayoutMigrationResult> migrateToFolderLayout(const vault::JobLayout& L, Catalog& catalog,
                                                    const LayoutMigrationPlan& requested)
{
    LayoutMigrationResult result;
    const std::wstring marker = markerFile(L);
    LayoutMigrationPlan plan = requested;
    std::wstring token;

    if (ops::pathExists(marker)) {
        // Retoma a reorganização interrompida com o plano gravado na época.
        auto loaded = loadMarker(marker);
        if (!loaded)
            return loaded.error();
        plan = loaded.value().first;
        token = loaded.value().second;
    } else {
        if (catalog.folderLayout)
            return result; // já organizado
        token = QStringLiteral("%1").arg(QRandomGenerator::system()->generate(), 8, 16, QLatin1Char('0')).toStdWString();
        if (auto st = saveMarker(marker, plan, token); !st)
            return st.error();
        if (auto st = win::createDirectories(L.current()); !st)
            return st.error();
        if (auto st = win::writeFileAtomically(win::joinPath(L.current(), L".synkro-layout-" + token), ""); !st)
            return st.error();
    }
    result.migrated = true;

    const std::wstring sentinelName = L".synkro-layout-" + token;
    const std::wstring current = L.current();
    const std::wstring staging = stagingDir(L);
    const std::wstring mainDir = win::joinPath(current, win::fromCatalogPath(plan.mainAlias));

    // 1. current\ inteiro vai para current.layout\<principal> (uma renomeação).
    if (ops::pathExists(win::joinPath(current, sentinelName))) {
        if (auto st = win::createDirectories(staging); !st)
            return st.error();
        if (auto st = renamePath(current, win::joinPath(staging, plan.mainAlias)); !st)
            return st.error();
    }
    // 2. current.layout\ vira o novo current\.
    if (ops::pathExists(staging)) {
        // Um current\ vazio pode ter sido recriado depois da queda: some antes.
        if (ops::pathExists(current))
            ::RemoveDirectoryW(win::toExtendedPath(current).c_str()); // só remove se estiver vazio
        if (ops::pathExists(current))
            return Error::make(ErrorCode::Corrupted, "reorganização do cofre inconsistente: current e current.layout existem em "
                                                         + win::toUtf8(L.root()));
        if (auto st = renamePath(staging, current); !st)
            return st.error();
    }
    const std::wstring movedSentinel = win::joinPath(mainDir, sentinelName);
    if (!ops::pathExists(movedSentinel)) {
        if (catalog.folderLayout) { // só faltou apagar o marcador
            ::DeleteFileW(win::toExtendedPath(marker).c_str());
            return result;
        }
        return Error::make(ErrorCode::Corrupted,
                           "reorganização do cofre inconsistente (sentinela não encontrado) em " + win::toUtf8(L.root()));
    }

    // 3. Pastas adicionais (0.9.0-beta.6) voltam para o lado da principal.
    for (const auto& extra : plan.extraAliases) {
        const std::wstring from = win::joinPath(mainDir, extra);
        const std::wstring to = win::joinPath(current, extra);
        if (ops::pathExists(from) && !ops::pathExists(to)) {
            if (auto st = renamePath(from, to); !st)
                return st.error();
        }
    }

    // 4. Catálogo com os caminhos novos e o registro da mudança.
    if (!catalog.folderLayout) {
        Manifest manifest;
        manifest.runId = plan.runId;
        const std::wstring mainPrefix = win::toCatalogPath(plan.mainAlias) + L"/";
        for (auto& e : catalog.entries) {
            bool extra = false;
            for (const auto& alias : plan.extraAliases)
                extra = extra || underPrefix(e.meta.relativePath, alias);
            if (extra)
                continue;
            ManifestRecord rec;
            rec.op = ManifestOp::Renamed;
            rec.previous = e;
            e.meta.relativePath = mainPrefix + e.meta.relativePath;
            rec.entry = e;
            rec.path = e.meta.relativePath;
            manifest.records.push_back(std::move(rec));
        }
        result.renamed = manifest.records.size();
        if (auto st = win::createDirectories(L.runs()); !st)
            return st.error();
        if (auto st = manifest.save(L.manifestFile(plan.runId)); !st)
            return st.error();
        RunSummary s;
        s.runId = plan.runId;
        s.jobId = L.jobId();
        s.status = QStringLiteral("success");
        s.startedAt = s.finishedAt = QDateTime::currentDateTimeUtc();
        s.filesScanned = catalog.entries.size();
        s.unchanged = catalog.entries.size();
        s.note = QStringLiteral("Cofre reorganizado: cada pasta de origem em sua subpasta (%1 arquivo(s) movidos, nada recopiado)")
                     .arg(result.renamed);
        (void)s.save(L.summaryFile(plan.runId));

        catalog.runId = plan.runId;
        catalog.folderLayout = true;
        catalog.sort();
        catalog.reindex();
        if (auto st = catalog.save(L.catalogFile()); !st)
            return st.error();
    }

    // 5. Limpeza.
    ::DeleteFileW(win::toExtendedPath(movedSentinel).c_str());
    ::DeleteFileW(win::toExtendedPath(marker).c_str());
    return result;
}

} // namespace synkro
