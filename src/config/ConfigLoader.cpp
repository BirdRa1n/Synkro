#include "config/ConfigLoader.h"

#include "config/Values.h"
#include "core/Paths.h"
#include "core/RunId.h"
#include "secrets/SecretStore.h"
#include "engine/VaultOps.h"
#include "platform/windows/Volume.h"
#include "platform/windows/Win32.h"
#include "vault/VaultLocator.h"

#include <QUuid>

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <functional>
#include <initializer_list>
#include <set>

namespace synkro::config {

// ---- ConfigSet / GlobalConfig ------------------------------------------------

const VaultConfig* GlobalConfig::findVault(const std::string& id) const
{
    for (const auto& v : vaults) {
        if (v.id == id)
            return &v;
    }
    return nullptr;
}

bool ConfigSet::hasErrors() const
{
    return errorCount() > 0;
}

std::size_t ConfigSet::errorCount() const
{
    return static_cast<std::size_t>(std::count_if(diagnostics.begin(), diagnostics.end(),
                                                  [](const Diagnostic& d) { return d.severity == Severity::Error; }));
}

std::size_t ConfigSet::warningCount() const
{
    return diagnostics.size() - errorCount();
}

const JobConfig* ConfigSet::findJob(const std::string& id) const
{
    for (const auto& j : jobs) {
        if (j.id == id)
            return &j;
    }
    return nullptr;
}

std::wstring defaultConfigDirectory()
{
    wchar_t buf[MAX_PATH] = {};
    if (::GetEnvironmentVariableW(L"SYNKRO_CONFIG_DIR", buf, MAX_PATH) > 0)
        return buf;
    return win::joinPath(dataDirectory(), L"config");
}

std::string formatDiagnostic(const Diagnostic& d)
{
    std::string out = d.severity == Severity::Error ? "✗ " : "⚠ ";
    out += win::toUtf8(d.file);
    if (d.line > 0)
        out += ":" + std::to_string(d.line) + ":" + std::to_string(d.column);
    out += "  ";
    if (!d.key.empty())
        out += d.key + ": ";
    out += d.message;
    return out;
}

// ---- Leitor com diagnóstico --------------------------------------------------

namespace {

std::string join(const std::string& path, const std::string& key)
{
    return path.empty() ? key : path + "." + key;
}

class Reader {
public:
    Reader(std::wstring file, std::vector<Diagnostic>& out) : m_file(std::move(file)), m_out(out) {}

    void error(const YAML::Node& at, const std::string& key, std::string msg) { add(Severity::Error, at, key, std::move(msg)); }
    void warn(const YAML::Node& at, const std::string& key, std::string msg) { add(Severity::Warning, at, key, std::move(msg)); }
    void errorAtLine(int line, const std::string& key, std::string msg)
    {
        m_out.push_back({Severity::Error, m_file, line, 1, key, std::move(msg)});
    }
    void warnAtLine(int line, const std::string& key, std::string msg)
    {
        m_out.push_back({Severity::Warning, m_file, line, 1, key, std::move(msg)});
    }

    bool requireMap(const YAML::Node& n, const std::string& path)
    {
        if (n.IsMap())
            return true;
        error(n, path, "esperado um bloco de campos (mapa)");
        return false;
    }

    // O yaml-cpp aceita chaves repetidas e descarta uma delas em silêncio;
    // numa configuração de backup isso esconderia um agendamento ou cofre.
    void checkDuplicates(const YAML::Node& map, const std::string& path)
    {
        if (!map.IsMap())
            return;
        std::set<std::string> seen;
        for (auto it = map.begin(); it != map.end(); ++it) {
            const std::string key = it->first.Scalar();
            if (!seen.insert(key).second)
                error(it->first, join(path, key), "campo repetido; apenas uma das ocorrências seria usada");
        }
    }

    void checkKeys(const YAML::Node& map, const std::string& path, std::initializer_list<const char*> allowed)
    {
        if (!map.IsMap())
            return;
        checkDuplicates(map, path);
        for (auto it = map.begin(); it != map.end(); ++it) {
            const std::string key = it->first.Scalar();
            bool ok = false;
            for (const char* a : allowed) {
                if (key == a) {
                    ok = true;
                    break;
                }
            }
            if (!ok)
                error(it->first, join(path, key), "campo desconhecido");
        }
    }

    std::optional<std::string> str(const YAML::Node& parent, const char* key, const std::string& path, bool required = false)
    {
        const YAML::Node n = parent[key];
        if (!n || n.IsNull()) {
            if (required)
                error(parent, join(path, key), "campo obrigatório");
            return std::nullopt;
        }
        if (!n.IsScalar()) {
            error(n, join(path, key), "esperado um texto");
            return std::nullopt;
        }
        return n.Scalar();
    }

    std::optional<bool> boolean(const YAML::Node& parent, const char* key, const std::string& path)
    {
        auto s = str(parent, key, path);
        if (!s)
            return std::nullopt;
        if (*s == "true" || *s == "yes" || *s == "on")
            return true;
        if (*s == "false" || *s == "no" || *s == "off")
            return false;
        error(parent[key], join(path, key), "esperado true ou false");
        return std::nullopt;
    }

    std::optional<long long> integer(const YAML::Node& parent, const char* key, const std::string& path, long long min,
                                     long long max)
    {
        auto s = str(parent, key, path);
        if (!s)
            return std::nullopt;
        try {
            size_t used = 0;
            const long long v = std::stoll(*s, &used);
            if (used == s->size() && v >= min && v <= max)
                return v;
        } catch (...) {
        }
        error(parent[key], join(path, key),
              "esperado um número inteiro entre " + std::to_string(min) + " e " + std::to_string(max));
        return std::nullopt;
    }

    std::optional<std::int64_t> duration(const YAML::Node& parent, const char* key, const std::string& path)
    {
        auto s = str(parent, key, path);
        if (!s)
            return std::nullopt;
        if (auto d = parseDuration(*s))
            return d;
        error(parent[key], join(path, key), "duração inválida (use 30s, 10m, 2h ou 90d)");
        return std::nullopt;
    }

    std::optional<double> ratio(const YAML::Node& parent, const char* key, const std::string& path)
    {
        auto s = str(parent, key, path);
        if (!s)
            return std::nullopt;
        if (auto r = parseRatio(*s))
            return r;
        error(parent[key], join(path, key), "proporção inválida (use 20% ou 0.2)");
        return std::nullopt;
    }

    std::optional<std::string> oneOf(const YAML::Node& parent, const char* key, const std::string& path,
                                     std::initializer_list<const char*> options, bool required = false)
    {
        auto s = str(parent, key, path, required);
        if (!s)
            return std::nullopt;
        std::string list;
        for (const char* o : options) {
            if (*s == o)
                return s;
            if (!list.empty())
                list += " | ";
            list += o;
        }
        error(parent[key], join(path, key), "valor inválido \"" + *s + "\" (use " + list + ")");
        return std::nullopt;
    }

    std::vector<std::string> strings(const YAML::Node& parent, const char* key, const std::string& path)
    {
        std::vector<std::string> out;
        const YAML::Node n = parent[key];
        if (!n || n.IsNull())
            return out;
        if (n.IsScalar()) {
            out.push_back(n.Scalar());
            return out;
        }
        if (!n.IsSequence()) {
            error(n, join(path, key), "esperada uma lista");
            return out;
        }
        for (const auto& item : n) {
            if (!item.IsScalar()) {
                error(item, join(path, key), "itens da lista precisam ser textos");
                continue;
            }
            out.push_back(item.Scalar());
        }
        return out;
    }

