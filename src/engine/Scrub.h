#pragma once

#include "config/Config.h"

#include <cstddef>
#include <string>
#include <vector>

namespace synkro {

struct ScrubReport {
    std::size_t checked = 0;
    std::size_t ok = 0;
    std::size_t repaired = 0;      // corrigidos a partir de outro cofre íntegro
    std::vector<std::string> problems; // pt-BR: o que continua corrompido/ausente
    std::vector<std::string> notes;
};

// Verificação periódica (architecture.md §11): para cada tarefa e cofre
// conectado, relê sem cache uma amostra dos arquivos/dumps e compara com o
// SHA-256 registrado. Com `repair`, um arquivo corrompido ou ausente é
// recuperado de outro cofre da mesma tarefa que tenha o mesmo conteúdo
// íntegro.
// `sampleRatio` em (0, 1]: 0.1 = 10% dos arquivos.
ScrubReport runScrub(const config::ConfigSet& set, double sampleRatio, bool repair);

} // namespace synkro
