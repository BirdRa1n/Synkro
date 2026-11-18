#pragma once

#include "database/Drivers.h"
#include "vault/JobLayout.h"

#include <QDateTime>

#include <string>
#include <vector>

namespace synkro::db {

// Metadados de um dump guardado no cofre (<dump>.dump.json), suficientes
// para entender e restaurar o arquivo sem o Synkro (database-backup.md §5.1).
struct DumpRecord {
    std::wstring jobId;
    std::wstring runId;
    std::string engine;
    std::string database;
    std::wstring relativePath;    // a partir da pasta da tarefa: dumps/<db>/<arquivo>
    std::string dumpFormat;
    std::string compression;
    std::uint64_t size = 0;
    Sha256Digest sha256{};
    QDateTime startedAt, finishedAt;
    std::string host;
    std::string toolPath, toolVersion;
    std::string command;
    std::string validationMethod, validationResult;
    bool tablesKnown = false;
    std::vector<std::string> tables;
    std::vector<std::string> warnings;
    bool suspicious = false;      // ex.: muito menor que o anterior
    std::string restoreHint;
    std::string createdByHost;

    QByteArray serialize() const;
    static Result<DumpRecord> parse(const QByteArray& data);
};

// Caminho do dump no cofre: dumps/<db>/<AAAA-MM-DDTHHMMSSZ>_<run><ext>.
std::wstring dumpRelativePath(const std::string& database, const QDateTime& startedUtc, const std::wstring& runId,
                              const std::string& extension);

// Metadados ao lado do dump: <stamp>_<run>.dump.json (o nome do dump sem extensão).
std::wstring metadataPathFor(const std::wstring& dumpPath);

// Todos os dumps de uma tarefa num cofre, do mais novo para o mais antigo,
// lidos dos .dump.json (que são a fonte da verdade; dumps.jsonl é só índice).
std::vector<DumpRecord> listDumps(const vault::JobLayout& layout, const std::string& database = {});

// Regrava jobs/<id>/dumps.jsonl a partir dos .dump.json.
Status rewriteDumpIndex(const vault::JobLayout& layout);

struct GfsPolicy {
    int keepLast = 7;
    int keepDaily = 14;
    int keepWeekly = 8;
    int keepMonthly = 12;
};

// Índices (em `newestFirst`) que a política avô-pai-filho mantém. O dump
// válido mais recente é sempre mantido.
std::vector<std::size_t> selectGfs(const std::vector<QDateTime>& newestFirst, const GfsPolicy& policy,
                                   const QDateTime& nowLocal);

struct DumpRetentionResult {
    std::size_t removed = 0;
    std::uint64_t bytesFreed = 0;
};

// Aplica a retenção GFS aos dumps de um banco num cofre.
Result<DumpRetentionResult> applyDumpRetention(const vault::JobLayout& layout, const std::string& database,
                                               const GfsPolicy& policy, const QDateTime& nowLocal);

} // namespace synkro::db
