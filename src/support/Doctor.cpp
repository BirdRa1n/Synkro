#include "support/Doctor.h"

#include "config/ConfigApply.h"
#include "config/ConfigLoader.h"
#include "config/Values.h"
#include "core/Paths.h"
#include "core/Version.h"
#include "crypto/VaultKey.h"
#include "database/Drivers.h"
#include "database/Tools.h"
#include "engine/VaultOps.h"
#include "platform/windows/Snapshot.h"
#include "platform/windows/TaskScheduler.h"
#include "platform/windows/Win32.h"
#include "secrets/SecretStore.h"
#include "state/Alerts.h"
#include "state/StateStore.h"

#include <algorithm>
#include <set>

namespace synkro::support {

namespace {

class Report {
public:
    explicit Report(std::vector<DoctorCheck>& out) : m_out(out) {}
    void area(std::string a) { m_area = std::move(a); }
    void ok(std::string t, std::string d = {}) { add(CheckLevel::Ok, std::move(t), std::move(d)); }
    void info(std::string t, std::string d = {}) { add(CheckLevel::Info, std::move(t), std::move(d)); }
    void warn(std::string t, std::string d = {}) { add(CheckLevel::Warn, std::move(t), std::move(d)); }
    void fail(std::string t, std::string d = {}) { add(CheckLevel::Fail, std::move(t), std::move(d)); }

private:
    void add(CheckLevel l, std::string t, std::string d) { m_out.push_back({m_area, l, std::move(t), std::move(d)}); }
    std::vector<DoctorCheck>& m_out;
    std::string m_area;
};

bool eventSourceRegistered()
{
    HKEY key = nullptr;
    const LONG rc = ::RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\EventLog\\Application\\Synkro", 0,
                                    KEY_READ, &key);
    if (rc == ERROR_SUCCESS)
        ::RegCloseKey(key);
    return rc == ERROR_SUCCESS;
}

// Ferramenta principal do dump de cada motor.
const char* dumpTool(const std::string& engine)
{
    if (engine == "postgresql")
        return "pg_dump";
    if (engine == "mysql" || engine == "mariadb")
        return "mysqldump";
    if (engine == "firebird")
        return "gbak";
    if (engine == "sqlserver")
        return "sqlcmd";
    return nullptr;
}

std::string gb(std::uint64_t bytes)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f GB", static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
    return buf;
}

} // namespace

const char* nameOf(CheckLevel level) noexcept
{
    switch (level) {
    case CheckLevel::Ok:   return "ok";
    case CheckLevel::Info: return "info";
    case CheckLevel::Warn: return "aviso";
    case CheckLevel::Fail: return "FALHA";
    }
    return "?";
}

CheckLevel worst(const std::vector<DoctorCheck>& checks)
{
    CheckLevel w = CheckLevel::Ok;
    for (const auto& c : checks)
        w = std::max(w, c.level);
    return w;
}

