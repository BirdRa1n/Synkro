#pragma once

#include "core/Result.h"

#include <string>

namespace synkro::win {

// Agenda o desligamento do Windows com contagem regressiva visível aos
// usuários logados (architecture.md §10.4). Pode ser cancelado com
// cancelShutdown() ou "shutdown /a" durante a contagem.
// `forceApps`: fecha os programas abertos ao fim da contagem em vez de esperar
// que cada um aceite (SHUTDOWN_FORCE_OTHERS, como `shutdown /s /t`).
Status scheduleShutdown(unsigned long countdownSeconds, const std::wstring& message, bool forceApps = true);
Status cancelShutdown();

// Variável de ambiente SYNKRO_NO_SHUTDOWN=1 desliga o recurso (testes,
// laboratório, manutenção).
bool shutdownDisabledByEnvironment();

} // namespace synkro::win
