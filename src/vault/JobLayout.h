#pragma once

#include "platform/windows/Win32.h"
#include "vault/Vault.h"

#include <string>

namespace synkro::vault {

// Caminhos de uma tarefa dentro de um cofre (architecture.md §4):
//
//   X:/SynkroVault/jobs/<job-id>/
//       job.yml                configuração da tarefa (sem segredos)
//       catalog.jsonl(.sha256) estado da última execução bem-sucedida
//       current/               espelho mais recente da origem
//       versions/<run-id>/     versões substituídas e arquivos excluídos
//       runs/<run-id>.*        manifest, diário e resumo de cada execução
//       .tmp/                  arquivos em gravação
class JobLayout {
public:
    JobLayout() = default;
    JobLayout(const std::wstring& vaultLocation, const std::wstring& jobId)
        : m_jobId(jobId)
        , m_location(vaultLocation)
        , m_root(win::joinPath(win::joinPath(vaultRoot(vaultLocation), L"jobs"), jobId))
    {
    }

    const std::wstring& jobId() const { return m_jobId; }
    const std::wstring& vaultLocation() const { return m_location; }
    const std::wstring& root() const { return m_root; }

    std::wstring current() const { return win::joinPath(m_root, L"current"); }
    std::wstring versionsRoot() const { return win::joinPath(m_root, L"versions"); }
    std::wstring versions(const std::wstring& runId) const { return win::joinPath(versionsRoot(), runId); }
    std::wstring runs() const { return win::joinPath(m_root, L"runs"); }
    std::wstring tmp() const { return win::joinPath(m_root, L".tmp"); }

    std::wstring catalogFile() const { return win::joinPath(m_root, L"catalog.jsonl"); }
    std::wstring jobConfigFile() const { return win::joinPath(m_root, L"job.yml"); }
    std::wstring lockFile() const { return win::joinPath(m_root, L"job.lock"); }

    std::wstring manifestFile(const std::wstring& runId) const { return win::joinPath(runs(), runId + L".manifest.jsonl"); }
    std::wstring journalFile(const std::wstring& runId) const { return win::joinPath(runs(), runId + L".journal.jsonl"); }
    std::wstring summaryFile(const std::wstring& runId) const { return win::joinPath(runs(), runId + L".summary.json"); }

    // Arquivo dentro de current\ ou versions\<run>\ a partir do caminho do catálogo.
    std::wstring currentFile(const std::wstring& catalogPath) const
    {
        return win::joinPath(current(), win::fromCatalogPath(catalogPath));
    }
    std::wstring versionFile(const std::wstring& runId, const std::wstring& catalogPath) const
    {
        return win::joinPath(versions(runId), win::fromCatalogPath(catalogPath));
    }

private:
    std::wstring m_jobId;
    std::wstring m_location;
    std::wstring m_root;
};

} // namespace synkro::vault
