#include "platform/windows/Snapshot.h"

#include "platform/windows/Win32.h"

#include <objbase.h>
#include <vss.h>
#include <vswriter.h>
#include <vsbackup.h>

namespace synkro::win {

bool isProcessElevated()
{
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const BOOL ok = ::GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
    ::CloseHandle(token);
    return ok && elevation.TokenIsElevated != 0;
}

std::wstring translateToSnapshot(const std::wstring& mountPoint, const std::wstring& device, const std::wstring& path)
{
    std::wstring p(path);
    for (auto& c : p) {
        if (c == L'/')
            c = L'\\';
    }
    std::wstring mp(mountPoint);
    if (!mp.empty() && mp.back() != L'\\')
        mp += L'\\';
    if (foldCase(p.substr(0, mp.size())) != foldCase(mp))
        return {};
    std::wstring dev(device);
    while (!dev.empty() && dev.back() == L'\\')
        dev.pop_back();
    const std::wstring rest = p.substr(mp.size());
    return rest.empty() ? dev + L'\\' : dev + L'\\' + rest;
}

namespace {

Error hrError(HRESULT hr, const char* context)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(hr));
    ErrorCode code = ErrorCode::IoError;
    if (hr == E_ACCESSDENIED)
        code = ErrorCode::AccessDenied;
    return Error::make(code, std::string("VSS: ") + context + " falhou (" + buf + ")", static_cast<unsigned long>(hr));
}

HRESULT waitAsync(IVssAsync* async)
{
    if (!async)
        return E_POINTER;
    HRESULT hr = async->Wait(INFINITE);
    HRESULT status = S_OK;
    if (SUCCEEDED(hr))
        hr = async->QueryStatus(&status, nullptr);
    async->Release();
    if (FAILED(hr))
        return hr;
    return status == VSS_S_ASYNC_FINISHED ? S_OK : status;
}

} // namespace

struct VolumeSnapshot::Impl {
    IVssBackupComponents* backup = nullptr;
    VSS_ID snapshotId{};
    bool comInitialized = false;
    std::wstring device;
    std::wstring mountPoint;

    ~Impl()
    {
        if (backup) {
            IVssAsync* async = nullptr;
            if (SUCCEEDED(backup->BackupComplete(&async)))
                waitAsync(async);
            LONG deleted = 0;
            VSS_ID nonDeleted{};
            backup->DeleteSnapshots(snapshotId, VSS_OBJECT_SNAPSHOT, TRUE, &deleted, &nonDeleted);
            backup->Release();
        }
        if (comInitialized)
            ::CoUninitialize();
    }
};

VolumeSnapshot::VolumeSnapshot() : d(std::make_unique<Impl>()) {}
VolumeSnapshot::VolumeSnapshot(VolumeSnapshot&&) noexcept = default;
VolumeSnapshot& VolumeSnapshot::operator=(VolumeSnapshot&&) noexcept = default;
VolumeSnapshot::~VolumeSnapshot() = default;

const std::wstring& VolumeSnapshot::devicePath() const
{
    return d->device;
}

const std::wstring& VolumeSnapshot::volumeMountPoint() const
{
    return d->mountPoint;
}

std::wstring VolumeSnapshot::translate(const std::wstring& originalPath) const
{
    return translateToSnapshot(d->mountPoint, d->device, originalPath);
}

Result<VolumeSnapshot> VolumeSnapshot::create(const std::wstring& volumeMountPoint)
{
    if (!isProcessElevated())
        return Error::make(ErrorCode::AccessDenied, "o snapshot VSS exige execução como administrador ou SYSTEM");

    VolumeSnapshot snap;
    snap.d->mountPoint = volumeMountPoint;
    if (!snap.d->mountPoint.empty() && snap.d->mountPoint.back() != L'\\')
        snap.d->mountPoint += L'\\';

    HRESULT hr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr))
        snap.d->comInitialized = true;
    else if (hr != RPC_E_CHANGED_MODE)
        return hrError(hr, "CoInitializeEx");
    // Pode já ter sido chamado pelo processo; RPC_E_TOO_LATE é aceitável.
    hr = ::CoInitializeSecurity(nullptr, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_PKT_PRIVACY,
                                RPC_C_IMP_LEVEL_IDENTIFY, nullptr, EOAC_NONE, nullptr);
    if (FAILED(hr) && hr != RPC_E_TOO_LATE)
        return hrError(hr, "CoInitializeSecurity");

    IVssBackupComponents* backup = nullptr;
    hr = CreateVssBackupComponents(&backup);
    if (FAILED(hr))
        return hrError(hr, "CreateVssBackupComponents");
    snap.d->backup = backup;

    if (FAILED(hr = backup->InitializeForBackup()))
        return hrError(hr, "InitializeForBackup");
    if (FAILED(hr = backup->SetContext(VSS_CTX_BACKUP)))
        return hrError(hr, "SetContext");
    // VSS_BT_COPY: não interfere em outras ferramentas de backup (logs, bits de arquivo).
    if (FAILED(hr = backup->SetBackupState(false, true, VSS_BT_COPY, false)))
        return hrError(hr, "SetBackupState");

    IVssAsync* async = nullptr;
    if (FAILED(hr = backup->GatherWriterMetadata(&async)) || FAILED(hr = waitAsync(async)))
        return hrError(hr, "GatherWriterMetadata");

    VSS_ID setId{};
    if (FAILED(hr = backup->StartSnapshotSet(&setId)))
        return hrError(hr, "StartSnapshotSet");

    std::wstring volume = snap.d->mountPoint;
    if (FAILED(hr = backup->AddToSnapshotSet(volume.data(), GUID_NULL, &snap.d->snapshotId)))
        return hrError(hr, "AddToSnapshotSet");

    async = nullptr;
    if (FAILED(hr = backup->PrepareForBackup(&async)) || FAILED(hr = waitAsync(async)))
        return hrError(hr, "PrepareForBackup");

    async = nullptr;
    if (FAILED(hr = backup->DoSnapshotSet(&async)) || FAILED(hr = waitAsync(async)))
        return hrError(hr, "DoSnapshotSet");

    VSS_SNAPSHOT_PROP prop{};
    if (FAILED(hr = backup->GetSnapshotProperties(snap.d->snapshotId, &prop)))
        return hrError(hr, "GetSnapshotProperties");
    snap.d->device = prop.m_pwszSnapshotDeviceObject ? prop.m_pwszSnapshotDeviceObject : L"";
    VssFreeSnapshotProperties(&prop);

    if (snap.d->device.empty())
        return Error::make(ErrorCode::Internal, "VSS não informou o dispositivo do snapshot");
    return snap;
}

} // namespace synkro::win
