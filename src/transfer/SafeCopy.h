#pragma once

#include "core/Result.h"
#include "hash/Sha256.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace synkro {

enum class VerifyMode {
    // O SHA-256 é calculado durante a leitura da origem e o tamanho gravado é
    // conferido. Rápido; padrão para arquivos comuns.
    HashOnCopy,
    // Além disso, o arquivo gravado é relido SEM o cache do Windows
    // (FILE_FLAG_NO_BUFFERING) e o hash é recalculado. Detecta falhas de
    // gravação no disco; padrão para bancos de dados.
    RereadUncached,
};

struct CopyOptions {
    VerifyMode verify = VerifyMode::HashOnCopy;

    // Pasta para o arquivo temporário. Deve estar NO MESMO VOLUME do destino
    // para que a troca final seja um rename atômico (no cofre: jobs\<id>\.tmp).
    // Vazio = pasta do próprio destino.
    std::wstring tempDirectory;

    bool preserveTimestamps = true;
    bool preserveAttributes = true;
    std::size_t bufferSize = 1u << 20;

    // Chamado depois que o temporário foi gravado e verificado, imediatamente
    // antes da troca final. O motor usa este ponto para mover a versão
    // anterior para versions/<run>/, reduzindo ao mínimo a janela em que
    // current/ fica sem o arquivo. Uma falha aqui cancela a cópia.
    std::function<Status()> beforeCommit;
};

struct CopyOutcome {
    std::uint64_t bytes = 0;
    Sha256Digest sha256{};
    std::uint64_t sourceLastWriteTime = 0;
};

// Copia `source` para `destination` com a sequência segura do Synkro:
//
//   1. lê a origem em blocos, calculando o SHA-256 enquanto grava num
//      temporário;
//   2. confere que a origem não mudou durante a cópia (tamanho e mtime);
//   3. preserva datas, força a gravação em disco (FlushFileBuffers);
//   4. verifica o temporário conforme `verify`;
//   5. renomeia o temporário sobre o destino (MoveFileExW REPLACE_EXISTING |
//      WRITE_THROUGH).
//
// Em qualquer falha o temporário é removido e o destino anterior permanece
// intacto. O destino nunca existe pela metade.
Result<CopyOutcome> copyFileVerified(const std::wstring& source,
                                     const std::wstring& destination,
                                     const CopyOptions& options = {});

} // namespace synkro
