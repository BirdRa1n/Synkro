#pragma once

#include "core/Result.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace synkro {

struct ProcessSpec {
    // args[0] é o executável. Os argumentos são citados conforme as regras do
    // CommandLineToArgvW, então espaços e aspas chegam intactos.
    std::vector<std::wstring> args;
    std::wstring workingDirectory;

    // Variáveis acrescentadas/sobrescritas no ambiente do processo filho
    // (somente nele: segredos nunca vão para o ambiente do próprio worker).
    std::vector<std::pair<std::wstring, std::wstring>> environment;

    std::int64_t timeoutMs = 0; // 0 = sem limite

    // Arquivo entregue como entrada padrão (ex.: restauração com o cliente
    // mysql). Vazio = NUL.
    std::wstring stdinFile;

    // Se definido, recebe a saída padrão em blocos, sem acumular (dumps).
    // Uma falha interrompe o processo.
    std::function<Status(const char* data, std::size_t size)> onStdout;

    // Evento do Windows (HANDLE) que, sinalizado, encerra o processo — usado
    // por processos de longa duração como a trava no servidor de banco.
    void* cancelEvent = nullptr;

    std::size_t maxCapturedStdout = 1u << 20; // quando onStdout não é usado
    std::size_t maxCapturedStderr = 64u << 10;
};

struct ProcessResult {
    unsigned long exitCode = 0;
    bool timedOut = false;
    bool cancelled = false; // encerrado por cancelEvent
    std::string stdoutText; // vazio quando onStdout é usado
    std::string stderrText; // últimos bytes de stderr (limitado)
};

// Executa um processo dentro de um Job Object com KILL_ON_JOB_CLOSE: se o
// worker morrer ou o tempo limite estourar, toda a árvore de processos é
// encerrada. Nenhuma janela é aberta.
Result<ProcessResult> runProcess(const ProcessSpec& spec);

// Linha de comando do Windows a partir de argumentos (exposto para testes).
std::wstring buildCommandLine(const std::vector<std::wstring>& args);

} // namespace synkro
