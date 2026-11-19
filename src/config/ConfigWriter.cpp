#include "config/ConfigWriter.h"

#include "config/Values.h"
#include "hash/Sha256.h"
#include "platform/windows/Win32.h"

#include <cstdio>
#include <sstream>

namespace synkro::config {

namespace {

constexpr char kMarker[] = "# Gerado pelo Synkro Desktop. Edições manuais são preservadas: depois de editar\n"
                           "# este arquivo, o desktop passa a abri-lo no editor YAML em vez de regravá-lo.\n";
constexpr char kHashPrefix[] = "# synkro-desktop-sha256: ";

std::string quote(const std::string& s)
{
    std::string o = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\')
            o += '\\';
        o += c;
    }
    return o + "\"";
}

std::string quote(const std::wstring& s)
{
    std::string utf8 = win::toUtf8(s);
    for (auto& c : utf8) {
        if (c == '\\')
            c = '/'; // caminhos com '/' dispensam escape e são aceitos pelo Synkro
    }
    return quote(utf8);
}

std::string list(const std::vector<std::string>& v)
{
    std::string o = "[";
    for (size_t i = 0; i < v.size(); ++i)
        o += (i ? ", " : "") + v[i];
    return o + "]";
}

std::string quotedList(const std::vector<std::string>& v)
{
    std::string o = "[";
    for (size_t i = 0; i < v.size(); ++i)
        o += (i ? ", " : "") + quote(v[i]);
    return o + "]";
}

std::string percent(double r)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%g%%", r * 100.0);
    return buf;
}

std::string days(const std::vector<int>& d)
{
    static const char* names[] = {"mon", "tue", "wed", "thu", "fri", "sat", "sun"};
    std::vector<std::string> out;
    for (int x : d)
        out.push_back(names[x - 1]);
    return list(out);
}

std::string trigger(const ScheduleTrigger& t)
{
    const std::string at = quote(describeTime(t.atMinutes));
    switch (t.kind) {
    case TriggerKind::Daily:
        return "{ daily: " + at + " }";
    case TriggerKind::Weekly:
        return "{ weekly: { days: " + days(t.days) + ", at: " + at + " } }";
    case TriggerKind::Monthly:
        return "{ monthly: { day: " + (t.dayOfMonth < 0 ? std::string("last") : std::to_string(t.dayOfMonth)) + ", at: " + at
               + " } }";
    case TriggerKind::Every: {
        std::string s = "{ every: " + formatDuration(t.intervalSeconds);
        if (t.betweenStart >= 0)
            s += ", between: [" + quote(describeTime(t.betweenStart)) + ", " + quote(describeTime(t.betweenEnd)) + "]";
        if (!t.days.empty())
            s += ", days: " + days(t.days);
        return s + " }";
    }
    case TriggerKind::OnStartup:
        return t.delaySeconds > 0 ? "{ on_startup: { delay: " + formatDuration(t.delaySeconds) + " } }" : "{ on_startup: {} }";
    case TriggerKind::OnLogon:
        return t.delaySeconds > 0 ? "{ on_logon: { delay: " + formatDuration(t.delaySeconds) + " } }" : "{ on_logon: {} }";
    case TriggerKind::Manual:
        return "manual";
    }
    return "manual";
}

std::string secret(const SecretRef& s)
{
    if (s.kind == SecretRef::Kind::Secret)
        return "{ secret: " + s.name + " }";
    if (s.kind == SecretRef::Kind::Env)
        return "{ env: " + s.name + " }";
    return "";
}

std::string withHeader(const std::string& body)
{
    auto digest = Sha256::ofBytes(body);
    return std::string(kMarker) + kHashPrefix + (digest ? toHex(digest.value()) : "") + "\n" + body;
}

void hooks(std::ostringstream& o, const Hooks& h)
{
    if (h.before.empty() && h.after.empty() && h.onSuccess.empty() && h.onFailure.empty())
        return;
    o << "\nhooks:\n";
    auto phase = [&](const char* name, const std::vector<HookCommand>& v) {
        if (v.empty())
            return;
        o << "  " << name << ":\n";
        for (const auto& c : v)
            o << "    - { run: " << quotedList(c.run) << ", timeout: " << formatDuration(c.timeoutSeconds)
              << ", on_error: " << (c.failOnError ? "fail" : "warn") << " }\n";
    };
    phase("before", h.before);
    phase("after", h.after);
    phase("on_success", h.onSuccess);
    phase("on_failure", h.onFailure);
}

} // namespace

std::string formatDuration(std::int64_t s)
{
    if (s > 0 && s % 86400 == 0)
        return std::to_string(s / 86400) + "d";
    if (s > 0 && s % 3600 == 0)
        return std::to_string(s / 3600) + "h";
    if (s > 0 && s % 60 == 0)
        return std::to_string(s / 60) + "m";
    return std::to_string(s) + "s";
}

