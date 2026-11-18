#pragma once

#include "config/Config.h"

#include <string>

namespace synkro::config {

// Geração dos YAMLs pelo desktop (docs/configuration.md §3.1).
//
// Arquivos gerados começam com um cabeçalho que inclui o hash do conteúdo.
// Se o usuário editar o arquivo à mão, o hash deixa de conferir e o desktop
// passa a abrir o arquivo no editor YAML em vez de regravá-lo, para nunca
// apagar comentários ou ajustes manuais.

std::string emitGlobal(const GlobalConfig& g);
std::string emitJob(const JobConfig& j);

// true se o texto foi gerado pelo desktop e não foi alterado depois.
bool isPristineGenerated(const std::string& text);

// Duração no formato da configuração ("90d", "2h", "10m", "30s").
std::string formatDuration(std::int64_t seconds);

} // namespace synkro::config
