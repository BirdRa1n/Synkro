#include "state/Alerts.h"

#include "config/Values.h"
#include "platform/windows/Volume.h"
#include "platform/windows/Win32.h"
#include "vault/VaultLocator.h"

#include <set>

namespace synkro {

namespace {

std::string ago(const QDateTime& when, const QDateTime& now)
{
    const qint64 s = when.secsTo(now);
    return config::describeDuration(s >= 86400 ? s / 86400 * 86400 : s >= 3600 ? s / 3600 * 3600 : std::max<qint64>(s / 60, 1) * 60);
}

std::string percent(double r)
{
    return config::describePercent(r);
}

} // namespace

std::vector<VaultObservation> observeVaults(const config::ConfigSet& set, StateStore& state)
{
    std::vector<VaultObservation> out;
    for (const auto& v : set.global.vaults) {
        VaultObservation o;
        o.id = v.id;
        o.vaultId = QString::fromStdString(v.vaultId);
        if (auto located = vault::locateVault(o.vaultId, v.path)) {
            o.connected = true;
            o.location = located->location;
            if (auto vol = win::queryVolume(o.location)) {
                o.freeBytes = vol.value().freeBytes;
                o.totalBytes = vol.value().totalBytes;
            }
            (void)state.recordVaultSeen(o.vaultId, o.location, o.freeBytes, o.totalBytes, false);
        }
        out.push_back(std::move(o));
    }
    return out;
}

std::vector<AlertRow> computeAlerts(const config::ConfigSet& set, StateStore& state,
                                    const std::vector<VaultObservation>& vaults, const QDateTime& now)
{
    std::vector<AlertRow> alerts;
    auto add = [&](std::string code, std::string subject, AlertSeverity sev, std::string msg) {
        alerts.push_back({std::move(code), std::move(subject), sev, std::move(msg), {}, {}});
    };

    // Configuração inválida também é alerta: o Synkro segue com a última válida.
    if (set.hasErrors())
        add("config_invalid", "config", AlertSeverity::Critical,
            "a configuração tem " + std::to_string(set.errorCount())
                + " erro(s); o Synkro continua usando a última configuração válida (synkro-worker config validate)");

    for (const auto& job : set.jobs) {
        if (!job.enabled || job.schedule.empty())
            continue; // tarefas manuais não têm expectativa de horário
        const std::wstring jobId = win::fromUtf8(job.id);
        const auto last = state.lastRun(jobId);
        const auto success = state.lastSuccess(jobId);

        if (success) {
            if (success->finishedAt.secsTo(now) > set.global.alertNoSuccessForSeconds)
                add("no_success", job.id, AlertSeverity::Critical,
                    "\"" + job.name + "\" não tem backup bem-sucedido há " + ago(success->finishedAt, now));
        } else if (last) {
            add("no_success", job.id, AlertSeverity::Critical, "\"" + job.name + "\" nunca terminou com sucesso");
        } else {
            add("never_run", job.id, AlertSeverity::Warning, "\"" + job.name + "\" ainda não foi executada");
        }

        if (last) {
            if (last->status == "failed")
                add("last_run_failed", job.id, AlertSeverity::Critical,
                    "a última execução de \"" + job.name + "\" falhou" + (last->error.empty() ? "" : ": " + last->error));
            else if (last->status == "blocked")
                add("last_run_blocked", job.id, AlertSeverity::Critical,
                    "a última execução de \"" + job.name + "\" foi bloqueada por uma proteção; revise a origem");
            else if (last->status == "degraded")
                add("last_run_degraded", job.id, AlertSeverity::Warning,
                    "a última execução de \"" + job.name + "\" não gravou em todos os cofres");
        }
    }

    for (const auto& o : vaults) {
        const config::VaultConfig* vc = set.global.findVault(o.id);
        if (!vc)
            continue;
        if (o.connected) {
            if (o.totalBytes > 0) {
                const double free = static_cast<double>(o.freeBytes) / static_cast<double>(o.totalBytes);
                if (free < vc->freeSpaceAlert)
                    add("low_space", o.id, free < vc->freeSpaceAlert / 2 ? AlertSeverity::Critical : AlertSeverity::Warning,
                        "o cofre \"" + vc->label + "\" está com " + percent(free) + " livre (limite "
                            + percent(vc->freeSpaceAlert) + ")");
            }
            continue;
        }
        const auto seen = state.vaultSeen(o.vaultId);
        const QDateTime lastSeen = seen ? seen->lastSeenAt : QDateTime();
        if (!lastSeen.isValid())
            add("vault_absent", o.id, AlertSeverity::Warning, "o cofre \"" + vc->label + "\" nunca foi conectado a esta máquina");
        else if (lastSeen.secsTo(now) > vc->maxAbsenceSeconds)
            add("vault_absent", o.id, vc->rotation ? AlertSeverity::Warning : AlertSeverity::Critical,
                "o cofre \"" + vc->label + "\" não é conectado há " + ago(lastSeen, now) + " (limite "
                    + config::describeDuration(vc->maxAbsenceSeconds) + ")");
    }
    return alerts;
}

Result<AlertEvaluation> reconcileAlerts(StateStore& state, const std::vector<AlertRow>& desired)
{
    AlertEvaluation ev;
    std::set<std::pair<std::string, std::string>> keys;
    for (const auto& a : desired) {
        keys.insert({a.code, a.subject});
        auto isNew = state.raiseAlert(a);
        if (!isNew)
            return isNew.error();
        if (isNew.value())
            ev.opened.push_back(a);
        ev.active.push_back(a);
    }
    for (const auto& open : state.openAlerts()) {
        if (keys.count({open.code, open.subject}))
            continue;
        if (auto st = state.resolveAlert(open.code, open.subject); !st)
            return st.error();
        ev.resolved.push_back(open);
    }
    return ev;
}

std::vector<AlertRow> refreshAlerts(const config::ConfigSet& set, StateStore& state, std::vector<VaultObservation>* observed)
{
    const auto vaults = observeVaults(set, state);
    if (observed)
        *observed = vaults;
    auto desired = computeAlerts(set, state, vaults, QDateTime::currentDateTimeUtc());
    const std::string scrub = state.getValue("scrub_problems");
    if (!scrub.empty())
        desired.push_back({"scrub_problems", "cofres", AlertSeverity::Critical, scrub, {}, {}});
    auto ev = reconcileAlerts(state, desired);
    if (!ev)
        return desired;
    if (set.global.alertEventLog) {
        for (const auto& a : ev.value().opened)
            reportToEventLog(a);
        for (const auto& a : ev.value().resolved)
            reportToEventLog(a, true);
    }
    return state.openAlerts(); // com as datas de abertura
}

void reportToEventLog(const AlertRow& alert, bool resolved)
{
    HANDLE source = ::RegisterEventSourceW(nullptr, L"Synkro");
    if (!source)
        return;
    const std::wstring text = win::fromUtf8((resolved ? "RESOLVIDO: " : "") + alert.message + " [" + alert.code + "/"
                                            + alert.subject + "]");
    const wchar_t* strings[] = {text.c_str()};
    const WORD type = resolved ? EVENTLOG_INFORMATION_TYPE
                               : (alert.severity == AlertSeverity::Critical ? EVENTLOG_ERROR_TYPE : EVENTLOG_WARNING_TYPE);
    // ID 1000; a mensagem completa vai como string de inserção.
    ::ReportEventW(source, type, 0, 1000, nullptr, 1, 0, strings, nullptr);
    ::DeregisterEventSource(source);
}

} // namespace synkro
