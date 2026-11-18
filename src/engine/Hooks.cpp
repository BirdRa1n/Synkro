#include "engine/Hooks.h"

#include "platform/windows/Win32.h"
#include "process/ProcessRunner.h"

namespace synkro {

std::wstring expandVariables(const std::wstring& text, const std::map<std::wstring, std::wstring>& vars)
{
    std::wstring out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == L'{') {
            const size_t end = text.find(L'}', i + 1);
            if (end != std::wstring::npos) {
                const auto it = vars.find(text.substr(i + 1, end - i - 1));
                if (it != vars.end()) {
                    out += it->second;
                    i = end;
                    continue;
                }
            }
        }
        out += text[i];
    }
    return out;
}

bool runHooks(const std::vector<HookSpec>& hooks, const char* phase, const std::map<std::wstring, std::wstring>& vars,
              std::vector<HookOutcome>& outcomes)
{
    for (const auto& h : hooks) {
        HookOutcome o;
        o.phase = phase;
        o.critical = h.failOnError;

        ProcessSpec spec;
        for (const auto& a : h.args)
            spec.args.push_back(expandVariables(a, vars));
        spec.timeoutMs = h.timeoutSeconds * 1000;
        o.command = buildCommandLine(spec.args);

        auto r = runProcess(spec);
        if (!r) {
            o.message = r.error().message;
        } else if (r.value().timedOut) {
            o.message = "tempo limite de " + std::to_string(h.timeoutSeconds) + "s excedido; comando encerrado";
        } else {
            o.exitCode = r.value().exitCode;
            o.ok = r.value().exitCode == 0;
            if (!o.ok) {
                std::string tail = r.value().stderrText;
                while (!tail.empty() && (tail.back() == '\n' || tail.back() == '\r'))
                    tail.pop_back();
                if (tail.size() > 400)
                    tail = tail.substr(tail.size() - 400);
                o.message = "código de saída " + std::to_string(o.exitCode) + (tail.empty() ? "" : ": " + tail);
            }
        }

        const bool stop = !o.ok && h.failOnError;
        outcomes.push_back(std::move(o));
        if (stop)
            return false;
    }
    return true;
}

} // namespace synkro
