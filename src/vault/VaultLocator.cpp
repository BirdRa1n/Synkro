#include "vault/VaultLocator.h"

#include "platform/windows/Win32.h"

#include <QUuid>

#include <vector>

namespace synkro::vault {

namespace {

std::vector<std::wstring> mountPoints()
{
    std::vector<std::wstring> out;
    wchar_t volume[MAX_PATH] = {};
    HANDLE find = ::FindFirstVolumeW(volume, MAX_PATH);
    if (find == INVALID_HANDLE_VALUE)
        return out;
    do {
        DWORD needed = 0;
        ::GetVolumePathNamesForVolumeNameW(volume, nullptr, 0, &needed);
        if (needed == 0)
            continue;
        std::vector<wchar_t> names(needed);
        if (!::GetVolumePathNamesForVolumeNameW(volume, names.data(), needed, &needed))
            continue;
        // Lista de strings terminadas em \0, encerrada por \0\0.
        for (const wchar_t* p = names.data(); *p; p += wcslen(p) + 1)
            out.emplace_back(p);
    } while (::FindNextVolumeW(find, volume, MAX_PATH));
    ::FindVolumeClose(find);
    return out;
}

bool sameId(const QString& a, const QString& b)
{
    return QUuid::fromString(a) == QUuid::fromString(b) && !QUuid::fromString(a).isNull();
}

} // namespace

std::vector<LocatedVault> discoverVaults()
{
    // Evita a caixa "Insira um disco na unidade" do Windows em leitores vazios.
    const UINT previous = ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    std::vector<LocatedVault> found;
    for (const auto& mount : mountPoints()) {
        auto id = readIdentity(mount);
        if (id)
            found.push_back({mount, std::move(id).value()});
    }
    ::SetErrorMode(previous);
    return found;
}

std::optional<LocatedVault> locateVault(const QString& vaultId, const std::wstring& hint)
{
    if (!hint.empty()) {
        auto id = readIdentity(hint);
        if (id && sameId(id.value().vaultId, vaultId))
            return LocatedVault{hint, std::move(id).value()};
    }
    for (auto& v : discoverVaults()) {
        if (sameId(v.identity.vaultId, vaultId))
            return v;
    }
    return std::nullopt;
}

} // namespace synkro::vault
