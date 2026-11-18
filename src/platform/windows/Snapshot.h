#pragma once

#include "core/Result.h"

#include <memory>
#include <string>

namespace synkro::win {

// true se o processo roda com token elevado (administrador ou SYSTEM).
bool isProcessElevated();

// Snapshot VSS de um volume (architecture.md §6.3): uma cópia congelada,
// consistente "como se tivesse faltado energia", de onde os arquivos em uso
// podem ser lidos. O snapshot é não persistente e é apagado no destrutor.
//
// Exige privilégio de administrador.
class VolumeSnapshot {
public:
    static Result<VolumeSnapshot> create(const std::wstring& volumeMountPoint);

    VolumeSnapshot(VolumeSnapshot&&) noexcept;
    VolumeSnapshot& operator=(VolumeSnapshot&&) noexcept;
    ~VolumeSnapshot();

    // Ex.: \\?\GLOBALROOT\Device\HarddiskVolumeShadowCopy12
    const std::wstring& devicePath() const;
    const std::wstring& volumeMountPoint() const;

    // "C:\PDV\XML" -> "\\?\GLOBALROOT\Device\HarddiskVolumeShadowCopy12\PDV\XML"
    std::wstring translate(const std::wstring& originalPath) const;

private:
    VolumeSnapshot();
    struct Impl;
    std::unique_ptr<Impl> d;
};

// Tradução de caminho pura (exposta para testes).
std::wstring translateToSnapshot(const std::wstring& mountPoint, const std::wstring& device, const std::wstring& path);

} // namespace synkro::win