    SecretRef secret(const YAML::Node& parent, const char* key, const std::string& path)
    {
        const YAML::Node n = parent[key];
        if (!n || n.IsNull())
            return {};
        if (n.IsScalar()) {
            error(n, join(path, key),
                  "senhas em texto puro não são permitidas. Use { secret: <nome> } e cadastre com: "
                  "synkro-worker secret set <nome>");
            return {};
        }
        if (!n.IsMap() || n.size() != 1) {
            error(n, join(path, key), "use { secret: <nome> } ou { env: <VARIAVEL> }");
            return {};
        }
        if (n["secret"]) {
            const std::string name = n["secret"].Scalar();
            if (!SecretStore::isValidName(name))
                error(n["secret"], join(path, key), "nome de segredo inválido (use a-z, 0-9, '-', '_' e '.')");
            if (secretSink)
                secretSink->push_back({join(path, key), name, n.Mark().line + 1});
            return {SecretRef::Kind::Secret, name};
        }
        if (n["env"])
            return {SecretRef::Kind::Env, n["env"].Scalar()};
        error(n, join(path, key), "use { secret: <nome> } ou { env: <VARIAVEL> }");
        return {};
    }

    std::optional<ScheduleTrigger> trigger(const YAML::Node& n, const std::string& path)
    {
        ScheduleTrigger t;
        if (n.IsScalar()) {
            if (n.Scalar() == "manual") {
                t.kind = TriggerKind::Manual;
                return t;
            }
            error(n, path, "gatilho inválido \"" + n.Scalar() + "\"");
            return std::nullopt;
        }
        if (!requireMap(n, path))
            return std::nullopt;

        auto time = [&](const YAML::Node& parent, const char* key, const std::string& p) -> std::optional<int> {
            auto s = str(parent, key, p, true);
            if (!s)
                return std::nullopt;
            if (auto m = parseTimeOfDay(*s))
                return m;
            error(parent[key], join(p, key), "horário inválido (use HH:MM, ex.: \"22:00\")");
            return std::nullopt;
        };
        auto days = [&](const YAML::Node& parent, const std::string& p, bool required) -> std::optional<std::vector<int>> {
            const auto list = strings(parent, "days", p);
            if (list.empty()) {
                if (required)
                    error(parent, join(p, "days"), "campo obrigatório");
                return required ? std::nullopt : std::optional<std::vector<int>>(std::vector<int>{});
            }
            std::vector<int> out;
            for (const auto& d : list) {
                auto w = parseWeekday(d);
                if (!w) {
                    error(parent["days"], join(p, "days"), "dia inválido \"" + d + "\" (use mon, tue, wed, thu, fri, sat, sun)");
                    return std::nullopt;
                }
                out.push_back(*w);
            }
            return out;
        };

        if (n["every"]) {
            checkKeys(n, path, {"every", "between", "days"});
            t.kind = TriggerKind::Every;
            auto iv = duration(n, "every", path);
            if (!iv)
                return std::nullopt;
            if (*iv < 300) {
                error(n["every"], join(path, "every"), "o intervalo mínimo é 5m");
                return std::nullopt;
            }
            t.intervalSeconds = *iv;
            if (const YAML::Node b = n["between"]) {
                if (!b.IsSequence() || b.size() != 2) {
                    error(b, join(path, "between"), "use [\"HH:MM\", \"HH:MM\"]");
                    return std::nullopt;
                }
                auto a = parseTimeOfDay(b[0].Scalar());
                auto z = parseTimeOfDay(b[1].Scalar());
                if (!a || !z || *a >= *z) {
                    error(b, join(path, "between"), "intervalo de horário inválido");
                    return std::nullopt;
                }
                t.betweenStart = *a;
                t.betweenEnd = *z;
            }
            auto d = days(n, path, false);
            if (!d)
                return std::nullopt;
            t.days = *d;
            return t;
        }

        if (n.size() != 1) {
            error(n, path, "cada gatilho deve ter um único tipo (daily, weekly, monthly, every, on_startup, on_logon)");
            return std::nullopt;
        }
        const std::string kind = n.begin()->first.Scalar();
        const YAML::Node v = n.begin()->second;
        const std::string p = join(path, kind);

        if (kind == "daily") {
            t.kind = TriggerKind::Daily;
            if (!v.IsScalar() || !parseTimeOfDay(v.Scalar())) {
                error(v, p, "horário inválido (use HH:MM, ex.: \"22:00\")");
                return std::nullopt;
            }
            t.atMinutes = *parseTimeOfDay(v.Scalar());
            return t;
        }
        if (kind == "weekly") {
            t.kind = TriggerKind::Weekly;
            if (!requireMap(v, p))
                return std::nullopt;
            checkKeys(v, p, {"days", "at"});
            auto d = days(v, p, true);
            auto at = time(v, "at", p);
            if (!d || !at)
                return std::nullopt;
            t.days = *d;
            t.atMinutes = *at;
            return t;
        }
        if (kind == "monthly") {
            t.kind = TriggerKind::Monthly;
            if (!requireMap(v, p))
                return std::nullopt;
            checkKeys(v, p, {"day", "at"});
            auto at = time(v, "at", p);
            auto day = str(v, "day", p, true);
            if (!at || !day)
                return std::nullopt;
            if (*day == "last") {
                t.dayOfMonth = -1;
            } else {
                auto dn = integer(v, "day", p, 1, 28);
                if (!dn)
                    return std::nullopt;
                t.dayOfMonth = static_cast<int>(*dn);
            }
            t.atMinutes = *at;
            return t;
        }
        if (kind == "on_startup" || kind == "on_logon") {
            t.kind = kind == "on_startup" ? TriggerKind::OnStartup : TriggerKind::OnLogon;
            if (v && !v.IsNull()) {
                if (!requireMap(v, p))
                    return std::nullopt;
                checkKeys(v, p, {"delay"});
                if (auto dl = duration(v, "delay", p))
                    t.delaySeconds = *dl;
            }
            return t;
        }
        error(n, path, "tipo de gatilho desconhecido \"" + kind + "\"");
        return std::nullopt;
    }

    std::vector<ScheduleTrigger> schedule(const YAML::Node& parent, const char* key, const std::string& path)
    {
        std::vector<ScheduleTrigger> out;
        const YAML::Node n = parent[key];
        if (!n || n.IsNull())
            return out;
        if (n.IsSequence()) {
            int i = 0;
            for (const auto& item : n) {
                if (auto t = trigger(item, join(path, key) + "[" + std::to_string(i) + "]"))
                    out.push_back(*t);
                ++i;
            }
        } else if (auto t = trigger(n, join(path, key))) {
            out.push_back(*t);
        }
        return out;
    }

    void hookList(const YAML::Node& parent, const char* key, const std::string& path, std::vector<HookCommand>& out)
    {
        const YAML::Node n = parent[key];
        if (!n || n.IsNull())
            return;
        if (!n.IsSequence()) {
            error(n, join(path, key), "esperada uma lista de comandos");
            return;
        }
        int i = 0;
        for (const auto& item : n) {
            const std::string p = join(path, key) + "[" + std::to_string(i++) + "]";
            if (!requireMap(item, p))
                continue;
            checkKeys(item, p, {"run", "timeout", "on_error"});
            HookCommand h;
            h.run = strings(item, "run", p);
            if (h.run.empty())
                error(item, join(p, "run"), "informe o comando como lista: [\"programa.exe\", \"arg\"]");
            if (auto t = duration(item, "timeout", p))
                h.timeoutSeconds = *t;
            if (auto e = oneOf(item, "on_error", p, {"fail", "warn"}))
                h.failOnError = *e == "fail";
            out.push_back(std::move(h));
        }
    }