bool isPristineGenerated(const std::string& original)
{
    // Editores no Windows podem converter para CRLF sem alterar o conteúdo.
    std::string text;
    text.reserve(original.size());
    for (char c : original)
        if (c != '\r')
            text += c;
    const std::string marker = kMarker;
    if (text.compare(0, marker.size(), marker) != 0)
        return false;
    const size_t hashStart = marker.size() + std::string(kHashPrefix).size();
    if (text.compare(marker.size(), std::string(kHashPrefix).size(), kHashPrefix) != 0)
        return false;
    const size_t nl = text.find('\n', hashStart);
    if (nl == std::string::npos)
        return false;
    const std::string stored = text.substr(hashStart, nl - hashStart);
    auto digest = Sha256::ofBytes(text.substr(nl + 1));
    return digest && toHex(digest.value()) == stored;
}

std::string emitGlobal(const GlobalConfig& g)
{
    std::ostringstream o;
    o << "version: 1\n";
    if (!g.instanceName.empty())
        o << "\ninstance:\n  name: " << quote(g.instanceName) << "\n";

    o << "\nvaults:\n";
    if (g.vaults.empty())
        o << "  []\n";
    for (const auto& v : g.vaults) {
        o << "  - id: " << v.id << "\n"
          << "    vault_id: " << v.vaultId << "\n"
          << "    label: " << quote(v.label) << "\n";
        if (!v.path.empty())
            o << "    path: " << quote(v.path) << "\n";
        o << "    rotation: " << (v.rotation ? "true" : "false") << "\n"
          << "    max_absence: " << formatDuration(v.maxAbsenceSeconds) << "\n"
          << "    free_space_alert: " << percent(v.freeSpaceAlert) << "\n";
    }

    o << "\ndefaults:\n";
    if (!g.defaultVaults.empty())
        o << "  vaults: " << list(g.defaultVaults) << "\n";
    o << "  min_vaults_ok: " << (g.defaultMinVaultsOk == 0 ? std::string("all") : std::to_string(g.defaultMinVaultsOk)) << "\n"
      << "  timeout: " << formatDuration(g.defaultTimeoutSeconds) << "\n"
      << "  schedule_catch_up: " << (g.scheduleCatchUp ? "true" : "false") << "\n";

    o << "\nalerts:\n"
      << "  no_success_for: " << formatDuration(g.alertNoSuccessForSeconds) << "\n"
      << "  tray: " << (g.alertTray ? "true" : "false") << "\n"
      << "  windows_event_log: " << (g.alertEventLog ? "true" : "false") << "\n";

    if (g.scrubSchedule) {
        o << "\nmaintenance:\n  scrub:\n"
          << "    schedule: " << trigger(*g.scrubSchedule) << "\n"
          << "    sample: " << percent(g.scrubSample) << "\n"
          << "    repair_from_other_vaults: " << (g.scrubRepair ? "true" : "false") << "\n";
    }

    o << "\nshutdown:\n  countdown: " << formatDuration(g.shutdownCountdownSeconds) << "\n";
    if (!g.shutdownAllowedGroups.empty())
        o << "  allowed_after: " << list(g.shutdownAllowedGroups) << "\n";

    o << "\nlogging:\n  level: " << g.logLevel << "\n  keep: " << formatDuration(g.logKeepSeconds) << "\n";
    return withHeader(o.str());
}

