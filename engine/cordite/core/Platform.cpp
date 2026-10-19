#include "cordite/core/Platform.h"

#include "cordite/core/Base.h"
#include "cordite/core/FileSystem.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <system_error>
#include <thread>
#include <vector>

#if CORDITE_PLATFORM_WINDOWS
#include <crtdbg.h>
#include <windows.h>
// After windows.h, which it needs.
#include <shlobj.h>
#elif CORDITE_PLATFORM_MACOS
#include <mach-o/dyld.h>
#endif

namespace cordite {
namespace {

/// A directory that the environment names, if it names one from the root - the rule of
/// POSIX, whatever system asks.
std::optional<std::filesystem::path> rootedPathOf(const std::optional<std::string>& value) {
    if (!value || !value->starts_with('/')) {
        return std::nullopt;
    }
    return pathFromUtf8(*value);
}

} // namespace

std::string executableFileName(std::string_view name) {
#if CORDITE_PLATFORM_WINDOWS
    return std::string(name) + ".exe";
#else
    return std::string(name);
#endif
}

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

std::optional<std::filesystem::path> applicationBundleOf(const std::filesystem::path& program) {
    const std::filesystem::path programs = program.parent_path();
    const std::filesystem::path contents = programs.parent_path();
    std::filesystem::path bundle = contents.parent_path();
    if (program.filename().empty() || programs.filename() != "MacOS" || contents.filename() != "Contents" ||
        bundle.extension() != ".app") {
        return std::nullopt;
    }
    return bundle;
}

std::optional<std::filesystem::path> bundledLibrary(const std::filesystem::path& program, std::string_view fileName) {
    const std::optional<std::filesystem::path> bundle = applicationBundleOf(program);
    if (!bundle || fileName.empty()) {
        return std::nullopt;
    }
    std::filesystem::path library = *bundle / "Contents" / "Frameworks" / pathFromUtf8(fileName);
    std::error_code error;
    if (!std::filesystem::is_regular_file(library, error)) {
        return std::nullopt;
    }
    return library;
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

#if CORDITE_PLATFORM_WINDOWS
namespace {

/// Whether a standard handle was given a pipe or a file to write to. A character device -
/// a console the program is not attached to, or NUL - is taken for the console.
bool writesToPipeOrFile(HANDLE handle) {
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    const DWORD type = GetFileType(handle);
    return type == FILE_TYPE_PIPE || type == FILE_TYPE_DISK;
}

/// Points a standard handle, and the stream of the C runtime that writes through it, at the
/// console the program is attached to.
void writeToConsole(DWORD which, FILE* stream) {
    // Open for as long as the program runs, as a standard handle is.
    HANDLE console = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (console == INVALID_HANDLE_VALUE) {
        return;
    }
    SetStdHandle(which, console);
    FILE* reopened = nullptr;
    // A stream that cannot be reopened writes nowhere, as it did before.
    static_cast<void>(freopen_s(&reopened, "CONOUT$", "w", stream));
}

std::wstring wideOf(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
    return wide;
}

} // namespace
#endif

void attachToParentConsole() {
#if CORDITE_PLATFORM_WINDOWS
    const bool output = writesToPipeOrFile(GetStdHandle(STD_OUTPUT_HANDLE));
    const bool error = writesToPipeOrFile(GetStdHandle(STD_ERROR_HANDLE));
    if ((output && error) || AttachConsole(ATTACH_PARENT_PROCESS) == 0) {
        return;
    }
    if (!output) {
        writeToConsole(STD_OUTPUT_HANDLE, stdout);
    }
    if (!error) {
        writeToConsole(STD_ERROR_HANDLE, stderr);
    }
#endif
}

void showErrorDialog(std::string_view title, std::string_view message) {
#if CORDITE_PLATFORM_WINDOWS
    // The system remembers suppressErrorDialogs(), in the error mode of the process.
    if ((GetErrorMode() & SEM_FAILCRITICALERRORS) != 0) {
        return;
    }
    MessageBoxW(nullptr, wideOf(message).c_str(), wideOf(title).c_str(), MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
#else
    static_cast<void>(title);
    static_cast<void>(message);
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

std::filesystem::path xdgConfigurationDirectory(const std::optional<std::string>& configHome,
                                                const std::optional<std::string>& home) {
    if (const std::optional<std::filesystem::path> directory = rootedPathOf(configHome)) {
        return *directory;
    }
    if (const std::optional<std::filesystem::path> directory = rootedPathOf(home)) {
        return *directory / ".config";
    }
    return {};
}

std::filesystem::path userConfigurationDirectory() {
#if CORDITE_PLATFORM_WINDOWS
    PWSTR path = nullptr;
    const HRESULT found = SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_DEFAULT, nullptr, &path);
    std::filesystem::path directory;
    if (SUCCEEDED(found) && path != nullptr) {
        directory = std::filesystem::path(path);
    }
    // Freed whether the call succeeded or not, as the function asks.
    CoTaskMemFree(path);
    return directory;
#elif CORDITE_PLATFORM_MACOS
    const std::optional<std::filesystem::path> home = rootedPathOf(environmentVariable("HOME"));
    return home ? *home / "Library" / "Application Support" : std::filesystem::path();
#else
    return xdgConfigurationDirectory(environmentVariable("XDG_CONFIG_HOME"), environmentVariable("HOME"));
#endif
}

} // namespace cordite
