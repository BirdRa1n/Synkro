#pragma once

#include "platform/windows/Volume.h"

#include <cstdint>
#include <string>
#include <vector>

namespace synkro::vault {

// Resultado de cada verificação feita ao adicionar um disco como cofre
// (architecture.md §3.1).
enum class CheckSeverity { Ok, Info, Warning, Blocking };

enum class CheckId {
    VolumeReadable,
    DifferentDiskFromSources,
    DifferentDiskFromOtherVaults,
    NotSystemVolume,
    FileSystem,
    FreeSpace,
    WriteTest,
    Removable,
    ExistingVault,
    DiskHealth,
};

struct CheckFinding {
    CheckId id;
    CheckSeverity severity;
    std::string title;   // pt-BR, curto
    std::string detail;  // pt-BR, explicação para o usuário
};

struct CandidateRequest {
    std::wstring candidatePath;              // ex.: "E:\"
    std::wstring vaultLocation;              // onde o cofre ficará; vazio = raiz do volume
    std::vector<std::wstring> sourcePaths;   // origens das tarefas que usarão o cofre
    std::vector<std::wstring> otherVaults;   // outros cofres da mesma tarefa
    std::uint64_t estimatedSourceBytes = 0;  // 0 = pular checagem de espaço
    double spaceFactor = 1.5;                // espaço necessário = origem × fator
    bool runWriteTest = true;
};

struct CandidateReport {
    win::VolumeInfo volume;
    std::vector<CheckFinding> findings;

    bool accepted() const noexcept;      // nenhum item Blocking
    bool hasWarnings() const noexcept;   // exige confirmação do usuário
};

// Executa todas as verificações. Nunca grava nada além do arquivo de teste,
// que é removido em seguida.
CandidateReport checkCandidate(const CandidateRequest& request);

const char* nameOf(CheckSeverity severity) noexcept;

} // namespace synkro::vault
