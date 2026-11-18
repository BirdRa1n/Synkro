#pragma once

#include "catalog/Manifest.h"
#include "platform/windows/Win32.h"

#include <QByteArray>

#include <string>
#include <vector>

namespace synkro {

// Diário de operações de uma execução (runs/<run>.journal.jsonl).
//
// Cada operação concluída no cofre é anotada aqui antes de o catálogo ser
// regravado. Se a execução for interrompida (queda de energia, processo
// encerrado), o diário permite incorporar ao catálogo o que já foi feito.
// Ao fim de uma execução bem-sucedida o diário é apagado; sua presença
// indica uma execução interrompida.
//
// As linhas são acumuladas e sincronizadas em disco a cada `flushEvery`
// operações. Operações não sincronizadas que se perderem numa queda são
// seguras: a próxima execução apenas as refaz.
class Journal {
public:
    static Result<Journal> create(const std::wstring& path, const std::wstring& runId, int flushEvery = 256);

    Journal(Journal&&) noexcept = default;
    Journal& operator=(Journal&&) noexcept = default;
    ~Journal();

    Status recordDone(const ManifestRecord& record);
    Status recordEnd(const std::string& status);
    Status flush();
    void close();

private:
    Journal() = default;
    Status appendLine(const QByteArray& line);

    win::UniqueHandle m_file;
    QByteArray m_pending;
    int m_pendingCount = 0;
    int m_flushEvery = 256;
};

struct JournalContents {
    std::wstring runId;
    std::vector<ManifestRecord> done;
    bool ended = false;
    std::string endStatus;
    bool truncatedTail = false; // última linha incompleta (queda durante a gravação)
};

// Lê um diário, tolerando uma última linha incompleta.
Result<JournalContents> readJournal(const std::wstring& path);

} // namespace synkro
