#pragma once

#include "config/Config.h"

#include <string>

namespace synkro::config {

// Descrição em linguagem simples (pt-BR) do que uma tarefa vai fazer —
// saída de `synkro-worker config plan` e do painel "O que esta tarefa vai
// fazer" do desktop (docs/configuration.md §11).
std::string describeJob(const ConfigSet& set, const JobConfig& job);

std::string describeTrigger(const ScheduleTrigger& t);

} // namespace synkro::config
