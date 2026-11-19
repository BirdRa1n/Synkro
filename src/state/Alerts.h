#pragma once

#include "config/Config.h"
#include "state/StateStore.h"

#include <QDateTime>

#include <string>
#include <vector>

namespace synkro {

struct AlertEvaluation {
    std::vector<AlertRow> active;   // alertas que devem estar abertos agora
    std::vector<AlertRow> opened;   // novos nesta avaliação (vão para o Visualizador de Eventos)
    std::vector<AlertRow> resolved;
};

// Situação de um cofre no momento da avaliação (localizado ou não).
struct VaultObservation {
    std::string id;                 // id na configuração
    QString vaultId;
    bool connected = false;
    std::wstring location;
    std::uint64_t freeBytes = 0, totalBytes = 0;
};

// Calcula os alertas de architecture.md §12 a partir da configuração, do
// histórico e da situação dos cofres, sem efeitos colaterais.
std::vector<AlertRow> computeAlerts(const config::ConfigSet& set, StateStore& state,
                                    const std::vector<VaultObservation>& vaults, const QDateTime& nowUtc);

// Aplica o resultado no banco de estado (abre novos, resolve os que sumiram).
Result<AlertEvaluation> reconcileAlerts(StateStore& state, const std::vector<AlertRow>& desired);

// Localiza os cofres declarados e atualiza vaults_seen para os conectados.
std::vector<VaultObservation> observeVaults(const config::ConfigSet& set, StateStore& state);

// Registra um alerta no log "Application" do Windows (origem "Synkro").
void reportToEventLog(const AlertRow& alert, bool resolved = false);

// Ciclo completo usado pelo worker e pelo desktop: observa os cofres, calcula
// os alertas (inclusive o da última verificação periódica), grava no estado e
// envia novidades ao Visualizador de Eventos. Devolve os alertas ativos.
std::vector<AlertRow> refreshAlerts(const config::ConfigSet& set, StateStore& state,
                                    std::vector<VaultObservation>* observed = nullptr);

} // namespace synkro
