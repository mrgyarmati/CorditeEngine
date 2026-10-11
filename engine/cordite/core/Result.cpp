#include "cordite/core/Result.h"

namespace cordite {

std::string_view nameOf(ErrorCode code) {
    switch (code) {
        case ErrorCode::Unknown:
            return "Unknown";
        case ErrorCode::InvalidArgument:
            return "InvalidArgument";
        case ErrorCode::NotFound:
            return "NotFound";
        case ErrorCode::AlreadyExists:
            return "AlreadyExists";
        case ErrorCode::IoError:
            return "IoError";
        case ErrorCode::ParseError:
            return "ParseError";
        case ErrorCode::Unsupported:
            return "Unsupported";
        case ErrorCode::OutOfRange:
            return "OutOfRange";
        case ErrorCode::InvalidState:
            return "InvalidState";
        case ErrorCode::VersionMismatch:
            return "VersionMismatch";
        case ErrorCode::DeviceError:
            return "DeviceError";
        case ErrorCode::ScriptError:
            return "ScriptError";
    }
    return "Unknown";
}

std::string Error::describe() const {
    return fmt::format("{}: {}", nameOf(code), message);
}

Error Error::withContext(std::string_view context) const {
    return Error{code, fmt::format("{}: {}", context, message)};
}

} // namespace cordite
