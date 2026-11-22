#pragma once

#include "catalog/Catalog.h"

#include <optional>
#include <string>
#include <vector>

namespace synkro {

enum class ManifestOp { Added, Modified, Deleted, Failed, Renamed };

const char* nameOf(ManifestOp op) noexcept;

// Uma mudança aplicada (ou tentada) numa execução.
//
// - Added:    `entry` é o arquivo novo em current
// - Modified: `entry` é a nova versão; `previous` foi movido para versions/<run>
// - Deleted:  `previous` foi movido para versions/<run>
// - Failed:   `error` descreve o problema; nada mudou para esse caminho
// - Renamed:  o arquivo mudou de lugar no cofre sem ser copiado (reorganização
//             em subpastas): `previous` tem o caminho antigo, `entry`/`path` o novo
struct ManifestRecord {
    ManifestOp op = ManifestOp::Added;
    std::wstring path;
    std::optional<CatalogEntry> entry;
    std::optional<CatalogEntry> previous;
    std::string error;
};

QJsonObject toJson(const ManifestRecord& record);
Result<ManifestRecord> manifestRecordFromJson(const QJsonObject& o);

// runs\<run-id>.manifest.jsonl — o que mudou em cada execução. Junto com o
// catálogo, permite reconstruir o estado da origem em qualquer execução retida
// (restauração point-in-time).
struct Manifest {
    std::wstring runId;
    std::vector<ManifestRecord> records;

    QByteArray serialize() const;
    static Result<Manifest> parse(const QByteArray& data);

    Status save(const std::wstring& path) const;
    static Result<Manifest> load(const std::wstring& path);
};

} // namespace synkro
