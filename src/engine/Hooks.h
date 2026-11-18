#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace synkro {

// Comando extra configurado em `hooks` (docs/configuration.md §6).
struct HookSpec {
    std::vector<std::wstring> args;   // args[0] = executável; aceita {job}, {run_id}, {date}
    std::int64_t timeoutSeconds = 300;
    bool failOnError = true;          // on_error: fail | warn
};

struct HookSet {
    std::vector<HookSpec> before, after, onSuccess, onFailure;
    bool empty() const { return before.empty() && after.empty() && onSuccess.empty() && onFailure.empty(); }
};

struct HookOutcome {
    std::string phase;                // before | after | on_success | on_failure
    std::wstring command;
    bool ok = false;
    bool critical = false;            // falha com on_error: fail
    unsigned long exitCode = 0;
    std::string message;              // erro ou trecho final da saída de erro
};

// Substitui {chave} pelos valores (ex.: {job}, {run_id}, {date}).
std::wstring expandVariables(const std::wstring& text, const std::map<std::wstring, std::wstring>& vars);

// Executa os comandos na ordem. Devolve false se algum comando com
// on_error: fail falhou — nesse caso os seguintes da mesma fase não rodam.
bool runHooks(const std::vector<HookSpec>& hooks, const char* phase, const std::map<std::wstring, std::wstring>& vars,
              std::vector<HookOutcome>& outcomes);

} // namespace synkro