    const std::wstring& file() const { return m_file; }

    std::vector<SecretUse>* secretSink = nullptr;

private:
    void add(Severity sev, const YAML::Node& at, const std::string& key, std::string msg)
    {
        int line = 0, col = 0;
        if (at.IsDefined()) {
            const YAML::Mark m = at.Mark();
            if (m.line >= 0) {
                line = m.line + 1;
                col = m.column + 1;
            }
        }
        m_out.push_back({sev, m_file, line, col, key, std::move(msg)});
    }

    std::wstring m_file;
    std::vector<Diagnostic>& m_out;
};

std::optional<YAML::Node> parseYaml(const std::string& text, Reader& r)
{
    try {
        return YAML::Load(text);
    } catch (const YAML::ParserException& e) {
        r.errorAtLine(e.mark.line + 1, {}, "YAML inválido: " + e.msg);
    } catch (const std::exception& e) {
        r.errorAtLine(0, {}, std::string("YAML inválido: ") + e.what());
    }
    return std::nullopt;
}

bool isLocalHost(const std::string& h)
{
    return h == "localhost" || h == "127.0.0.1" || h == "::1" || h == ".";
}

bool isNetworkPath(const std::wstring& p)
{
    if (p.rfind(L"\\\\", 0) == 0 || p.rfind(L"//", 0) == 0)
        return true;
    if (p.size() >= 2 && p[1] == L':') {
        const std::wstring root = p.substr(0, 2) + L"\\";
        return ::GetDriveTypeW(root.c_str()) == DRIVE_REMOTE;
    }
    return false;
}

bool isAbsoluteWindowsPath(const std::wstring& p)
{
    return (p.size() >= 3 && p[1] == L':' && (p[2] == L'\\' || p[2] == L'/')) || p.rfind(L"\\\\", 0) == 0;
}

std::wstring w(const std::string& s)
{
    return win::fromUtf8(s);
}

// Parâmetros que o Synkro controla e que não podem vir em extra_args
// (docs/configuration.md §7.6).
const std::set<std::string>& forbiddenArgs(const std::string& engine)
{
    static const std::set<std::string> pg = {"--file", "-f", "--format", "-F", "--password", "-W",
                                             "--host", "-h", "--port", "-p", "--username", "-U", "--dbname", "-d"};
    static const std::set<std::string> my = {"--result-file", "-r", "--password", "-p", "--defaults-file",
                                             "--defaults-extra-file", "--host", "-h", "--user", "-u",
                                             "--databases", "-B", "--all-databases", "-A"};
    static const std::set<std::string> none;
    if (engine == "postgresql")
        return pg;
    if (engine == "mysql" || engine == "mariadb")
        return my;
    return none;
}

std::vector<std::string> checkExtraArgs(Reader& r, const YAML::Node& section, const std::string& path,
                                        const std::string& engine)
{
    const auto args = r.strings(section, "extra_args", path);
    const auto& forbidden = forbiddenArgs(engine);
    for (const auto& a : args) {
        const std::string flag = a.substr(0, a.find('='));
        if (forbidden.count(flag))
            r.error(section["extra_args"], join(path, "extra_args"),
                    "o parâmetro " + flag + " é controlado pelo Synkro; use os campos próprios da configuração");
    }
    return args;
}

// ---- Tarefa: arquivos -----------------------------------------------------------

void parseFilesJob(Reader& r, const YAML::Node& doc, JobConfig& job)
{
    FilesJob& f = job.files;
    if (auto s = r.str(doc, "source", "", true)) {
        f.source = w(*s);
        std::replace(f.source.begin(), f.source.end(), L'/', L'\\');
        if (!isAbsoluteWindowsPath(f.source))
            r.error(doc["source"], "source", "informe o caminho completo da pasta (ex.: \"C:/PDV\")");
    }
    for (const auto& s : r.strings(doc, "include", ""))
        f.include.push_back(w(s));
    for (const auto& s : r.strings(doc, "exclude", ""))
        f.exclude.push_back(w(s));

    if (const YAML::Node c = doc["consistency"]) {
        if (r.requireMap(c, "consistency")) {
            r.checkKeys(c, "consistency", {"snapshot", "on_snapshot_failure"});
            if (auto s = r.oneOf(c, "snapshot", "consistency", {"vss", "none"}))
                f.vss = *s == "vss";
            if (auto s = r.oneOf(c, "on_snapshot_failure", "consistency", {"continue", "fail"}))
                f.continueWithoutSnapshot = *s == "continue";
        }
    }
    if (const YAML::Node c = doc["change_detection"]) {
        if (r.requireMap(c, "change_detection")) {
            r.checkKeys(c, "change_detection", {"full_hash_every"});
            if (auto n = r.integer(c, "full_hash_every", "change_detection", 0, 10000))
                f.fullHashEvery = static_cast<int>(*n);
        }
    }
    if (auto v = r.oneOf(doc, "verify", "", {"hash_on_copy", "reread_uncached"}))
        f.verifyUncached = *v == "reread_uncached";

    if (const YAML::Node ret = doc["retention"]) {
        if (r.requireMap(ret, "retention")) {
            r.checkKeys(ret, "retention", {"keep_versions", "keep_min_versions", "keep_deleted"});
            if (auto d = r.duration(ret, "keep_versions", "retention"))
                f.keepVersionsSeconds = *d;
            if (auto n = r.integer(ret, "keep_min_versions", "retention", 0, 1000))
                f.keepMinVersions = static_cast<int>(*n);
            if (auto d = r.duration(ret, "keep_deleted", "retention"))
                f.keepDeletedSeconds = *d;
            if (f.keepDeletedSeconds < f.keepVersionsSeconds)
                r.warn(ret, "retention.keep_deleted",
                       "menor que keep_versions: arquivos excluídos expiram junto com as versões");
        }
    }
    if (const YAML::Node g = doc["guards"]) {
        if (r.requireMap(g, "guards")) {
            r.checkKeys(g, "guards", {"max_deleted", "max_modified", "sentinel_file"});
            if (auto x = r.ratio(g, "max_deleted", "guards"))
                f.maxDeleted = *x;
            if (auto x = r.ratio(g, "max_modified", "guards"))
                f.maxModified = *x;
            if (auto s = r.str(g, "sentinel_file", "guards"))
                f.sentinelFile = w(*s);
        }
    }
}

// ---- Tarefa: banco de dados -------------------------------------------------

void parseDatabaseJob(Reader& r, const YAML::Node& doc, JobConfig& job)
{
    DatabaseJob& d = job.database;
    auto engine = r.str(doc, "engine", "", true);
    if (!engine)
        return;
    if (*engine == "firebird" || *engine == "sqlserver") {
        r.error(doc["engine"], "engine", "o motor " + *engine + " está previsto para a Fase 4 do Synkro");
        return;
    }
    if (*engine != "postgresql" && *engine != "mysql" && *engine != "mariadb" && *engine != "sqlite"
        && *engine != "custom") {
        r.error(doc["engine"], "engine", "motor inválido (use postgresql | mysql | mariadb | sqlite | custom)");
        return;
    }
    d.engine = *engine;
    const bool server = d.engine == "postgresql" || d.engine == "mysql" || d.engine == "mariadb";

    if (server) {
        const YAML::Node c = doc["connection"];
        if (!c) {
            r.error(doc, "connection", "campo obrigatório para " + d.engine);
        } else if (r.requireMap(c, "connection")) {
            r.checkKeys(c, "connection", {"host", "port", "user", "password", "sslmode", "ssl_mode", "ssl_ca",
                                          "connect_timeout", "retries", "allow_insecure"});
            d.host = r.str(c, "host", "connection", true).value_or("");
            const long long defPort = d.engine == "postgresql" ? 5432 : 3306;
            d.port = static_cast<int>(r.integer(c, "port", "connection", 1, 65535).value_or(defPort));
            d.user = r.str(c, "user", "connection", true).value_or("");
            d.password = r.secret(c, "password", "connection");
            d.remote = !d.host.empty() && !isLocalHost(d.host);
            d.allowInsecure = r.boolean(c, "allow_insecure", "connection").value_or(false);
            if (auto ca = r.str(c, "ssl_ca", "connection")) {
                d.sslCa = w(*ca);
                std::replace(d.sslCa.begin(), d.sslCa.end(), L'/', L'\\');
            }
            if (auto t = r.duration(c, "connect_timeout", "connection"))
                d.connectTimeoutSeconds = *t;
            if (auto n = r.integer(c, "retries", "connection", 0, 20))
                d.retries = static_cast<int>(*n);

            if (d.engine == "postgresql") {
                if (c["ssl_mode"])
                    r.error(c["ssl_mode"], "connection.ssl_mode", "no PostgreSQL o campo é sslmode");
                d.sslMode = r.oneOf(c, "sslmode", "connection", {"disable", "prefer", "require", "verify-ca", "verify-full"})
                                .value_or(d.remote ? "require" : "prefer");
            } else {
                if (c["sslmode"])
                    r.error(c["sslmode"], "connection.sslmode", "no MySQL/MariaDB o campo é ssl_mode");
                d.sslMode = r.oneOf(c, "ssl_mode", "connection",
                                    {"disabled", "preferred", "required", "verify_ca", "verify_identity"})
                                .value_or(d.remote ? "required" : "preferred");
            }
            const bool insecure = d.sslMode == "disable" || d.sslMode == "prefer" || d.sslMode == "disabled"
                                  || d.sslMode == "preferred";
            if (d.remote && insecure && !d.allowInsecure)
                r.error(c, "connection", "servidor remoto sem criptografia obrigatória: use sslmode/ssl_mode "
                                         "require(d) ou informe allow_insecure: true");
            if (d.remote && d.allowInsecure)
                r.warn(c, "connection.allow_insecure", "conexão remota sem criptografia garantida");
            if (d.password.kind == SecretRef::Kind::None && !c["password"])
                r.warn(c, "connection.password", "sem senha configurada");
        }

        const YAML::Node dbs = doc["databases"];
        if (!dbs) {
            r.error(doc, "databases", "informe os bancos (lista) ou \"all\"");
        } else if (dbs.IsScalar() && dbs.Scalar() == "all") {
            d.allDatabases = true;
        } else if (dbs.IsSequence()) {
            int i = 0;
            for (const auto& item : dbs) {
                const std::string p = "databases[" + std::to_string(i++) + "]";
                if (item.IsScalar()) {
                    d.databases.push_back({item.Scalar(), {}});
                } else if (item.IsMap()) {
                    r.checkKeys(item, p, {"name", "exclude_tables"});
                    DatabaseEntry e;
                    e.name = r.str(item, "name", p, true).value_or("");
                    e.excludeTables = r.strings(item, "exclude_tables", p);
                    d.databases.push_back(std::move(e));
                } else {
                    r.error(item, p, "use o nome do banco ou { name, exclude_tables }");
                }
            }
            if (d.databases.empty())
                r.error(dbs, "databases", "lista de bancos vazia");
        } else {
            r.error(dbs, "databases", "informe os bancos (lista) ou \"all\"");
        }
    } else if (doc["connection"]) {
        r.error(doc["connection"], "connection", "não se aplica ao motor " + d.engine);
    }

    // Opções específicas do motor.
    const YAML::Node pgSec = doc["postgresql"], mySec = doc["mysql"], liteSec = doc["sqlite"], cuSec = doc["custom"];
    auto wrongSection = [&](const YAML::Node& n, const char* name) {
        if (n)
            r.error(n, name, std::string("seção ") + name + " não se aplica ao motor " + d.engine);
    };
    if (d.engine != "postgresql")
        wrongSection(pgSec, "postgresql");
    if (d.engine != "mysql" && d.engine != "mariadb")
        wrongSection(mySec, "mysql");
    if (d.engine != "sqlite")
        wrongSection(liteSec, "sqlite");
    if (d.engine != "custom")
        wrongSection(cuSec, "custom");

    if (d.engine == "postgresql" && pgSec && r.requireMap(pgSec, "postgresql")) {
        r.checkKeys(pgSec, "postgresql", {"format", "compress", "include_globals", "jobs", "extra_args"});
        const std::string format = r.oneOf(pgSec, "format", "postgresql", {"custom", "plain", "directory"}).value_or("custom");
        if (format == "directory")
            r.error(pgSec["format"], "postgresql.format",
                    "o formato directory ainda não é suportado; use custom (recomendado) ou plain");
        d.options["format"] = format;
        if (auto n = r.integer(pgSec, "compress", "postgresql", 0, 9))
            d.options["compress"] = std::to_string(*n);
        if (auto b = r.boolean(pgSec, "include_globals", "postgresql"))
            d.options["include_globals"] = *b ? "true" : "false";
        if (auto n = r.integer(pgSec, "jobs", "postgresql", 1, 64)) {
            if (*n > 1 && format != "directory")
                r.error(pgSec["jobs"], "postgresql.jobs", "jobs > 1 exige format: directory");
            d.options["jobs"] = std::to_string(*n);
        }
        d.extraArgs = checkExtraArgs(r, pgSec, "postgresql", d.engine);
    }
    if ((d.engine == "mysql" || d.engine == "mariadb") && mySec && r.requireMap(mySec, "mysql")) {
        r.checkKeys(mySec, "mysql", {"single_transaction", "routines", "triggers", "events", "myisam_strategy",
                                     "compression", "compression_level", "extra_args"});
        for (const char* b : {"single_transaction", "routines", "triggers", "events"})
            if (auto v = r.boolean(mySec, b, "mysql"))
                d.options[b] = *v ? "true" : "false";
        if (r.boolean(mySec, "single_transaction", "mysql") == std::optional<bool>(false))
            r.warn(mySec["single_transaction"], "mysql.single_transaction",
                   "sem single_transaction o dump não representa um instante único do banco");
        if (auto s = r.oneOf(mySec, "myisam_strategy", "mysql", {"warn", "lock"}))
            d.options["myisam_strategy"] = *s;
        if (auto s = r.oneOf(mySec, "compression", "mysql", {"zstd", "none"}))
            d.options["compression"] = *s;
        if (auto n = r.integer(mySec, "compression_level", "mysql", 1, 19))
            d.options["compression_level"] = std::to_string(*n);
        d.extraArgs = checkExtraArgs(r, mySec, "mysql", d.engine);
    }
    if (d.engine == "sqlite") {
        if (!liteSec) {
            r.error(doc, "sqlite", "informe sqlite.files");
        } else if (r.requireMap(liteSec, "sqlite")) {
            r.checkKeys(liteSec, "sqlite", {"files", "integrity_check", "compression", "busy_timeout"});
            for (const auto& s : r.strings(liteSec, "files", "sqlite")) {
                std::wstring p = w(s);
                std::replace(p.begin(), p.end(), L'/', L'\\');
                if (isNetworkPath(p))
                    r.error(liteSec["files"], "sqlite.files",
                            "SQLite em compartilhamento de rede não é suportado; instale o Synkro na máquina do arquivo");
                else if (!isAbsoluteWindowsPath(p))
                    r.error(liteSec["files"], "sqlite.files", "use caminhos completos (ex.: \"C:/PDV/dados/caixa.db\")");
                d.sqliteFiles.push_back(p);
            }
            if (d.sqliteFiles.empty())
                r.error(liteSec, "sqlite.files", "informe pelo menos um arquivo");
            if (auto b = r.boolean(liteSec, "integrity_check", "sqlite"))
                d.options["integrity_check"] = *b ? "true" : "false";
            if (auto s = r.oneOf(liteSec, "compression", "sqlite", {"zstd", "none"}))
                d.options["compression"] = *s;
            if (auto t = r.duration(liteSec, "busy_timeout", "sqlite"))
                d.options["busy_timeout"] = std::to_string(*t);
        }
    }
    if (d.engine == "custom") {
        if (!cuSec) {
            r.error(doc, "custom", "informe custom.command");
        } else if (r.requireMap(cuSec, "custom")) {
            r.checkKeys(cuSec, "custom", {"command", "output", "output_name", "success_exit_codes", "env", "validate",
                                          "restore", "compression"});
            d.customCommand = r.strings(cuSec, "command", "custom");
            if (d.customCommand.empty())
                r.error(cuSec, "custom.command", "informe o comando como lista: [\"programa.exe\", \"{output}\"]");
            const std::string output = r.oneOf(cuSec, "output", "custom", {"file", "stdout"}).value_or("file");
            d.options["output"] = output;
            if (output == "file") {
                bool hasPlaceholder = false;
                for (const auto& a : d.customCommand)
                    hasPlaceholder = hasPlaceholder || a.find("{output}") != std::string::npos;
                if (!hasPlaceholder)
                    r.error(cuSec["command"], "custom.command", "com output: file o comando precisa usar {output}");
            }
            if (auto s = r.str(cuSec, "output_name", "custom"))
                d.options["output_name"] = *s;
            if (const YAML::Node env = cuSec["env"]) {
                if (r.requireMap(env, "custom.env")) {
                    for (auto it = env.begin(); it != env.end(); ++it) {
                        const std::string var = it->first.Scalar();
                        d.customEnv[var] = r.secret(env, var.c_str(), "custom.env");
                    }
                }
            }
            if (const YAML::Node codes = cuSec["success_exit_codes"]) {
                d.successExitCodes.clear();
                if (!codes.IsSequence()) {
                    r.error(codes, "custom.success_exit_codes", "esperada uma lista de números");
                } else {
                    for (const auto& c : codes) {
                        try {
                            d.successExitCodes.push_back(std::stoi(c.Scalar()));
                        } catch (...) {
                            r.error(c, "custom.success_exit_codes", "esperado um número inteiro");
                        }
                    }
                }
            }
            if (const YAML::Node val = cuSec["validate"])
                if (val.IsMap())
                    d.customValidate = r.strings(val, "command", "custom.validate");
            if (const YAML::Node res = cuSec["restore"])
                if (res.IsMap())
                    d.customRestore = r.strings(res, "command", "custom.restore");
            for (const char* sub : {"validate", "restore"}) {
                if (const YAML::Node s = cuSec[sub]) {
                    if (r.requireMap(s, std::string("custom.") + sub)) {
                        r.checkKeys(s, std::string("custom.") + sub, {"command"});
                        if (r.strings(s, "command", std::string("custom.") + sub).empty())
                            r.error(s, std::string("custom.") + sub + ".command", "campo obrigatório");
                    }
                }
            }
            if (auto s = r.oneOf(cuSec, "compression", "custom", {"zstd", "none"}))
                d.options["compression"] = *s;
        }
    }

    if (const YAML::Node t = doc["tools"]) {
        if (r.requireMap(t, "tools")) {
            r.checkKeys(t, "tools", {"pg_dump", "pg_dumpall", "pg_restore", "psql", "createdb", "mysqldump", "mariadb-dump",
                                     "mysql", "mariadb", "gbak", "sqlcmd"});
            for (auto it = t.begin(); it != t.end(); ++it) {
                const std::string tool = it->first.Scalar();
                const std::string value = it->second.IsScalar() ? it->second.Scalar() : "";
                if (value.empty() || value == "auto")
                    continue;
                std::wstring p = w(value);
                std::replace(p.begin(), p.end(), L'/', L'\\');
                if (!isAbsoluteWindowsPath(p))
                    r.error(it->second, "tools." + tool, "informe \"auto\" ou o caminho completo do executável");
                d.tools[tool] = p;
            }
        }
    }
    d.staging = r.oneOf(doc, "staging", "", {"local", "first_vault"}).value_or("local");
    if (auto n = r.integer(doc, "dump_retries", "", 0, 5))
        d.dumpRetries = static_cast<int>(*n);
    if (const YAML::Node rem = doc["remote"]) {
        if (r.requireMap(rem, "remote")) {
            r.checkKeys(rem, "remote", {"server_lock"});
            d.serverLock = r.boolean(rem, "server_lock", "remote").value_or(d.remote);
        }
    } else {
        d.serverLock = d.remote;
    }

    if (const YAML::Node ret = doc["retention"]) {
        if (r.requireMap(ret, "retention")) {
            r.checkKeys(ret, "retention", {"keep_last", "keep_daily", "keep_weekly", "keep_monthly"});
            if (auto n = r.integer(ret, "keep_last", "retention", 1, 10000)) d.keepLast = static_cast<int>(*n);
            if (auto n = r.integer(ret, "keep_daily", "retention", 0, 10000)) d.keepDaily = static_cast<int>(*n);
            if (auto n = r.integer(ret, "keep_weekly", "retention", 0, 10000)) d.keepWeekly = static_cast<int>(*n);
            if (auto n = r.integer(ret, "keep_monthly", "retention", 0, 10000)) d.keepMonthly = static_cast<int>(*n);
        }
    }
    if (const YAML::Node g = doc["guards"]) {
        if (r.requireMap(g, "guards")) {
            r.checkKeys(g, "guards", {"on_empty_database", "on_missing_tables", "min_size_ratio"});
            if (auto s = r.oneOf(g, "on_empty_database", "guards", {"block", "warn"}))
                d.blockOnEmptyDatabase = *s == "block";
            if (auto s = r.oneOf(g, "on_missing_tables", "guards", {"block", "warn"}))
                d.blockOnMissingTables = *s == "block";
            if (auto x = r.ratio(g, "min_size_ratio", "guards"))
                d.minSizeRatio = *x;
        }
    }
    if (const YAML::Node rt = doc["restore_test"]) {
        if (r.requireMap(rt, "restore_test")) {
            r.checkKeys(rt, "restore_test", {"enabled", "schedule", "connection", "exact_count_max_rows", "checks"});
            d.restoreTest = r.boolean(rt, "enabled", "restore_test").value_or(false);
            auto& rtc = d.restoreTestConfig;
            rtc.enabled = d.restoreTest;
            if (const YAML::Node s = rt["schedule"])
                rtc.schedule = r.trigger(s, "restore_test.schedule");
            if (const YAML::Node c = rt["connection"]) {
                if (c.IsScalar() && c.Scalar() == "same") {
                    if (d.remote)
                        r.warn(c, "restore_test.connection",
                               "teste de restauração no servidor de produção remoto; prefira um servidor de teste");
                } else if (r.requireMap(c, "restore_test.connection")) {
                    r.checkKeys(c, "restore_test.connection", {"host", "port", "user", "password"});
                    rtc.sameConnection = false;
                    rtc.host = r.str(c, "host", "restore_test.connection", true).value_or("");
                    rtc.port = static_cast<int>(r.integer(c, "port", "restore_test.connection", 1, 65535)
                                                    .value_or(d.engine == "postgresql" ? 5432 : 3306));
                    rtc.user = r.str(c, "user", "restore_test.connection", true).value_or("");
                    rtc.password = r.secret(c, "password", "restore_test.connection");
                }
            }
            if (const YAML::Node checks = rt["checks"]; checks && checks.IsSequence()) {
                for (const auto& ck : checks) {
                    if (ck.IsMap() && ck["sql"])
                        rtc.checks.push_back({ck["name"] ? ck["name"].Scalar() : std::string("verificação"), ck["sql"].Scalar()});
                }
            }
            (void)r.integer(rt, "exact_count_max_rows", "restore_test", 0, 1'000'000'000'000LL);
            if (const YAML::Node checks = rt["checks"]) {
                if (!checks.IsSequence()) {
                    r.error(checks, "restore_test.checks", "esperada uma lista");
                } else {
                    int i = 0;
                    for (const auto& c : checks) {
                        const std::string p = "restore_test.checks[" + std::to_string(i++) + "]";
                        if (r.requireMap(c, p)) {
                            r.checkKeys(c, p, {"name", "sql"});
                            (void)r.str(c, "name", p, true);
                            (void)r.str(c, "sql", p, true);
                        }
                    }
                }
            }
        }
    }
}

} // namespace

// ---- Documentos ----------------------------------------------------------------

namespace {

void parseGlobalImpl(Reader& r, const std::optional<YAML::Node>& doc, GlobalConfig& g)
{
    if (!doc || !r.requireMap(*doc, ""))
        return;

    r.checkKeys(*doc, "", {"version", "instance", "vaults", "defaults", "alerts", "maintenance", "shutdown", "logging"});
    if (!(*doc)["version"])
        r.error(*doc, "version", "campo obrigatório");
    else if (auto v = r.integer(*doc, "version", "", 1, 1000); v && *v != 1)
        r.error((*doc)["version"], "version", "versão de formato não suportada");

    if (const YAML::Node inst = (*doc)["instance"]) {
        if (r.requireMap(inst, "instance")) {
            r.checkKeys(inst, "instance", {"name"});
            g.instanceName = r.str(inst, "name", "instance").value_or("");
        }
    }

    if (const YAML::Node vs = (*doc)["vaults"]) {
        if (!vs.IsSequence()) {
            r.error(vs, "vaults", "esperada uma lista de cofres");
        } else {
            int i = 0;
            std::set<std::string> ids, uuids;
            for (const auto& v : vs) {
                const std::string p = "vaults[" + std::to_string(i++) + "]";
                if (!r.requireMap(v, p))
                    continue;
                r.checkKeys(v, p, {"id", "vault_id", "label", "path", "rotation", "max_absence", "free_space_alert"});
                VaultConfig vc;
                vc.id = r.str(v, "id", p, true).value_or("");
                if (!vc.id.empty() && !isValidJobId(w(vc.id)))
                    r.error(v["id"], join(p, "id"), "use letras minúsculas, números e hífen");
                if (!vc.id.empty() && !ids.insert(vc.id).second)
                    r.error(v["id"], join(p, "id"), "id de cofre repetido");
                vc.vaultId = r.str(v, "vault_id", p, true).value_or("");
                if (!vc.vaultId.empty()) {
                    if (QUuid::fromString(QString::fromStdString(vc.vaultId)).isNull())
                        r.error(v["vault_id"], join(p, "vault_id"), "UUID inválido (veja synkro-worker vault show)");
                    else if (!uuids.insert(QUuid::fromString(QString::fromStdString(vc.vaultId)).toString().toStdString()).second)
                        r.error(v["vault_id"], join(p, "vault_id"), "o mesmo disco aparece duas vezes");
                }
                vc.label = r.str(v, "label", p).value_or(vc.id);
                if (auto s = r.str(v, "path", p)) {
                    vc.path = w(*s);
                    std::replace(vc.path.begin(), vc.path.end(), L'/', L'\\');
                }
                vc.rotation = r.boolean(v, "rotation", p).value_or(false);
                vc.maxAbsenceSeconds = r.duration(v, "max_absence", p).value_or(vc.rotation ? 7 * 86400 : 86400);
                vc.freeSpaceAlert = r.ratio(v, "free_space_alert", p).value_or(0.15);
                g.vaults.push_back(std::move(vc));
            }
        }
    }

    if (const YAML::Node d = (*doc)["defaults"]) {
        if (r.requireMap(d, "defaults")) {
            r.checkKeys(d, "defaults", {"vaults", "min_vaults_ok", "timeout", "schedule_catch_up"});
            g.defaultVaults = r.strings(d, "vaults", "defaults");
            for (const auto& id : g.defaultVaults) {
                if (!g.findVault(id))
                    r.error(d["vaults"], "defaults.vaults", "cofre \"" + id + "\" não está declarado em vaults");
            }
            if (auto s = r.str(d, "min_vaults_ok", "defaults"); s && *s != "all") {
                if (auto n = r.integer(d, "min_vaults_ok", "defaults", 1, 64))
                    g.defaultMinVaultsOk = static_cast<int>(*n);
            }
            if (auto t = r.duration(d, "timeout", "defaults"))
                g.defaultTimeoutSeconds = *t;
            if (auto b = r.boolean(d, "schedule_catch_up", "defaults"))
                g.scheduleCatchUp = *b;
        }
    }

    if (const YAML::Node a = (*doc)["alerts"]) {
        if (r.requireMap(a, "alerts")) {
            r.checkKeys(a, "alerts", {"no_success_for", "tray", "windows_event_log"});
            if (auto t = r.duration(a, "no_success_for", "alerts"))
                g.alertNoSuccessForSeconds = *t;
            g.alertTray = r.boolean(a, "tray", "alerts").value_or(true);
            g.alertEventLog = r.boolean(a, "windows_event_log", "alerts").value_or(true);
        }
    }

    if (const YAML::Node m = (*doc)["maintenance"]) {
        if (r.requireMap(m, "maintenance")) {
            r.checkKeys(m, "maintenance", {"scrub"});
            if (const YAML::Node s = m["scrub"]) {
                if (r.requireMap(s, "maintenance.scrub")) {
                    r.checkKeys(s, "maintenance.scrub", {"schedule", "sample", "repair_from_other_vaults"});
                    if (const YAML::Node sch = s["schedule"])
                        g.scrubSchedule = r.trigger(sch, "maintenance.scrub.schedule");
                    if (auto x = r.ratio(s, "sample", "maintenance.scrub")) {
                        if (*x <= 0)
                            r.error(s["sample"], "maintenance.scrub.sample", "a amostra precisa ser maior que 0%");
                        g.scrubSample = *x;
                    }
                    g.scrubRepair = r.boolean(s, "repair_from_other_vaults", "maintenance.scrub").value_or(true);
                }
            }
        }
    }

    if (const YAML::Node s = (*doc)["shutdown"]) {
        if (r.requireMap(s, "shutdown")) {
            r.checkKeys(s, "shutdown", {"countdown", "allowed_after"});
            if (auto t = r.duration(s, "countdown", "shutdown")) {
                if (*t < 30)
                    r.error(s["countdown"], "shutdown.countdown", "mínimo de 30s para o usuário poder cancelar");
                g.shutdownCountdownSeconds = *t;
            }
            g.shutdownAllowedGroups = r.strings(s, "allowed_after", "shutdown");
        }
    }

    if (const YAML::Node l = (*doc)["logging"]) {
        if (r.requireMap(l, "logging")) {
            r.checkKeys(l, "logging", {"level", "keep"});
            g.logLevel = r.oneOf(l, "level", "logging", {"debug", "info", "warn", "error"}).value_or("info");
            if (auto t = r.duration(l, "keep", "logging"))
                g.logKeepSeconds = *t;
        }
    }
}

void parseJobImpl(Reader& r, const std::optional<YAML::Node>& doc, const GlobalConfig& global, JobConfig& job)
{
    if (!doc || !r.requireMap(*doc, ""))
        return;
    r.secretSink = &job.secrets;
    job.line = doc->Mark().line + 1;

    static const std::initializer_list<const char*> common = {
        "version", "id", "name", "type", "enabled", "group", "vaults", "min_vaults_ok", "schedule",
        "depends_on", "depends_mode", "timeout", "hooks", "shutdown_after"};
    static const std::initializer_list<const char*> filesKeys = {
        "source", "include", "exclude", "consistency", "change_detection", "verify", "retention", "guards"};
    static const std::initializer_list<const char*> dbKeys = {
        "engine", "connection", "databases", "postgresql", "mysql", "sqlite", "custom", "tools", "staging",
        "dump_retries", "remote", "retention", "guards", "restore_test"};

    if (!(*doc)["version"])
        r.error(*doc, "version", "campo obrigatório");
    else if (auto v = r.integer(*doc, "version", "", 1, 1000); v && *v != 1)
        r.error((*doc)["version"], "version", "versão de formato não suportada");

    job.id = r.str(*doc, "id", "", true).value_or("");
    if (!job.id.empty() && !isValidJobId(w(job.id)))
        r.error((*doc)["id"], "id", "use letras minúsculas, números e hífen (ex.: pdv-arquivos)");
    job.name = r.str(*doc, "name", "", true).value_or(job.id);
    job.type = r.oneOf(*doc, "type", "", {"files", "database"}, true).value_or("");

    // Campos permitidos dependem do tipo.
    r.checkDuplicates(*doc, "");
    if (doc->IsMap()) {
        for (auto it = doc->begin(); it != doc->end(); ++it) {
            const std::string key = it->first.Scalar();
            auto in = [&](const std::initializer_list<const char*>& set) {
                return std::any_of(set.begin(), set.end(), [&](const char* k) { return key == k; });
            };
            if (in(common))
                continue;
            if (job.type == "files" && in(filesKeys))
                continue;
            if (job.type == "database" && in(dbKeys))
                continue;
            if (job.type.empty())
                continue;
            r.error(it->first, key,
                    in(filesKeys) || in(dbKeys) ? "campo não se aplica a tarefas do tipo " + job.type : "campo desconhecido");
        }
    }

    job.enabled = r.boolean(*doc, "enabled", "").value_or(true);
    job.group = r.str(*doc, "group", "").value_or("");
    job.vaults = r.strings(*doc, "vaults", "");
    if (job.vaults.empty())
        job.vaults = global.defaultVaults;
    if (auto s = r.str(*doc, "min_vaults_ok", ""); s && *s != "all") {
        if (auto n = r.integer(*doc, "min_vaults_ok", "", 1, 64))
            job.minVaultsOk = static_cast<int>(*n);
    } else if (!s) {
        job.minVaultsOk = global.defaultMinVaultsOk;
    }
    job.schedule = r.schedule(*doc, "schedule", "");
    job.dependsOn = r.strings(*doc, "depends_on", "");
    job.dependsOnSuccess = r.oneOf(*doc, "depends_mode", "", {"always", "on_success"}).value_or("always") == "on_success";
    job.timeoutSeconds = r.duration(*doc, "timeout", "").value_or(global.defaultTimeoutSeconds);
    job.shutdownAfter = r.boolean(*doc, "shutdown_after", "").value_or(false);

    if (const YAML::Node h = (*doc)["hooks"]) {
        if (r.requireMap(h, "hooks")) {
            r.checkKeys(h, "hooks", {"before", "after", "on_success", "on_failure"});
            r.hookList(h, "before", "hooks", job.hooks.before);
            r.hookList(h, "after", "hooks", job.hooks.after);
            r.hookList(h, "on_success", "hooks", job.hooks.onSuccess);
            r.hookList(h, "on_failure", "hooks", job.hooks.onFailure);
        }
    }

    if (job.type == "files")
        parseFilesJob(r, *doc, job);
    else if (job.type == "database")
        parseDatabaseJob(r, *doc, job);

    // Cofres referenciados.
    if (job.vaults.empty())
        r.error(*doc, "vaults", "nenhum cofre definido (nem na tarefa, nem em defaults.vaults)");
    std::set<std::string> seen;
    for (const auto& v : job.vaults) {
        if (!global.findVault(v))
            r.error((*doc)["vaults"] ? (*doc)["vaults"] : *doc, "vaults", "cofre \"" + v + "\" não está declarado em synkro.yml");
        if (!seen.insert(v).second)
            r.error((*doc)["vaults"], "vaults", "cofre \"" + v + "\" repetido");
    }
    if (job.minVaultsOk > static_cast<int>(job.vaults.size()))
        r.error((*doc)["min_vaults_ok"], "min_vaults_ok", "maior que o número de cofres da tarefa");
    if (job.shutdownAfter && !job.group.empty() && !global.shutdownAllowedGroups.empty()
        && std::find(global.shutdownAllowedGroups.begin(), global.shutdownAllowedGroups.end(), job.group)
               == global.shutdownAllowedGroups.end())
        r.warn((*doc)["shutdown_after"], "shutdown_after", "o grupo \"" + job.group + "\" não está em shutdown.allowed_after");
}

} // namespace

GlobalConfig parseGlobalDocument(const std::string& yaml, const std::wstring& fileName, std::vector<Diagnostic>& diags)
{
    GlobalConfig g;
    Reader r(fileName, diags);
    try {
        parseGlobalImpl(r, parseYaml(yaml, r), g);
    } catch (const YAML::Exception& e) {
        r.errorAtLine(e.mark.line + 1, {}, "estrutura inválida: " + e.msg);
    }
    return g;
}

JobConfig parseJobDocument(const std::string& yaml, const std::wstring& fileName, const GlobalConfig& global,
                           std::vector<Diagnostic>& diags)
{
    JobConfig job;
    job.file = fileName;
    Reader r(fileName, diags);
    try {
        parseJobImpl(r, parseYaml(yaml, r), global, job);
    } catch (const YAML::Exception& e) {
        r.errorAtLine(e.mark.line + 1, {}, "estrutura inválida: " + e.msg);
    }
    return job;
}

// ---- Conjunto completo -------------------------------------------------------

namespace {

void checkDependencies(ConfigSet& set)
{
    std::map<std::string, const JobConfig*> byId;
    for (const auto& j : set.jobs)
        byId[j.id] = &j;

    for (const auto& j : set.jobs) {
        for (const auto& dep : j.dependsOn) {
            if (!byId.count(dep))
                set.diagnostics.push_back({Severity::Error, j.file, j.line, 1, "depends_on",
                                           "a tarefa \"" + dep + "\" não existe"});
            if (dep == j.id)
                set.diagnostics.push_back({Severity::Error, j.file, j.line, 1, "depends_on",
                                           "a tarefa não pode depender de si mesma"});
        }
    }

    // Ciclos (DFS).
    std::map<std::string, int> state; // 0 = novo, 1 = visitando, 2 = concluído
    std::function<bool(const std::string&, std::vector<std::string>&)> dfs =
        [&](const std::string& id, std::vector<std::string>& stack) -> bool {
        state[id] = 1;
        stack.push_back(id);
        auto it = byId.find(id);
        if (it != byId.end()) {
            for (const auto& dep : it->second->dependsOn) {
                if (!byId.count(dep) || dep == id)
                    continue;
                if (state[dep] == 1) {
                    stack.push_back(dep);
                    return true;
                }
                if (state[dep] == 0 && dfs(dep, stack))
                    return true;
            }
        }
        stack.pop_back();
        state[id] = 2;
        return false;
    };
    for (const auto& j : set.jobs) {
        std::vector<std::string> stack;
        if (state[j.id] == 0 && dfs(j.id, stack)) {
            std::string chain;
            for (const auto& s : stack)
                chain += (chain.empty() ? "" : " -> ") + s;
            set.diagnostics.push_back({Severity::Error, j.file, j.line, 1, "depends_on", "dependência circular: " + chain});
            break;
        }
    }
}

std::vector<std::uint32_t> physicalDisksOf(const std::wstring& path)
{
    auto v = win::queryVolume(path);
    return v ? v.value().physicalDisks : std::vector<std::uint32_t>{};
}

bool overlaps(const std::vector<std::uint32_t>& a, const std::vector<std::uint32_t>& b)
{
    for (auto x : a) {
        if (std::find(b.begin(), b.end(), x) != b.end())
            return true;
    }
    return false;
}

void checkPhysicalDisks(ConfigSet& set)
{
    std::map<std::string, std::optional<vault::LocatedVault>> located;
    for (const auto& v : set.global.vaults) {
        if (v.vaultId.empty())
            continue;
        located[v.id] = vault::locateVault(QString::fromStdString(v.vaultId), v.path);
        if (!located[v.id])
            set.diagnostics.push_back({Severity::Warning, L"synkro.yml", 0, 0, "vaults." + v.id,
                                       "cofre \"" + v.label + "\" não está conectado agora; a checagem de disco "
                                       "físico será feita na execução"});
    }

    for (const auto& j : set.jobs) {
        std::vector<std::pair<std::string, std::vector<std::uint32_t>>> vaultDisks;
        for (const auto& vid : j.vaults) {
            const auto it = located.find(vid);
            if (it == located.end())
                continue;
            if (!it->second)
                continue; // já avisado uma vez, em synkro.yml
            vaultDisks.emplace_back(vid, physicalDisksOf(it->second->location));
        }

        std::vector<std::uint32_t> sourceDisks;
        if (j.type == "files" && !j.files.source.empty() && ops::pathExists(j.files.source))
            sourceDisks = physicalDisksOf(j.files.source);
        else if (j.type == "files" && !j.files.source.empty())
            set.diagnostics.push_back({Severity::Warning, j.file, j.line, 1, "source",
                                       "a pasta de origem não existe nesta máquina"});

        for (size_t a = 0; a < vaultDisks.size(); ++a) {
            if (!sourceDisks.empty() && overlaps(vaultDisks[a].second, sourceDisks))
                set.diagnostics.push_back({Severity::Error, j.file, j.line, 1, "vaults",
                                           "o cofre \"" + vaultDisks[a].first
                                               + "\" está no mesmo disco físico da origem"});
            for (size_t b = a + 1; b < vaultDisks.size(); ++b) {
                if (overlaps(vaultDisks[a].second, vaultDisks[b].second))
                    set.diagnostics.push_back({Severity::Error, j.file, j.line, 1, "vaults",
                                               "\"" + vaultDisks[a].first + "\" e \"" + vaultDisks[b].first
                                                   + "\" estão no mesmo disco físico"});
            }
        }
    }
}

} // namespace

ConfigSet loadConfig(const std::wstring& directory, const LoadOptions& options)
{
    ConfigSet set;
    set.directory = directory;

    auto global = win::readWholeFile(win::joinPath(directory, L"synkro.yml"), 4u << 20);
    if (!global) {
        set.diagnostics.push_back({Severity::Error, L"synkro.yml", 0, 0, {},
                                   "não foi possível ler: " + global.error().message});
        return set;
    }
    set.global = parseGlobalDocument(global.value(), L"synkro.yml", set.diagnostics);

    const std::wstring jobsDir = win::joinPath(directory, L"jobs");
    std::set<std::string> ids;
    for (const auto& name : ops::listFiles(jobsDir, L".yml")) {
        const std::wstring rel = L"jobs\\" + name;
        auto text = win::readWholeFile(win::joinPath(jobsDir, name), 4u << 20);
        if (!text) {
            set.diagnostics.push_back({Severity::Error, rel, 0, 0, {}, "não foi possível ler: " + text.error().message});
            continue;
        }
        JobConfig job = parseJobDocument(text.value(), rel, set.global, set.diagnostics);
        if (!job.id.empty()) {
            if (!ids.insert(job.id).second)
                set.diagnostics.push_back({Severity::Error, rel, job.line, 1, "id", "id de tarefa repetido: " + job.id});
            const std::wstring expected = win::fromUtf8(job.id) + L".yml";
            if (win::foldCase(name) != win::foldCase(expected))
                set.diagnostics.push_back({Severity::Warning, rel, job.line, 1, "id",
                                           "recomenda-se que o arquivo se chame " + win::toUtf8(expected)});
        }
        set.jobs.push_back(std::move(job));
    }

    checkDependencies(set);
    if (options.checkPhysicalDisks)
        checkPhysicalDisks(set);

    // Segredos referenciados precisam estar cadastrados (só os nomes são lidos).
    if (options.checkSecrets) {
        const SecretStore store(options.secretsFile.empty() ? secretsFile() : options.secretsFile);
        for (const auto& j : set.jobs) {
            for (const auto& s : j.secrets) {
                if (!store.contains(s.name))
                    set.diagnostics.push_back({Severity::Warning, j.file, s.line, 1, s.key,
                                               "segredo \"" + s.name + "\" não cadastrado nesta máquina; cadastre com: "
                                               "synkro-worker secret set " + s.name});
            }
        }
    }
    return set;
}

} // namespace synkro::config