std::string emitJob(const JobConfig& j)
{
    std::ostringstream o;
    o << "version: 1\n"
      << "id: " << j.id << "\n"
      << "name: " << quote(j.name) << "\n"
      << "type: " << j.type << "\n";
    if (!j.enabled)
        o << "enabled: false\n";
    if (!j.group.empty())
        o << "group: " << j.group << "\n";
    if (!j.vaults.empty())
        o << "vaults: " << list(j.vaults) << "\n";
    if (j.minVaultsOk > 0)
        o << "min_vaults_ok: " << j.minVaultsOk << "\n";

    o << "\nschedule:\n";
    if (j.schedule.empty())
        o << "  - manual\n";
    for (const auto& t : j.schedule)
        o << "  - " << trigger(t) << "\n";
    if (!j.dependsOn.empty())
        o << "depends_on: " << list(j.dependsOn) << "\n";
    if (j.timeoutSeconds > 0)
        o << "timeout: " << formatDuration(j.timeoutSeconds) << "\n";

    if (j.type == "files") {
        const FilesJob& f = j.files;
        o << "\nsource: " << quote(f.source) << "\n";
        auto patterns = [&](const char* key, const std::vector<std::wstring>& v) {
            if (v.empty())
                return;
            o << key << ":\n";
            for (const auto& p : v)
                o << "  - " << quote(p) << "\n";
        };
        patterns("include", f.include);
        patterns("exclude", f.exclude);
        o << "\nconsistency:\n  snapshot: " << (f.vss ? "vss" : "none")
          << "\n  on_snapshot_failure: " << (f.continueWithoutSnapshot ? "continue" : "fail") << "\n"
          << "verify: " << (f.verifyUncached ? "reread_uncached" : "hash_on_copy") << "\n"
          << "\nretention:\n  keep_versions: " << formatDuration(f.keepVersionsSeconds) << "\n"
          << "  keep_min_versions: " << f.keepMinVersions << "\n"
          << "  keep_deleted: " << formatDuration(f.keepDeletedSeconds) << "\n"
          << "\nguards:\n  max_deleted: " << percent(f.maxDeleted) << "\n  max_modified: " << percent(f.maxModified) << "\n";
        if (!f.sentinelFile.empty())
            o << "  sentinel_file: " << quote(f.sentinelFile) << "\n";
    } else {
        const DatabaseJob& d = j.database;
        o << "\nengine: " << d.engine << "\n";
        if (d.engine == "postgresql" || d.engine == "mysql" || d.engine == "mariadb" || d.engine == "firebird"
            || d.engine == "sqlserver") {
            o << "connection:\n  host: " << quote(d.host) << "\n";
            if (d.port > 0)
                o << "  port: " << d.port << "\n";
            if (d.windowsAuth) {
                o << "  auth: windows\n";
            } else {
                if (d.engine == "sqlserver")
                    o << "  auth: sql\n";
                o << "  user: " << quote(d.user) << "\n";
                if (d.password.kind != SecretRef::Kind::None)
                    o << "  password: " << secret(d.password) << "\n";
            }
            if (!d.sslMode.empty() && d.engine != "firebird")
                o << (d.engine == "postgresql" ? "  sslmode: " : d.engine == "sqlserver" ? "  encrypt: " : "  ssl_mode: ")
                  << d.sslMode << "\n";
            if (d.engine == "sqlserver" && d.options.count("trust_server_certificate"))
                o << "  trust_server_certificate: " << d.options.at("trust_server_certificate") << "\n";
            if (!d.sslCa.empty())
                o << "  ssl_ca: " << quote(d.sslCa) << "\n";
            if (d.allDatabases) {
                o << "databases: all\n";
            } else {
                o << "databases:\n";
                for (const auto& db : d.databases) {
                    o << "  - name: " << quote(db.name) << "\n";
                    if (d.engine == "firebird")
                        o << "    path: " << quote(db.path.empty() ? db.name : db.path) << "\n";
                    if (!db.excludeTables.empty())
                        o << "    exclude_tables: " << quotedList(db.excludeTables) << "\n";
                }
            }
            if (d.engine == "postgresql" && d.options.count("include_globals"))
                o << "postgresql:\n  include_globals: " << d.options.at("include_globals") << "\n";
            if (d.engine == "sqlserver" && d.options.count("backup_dir"))
                o << "sqlserver:\n  backup_dir: " << quote(d.options.at("backup_dir")) << "\n";
        } else if (d.engine == "sqlite") {
            o << "sqlite:\n  files:\n";
            for (const auto& f : d.sqliteFiles)
                o << "    - " << quote(f) << "\n";
            o << "  integrity_check: true\n";
        } else if (d.engine == "custom") {
            o << "custom:\n  command: " << quotedList(d.customCommand) << "\n";
            if (d.options.count("output"))
                o << "  output: " << d.options.at("output") << "\n";
            if (d.options.count("output_name"))
                o << "  output_name: " << quote(d.options.at("output_name")) << "\n";
        }
        if (!d.tools.empty()) {
            o << "tools:\n";
            for (const auto& [tool, path] : d.tools)
                o << "  " << tool << ": " << quote(path) << "\n";
        }
        o << "\nretention:\n  keep_last: " << d.keepLast << "\n  keep_daily: " << d.keepDaily << "\n  keep_weekly: "
          << d.keepWeekly << "\n  keep_monthly: " << d.keepMonthly << "\n";
        o << "\nguards:\n  on_empty_database: " << (d.blockOnEmptyDatabase ? "block" : "warn")
          << "\n  on_missing_tables: " << (d.blockOnMissingTables ? "block" : "warn")
          << "\n  min_size_ratio: " << d.minSizeRatio << "\n";
    }

    hooks(o, j.hooks);
    if (j.shutdownAfter)
        o << "\nshutdown_after: true\n";
    return withHeader(o.str());
}

} // namespace synkro::config
