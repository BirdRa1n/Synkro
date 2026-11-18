#include "platform/windows/Volume.h"

#include "platform/windows/Win32.h"

#include <winioctl.h>

#include <algorithm>
#include <vector>

namespace synkro::win {

const char* nameOf(DriveKind kind) noexcept
{
    switch (kind) {
    case DriveKind::Fixed:     return "fixed";
    case DriveKind::Removable: return "removable";
    case DriveKind::Network:   return "network";
    case DriveKind::Optical:   return "optical";
    case DriveKind::RamDisk:   return "ramdisk";
    case DriveKind::Unknown:   break;
    }
    return "unknown";
}

namespace {

DriveKind mapDriveType(UINT type)
{
    switch (type) {
    case DRIVE_FIXED:     return DriveKind::Fixed;
    case DRIVE_REMOVABLE: return DriveKind::Removable;
    case DRIVE_REMOTE:    return DriveKind::Network;
    case DRIVE_CDROM:     return DriveKind::Optical;
    case DRIVE_RAMDISK:   return DriveKind::RamDisk;
    default:              return DriveKind::Unknown;
    }
}

// Discos USB externos costumam se apresentar como DRIVE_FIXED. O tipo de
// barramento revela se o dispositivo é removível na prática.
bool isUsbBus(HANDLE volume)
{
    STORAGE_PROPERTY_QUERY query{};
    query.PropertyId = StorageDeviceProperty;
    query.QueryType = PropertyStandardQuery;

    std::vector<BYTE> buffer(1024);
    DWORD returned = 0;
    if (!::DeviceIoControl(volume, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), buffer.data(),
                           static_cast<DWORD>(buffer.size()), &returned, nullptr))
        return false;
    const auto* desc = reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(buffer.data());
    return desc->BusType == BusTypeUsb || desc->RemovableMedia;
}

std::vector<std::uint32_t> diskExtents(HANDLE volume)
{
    std::vector<std::uint32_t> disks;
    std::vector<BYTE> buffer(sizeof(VOLUME_DISK_EXTENTS) + 16 * sizeof(DISK_EXTENT));
    DWORD returned = 0;
    if (!::DeviceIoControl(volume, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0, buffer.data(),
                           static_cast<DWORD>(buffer.size()), &returned, nullptr))
        return disks;

    const auto* extents = reinterpret_cast<const VOLUME_DISK_EXTENTS*>(buffer.data());
    for (DWORD i = 0; i < extents->NumberOfDiskExtents; ++i) {
        const auto n = static_cast<std::uint32_t>(extents->Extents[i].DiskNumber);
        if (std::find(disks.begin(), disks.end(), n) == disks.end())
            disks.push_back(n);
    }
    std::sort(disks.begin(), disks.end());
    return disks;
}

std::wstring systemMountPoint()
{
    wchar_t windowsDir[MAX_PATH] = {};
    if (::GetWindowsDirectoryW(windowsDir, MAX_PATH) == 0)
        return {};
    wchar_t mount[MAX_PATH] = {};
    if (!::GetVolumePathNameW(windowsDir, mount, MAX_PATH))
        return {};
    return mount;
}

} // namespace

Result<VolumeInfo> queryVolume(const std::wstring& path)
{
    if (path.empty())
        return Error::make(ErrorCode::InvalidArgument, "caminho não informado");

    std::wstring normalized(path);
    std::replace(normalized.begin(), normalized.end(), L'/', L'\\');

    std::vector<wchar_t> mount(normalized.size() + MAX_PATH + 2, L'\0');
    if (!::GetVolumePathNameW(normalized.c_str(), mount.data(), static_cast<DWORD>(mount.size())))
        return lastError("identificar o volume");

    VolumeInfo info;
    info.mountPoint = mount.data();
    if (!info.mountPoint.empty() && info.mountPoint.back() != L'\\')
        info.mountPoint += L'\\';

    info.kind = mapDriveType(::GetDriveTypeW(info.mountPoint.c_str()));

    wchar_t guid[MAX_PATH] = {};
    if (::GetVolumeNameForVolumeMountPointW(info.mountPoint.c_str(), guid, MAX_PATH))
        info.volumeGuidPath = guid;

    wchar_t label[MAX_PATH + 1] = {};
    wchar_t fs[MAX_PATH + 1] = {};
    DWORD serial = 0, maxComponent = 0, fsFlags = 0;
    if (!::GetVolumeInformationW(info.mountPoint.c_str(), label, MAX_PATH + 1, &serial, &maxComponent, &fsFlags,
                                 fs, MAX_PATH + 1))
        return lastError("ler informações do volume");
    info.label = label;
    info.fileSystem = fs;
    info.serialNumber = serial;

    ULARGE_INTEGER freeToCaller{}, total{}, totalFree{};
    if (::GetDiskFreeSpaceExW(info.mountPoint.c_str(), &freeToCaller, &total, &totalFree)) {
        info.totalBytes = total.QuadPart;
        info.freeBytes = freeToCaller.QuadPart;
    }

    info.isSystemVolume = foldCase(info.mountPoint) == foldCase(systemMountPoint());

    // Handle do volume sem direitos de acesso: suficiente para os IOCTLs de
    // consulta e não exige administrador.
    if (!info.volumeGuidPath.empty()) {
        std::wstring device = info.volumeGuidPath;
        if (!device.empty() && device.back() == L'\\')
            device.pop_back();
        UniqueHandle volume(::CreateFileW(device.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                          OPEN_EXISTING, 0, nullptr));
        if (volume) {
            info.physicalDisks = diskExtents(volume.get());
            if (info.kind == DriveKind::Fixed && isUsbBus(volume.get()))
                info.kind = DriveKind::Removable;
        }
    }

    return info;
}

} // namespace synkro::win
