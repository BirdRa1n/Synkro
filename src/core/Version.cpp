#include "core/Version.h"

#include "core/Result.h"

namespace synkro {

const char* versionString() noexcept
{
    return SYNKRO_VERSION_STRING;
}

const char* toString(ErrorCode code) noexcept
{
    switch (code) {
    case ErrorCode::InvalidArgument:         return "invalid_argument";
    case ErrorCode::NotFound:                return "not_found";
    case ErrorCode::AccessDenied:            return "access_denied";
    case ErrorCode::AlreadyExists:           return "already_exists";
    case ErrorCode::IoError:                 return "io_error";
    case ErrorCode::DiskFull:                return "disk_full";
    case ErrorCode::DeviceNotReady:          return "device_not_ready";
    case ErrorCode::SourceChangedDuringCopy: return "source_changed_during_copy";
    case ErrorCode::VerificationFailed:      return "verification_failed";
    case ErrorCode::Corrupted:               return "corrupted";
    case ErrorCode::Unsupported:             return "unsupported";
    case ErrorCode::Internal:                return "internal";
    }
    return "unknown";
}

} // namespace synkro
