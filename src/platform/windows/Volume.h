#pragma once

#include "core/Result.h"

#include <cstdint>
#include <string>
#include <vector>

namespace synkro::win {

enum class DriveKind { Unknown, Fixed, Removable, Network, Optical, RamDisk };

struct VolumeInfo {
    std::wstring mountPoint;        // ex.: "E:\"
    std::wstring volumeGuidPath;    // ex.: "\\?\Volume{...}\" (vazio em unidades de rede)
    std::wstring label;
    std::wstring fileSystem;        // "NTFS", "exFAT", "FAT32", "ReFS"...
    std::uint32_t serialNumber = 0;
    DriveKind kind = DriveKind::Unknown;
    bool isSystemVolume = false;    // volume onde está o Windows
    std::uint64_t totalBytes = 0;
    std::uint64_t freeBytes = 0;    // disponível para o processo
    std::vector<std::uint32_t> physicalDisks; // números de disco físico (\\.\PhysicalDriveN)
};

// Informações do volume que contém `path` (arquivo ou pasta, que precisa
// existir). Os discos físicos vêm de IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS e
// não exigem privilégio de administrador.
Result<VolumeInfo> queryVolume(const std::wstring& path);

const char* toString(DriveKind kind) noexcept;

} // namespace synkro::win
