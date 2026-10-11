#include "cordite/core/Platform.h"

#include "cordite/core/Base.h"

#include <algorithm>
#include <cstdlib>
#include <system_error>
#include <thread>
#include <vector>

#if CORDITE_PLATFORM_WINDOWS
#include <crtdbg.h>
#include <windows.h>
#elif CORDITE_PLATFORM_MACOS
#include <mach-o/dyld.h>
#endif

namespace cordite {

std::filesystem::path executablePath() {
#if CORDITE_PLATFORM_WINDOWS
    // GetModuleFileNameW truncates silently, so grow the buffer until the path fits.
    std::vector<wchar_t> buffer(MAX_PATH);
    while (true) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return {};
        }
        if (length < buffer.size()) {
            return std::wstring(buffer.data(), length);
        }
        buffer.resize(buffer.size() * 2);
    }
#elif CORDITE_PLATFORM_MACOS
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buffer(size);
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
        return {};
    }
    std::error_code error;
    const std::filesystem::path resolved = std::filesystem::canonical(buffer.data(), error);
    return error ? std::filesystem::path(buffer.data()) : resolved;
#else
    std::error_code error;
    const std::filesystem::path resolved = std::filesystem::read_symlink("/proc/self/exe", error);
    return error ? std::filesystem::path() : resolved;
#endif
}

std::filesystem::path executableDirectory() {
    return executablePath().parent_path();
}

bool isDebuggerPresent() {
#if CORDITE_PLATFORM_WINDOWS
    return IsDebuggerPresent() != FALSE;
#else
    return false;
#endif
}

void suppressErrorDialogs() {
#if CORDITE_PLATFORM_WINDOWS
    // Missing DLLs and crashes: fail the process instead of asking the user.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    // abort(): no message box, no error-reporting hand-off; just exit with its status.
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#if defined(_DEBUG)
    // The debug C runtime reports failed assert() calls and its own checks in a dialog
    // with Abort/Retry/Ignore buttons. Send them to standard error instead.
    for (const int reportType : {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT}) {
        _CrtSetReportMode(reportType, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(reportType, _CRTDBG_FILE_STDERR);
    }
#endif
#endif
}

std::optional<std::string> environmentVariable(std::string_view name) {
    const std::string key(name);
    const char* value = std::getenv(key.c_str());
    if (value == nullptr) {
        return std::nullopt;
    }
    return std::string(value);
}

uint32_t hardwareThreadCount() {
    return std::max(1u, std::thread::hardware_concurrency());
}

} // namespace cordite