std::vector<DoctorCheck> runDoctor(const config::ConfigSet& set, const DoctorOptions& opt)
{
    std::vector<DoctorCheck> out;
    Report r(out);
    const bool elevated = win::isProcessElevated();
    const std::wstring secretsPath = opt.secretsFile.empty() ? secretsFile() : opt.secretsFile;

    // ---- Instalação ------------------------------------------------------------
    r.area("Instalação");
    r.info(std::string("Synkro ") + versionString(), win::toUtf8(dataDirectory()));
    if (elevated)
        r.ok("Executando como administrador", "todas as checagens são possíveis");
    else
        r.warn("Executando sem administrador",
               "segredos e tarefas do Agendador não podem ser conferidos; rode o doctor num terminal como administrador");
    auto state = StateStore::open(stateDatabaseFile());
    if (state)
        r.ok("Banco de estado acessível");
    else
        r.fail("Banco de estado inacessível", state.error().message + " (permissões da pasta state\\; reinstale com install.ps1)");
    if (eventSourceRegistered())
        r.ok("Origem \"Synkro\" no Visualizador de Eventos");
    else
        r.warn("Origem \"Synkro\" não registrada no Visualizador de Eventos", "o instalador registra; rode install.ps1");

    // ---- Configuração ----------------------------------------------------------
    r.area("Configuração");
    if (set.hasErrors()) {
        std::string first;
        int n = 0;
        for (const auto& d : set.diagnostics) {
            if (d.severity != config::Severity::Error || n++ >= 3)
                continue;
            first += (first.empty() ? "" : "; ") + config::formatDiagnostic(d);
        }
        r.fail(std::to_string(set.errorCount()) + " erro(s) na configuração", first);
    } else {
        r.ok("Configuração válida", std::to_string(set.jobs.size()) + " tarefa(s), " + std::to_string(set.global.vaults.size())
                                        + " cofre(s)");
    }
    if (set.warningCount() > 0)
        r.info(std::to_string(set.warningCount()) + " aviso(s) na configuração", "synkro-worker config validate mostra cada um");

    // Segredos referenciados (só os nomes; os valores nunca são lidos aqui).
    std::set<std::string> needed;
    for (const auto& s : set.global.secrets)
        needed.insert(s.name);
    for (const auto& j : set.jobs)
        for (const auto& s : j.secrets)
            needed.insert(s.name);
    if (!needed.empty()) {
        auto names = SecretStore(secretsPath).names();
        if (!names) {
            r.warn("Segredos não conferidos", "sem acesso ao secrets.dat (" + names.error().message + "); rode como administrador");
        } else {
            std::string missing;
            for (const auto& n : needed)
                if (std::find(names.value().begin(), names.value().end(), n) == names.value().end())
                    missing += (missing.empty() ? "" : ", ") + n;
            if (missing.empty())
                r.ok("Segredos cadastrados", std::to_string(needed.size()) + " segredo(s)");
            else
                r.fail("Segredos não cadastrados: " + missing, "synkro-worker secret set <nome> (como administrador)");
        }
    }

    // ---- Agendador ---------------------------------------------------------------
    if (opt.checkScheduler) {
        r.area("Agendador");
        const auto desired = config::desiredTasks(set, opt.workerExe);
        auto existing = win::listTasks();
        if (!existing) {
            r.warn("Tarefas do Agendador não conferidas", existing.error().message + " (rode como administrador)");
        } else {
            std::set<std::wstring> have;
            for (const auto& n : existing.value())
                have.insert(win::foldCase(n));
            std::string missing;
            for (const auto& t : desired)
                if (!have.count(win::foldCase(t.name)))
                    missing += (missing.empty() ? "" : ", ") + win::toUtf8(t.name);
            if (missing.empty())
                r.ok("Tarefas do Agendador registradas", std::to_string(desired.size()) + " tarefa(s) em \\Synkro");
            else
                r.fail("Tarefas ausentes no Agendador: " + missing,
                       "synkro-worker config apply (como administrador) ou Configurações → Aplicar e agendar");
        }
    }

    // ---- Cofres ------------------------------------------------------------------
    r.area("Cofres");
    if (set.global.vaults.empty())
        r.fail("Nenhum cofre cadastrado", "Cofres → Adicionar cofre");
    std::vector<VaultObservation> observed;
    if (state)
        observed = observeVaults(set, state.value());
    for (const auto& o : observed) {
        const config::VaultConfig* vc = set.global.findVault(o.id);
        if (!vc)
            continue;
        const std::string name = "\"" + vc->label + "\"";
        if (!o.connected) {
            if (vc->rotation)
                r.info(name + " guardado fora (rodízio)", "confira no dia da troca: o backup só grava nele conectado");
            else
                r.fail(name + " inacessível", o.unavailableReason.empty() ? "conecte o disco" : o.unavailableReason);
            continue;
        }
        std::string detail = win::toUtf8(o.location);
        if (o.totalBytes > 0)
            detail += " · " + gb(o.freeBytes) + " livres de " + gb(o.totalBytes);
        const double free = o.totalBytes ? static_cast<double>(o.freeBytes) / static_cast<double>(o.totalBytes) : 1.0;
        if (free < vc->freeSpaceAlert)
            r.warn(name + " com pouco espaço", detail);
        else
            r.ok(name + " conectado", detail);
        if (o.health >= 1)
            (o.health == 2 ? r.fail(name + ": disco com defeito", o.healthText)
                           : r.warn(name + ": disco com sinais de falha", o.healthText));
        if (crypto::isEncrypted(o.location)) {
            auto cipher = crypto::cipherFor(o.location, opt.secretsFile);
            if (cipher)
                r.ok(name + " cifrado, chave disponível nesta máquina");
            else if (cipher.error().code == ErrorCode::KeyRequired && !elevated)
                r.warn(name + " cifrado: chave não conferida", "rode como administrador");
            else
                r.fail(name + " cifrado sem a chave nesta máquina", "synkro-worker vault unlock " + win::toUtf8(o.location));
        }
    }

    // ---- Tarefas -----------------------------------------------------------------
    r.area("Tarefas");
    if (set.jobs.empty())
        r.fail("Nenhuma tarefa configurada", "Tarefas → Nova tarefa");
    for (const auto& j : set.jobs) {
        const std::string name = "\"" + j.name + "\"";
        if (!j.enabled) {
            r.info(name + " desativada");
            continue;
        }
        if (j.vaults.empty())
            r.fail(name + " sem cofre");
        else if (j.vaults.size() == 1)
            r.warn(name + " grava em um único cofre", "sem redundância: um disco com defeito perde o backup; acrescente outro cofre");
        if (j.schedule.empty())
            r.warn(name + " sem agendamento", "só roda quando alguém mandar; defina um horário");

        if (j.type == "files") {
            if (!ops::pathExists(j.files.source))
                r.fail(name + ": a origem não existe", win::toUtf8(j.files.source));
            if (j.files.vss && !elevated)
                r.info(name + ": snapshot VSS só como administrador/SYSTEM", "o worker agendado roda como SYSTEM");
        } else if (j.type == "database") {
            const auto& d = j.database;
            if (d.engine == "sqlite") {
                if (db::expandSqliteFiles(d.sqliteFiles).empty())
                    r.fail(name + ": nenhum arquivo SQLite encontrado");
            } else if (const char* tool = dumpTool(d.engine)) {
                auto path = db::locateTool(tool, d.tools);
                if (!path && d.engine == "mariadb")
                    path = db::locateTool("mariadb-dump", d.tools);
                if (path)
                    r.ok(name + ": " + tool + " encontrado", db::toolVersion(path.value(),
                                                                                std::string(tool) == "gbak" ? L"-z"
                                                                                : std::string(tool) == "sqlcmd" ? L"-?"
                                                                                                                 : L"--version"));
                else
                    r.fail(name + ": " + tool + " não encontrado",
                           "instale as ferramentas cliente do banco ou informe o caminho em tools:");
            }
            if (!d.restoreTest)
                r.info(name + ": sem teste de restauração automático",
                       "recomendado no piloto: restore_test: { enabled: true } (database-backup.md §9.1)");
        }

        if (state) {
            const auto ok = state.value().lastSuccess(win::fromUtf8(j.id));
            const auto last = state.value().lastRun(win::fromUtf8(j.id));
            if (ok)
                r.ok(name + ": último sucesso em " + ok->finishedAt.toLocalTime().toString(QStringLiteral("dd/MM/yyyy HH:mm")).toStdString());
            else if (last)
                r.warn(name + ": ainda sem execução bem-sucedida", "última: " + last->status + (last->error.empty() ? "" : " — " + last->error));
            else
                r.info(name + ": ainda não executada", "rode uma vez agora: synkro-worker backup --job " + j.id);
        }
    }

    // ---- Notificações ------------------------------------------------------------
    r.area("Notificações");
    const auto& ch = set.global.notifications.channels;
    if (ch.empty()) {
        r.warn("Nenhum canal de notificação",
               "sem e-mail/webhook/Telegram, uma falha só aparece neste PDV; configure notifications (configuration.md §5.1)");
    } else {
        r.ok(std::to_string(ch.size()) + " canal(is) de notificação", "teste com: synkro-worker notify test");
        if (std::none_of(ch.begin(), ch.end(), [](const config::NotificationChannel& c) { return c.dailyReportAt >= 0; }))
            r.info("Sem relatório diário", "recomendado no piloto: daily_report: \"08:00\" (sinal de vida do PDV)");
    }
    if (state) {
        if (const auto failing = state.value().failingChannels(3); !failing.empty())
            r.warn("Canal de notificação falhando: " + failing.front().first, failing.front().second);
    }
    return out;
}

} // namespace synkro::support
