#include "vault/VaultCandidateCheck.h"

#include "hash/Sha256.h"
#include "platform/windows/DiskHealth.h"
#include "platform/windows/NetworkShare.h"
#include "platform/windows/Win32.h"
#include "vault/Vault.h"

#include <QUuid>

#include <algorithm>
#include <cstdio>
#include <set>

namespace synkro::vault {

const char* nameOf(CheckSeverity severity) noexcept
{
    switch (severity) {
    case CheckSeverity::Ok:       return "ok";
    case CheckSeverity::Info:     return "info";
    case CheckSeverity::Warning:  return "warning";
    case CheckSeverity::Blocking: return "blocking";
    }
    return "unknown";
}

bool CandidateReport::accepted() const noexcept
{
    return std::none_of(findings.begin(), findings.end(),
                        [](const CheckFinding& f) { return f.severity == CheckSeverity::Blocking; });
}

bool CandidateReport::hasWarnings() const noexcept
{
    return std::any_of(findings.begin(), findings.end(),
                       [](const CheckFinding& f) { return f.severity == CheckSeverity::Warning; });
}

namespace {

std::string disksToString(const std::vector<std::uint32_t>& disks)
{
    std::string out;
    for (auto d : disks) {
        if (!out.empty())
            out += ", ";
        out += "Disco " + std::to_string(d);
    }
    return out.empty() ? "desconhecido" : out;
}

std::string gib(std::uint64_t bytes)
{
    const double v = static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f GB", v);
    return buf;
}

// Discos físicos de uma lista de caminhos (ignora os que não puderem ser lidos).
std::set<std::uint32_t> disksOf(const std::vector<std::wstring>& paths, std::vector<std::wstring>& unreadable)
{
    std::set<std::uint32_t> disks;
    for (const auto& p : paths) {
        auto v = win::queryVolume(p);
        if (!v || v.value().physicalDisks.empty()) {
            unreadable.push_back(p);
            continue;
        }
        disks.insert(v.value().physicalDisks.begin(), v.value().physicalDisks.end());
    }
    return disks;
}

bool overlaps(const std::vector<std::uint32_t>& a, const std::set<std::uint32_t>& b)
{
    return std::any_of(a.begin(), a.end(), [&](std::uint32_t d) { return b.count(d) > 0; });
}

Status writeTest(const std::wstring& mountPoint)
{
    const std::wstring path = win::joinPath(
        mountPoint, L"synkro-write-test-" + QUuid::createUuid().toString(QUuid::WithoutBraces).toStdWString() + L".tmp");

    // 1 MiB de conteúdo conhecido, gravado, sincronizado e relido sem cache.
    std::string payload(1u << 20, '\0');
    for (size_t i = 0; i < payload.size(); ++i)
        payload[i] = static_cast<char>((i * 2654435761u) >> 24);
    auto expected = Sha256::ofBytes(payload);

    auto st = win::writeFileAtomically(path, payload);
    if (!st)
        return st;
    auto actual = Sha256::ofFile(path, /*bypassCache=*/true);
    ::DeleteFileW(win::toExtendedPath(path).c_str());
    if (!actual)
        return actual.error();
    if (actual.value() != expected.value())
        return Error::make(ErrorCode::VerificationFailed, "o conteúdo relido difere do gravado");
    return Status::success();
}

} // namespace

CandidateReport checkCandidate(const CandidateRequest& req)
{
    CandidateReport report;
    auto add = [&](CheckId id, CheckSeverity sev, std::string title, std::string detail) {
        report.findings.push_back({id, sev, std::move(title), std::move(detail)});
    };

    if (win::isUncPath(req.candidatePath)) {
        if (auto st = win::probeSmbServer(win::uncServer(req.candidatePath)); !st) {
            add(CheckId::VolumeReadable, CheckSeverity::Blocking, "Servidor inacessível", st.error().message);
            return report;
        }
    }
    auto volume = win::queryVolume(req.candidatePath);
    if (!volume) {
        add(CheckId::VolumeReadable, CheckSeverity::Blocking, "Disco inacessível", volume.error().message);
        return report;
    }
    report.volume = volume.value();
    const win::VolumeInfo& v = report.volume;
    add(CheckId::VolumeReadable, CheckSeverity::Ok,
        v.kind == win::DriveKind::Network ? "Compartilhamento acessível" : "Disco acessível",
        win::toUtf8(v.mountPoint) + " · "
            + (v.kind == win::DriveKind::Network ? std::string("compartilhamento de rede") : disksToString(v.physicalDisks)));

    const bool network = v.kind == win::DriveKind::Network;
    const bool unc = win::isUncPath(req.candidatePath);

    // 1. Disco físico diferente das origens (em rede: outra máquina).
    if (network && !unc) {
        add(CheckId::DifferentDiskFromSources, CheckSeverity::Blocking, "Unidade de rede mapeada",
            "A letra de unidade só existe na sessão do usuário; o serviço do Synkro (SYSTEM) não a enxerga. "
            "Use o caminho UNC, ex.: \\\\servidor\\backup\\pdv01.");
    } else if (network) {
        const std::wstring server = win::uncServer(req.candidatePath);
        if (win::isThisComputer(server)) {
            add(CheckId::DifferentDiskFromSources, CheckSeverity::Blocking, "Compartilhamento desta própria máquina",
                "Um cofre aqui se perde junto com este computador. Use um servidor ou NAS diferente.");
        } else {
            add(CheckId::DifferentDiskFromSources, CheckSeverity::Ok, "Cofre em outra máquina",
                "Servidor " + win::toUtf8(server) + ": protege contra a perda deste PDV inteiro (defeito, roubo, "
                "incêndio), mas depende da rede e do servidor.");
            bool sameServer = false;
            for (const auto& other : req.otherVaults)
                sameServer = sameServer || (win::isUncPath(other) && win::foldCase(win::uncServer(other)) == win::foldCase(server));
            if (!req.otherVaults.empty())
                add(CheckId::DifferentDiskFromOtherVaults, sameServer ? CheckSeverity::Warning : CheckSeverity::Ok,
                    sameServer ? "Mesmo servidor de outro cofre" : "Diferente dos outros cofres",
                    sameServer ? "Os dois cofres caem juntos com o servidor; prefira outro servidor ou um disco local."
                               : "");
        }
    } else if (v.physicalDisks.empty()) {
        add(CheckId::DifferentDiskFromSources, CheckSeverity::Warning, "Disco físico não identificado",
            "Não foi possível confirmar que o cofre está num disco diferente da origem.");
    } else {
        std::vector<std::wstring> unreadable;
        const auto sourceDisks = disksOf(req.sourcePaths, unreadable);
        if (overlaps(v.physicalDisks, sourceDisks))
            add(CheckId::DifferentDiskFromSources, CheckSeverity::Blocking, "Mesmo disco físico da origem",
                "Outra partição do mesmo disco é perdida junto com ele. Escolha um disco diferente.");
        else if (!unreadable.empty())
            add(CheckId::DifferentDiskFromSources, CheckSeverity::Warning, "Origem não verificada",
                "Não foi possível ler o disco de uma das origens.");
        else
            add(CheckId::DifferentDiskFromSources, CheckSeverity::Ok, "Disco físico diferente da origem",
                disksToString(v.physicalDisks));

        // 2. Disco físico diferente dos outros cofres.
        if (!req.otherVaults.empty()) {
            std::vector<std::wstring> unreadableVaults;
            const auto vaultDisks = disksOf(req.otherVaults, unreadableVaults);
            if (overlaps(v.physicalDisks, vaultDisks))
                add(CheckId::DifferentDiskFromOtherVaults, CheckSeverity::Blocking,
                    "Mesmo disco físico de outro cofre",
                    "Dois cofres no mesmo disco não são redundância. Escolha outro disco.");
            else
                add(CheckId::DifferentDiskFromOtherVaults, CheckSeverity::Ok, "Diferente dos outros cofres", "");
        }
    }

    // 3. Não é o volume do sistema.
    if (v.isSystemVolume)
        add(CheckId::NotSystemVolume, CheckSeverity::Blocking, "Volume do Windows",
            "O cofre não pode ficar no mesmo volume do sistema operacional.");

    // 4. Sistema de arquivos.
    const std::wstring fs = win::foldCase(v.fileSystem);
    if (fs == L"NTFS" || fs == L"REFS")
        add(CheckId::FileSystem, CheckSeverity::Ok, "Sistema de arquivos " + win::toUtf8(v.fileSystem), "");
    else if (network)
        add(CheckId::FileSystem, CheckSeverity::Warning, "Sistema de arquivos " + win::toUtf8(v.fileSystem) + " no servidor",
            "O servidor não se apresenta como NTFS; o teste de gravação abaixo confere se a cópia funciona.");
    else if (fs == L"EXFAT")
        add(CheckId::FileSystem, CheckSeverity::Warning, "Sistema de arquivos exFAT",
            "Funciona, mas não tem journaling: remover o disco sem ejetar pode corromper dados. NTFS é recomendado.");
    else
        add(CheckId::FileSystem, CheckSeverity::Blocking, "Sistema de arquivos " + win::toUtf8(v.fileSystem),
            "Não suportado (FAT32 não aceita arquivos acima de 4 GB). Formate o disco em NTFS.");

    // 5. Espaço livre.
    if (req.estimatedSourceBytes > 0) {
        const auto needed = static_cast<std::uint64_t>(static_cast<double>(req.estimatedSourceBytes) * req.spaceFactor);
        if (v.freeBytes >= needed)
            add(CheckId::FreeSpace, CheckSeverity::Ok, "Espaço suficiente",
                gib(v.freeBytes) + " livres; estimado " + gib(needed));
        else
            add(CheckId::FreeSpace, CheckSeverity::Warning, "Pouco espaço livre",
                gib(v.freeBytes) + " livres; recomendado " + gib(needed));
    }

    // Em rede, o cofre fica na pasta indicada (o compartilhamento costuma ser de vários PDVs).
    const std::wstring location = !req.vaultLocation.empty() ? req.vaultLocation
                                  : unc                        ? win::normalizeUnc(req.candidatePath)
                                                               : v.mountPoint;

    // 6. Cofre já existente.
    auto existing = readIdentity(location);
    if (existing)
        add(CheckId::ExistingVault, CheckSeverity::Info, "Já contém um cofre do Synkro",
            "Cofre \"" + existing.value().label.toStdString() + "\" (" + existing.value().vaultId.toStdString()
                + "). Ele será reaproveitado, não apagado.");

    // 7. Teste de gravação e releitura sem cache.
    if (req.runWriteTest) {
        if (auto st = writeTest(location); st)
            add(CheckId::WriteTest, CheckSeverity::Ok, "Teste de gravação e leitura",
                "Gravou 1 MB, releu sem cache e conferiu o SHA-256.");
        else
            add(CheckId::WriteTest, CheckSeverity::Blocking, "Falha no teste de gravação", st.error().message);
    }

    // 8. Saúde do disco físico (SMART, via WMI).
    if (!v.physicalDisks.empty()) {
        for (auto n : v.physicalDisks) {
            auto h = win::healthOfDisk(n);
            if (!h) {
                add(CheckId::DiskHealth, CheckSeverity::Info, "Saúde do disco desconhecida",
                    "O Windows não informou o estado deste disco.");
                continue;
            }
            const std::string text = h->model + " — " + win::describeHealth(*h);
            if (h->status == win::HealthStatus::Healthy)
                add(CheckId::DiskHealth, CheckSeverity::Ok, "Saúde do disco", text);
            else if (h->status == win::HealthStatus::Unknown)
                add(CheckId::DiskHealth, CheckSeverity::Info, "Saúde do disco desconhecida", text);
            else
                add(CheckId::DiskHealth, CheckSeverity::Warning, "Disco com sinais de falha",
                    text + ". Um disco assim pode perder o backup; prefira outro.");
        }
    }

    // 9. Removível (habilita a política de rodízio).
    if (v.kind == win::DriveKind::Removable)
        add(CheckId::Removable, CheckSeverity::Info, "Disco removível",
            "Pode ser usado como cofre de rodízio, guardado fora do local.");

    return report;
}

} // namespace synkro::vault
