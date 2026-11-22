#include "engine/BackupEngine.h"

#include "catalog/Catalog.h"
#include "catalog/Manifest.h"
#include "core/RunId.h"
#include "crypto/VaultKey.h"
#include "engine/Journal.h"
#include "engine/LayoutMigration.h"
#include "engine/Recovery.h"
#include "engine/Retention.h"
#include "engine/RunSummary.h"
#include "engine/VaultOps.h"
#include "platform/windows/Locks.h"
#include "platform/windows/Snapshot.h"
#include "platform/windows/Volume.h"
#include "scan/ChangeDetector.h"
#include "vault/JobLayout.h"
#include "vault/Vault.h"

#include <QDateTime>

#include <QStringList>

#include <algorithm>
#include <map>
#include <optional>

namespace synkro {

const char* nameOf(VaultRunStatus s) noexcept
{
    switch (s) {
    case VaultRunStatus::Success: return "success";
    case VaultRunStatus::Partial: return "partial";
    case VaultRunStatus::Failed:  return "failed";
    case VaultRunStatus::Blocked: return "blocked";
    }
    return "unknown";
}

const char* nameOf(RunStatus s) noexcept
{
    switch (s) {
    case RunStatus::Success:        return "success";
    case RunStatus::Degraded:       return "degraded";
    case RunStatus::Failed:         return "failed";
    case RunStatus::Blocked:        return "blocked";
    case RunStatus::AlreadyRunning: return "already_running";
    }
    return "unknown";
}

namespace {

constexpr std::size_t kMaxReportedFileErrors = 50;

// Uma pasta de origem já resolvida: de onde ler (talvez dentro do snapshot) e
// o prefixo dos seus arquivos no catálogo ("" = pasta única, na raiz).
struct ReadRoot {
    std::wstring prefix;   // "<alias>/" no formato do catálogo
    std::wstring readPath;
    std::wstring label;    // caminho real, para mensagens
};

// Varredura de uma pasta, com os caminhos relativos a ela (sem prefixo).
struct SourceScan {
    ReadRoot root;
    ScanResult scan;
    bool main = false;
};

bool startsWith(const std::wstring& s, const std::wstring& prefix)
{
    return !prefix.empty() && s.size() > prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

// Junta as varreduras no formato do cofre: com `folders`, a pasta principal
// ganha o prefixo <mainAlias>/; sem, fica na raiz (pasta única).
std::pair<ScanResult, std::vector<ReadRoot>> mergeScans(const std::vector<SourceScan>& scans, bool folders,
                                                        const std::wstring& mainAlias)
{
    ScanResult all;
    std::vector<ReadRoot> roots;
    for (const auto& s : scans) {
        ReadRoot root = s.root;
        if (s.main)
            root.prefix = folders ? win::toCatalogPath(mainAlias) + L"/" : std::wstring();
        for (const auto& f : s.scan.files) {
            FileEntry e = f;
            e.relativePath = root.prefix + e.relativePath;
            all.files.push_back(std::move(e));
        }
        for (const auto& l : s.scan.skippedLinks)
            all.skippedLinks.push_back(root.prefix + l);
        for (const auto& i : s.scan.issues)
            all.issues.push_back({root.prefix + i.relativePath, i.error});
        all.totalBytes += s.scan.totalBytes;
        all.directoryCount += s.scan.directoryCount;
        roots.push_back(std::move(root));
    }
    if (scans.size() > 1 || folders) {
        std::sort(all.files.begin(), all.files.end(),
                  [](const FileEntry& a, const FileEntry& b) { return a.relativePath < b.relativePath; });
        std::sort(all.skippedLinks.begin(), all.skippedLinks.end());
    }
    // A pasta principal primeiro: sourcePathOf() a usa quando nenhum prefixo casa.
    std::stable_partition(roots.begin(), roots.end(), [](const ReadRoot& r) { return r.prefix.empty(); });
    return {std::move(all), std::move(roots)};
}

class VaultRun {
public:
    VaultRun(const BackupRequest& req, const std::wstring& runId, const std::vector<SourceScan>& scans,
             const VaultTarget& target)
        : m_req(req), m_runId(runId), m_scans(scans), m_target(target)
    {
        m_result.location = target.location;
    }

    VaultRunResult run()
    {
        if (auto st = execute(); !st) {
            m_result.status = VaultRunStatus::Failed;
            m_result.error = st.error().message;
        }
        return m_result;
    }

private:
    Status execute()
    {
        if (m_target.location.empty()) {
            m_result.vaultId = m_target.expectedVaultId;
            return Error::make(ErrorCode::DeviceNotReady,
                               "o cofre \"" + m_target.label.toStdString() + "\" não está conectado"
                                   + (m_target.unavailableReason.empty() ? "" : ": " + m_target.unavailableReason));
        }

        // Identidade: nunca gravar num disco que não seja o cofre esperado.
        auto identity = vault::readIdentity(m_target.location);
        if (!identity)
            return Error::make(identity.error().code,
                               "cofre não encontrado ou inválido em " + win::toUtf8(m_target.location) + ": "
                                   + identity.error().message);
        m_result.vaultId = identity.value().vaultId;
        if (!m_target.expectedVaultId.isEmpty() && identity.value().vaultId != m_target.expectedVaultId)
            return Error::make(ErrorCode::InvalidArgument,
                               "o disco em " + win::toUtf8(m_target.location) + " não é o cofre esperado");

        // Cofre cifrado: sem a chave, nada é gravado (nem em claro).
        auto cipher = crypto::cipherFor(m_target.location, m_req.secretsFile);
        if (!cipher)
            return cipher.error();
        m_cipher = std::move(cipher).value();

        m_layout = vault::JobLayout(m_target.location, m_req.jobId);
        for (const auto& dir : {m_layout.current(), m_layout.runs(), m_layout.tmp(), m_layout.versionsRoot()}) {
            if (auto st = win::createDirectories(dir); !st)
                return st;
        }

        auto lock = win::FileLock::tryAcquire(m_layout.lockFile());
        if (!lock)
            return lock.error();

        // Inventário anterior.
        Catalog catalog;
        auto loaded = Catalog::load(m_layout.catalogFile());
        if (loaded) {
            catalog = std::move(loaded).value();
        } else if (loaded.error().code != ErrorCode::NotFound) {
            return Error::make(loaded.error().code,
                               "catálogo do cofre ilegível; nada foi alterado: " + loaded.error().message);
        }

        auto recovered = recoverInterruptedRuns(m_layout, catalog);
        if (!recovered)
            return recovered.error();
        m_result.recoveredRuns = recovered.value().size();

        // Layout: várias pastas (ou cofre já organizado assim) = uma subpasta
        // por pasta. Cofre antigo é reorganizado agora, só com renomeações.
        const bool folders = !m_req.moreSources.empty() || catalog.folderLayout || layoutMigrationPending(m_layout);
        if (folders && !catalog.folderLayout) {
            if (catalog.entries.empty() && !layoutMigrationPending(m_layout)) {
                catalog.folderLayout = true; // cofre novo para a tarefa: já nasce organizado
            } else {
                LayoutMigrationPlan plan;
                plan.mainAlias = m_req.mainAlias;
                for (const auto& s : m_req.moreSources)
                    plan.extraAliases.push_back(s.alias);
                plan.runId = migrationRunId(catalog.runId);
                auto migrated = migrateToFolderLayout(m_layout, catalog, plan);
                if (!migrated)
                    return Error::make(migrated.error().code,
                                       "não foi possível reorganizar o cofre em subpastas (nada foi copiado; tenta de novo na "
                                       "próxima execução): " + migrated.error().message);
                m_result.reorganized = migrated.value().renamed;
            }
        } else if (layoutMigrationPending(m_layout)) {
            // Marcador antigo com o catálogo já organizado: só termina a limpeza.
            if (auto migrated = migrateToFolderLayout(m_layout, catalog, {}); !migrated)
                return migrated.error();
        }
        auto merged = mergeScans(m_scans, folders, m_req.mainAlias);
        m_scan = std::move(merged.first);
        m_roots = std::move(merged.second);

        std::vector<KnownFile> known;
        known.reserve(catalog.entries.size());
        for (const auto& e : catalog.entries)
            known.push_back({e.meta, e.sha256});
        const ChangeSet changes = detectChanges(known, m_scan.files);
        m_result.unchanged = changes.unchanged;

        if (!m_req.overrideGuards) {
            GuardVerdict verdict = evaluateFolderGuards(catalog);
            if (!verdict.blocked())
                verdict = evaluateChangeGuards(m_req.guards, changes, m_scan.files.size());
            if (verdict.blocked()) {
                m_result.status = VaultRunStatus::Blocked;
                m_result.guard = verdict;
                writeSummary("blocked", QString::fromStdString(verdict.message));
                return Status::success();
            }
        }

        const QDateTime startedAt = QDateTime::currentDateTimeUtc();
        auto journal = Journal::create(m_layout.journalFile(m_runId), m_runId);
        if (!journal)
            return journal.error();

        Manifest manifest;
        manifest.runId = m_runId;
        const std::size_t total = changes.added.size() + changes.modified.size() + changes.deleted.size();
        std::size_t done = 0;

        auto emitFile = [&](const std::wstring& path) {
            ++done;
            if (m_req.progress)
                m_req.progress({ProgressKind::FileDone, m_target.location, path, done, total});
        };

        for (const auto& ch : changes.added) {
            const FileEntry& cur = m_scan.files[ch.currentIndex];
            ManifestRecord rec = copyIn(cur, nullptr);
            if (auto st = commit(journal.value(), manifest, catalog, std::move(rec)); !st)
                return st;
            emitFile(cur.relativePath);
        }
        for (const auto& ch : changes.modified) {
            const FileEntry& cur = m_scan.files[ch.currentIndex];
            const CatalogEntry* prev = catalog.find(known[ch.previousIndex].meta.relativePath);
            ManifestRecord rec = copyIn(cur, prev);
            if (auto st = commit(journal.value(), manifest, catalog, std::move(rec)); !st)
                return st;
            emitFile(cur.relativePath);
        }
        for (const auto& ch : changes.deleted) {
            const CatalogEntry* prev = catalog.find(ch.relativePath);
            ManifestRecord rec;
            rec.path = ch.relativePath;
            if (!prev) {
                emitFile(ch.relativePath);
                continue;
            }
            rec.previous = *prev;
            if (auto st = ops::moveCurrentToVersions(m_layout, m_runId, prev->meta.relativePath); st) {
                rec.op = ManifestOp::Deleted;
            } else if (!ops::pathExists(m_layout.currentFile(prev->meta.relativePath))) {
                rec.op = ManifestOp::Deleted; // já não estava em current/: só retira do catálogo
            } else {
                rec.op = ManifestOp::Failed;
                rec.error = st.error().message;
                rec.previous.reset();
            }
            if (auto st = commit(journal.value(), manifest, catalog, std::move(rec)); !st)
                return st;
            emitFile(ch.relativePath);
        }

        // Fechamento: manifest, catálogo e resumo; só então o diário some.
        if (auto st = journal.value().recordEnd("completed"); !st)
            return st;
        if (auto st = manifest.save(m_layout.manifestFile(m_runId)); !st)
            return st;
        catalog.runId = m_runId;
        if (auto st = catalog.save(m_layout.catalogFile()); !st)
            return st;

        m_result.status = m_result.failed > 0 ? VaultRunStatus::Partial : VaultRunStatus::Success;
        writeSummary(nameOf(m_result.status), {}, startedAt);

        journal.value().close();
        ::DeleteFileW(win::toExtendedPath(m_layout.journalFile(m_runId)).c_str());
        ops::removeEmptyDirectories(m_layout.versions(m_runId), true);

        if (m_req.applyRetention && m_result.status == VaultRunStatus::Success)
            (void)applyRetention(m_layout, m_req.retention, QDateTime::currentDateTimeUtc());

        return Status::success();
    }

    // Com várias pastas, uma delas vazia não pode virar "tudo dela foi
    // excluído" (pendrive ou compartilhamento que não montou, por exemplo).
    GuardVerdict evaluateFolderGuards(const Catalog& catalog) const
    {
        if (m_roots.size() < 2)
            return {}; // uma pasta só: a guarda de origem vazia de sempre cobre
        for (const auto& root : m_roots) {
            auto belongs = [&](const std::wstring& path) {
                if (!root.prefix.empty())
                    return startsWith(path, root.prefix);
                return std::none_of(m_roots.begin(), m_roots.end(),
                                    [&](const ReadRoot& other) { return startsWith(path, other.prefix); });
            };
            std::size_t before = 0, now = 0;
            for (const auto& e : catalog.entries)
                before += belongs(e.meta.relativePath) ? 1 : 0;
            for (const auto& f : m_scan.files)
                now += belongs(f.relativePath) ? 1 : 0;
            if (before > 0 && now == 0)
                return {GuardCode::EmptySource, "a pasta " + win::toUtf8(root.label) + " está vazia, mas o último backup tinha "
                                                    + std::to_string(before) + " arquivos dela"};
        }
        return {};
    }

    std::wstring sourcePathOf(const std::wstring& relativePath) const
    {
        for (const auto& root : m_roots)
            if (startsWith(relativePath, root.prefix))
                return win::joinPath(root.readPath, win::fromCatalogPath(relativePath.substr(root.prefix.size())));
        return win::joinPath(m_roots.front().readPath, win::fromCatalogPath(relativePath));
    }

    // Copia um arquivo novo ou modificado para current/, guardando a versão
    // anterior em versions/<run>/ no último instante antes da troca.
    ManifestRecord copyIn(const FileEntry& cur, const CatalogEntry* previous)
    {
        ManifestRecord rec;
        rec.path = cur.relativePath;

        const std::wstring source = sourcePathOf(cur.relativePath);
        // Em troca de maiúsculas/minúsculas, o arquivo antigo em current/ tem o nome antigo.
        const std::wstring previousPath = previous ? previous->meta.relativePath : std::wstring();
        const std::wstring destination = m_layout.currentFile(previous ? previousPath : cur.relativePath);
        const bool hadPrevious = previous && ops::pathExists(destination);
        bool movedPrevious = false;

        CopyOptions opt;
        opt.verify = m_req.verify;
        opt.tempDirectory = m_layout.tmp();
        opt.encrypt = m_cipher.get();
        if (hadPrevious) {
            opt.beforeCommit = [&]() -> Status {
                auto st = ops::moveCurrentToVersions(m_layout, m_runId, previousPath);
                movedPrevious = st.ok();
                return st;
            };
        }

        auto copied = copyFileVerified(source, destination, opt);
        if (!copied) {
            // Se a versão anterior já saiu de current/ e a nova não entrou, devolve.
            if (movedPrevious && !ops::pathExists(destination))
                (void)ops::moveVersionBackToCurrent(m_layout, m_runId, previousPath);
            rec.op = ManifestOp::Failed;
            rec.error = copied.error().message;
            return rec;
        }

        // Troca só de maiúsculas/minúsculas: renomeia para o nome atual da origem.
        if (previous && previousPath != cur.relativePath) {
            ::MoveFileExW(win::toExtendedPath(destination).c_str(),
                          win::toExtendedPath(m_layout.currentFile(cur.relativePath)).c_str(), MOVEFILE_WRITE_THROUGH);
        }

        CatalogEntry entry;
        entry.meta = cur;
        entry.meta.size = copied.value().bytes;
        entry.meta.lastWriteTime = copied.value().sourceLastWriteTime;
        entry.sha256 = copied.value().sha256;
        entry.sinceRun = m_runId;

        rec.op = previous ? ManifestOp::Modified : ManifestOp::Added;
        rec.entry = entry;
        if (previous)
            rec.previous = *previous;
        return rec;
    }

    Status commit(Journal& journal, Manifest& manifest, Catalog& catalog, ManifestRecord rec)
    {
        switch (rec.op) {
        case ManifestOp::Added:
            ++m_result.added;
            m_result.bytesWritten += rec.entry->meta.size;
            if (rec.entry)
                catalog.upsert(*rec.entry);
            break;
        case ManifestOp::Modified:
            ++m_result.modified;
            m_result.bytesWritten += rec.entry->meta.size;
            if (rec.previous && rec.previous->meta.relativePath != rec.entry->meta.relativePath)
                catalog.remove(rec.previous->meta.relativePath);
            catalog.upsert(*rec.entry);
            break;
        case ManifestOp::Deleted:
            ++m_result.deleted;
            catalog.remove(rec.path);
            break;
        case ManifestOp::Failed:
            ++m_result.failed;
            if (m_result.fileErrors.size() < kMaxReportedFileErrors)
                m_result.fileErrors.emplace_back(rec.path, rec.error);
            break;
        case ManifestOp::Renamed:
            break; // só na reorganização do cofre (LayoutMigration)
        }
        if (auto st = journal.recordDone(rec); !st)
            return st;
        manifest.records.push_back(std::move(rec));
        return Status::success();
    }

    void writeSummary(const char* status, const QString& note, const QDateTime& startedAt = {})
    {
        RunSummary s;
        s.runId = m_runId;
        s.jobId = m_req.jobId;
        s.vaultId = m_result.vaultId;
        QStringList sources;
        for (const auto& root : m_roots)
            sources << QString::fromStdWString(root.label);
        s.source = sources.join(QStringLiteral("; "));
        s.status = QString::fromLatin1(status);
        s.startedAt = startedAt.isValid() ? startedAt : QDateTime::currentDateTimeUtc();
        s.finishedAt = QDateTime::currentDateTimeUtc();
        s.filesScanned = m_scan.files.size();
        s.added = m_result.added;
        s.modified = m_result.modified;
        s.deleted = m_result.deleted;
        s.failed = m_result.failed;
        s.unchanged = m_result.unchanged;
        s.bytesWritten = m_result.bytesWritten;
        s.note = note;
        (void)s.save(m_layout.summaryFile(m_runId));
    }

    // Id da "execução" de reorganização: logo antes deste backup e depois do
    // último registrado no catálogo, para a restauração desfazer na ordem certa.
    std::wstring migrationRunId(const std::wstring& previous) const
    {
        const QDateTime now = runIdTimestamp(m_runId);
        for (int back : {1, 0}) {
            std::wstring id = newRunId(now.addSecs(-back));
            if (back == 0)
                id = id.substr(0, 18) + L"0000";
            if ((previous.empty() || id > previous) && id < m_runId)
                return id;
        }
        return newRunId(now.addSecs(-1));
    }

    const BackupRequest& m_req;
    const std::wstring& m_runId;
    const std::vector<SourceScan>& m_scans;
    const VaultTarget& m_target;
    ScanResult m_scan;              // arquivos no formato deste cofre
    std::vector<ReadRoot> m_roots;  // pastas de origem (ou o equivalente dentro do snapshot VSS)
    vault::JobLayout m_layout;
    std::shared_ptr<const crypto::FileCipher> m_cipher; // cofre cifrado; nulo = em claro
    VaultRunResult m_result;
};

} // namespace

RunStatus aggregateRunStatus(const std::vector<VaultRunResult>& vaults, std::size_t minOk)
{
    std::size_t ok = 0;
    bool anyBlocked = false, anyPartial = false;
    for (const auto& v : vaults) {
        if (v.status == VaultRunStatus::Blocked)
            anyBlocked = true;
        if (v.status == VaultRunStatus::Success || v.status == VaultRunStatus::Partial)
            ++ok;
        if (v.status == VaultRunStatus::Partial)
            anyPartial = true;
    }
    if (anyBlocked)
        return RunStatus::Blocked;
    if (ok == 0)
        return RunStatus::Failed;
    // min_vaults_ok: quantos cofres precisam concluir para o run ser Sucesso;
    // abaixo disso (mas com pelo menos um) o run é Degradado.
    const std::size_t required = minOk == 0 ? vaults.size() : std::min(minOk, vaults.size());
    if (ok >= required && !anyPartial)
        return RunStatus::Success;
    return RunStatus::Degraded;
}

BackupResult runBackup(const BackupRequest& req)
{
    BackupResult result;
    result.runId = newRunId();

    if (!isValidJobId(req.jobId)) {
        result.error = "ID de tarefa inválido (use letras minúsculas, números e hífen)";
        return result;
    }
    if (req.vaults.empty()) {
        result.error = "nenhum cofre informado";
        return result;
    }

    auto jobLock = win::JobLock::tryAcquire(req.jobId);
    if (!jobLock) {
        result.status = jobLock.error().code == ErrorCode::AlreadyExists ? RunStatus::AlreadyRunning
                                                                         : RunStatus::Failed;
        result.error = jobLock.error().message;
        return result;
    }

    // depends_on: se uma dependência estiver rodando, espera ela terminar.
    for (const auto& dep : req.dependsOn) {
        auto depLock = win::JobLock::acquire(dep, static_cast<unsigned long>(req.dependencyWaitSeconds * 1000));
        if (!depLock) {
            result.error = "a tarefa " + win::toUtf8(dep) + " (depends_on) não terminou a tempo";
            return result;
        }
    }

    const std::map<std::wstring, std::wstring> vars = {
        {L"job", req.jobId},
        {L"run_id", result.runId},
        {L"date", QDate::currentDate().toString(Qt::ISODate).toStdWString()},
    };
    auto finishHooks = [&](BackupResult& r) {
        // after: sempre; on_success/on_failure conforme o resultado final.
        if (!runHooks(req.hooks.after, "after", vars, r.hooks) && r.status == RunStatus::Success)
            r.status = RunStatus::Degraded;
        if (r.status == RunStatus::Success)
            runHooks(req.hooks.onSuccess, "on_success", vars, r.hooks);
        else
            runHooks(req.hooks.onFailure, "on_failure", vars, r.hooks);
    };

    if (!runHooks(req.hooks.before, "before", vars, result.hooks)) {
        result.status = RunStatus::Failed;
        result.error = "um comando de hooks.before falhou; o backup não foi executado";
        runHooks(req.hooks.onFailure, "on_failure", vars, result.hooks);
        return result;
    }

    // Pastas de origem: a principal e as adicionais (<alias>/). O prefixo da
    // principal depende do layout de cada cofre (mergeScans).
    std::vector<ReadRoot> roots;
    roots.push_back({{}, req.sourceRoot, req.sourceRoot});
    for (const auto& s : req.moreSources)
        roots.push_back({win::toCatalogPath(s.alias) + L"/", s.path, s.path});

    // Snapshot VSS: cada pasta é lida de uma cópia congelada do seu volume
    // (um snapshot por volume, compartilhado pelas pastas do mesmo disco).
    std::map<std::wstring, win::VolumeSnapshot> snapshots;
    if (req.useSnapshot) {
        bool allSnapshotted = true;
        for (auto& root : roots) {
            auto volume = win::queryVolume(root.label);
            if (!volume) {
                allSnapshotted = false;
                result.snapshotNote = volume.error().message;
                continue;
            }
            const std::wstring mount = win::foldCase(volume.value().mountPoint);
            auto it = snapshots.find(mount);
            if (it == snapshots.end()) {
                auto snap = win::VolumeSnapshot::create(volume.value().mountPoint);
                if (!snap) {
                    allSnapshotted = false;
                    result.snapshotNote = snap.error().message;
                    continue;
                }
                it = snapshots.emplace(mount, std::move(snap).value()).first;
            }
            const std::wstring translated = it->second.translate(root.label);
            if (translated.empty())
                allSnapshotted = false;
            else
                root.readPath = translated;
        }
        result.snapshotUsed = !snapshots.empty();
        if (!allSnapshotted && !req.continueWithoutSnapshot) {
            result.status = RunStatus::Failed;
            result.error = "snapshot VSS indisponível: " + result.snapshotNote;
            snapshots.clear();
            finishHooks(result);
            return result;
        }
    }

    // Origem ausente nunca é interpretada como "tudo foi excluído".
    ScanOptions scanOpt;
    scanOpt.readFileIds = req.readFileIds;
    if (!req.filter.empty()) {
        const PathFilter& f = req.filter;
        scanOpt.filter = [&f](std::wstring_view p, bool isDir) { return f.accepts(p, isDir); };
    }
    std::vector<SourceScan> scans;
    for (std::size_t i = 0; i < roots.size(); ++i) {
        auto part = scanDirectory(roots[i].readPath, scanOpt);
        if (!part) {
            result.error = "origem inacessível (" + win::toUtf8(roots[i].label) + "): " + part.error().message;
            snapshots.clear();
            finishHooks(result);
            return result;
        }
        scans.push_back({roots[i], std::move(part).value(), i == 0});
    }
    // Visão "da configuração" (várias pastas = subpastas) para as guardas de
    // origem; cada cofre monta a sua conforme o layout dele.
    const ScanResult logical = mergeScans(scans, !req.moreSources.empty(), req.mainAlias).first;
    result.filesScanned = logical.files.size();
    result.bytesScanned = logical.totalBytes;
    if (req.progress)
        req.progress({ProgressKind::ScanFinished, {}, {}, logical.files.size(), logical.files.size()});

    if (!req.overrideGuards) {
        result.sourceGuard = evaluateSourceGuards(req.guards, logical);
        if (result.sourceGuard.blocked()) {
            result.status = RunStatus::Blocked;
            result.error = result.sourceGuard.message;
            finishHooks(result);
            return result;
        }
    }

    for (const auto& target : req.vaults) {
        if (req.progress)
            req.progress({ProgressKind::VaultStarted, target.location, {}, 0, 0});
        VaultRun run(req, result.runId, scans, target);
        result.vaults.push_back(run.run());
        if (req.progress)
            req.progress({ProgressKind::VaultFinished, target.location, {}, 0, 0});
    }

    result.status = aggregateRunStatus(result.vaults, req.minVaultsOk);
    snapshots.clear(); // libera os snapshots antes dos comandos "after"
    finishHooks(result);
    return result;
}

} // namespace synkro
