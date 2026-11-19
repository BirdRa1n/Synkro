#pragma once

// Modelo da configuração carregada dos YAMLs (docs/configuration.md).
// Os valores já vêm validados e com os padrões aplicados.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace synkro::config {

enum class Severity { Error, Warning };

struct Diagnostic {
    Severity severity = Severity::Error;
    std::wstring file;    // relativo à pasta de configuração
    int line = 0;         // 1-based; 0 = arquivo inteiro
    int column = 0;
    std::string key;      // caminho do campo: "connection.password"
    std::string message;  // pt-BR
};

// ---- Agendamento -------------------------------------------------------------

enum class TriggerKind { Daily, Weekly, Monthly, Every, OnStartup, OnLogon, Manual };

struct ScheduleTrigger {
    TriggerKind kind = TriggerKind::Manual;
    int atMinutes = 0;              // daily/weekly/monthly
    std::vector<int> days;          // weekly/every (1 = segunda)
    int dayOfMonth = 0;             // monthly: 1..28; -1 = último dia
    std::int64_t intervalSeconds = 0; // every
    int betweenStart = -1, betweenEnd = -1; // every (minutos); -1 = o dia todo
    std::int64_t delaySeconds = 0;  // on_startup / on_logon
};

// ---- Segredos ----------------------------------------------------------------

struct SecretRef {
    enum class Kind { None, Secret, Env } kind = Kind::None;
    std::string name;
};

// Onde um segredo é referenciado (para validar se está cadastrado).
struct SecretUse {
    std::string key;
    std::string name;
    int line = 0;
};

// ---- Cofres ------------------------------------------------------------------

struct VaultConfig {
    std::string id;
    std::string vaultId;      // UUID do vault.json
    std::string label;
    std::wstring path;        // opcional: dica de localização (ex.: "E:\")
    bool rotation = false;
    std::int64_t maxAbsenceSeconds = 7 * 86400;
    double freeSpaceAlert = 0.15;
};

// ---- Geral (synkro.yml) ------------------------------------------------------

struct GlobalConfig {
    std::string instanceName;
    std::vector<VaultConfig> vaults;

    std::vector<std::string> defaultVaults;
    int defaultMinVaultsOk = 0;           // 0 = todos
    std::int64_t defaultTimeoutSeconds = 4 * 3600;
    bool scheduleCatchUp = true;

    std::int64_t alertNoSuccessForSeconds = 26 * 3600;
    bool alertTray = true;
    bool alertEventLog = true;

    std::optional<ScheduleTrigger> scrubSchedule;
    double scrubSample = 0.10;
    bool scrubRepair = true;

    std::int64_t shutdownCountdownSeconds = 120;
    std::vector<std::string> shutdownAllowedGroups;

    std::string logLevel = "info";
    std::int64_t logKeepSeconds = 30 * 86400;

    const VaultConfig* findVault(const std::string& id) const;
};

// ---- Tarefas -----------------------------------------------------------------

struct HookCommand {
    std::vector<std::string> run;
    std::int64_t timeoutSeconds = 300;
    bool failOnError = true;      // on_error: fail | warn
};

struct Hooks {
    std::vector<HookCommand> before, after, onSuccess, onFailure;
};

struct FilesJob {
    std::wstring source;
    std::vector<std::wstring> include, exclude;
    bool vss = true;
    bool continueWithoutSnapshot = true;
    int fullHashEvery = 0;
    bool verifyUncached = false;
    std::int64_t keepVersionsSeconds = 90 * 86400;
    int keepMinVersions = 3;
    std::int64_t keepDeletedSeconds = 180 * 86400;
    double maxDeleted = 0.20;
    double maxModified = 0.50;
    std::wstring sentinelFile;
};

struct DatabaseEntry {
    std::string name;
    std::vector<std::string> excludeTables;
};

struct RestoreCheck {
    std::string name;
    std::string sql; // precisa devolver verdadeiro (t, true, 1) na primeira coluna
};

struct RestoreTestConfig {
    bool enabled = false;
    std::optional<ScheduleTrigger> schedule; // padrão: domingo 04:00
    bool sameConnection = true;
    std::string host;
    int port = 0;
    std::string user;
    SecretRef password;
    std::vector<RestoreCheck> checks;
};

struct DatabaseJob {
    std::string engine;           // postgresql | mysql | mariadb | sqlite | custom
    std::string host;
    int port = 0;
    std::string user;
    SecretRef password;
    std::string sslMode;
    std::wstring sslCa;
    bool remote = false;
    bool allowInsecure = false;
    std::int64_t connectTimeoutSeconds = 15;
    int retries = 3;
    int dumpRetries = 1;
    bool allDatabases = false;
    std::vector<DatabaseEntry> databases;
    std::vector<std::wstring> sqliteFiles;
    std::vector<std::string> customCommand;
    std::vector<std::string> customValidate;
    std::vector<std::string> customRestore;
    std::map<std::string, SecretRef> customEnv;
    std::vector<int> successExitCodes{0};
    std::map<std::string, std::wstring> tools; // ferramenta -> caminho explícito
    std::vector<std::string> extraArgs;
    std::string staging = "local";
    bool serverLock = false;
    // Retenção GFS
    int keepLast = 7, keepDaily = 14, keepWeekly = 8, keepMonthly = 12;
    bool blockOnEmptyDatabase = true;
    bool blockOnMissingTables = false;
    double minSizeRatio = 0.5;
    bool restoreTest = false;
    RestoreTestConfig restoreTestConfig;
    std::map<std::string, std::string> options; // opções específicas do motor, já validadas
};

struct JobConfig {
    std::wstring file;
    int line = 1;

    std::string id;
    std::string name;
    std::string type;             // files | database
    bool enabled = true;
    std::string group;
    std::vector<std::string> vaults;
    int minVaultsOk = 0;          // 0 = todos
    std::vector<ScheduleTrigger> schedule;
    std::vector<std::string> dependsOn;
    bool dependsOnSuccess = false;
    std::int64_t timeoutSeconds = 0;
    Hooks hooks;
    bool shutdownAfter = false;

    FilesJob files;
    DatabaseJob database;
    std::vector<SecretUse> secrets;
};

struct ConfigSet {
    std::wstring directory;
    GlobalConfig global;
    std::vector<JobConfig> jobs;
    std::vector<Diagnostic> diagnostics;

    bool hasErrors() const;
    std::size_t errorCount() const;
    std::size_t warningCount() const;
    const JobConfig* findJob(const std::string& id) const;
};

} // namespace synkro::config
