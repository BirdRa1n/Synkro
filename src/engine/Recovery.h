#pragma once

#include "catalog/Catalog.h"
#include "vault/JobLayout.h"

#include <cstddef>
#include <string>
#include <vector>

namespace synkro {

struct RecoveredRun {
    std::wstring runId;
    std::size_t recordsApplied = 0;       // operações do diário incorporadas ao catálogo
    std::size_t versionsMovedBack = 0;    // arquivos devolvidos de versions/ para current/
    bool journalTruncated = false;
};

// Recupera execuções interrompidas de uma tarefa num cofre
// (architecture.md §8). Para cada runs/<run>.journal.jsonl encontrado:
//
//  1. arquivos em versions/<run>/ cujo current/ correspondente sumiu (troca
//     interrompida no meio) voltam para current/ — exceto exclusões já
//     confirmadas no diário;
//  2. operações confirmadas no diário são aplicadas ao catálogo;
//  3. grava o manifest e o resumo da execução com status "interrupted";
//  4. grava o catálogo e remove o diário.
//
// Também limpa .tmp/. `catalog` é atualizado em memória e em disco.
Result<std::vector<RecoveredRun>> recoverInterruptedRuns(const vault::JobLayout& layout, Catalog& catalog);

} // namespace synkro
