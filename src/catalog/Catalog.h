#pragma once

#include "core/Result.h"
#include "hash/Sha256.h"
#include "scan/FileScanner.h"

#include <QJsonObject>

#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace synkro {

// Um arquivo presente em current\ do cofre.
struct CatalogEntry {
    FileEntry meta;            // metadados da ORIGEM no momento da cópia
    Sha256Digest sha256{};
    std::wstring sinceRun;     // execução que gravou esta versão
};

QJsonObject toJson(const CatalogEntry& entry);
Result<CatalogEntry> catalogEntryFromJson(const QJsonObject& o);

std::string toHexId(const FileId& id);
std::optional<Sha256Digest> parseSha256Hex(const QString& hex);

// catalog.jsonl: estado completo da última execução bem-sucedida.
//
// Formato (JSON Lines, legível sem o Synkro):
//   linha 1 : {"format":"synkro-catalog","format_version":1,"run_id":...,"count":N}
//   linhas  : um CatalogEntry por linha, ordenado por caminho
//
// Ao lado fica catalog.jsonl.sha256 com o hash do arquivo, usado para
// detectar corrupção.
class Catalog {
public:
    std::wstring runId;                 // execução que produziu este catálogo
    std::vector<CatalogEntry> entries;

    // Índice por caminho (sem diferenciar maiúsculas). Reconstruído por reindex().
    void reindex();
    const CatalogEntry* find(const std::wstring& catalogPath) const;
    CatalogEntry* find(const std::wstring& catalogPath);

    // Substitui/insere/remove mantendo o índice.
    void upsert(CatalogEntry entry);
    bool remove(const std::wstring& catalogPath);

    void sort();

    QByteArray serialize() const;
    static Result<Catalog> parse(const QByteArray& data);

    // Grava catalog.jsonl e catalog.jsonl.sha256 de forma atômica.
    Status save(const std::wstring& path) const;

    // NotFound se o catálogo ainda não existe (primeira execução);
    // Corrupted se o hash não confere ou o conteúdo é inválido.
    static Result<Catalog> load(const std::wstring& path);

private:
    std::unordered_map<std::wstring, std::size_t> m_index;
};

} // namespace synkro
