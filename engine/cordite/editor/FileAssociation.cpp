#include "cordite/editor/FileAssociation.h"

#include "cordite/core/Base.h"

#include <optional>

#if CORDITE_PLATFORM_WINDOWS
#include <windows.h>
// After windows.h, which it needs.
#include <shlobj.h>
#endif

namespace cordite {
namespace {

Status checkAssociation(const FileAssociation& association) {
    if (association.extension.size() < 2 || association.extension.front() != '.' ||
        association.extension.find_first_of("\\/ ") != std::string::npos) {
        return makeError(ErrorCode::InvalidArgument, "'{}' is no extension of a file: one begins with a dot",
                         association.extension);
    }
    if (association.programId.empty() || association.programId.find('\\') != std::string::npos) {
        return makeError(ErrorCode::InvalidArgument, "'{}' is no id of a type of file", association.programId);
    }
    if (association.program.empty()) {
        return makeError(ErrorCode::InvalidArgument, "no program is named to open files of {}", association.extension);
    }
    return {};
}

#if CORDITE_PLATFORM_WINDOWS

std::wstring wideOf(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
    return wide;
}

std::string utf8Of(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const int length =
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8.data(), length, nullptr, nullptr);
    return utf8;
}

/// A key under the classes, under HKEY_CURRENT_USER.
std::wstring keyOf(std::string_view classes, std::string_view name) {
    return wideOf(classes) + L"\\" + wideOf(name);
}

Error registryError(std::string_view action, const std::wstring& key, LSTATUS status) {
    return {.code = ErrorCode::IoError,
            .message = fmt::format("cannot {} HKEY_CURRENT_USER\\{} (error {})", action, utf8Of(key), status)};
}

/// The default value of a key: none when there is no such key or no such value.
Result<std::optional<std::wstring>> readDefault(const std::wstring& key) {
    DWORD bytes = 0;
    LSTATUS status = RegGetValueW(HKEY_CURRENT_USER, key.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, nullptr, &bytes);
    if (status == ERROR_FILE_NOT_FOUND) {
        return std::optional<std::wstring>();
    }
    if (status != ERROR_SUCCESS) {
        return tl::unexpected(registryError("read", key, status));
    }
    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    status = RegGetValueW(HKEY_CURRENT_USER, key.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, value.data(), &bytes);
    if (status != ERROR_SUCCESS) {
        return tl::unexpected(registryError("read", key, status));
    }
    // What was read ends with its terminator.
    value.resize(bytes / sizeof(wchar_t) > 0 ? bytes / sizeof(wchar_t) - 1 : 0);
    return std::optional<std::wstring>(std::move(value));
}

Status writeDefault(const std::wstring& key, const std::wstring& value) {
    HKEY handle = nullptr;
    LSTATUS status = RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE,
                                     nullptr, &handle, nullptr);
    if (status != ERROR_SUCCESS) {
        return tl::unexpected(registryError("create", key, status));
    }
    const auto bytes = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    status = RegSetValueExW(handle, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()), bytes);
    RegCloseKey(handle);
    if (status != ERROR_SUCCESS) {
        return tl::unexpected(registryError("write", key, status));
    }
    return {};
}

/// Deletes a key and everything below it; a key that is not there is deleted already.
Status deleteTree(const std::wstring& key) {
    const LSTATUS status = RegDeleteTreeW(HKEY_CURRENT_USER, key.c_str());
    if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) {
        return tl::unexpected(registryError("delete", key, status));
    }
    return {};
}

/// Tells the shell that what opens files has changed, so that it shows it at once. Only
/// for the person's own classes: a key of a test changes nothing the shell looks at.
void tellShell(std::string_view classes) {
    if (classes == kUserFileClasses) {
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    }
}

#else

Error unsupported() {
    return {.code = ErrorCode::Unsupported,
            .message = "files are associated with programs only on Windows; elsewhere an installation does it"};
}

#endif

} // namespace

FileAssociation projectFileAssociation(const std::filesystem::path& editor) {
    return {
        .extension = ".cordite", .programId = "Cordite.Project", .description = "Cordite project", .program = editor};
}

std::string openCommand(const std::filesystem::path& program) {
    std::filesystem::path native = program;
    native.make_preferred();
    const std::u8string text = native.u8string();
    return fmt::format(R"("{}" "%1")", std::string_view(reinterpret_cast<const char*>(text.data()), text.size()));
}

Result<bool> isAssociated(const FileAssociation& association, std::string_view classes) {
    CORDITE_TRY(checkAssociation(association));
#if CORDITE_PLATFORM_WINDOWS
    CORDITE_TRY_ASSIGN(const std::optional<std::wstring> type, readDefault(keyOf(classes, association.extension)));
    if (!type || *type != wideOf(association.programId)) {
        return false;
    }
    CORDITE_TRY_ASSIGN(const std::optional<std::wstring> command,
                       readDefault(keyOf(classes, association.programId + "\\shell\\open\\command")));
    return command && *command == wideOf(openCommand(association.program));
#else
    static_cast<void>(classes);
    return tl::unexpected(unsupported());
#endif
}

Status associate(const FileAssociation& association, std::string_view classes) {
    CORDITE_TRY(checkAssociation(association));
#if CORDITE_PLATFORM_WINDOWS
    CORDITE_TRY(writeDefault(keyOf(classes, association.programId), wideOf(association.description)));
    CORDITE_TRY(writeDefault(keyOf(classes, association.programId + "\\shell\\open\\command"),
                             wideOf(openCommand(association.program))));
    CORDITE_TRY(writeDefault(keyOf(classes, association.extension), wideOf(association.programId)));
    tellShell(classes);
    return {};
#else
    static_cast<void>(classes);
    return tl::unexpected(unsupported());
#endif
}

Status dissociate(const FileAssociation& association, std::string_view classes) {
    CORDITE_TRY(checkAssociation(association));
#if CORDITE_PLATFORM_WINDOWS
    const std::wstring extension = keyOf(classes, association.extension);
    CORDITE_TRY_ASSIGN(const std::optional<std::wstring> type, readDefault(extension));
    if (type && *type == wideOf(association.programId)) {
        CORDITE_TRY(deleteTree(extension));
    }
    CORDITE_TRY(deleteTree(keyOf(classes, association.programId)));
    tellShell(classes);
    return {};
#else
    static_cast<void>(classes);
    return tl::unexpected(unsupported());
#endif
}

} // namespace cordite
