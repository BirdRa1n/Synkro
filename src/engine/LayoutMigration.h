#pragma once

#include "catalog/Catalog.h"
#include "core/Result.h"
#include "vault/JobLayout.h"

#include <string>
#include <vector>

namespace synkro {

// Reorganização de um cofre de tarefa de arquivos para "uma subpasta por
// pasta de origem" (configuration.md §6).
//
// Antes: a pasta principal na raiz de current\ (tarefa de uma pasta só, ou
// várias pastas na 0.9.0-beta.6, com as adicionais em <alias>\ ao lado).
// Depois: current\<principal>\... e current\<adicional>\...
//
// Nada é copiado: current\ inteiro é renomeado para dentro de uma subpasta
// nova (uma operação no disco, por maior que seja a pasta) e as adicionais são
// devolvidas para o lugar delas. O catálogo passa a usar os caminhos novos e
// um manifest de "renamed" registra a mudança, para a restauração de
// execuções antigas continuar encontrando cada arquivo (versions\ fica como
// estava).
//
// Tolera queda de energia no meio: um marcador (layout-migration.json) e um
// arquivo-sentinela dentro de current\ dizem em que passo parou, e a próxima
// execução termina o trabalho.
struct LayoutMigrationPlan {
    std::wstring mainAlias;                 // subpasta da pasta principal
    std::vector<std::wstring> extraAliases; // pastas adicionais já em <alias>\ (0.9.0-beta.6)
    std::wstring runId;                     // id da "execução" de reorganização (ordena antes do backup)
};

struct LayoutMigrationResult {
    bool migrated = false;     // houve reorganização (agora ou retomada de uma interrompida)
    std::size_t renamed = 0;   // arquivos que mudaram de caminho no catálogo
};

// Retomada obrigatória: true se há uma reorganização interrompida neste cofre.
bool layoutMigrationPending(const vault::JobLayout& layout);

// Reorganiza (ou termina uma reorganização interrompida). `catalog` é o
// catálogo atual do cofre e sai atualizado (e gravado).
Result<LayoutMigrationResult> migrateToFolderLayout(const vault::JobLayout& layout, Catalog& catalog,
                                                    const LayoutMigrationPlan& plan);

} // namespace synkro
