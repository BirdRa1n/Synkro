#include "config/ConfigApply.h"

#include "engine/VaultOps.h"
#include "hash/Sha256.h"
#include "platform/windows/Snapshot.h"
#include "platform/windows/Win32.h"
#include "vault/JobLayout.h"
#include "vault/VaultLocator.h"

#include <algorithm>
#include <set>

namespace synkro::config {

std::string configHash(const std::wstring& directory)
{
    Sha256 h;
    auto feed = [&](const std::wstring& rel) {
        auto data = win::readWholeFile(win::joinPath(directory, rel), 4u << 20);
        const std::string name = win::toUtf8(rel);
        (void)h.update(name.data(), name.size() + 1);
        if (data)
            (void)h.update(data.value());
    };
    feed(L"synkro.yml");
    for (const auto& f : ops::listFiles(win::joinPath(directory, L"jobs"), L".yml"))
        feed(L"jobs\\" + f);
    auto d = h.finish();
    return d ? toHex(d.value()) : std::string();
}

std::vector<win::TaskDefinition> desiredTasks(const ConfigSet& set, const std::wstring& workerExe)
{
    std::vector<win::TaskDefinition> out;
    for (const auto& job : set.jobs) {
        if (!job.enabled || job.schedule.empty())
            continue;
        win::TaskDefinition t;
        t.name = L"job-" + win::fromUtf8(job.id);
        t.description = win::fromUtf8("Synkro: " + job.name);
        t.executable = workerExe;
        t.arguments = L"backup --job " + win::fromUtf8(job.id) + L" --trigger schedule";
        t.triggers = job.schedule;
        t.startWhenAvailable = set.global.scheduleCatchUp;
        // Margem para hooks e espera por dependências.
        t.executionTimeLimitSeconds = (job.timeoutSeconds > 0 ? job.timeoutSeconds : set.global.defaultTimeoutSeconds) + 3600;
        out.push_back(std::move(t));
    }

    win::TaskDefinition m;
    m.name = L"Maintenance";
    m.description = L"Synkro: aplica a configuração, confere cofres e alertas";
    m.executable = workerExe;
    m.arguments = L"maintenance";
    ScheduleTrigger every;
    every.kind = TriggerKind::Every;
    every.intervalSeconds = 15 * 60;
    ScheduleTrigger boot;
    boot.kind = TriggerKind::OnStartup;
    boot.delaySeconds = 2 * 60;
    m.triggers = {every, boot};
    m.executionTimeLimitSeconds = 30 * 60;
    out.push_back(std::move(m));

    if (set.global.scrubSchedule) {
        win::TaskDefinition s;
        s.name = L"Scrub";
        s.description = L"Synkro: verificação periódica dos cofres (releitura sem cache)";
        s.executable = workerExe;
        s.arguments = L"maintenance --scrub";
        s.triggers = {*set.global.scrubSchedule};
        s.executionTimeLimitSeconds = 12 * 3600;
        out.push_back(std::move(s));
    }
    return out;
}

Result<ApplyReport> applyConfig(const ConfigSet& set, const ApplyOptions& options)
{
    if (set.hasErrors())
        return Error::make(ErrorCode::InvalidArgument,
                           "a configuração tem " + std::to_string(set.errorCount()) + " erro(s) e não foi aplicada");
    ApplyReport report;

    // 1. Última configuração válida.
    const std::wstring lastGood = win::joinPath(set.directory, L".lastgood");
    if (auto st = win::createDirectories(win::joinPath(lastGood, L"jobs")); !st)
        return st.error();
    auto copyText = [&](const std::wstring& rel) -> Status {
        auto data = win::readWholeFile(win::joinPath(set.directory, rel), 4u << 20);
        if (!data)
            return data.error();
        return win::writeFileAtomically(win::joinPath(lastGood, rel), data.value());
    };
    if (auto st = copyText(L"synkro.yml"); !st)
        return st.error();
    std::set<std::wstring> currentJobs;
    for (const auto& f : ops::listFiles(win::joinPath(set.directory, L"jobs"), L".yml")) {
        currentJobs.insert(f);
        if (auto st = copyText(L"jobs\\" + f); !st)
            return st.error();
    }
    for (const auto& f : ops::listFiles(win::joinPath(lastGood, L"jobs"), L".yml")) {
        if (!currentJobs.count(f))
            ::DeleteFileW(win::toExtendedPath(win::joinPath(win::joinPath(lastGood, L"jobs"), f)).c_str());
    }
    report.done.push_back("cópia de segurança da configuração em .lastgood");

    // 2. job.yml em cada cofre conectado (restauração sem o PC original).
    if (options.copyToVaults) {
        for (const auto& job : set.jobs) {
            auto text = win::readWholeFile(win::joinPath(set.directory, job.file), 4u << 20);
            if (!text)
                continue;
            for (const auto& vid : job.vaults) {
                const VaultConfig* vc = set.global.findVault(vid);
                if (!vc)
                    continue;
                auto located = vault::locateVault(QString::fromStdString(vc->vaultId), vc->path);
                if (!located) {
                    report.warnings.push_back("cofre \"" + vc->label + "\" desconectado: job.yml de " + job.id
                                              + " será enviado na próxima aplicação");
                    continue;
                }
                const vault::JobLayout layout(located->location, win::fromUtf8(job.id));
                if (win::createDirectories(layout.root()) && win::writeFileAtomically(layout.jobConfigFile(), text.value()))
                    report.done.push_back("job.yml de " + job.id + " gravado em \"" + vc->label + "\"");
            }
        }
    }

    // 3. Agendador de Tarefas.
    if (options.registerTasks) {
        if (!win::isProcessElevated())
            return Error::make(ErrorCode::AccessDenied,
                               "para registrar as tarefas no Agendador do Windows execute como administrador "
                               "(o restante já foi aplicado)");
        const auto desired = desiredTasks(set, options.workerExe);
        std::set<std::wstring> names;
        for (const auto& t : desired) {
            if (auto st = win::registerTask(t); !st)
                return st.error();
            names.insert(win::foldCase(t.name));
            report.done.push_back("tarefa \\Synkro\\" + win::toUtf8(t.name) + " registrada");
        }
        if (auto existing = win::listTasks()) {
            for (const auto& name : existing.value()) {
                const bool ours = name.rfind(L"job-", 0) == 0 || name == L"Maintenance" || name == L"Scrub";
                if (ours && !names.count(win::foldCase(name)) && win::deleteTask(name))
                    report.done.push_back("tarefa antiga \\Synkro\\" + win::toUtf8(name) + " removida");
            }
        }
    }
    return report;
}

} // namespace synkro::config
