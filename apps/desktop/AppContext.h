#pragma once

#include "config/Config.h"
#include "core/Result.h"
#include "state/Alerts.h"
#include "state/StateStore.h"

#include <QObject>
#include <QString>

#include <memory>
#include <optional>
#include <string>
#include <vector>

// Dados compartilhados pelas telas: configuração, estado local, cofres
// observados e alertas. Recarregado periodicamente e após cada alteração.
class AppContext : public QObject {
    Q_OBJECT
public:
    static AppContext& instance();

    const std::wstring& configDir() const { return m_configDir; }
    const synkro::config::ConfigSet& config() const { return m_config; }
    synkro::StateStore* state() { return m_state ? &*m_state : nullptr; }
    const std::vector<synkro::VaultObservation>& vaults() const { return m_vaults; }
    const std::vector<synkro::AlertRow>& alerts() const { return m_alerts; }
    QString stateError() const { return m_stateError; }

    // Pior situação atual: 0 = tudo em dia, 1 = atenção, 2 = crítico.
    int severityLevel() const;

    void reload();

    // Caminho do synkro-worker.exe (ao lado do desktop).
    QString workerPath() const;

    // Grava um arquivo da configuração (relativo à pasta de configuração).
    synkro::Status writeConfigFile(const std::wstring& relative, const std::string& text);
    std::string readConfigFile(const std::wstring& relative) const;

    // Executa o worker elevado (UAC) e espera terminar. Para `config apply`.
    synkro::Result<unsigned long> runWorkerElevated(const QStringList& args);

signals:
    void reloaded();

private:
    AppContext();

    std::wstring m_configDir;
    synkro::config::ConfigSet m_config;
    std::optional<synkro::StateStore> m_state;
    QString m_stateError;
    std::vector<synkro::VaultObservation> m_vaults;
    std::vector<synkro::AlertRow> m_alerts;
};
