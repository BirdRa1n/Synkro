#pragma once

// Utilitários compartilhados pelos comandos do synkro-worker.

#include "config/Config.h"
#include "core/Result.h"
#include "engine/BackupEngine.h"

#include <QCommandLineParser>
#include <QString>
#include <QStringList>
#include <QTextStream>

#include <string>

namespace cli {

// Códigos de saída documentados (README e architecture.md §10.2).
enum ExitCode : int {
    ExitOk = 0,
    ExitDegraded = 1,
    ExitFailure = 2,
    ExitBlocked = 3,
    ExitAlreadyRunning = 4,
    ExitUsage = 64,
};

QTextStream& out();
QTextStream& err();

std::wstring nativeArg(const QString& path);
inline QString q(const std::wstring& s) { return QString::fromStdWString(s); }
inline QString q(const std::string& s) { return QString::fromStdString(s); }
QString humanBytes(std::uint64_t bytes);

int fail(const synkro::Error& e);
void usage();
bool parseSub(QCommandLineParser& p, const QStringList& args);

std::wstring configDirFrom(const QCommandLineParser& p);
bool loadAndReport(const std::wstring& dir, synkro::config::ConfigSet& set, bool quietWhenClean);

void printBackupResult(const synkro::BackupResult& r);
int exitFor(synkro::RunStatus s);

// Aceita a raiz de um disco ("E:\") ou uma pasta que contenha SynkroVault.
std::wstring vaultLocationArg(const QString& v);

struct RunOptions {
    std::string trigger = "manual";
    bool overrideGuards = false;
    bool allowShutdown = true;
};

// Executa uma tarefa da configuração (files ou database) e faz o
// pós-processamento: histórico, contato com os cofres, alertas e
// desligamento.
int runConfiguredJob(const synkro::config::ConfigSet& set, const synkro::config::JobConfig& job, const RunOptions& options);

// Comandos de operação (OpsCommands.cpp).
int cmdStatus(const QStringList& args);
int cmdMaintenance(const QStringList& args);
int cmdSecret(const QStringList& args);
int cmdRestoreDb(const QStringList& args);
int cmdRestoreTest(const QStringList& args);
int cmdDumps(const QStringList& args);
int cmdSchedule(const QStringList& args);
int cmdShutdown(const QStringList& args);
int cmdConfigApply(const QStringList& args);

} // namespace cli
