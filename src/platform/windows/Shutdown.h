#pragma once

#include "core/Result.h"

#include <string>

namespace synkro::win {

// Agenda o desligamento do Windows com contagem regressiva visível aos
// usuários logados (architecture.md §10.4). Pode ser cancelado com
// cancelShutdown() ou "shutdown /a" durante a contagem.
Status scheduleShutdown(unsigned long countdownSeconds, const std::wstring& message);
Status cancelShutdown();

// Variável de ambiente SYNKRO_NO_SHUTDOWN=1 desliga o recurso (testes,
// laboratório, manutenção).
bool shutdownDisabledByEnvironment();

} // namespace synkro::win
