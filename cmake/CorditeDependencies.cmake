# Third-party dependencies.
#
# Every dependency is fetched as an archive pinned by URL and SHA-256, so a build never
# depends on a mutable branch or tag. To add or upgrade one, compute the hash with
# scripts/hash-url.ps1, update the entry below and list it in docs/third-party.md.

include(FetchContent)

# Some dependencies still declare a cmake_minimum_required older than CMake 4 accepts.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

# cordite_declare_dependency(<name> URL <url> SHA256 <hash> [SOURCE_SUBDIR <dir>] [SOURCES_ONLY])
#
# SOURCES_ONLY downloads and unpacks the archive without running the dependency's own
# CMake project; use it for single-header libraries that we wrap in our own target.
function(cordite_declare_dependency name)
    cmake_parse_arguments(PARSE_ARGV 1 arg "SOURCES_ONLY" "URL;SHA256;SOURCE_SUBDIR" "")

    set(extra_arguments "")
    if(arg_SOURCES_ONLY)
        # FetchContent skips add_subdirectory() when SOURCE_SUBDIR has no CMakeLists.txt.
        list(APPEND extra_arguments SOURCE_SUBDIR cordite-sources-only)
    elseif(arg_SOURCE_SUBDIR)
        list(APPEND extra_arguments SOURCE_SUBDIR "${arg_SOURCE_SUBDIR}")
    endif()

    FetchContent_Declare(${name}
        URL "${arg_URL}"
        URL_HASH "SHA256=${arg_SHA256}"
        DOWNLOAD_EXTRACT_TIMESTAMP FALSE
        EXCLUDE_FROM_ALL
        SYSTEM
        ${extra_arguments})
endfunction()

# Sets an option of a dependency before its project is added.
macro(cordite_dependency_option name value)
    set(${name} ${value} CACHE INTERNAL "")
endmacro()

# --- fmt: text formatting ---------------------------------------------------------------
cordite_declare_dependency(fmt
    URL https://github.com/fmtlib/fmt/releases/download/12.2.0/fmt-12.2.0.zip
    SHA256 a2f4a8d51178f954e4c339007f77edd76ba0cb2e36f87a48e5a5403d9be5878f)
cordite_dependency_option(FMT_INSTALL OFF)
cordite_dependency_option(FMT_DOC OFF)
cordite_dependency_option(FMT_TEST OFF)

# --- spdlog: logging sinks --------------------------------------------------------------
cordite_declare_dependency(spdlog
    URL https://github.com/gabime/spdlog/archive/refs/tags/v1.17.0.tar.gz
    SHA256 d8862955c6d74e5846b3f580b1605d2428b11d97a410d86e2fb13e857cd3a744)
cordite_dependency_option(SPDLOG_FMT_EXTERNAL ON)
cordite_dependency_option(SPDLOG_INSTALL OFF)
cordite_dependency_option(SPDLOG_BUILD_EXAMPLE OFF)
cordite_dependency_option(SPDLOG_BUILD_TESTS OFF)
if(WIN32)
    # Lets the file sink take UTF-16 paths, so log files work in non-ASCII directories.
    cordite_dependency_option(SPDLOG_WCHAR_FILENAMES ON)
endif()

# --- nlohmann/json: JSON parsing and writing --------------------------------------------
cordite_declare_dependency(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    SHA256 42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa)
cordite_dependency_option(JSON_BuildTests OFF)
cordite_dependency_option(JSON_Install OFF)

# --- tl::expected: value-or-error return type -------------------------------------------
cordite_declare_dependency(tl_expected
    URL https://github.com/TartanLlama/expected/archive/refs/tags/v1.3.1.tar.gz
    SHA256 9a04f4f472fbb5c30bf60402f1ca626c4a76987f867978d0b8a35d7ab3fb8fe7)
cordite_dependency_option(EXPECTED_BUILD_TESTS OFF)
cordite_dependency_option(EXPECTED_BUILD_PACKAGE OFF)

# --- enkiTS: task scheduler backing the job system --------------------------------------
cordite_declare_dependency(enkits
    URL https://github.com/dougbinks/enkiTS/archive/refs/tags/v1.12.tar.gz
    SHA256 8373b6199d56816bd3ba58432eae74e2ebd5afcfdffb723073cc34730b189fd5)
cordite_dependency_option(ENKITS_BUILD_EXAMPLES OFF)
cordite_dependency_option(ENKITS_BUILD_C_INTERFACE OFF)
cordite_dependency_option(ENKITS_BUILD_SHARED OFF)
cordite_dependency_option(ENKITS_INSTALL OFF)

# --- xxHash: content hashing (single header, compiled inside cordite_core) --------------
cordite_declare_dependency(xxhash
    URL https://github.com/Cyan4973/xxHash/archive/refs/tags/v0.8.4.tar.gz
    SHA256 5738270935e7c3d38a79b3adf7c9692566ce7895a25f67de43ad52ab504acd32
    SOURCES_ONLY)

# --- glm: vector and matrix math --------------------------------------------------------
cordite_declare_dependency(glm
    URL https://github.com/g-truc/glm/archive/refs/tags/1.0.3.tar.gz
    SHA256 6775e47231a446fd086d660ecc18bcd076531cfedd912fbd66e576b118607001)
cordite_dependency_option(GLM_BUILD_LIBRARY OFF)
cordite_dependency_option(GLM_BUILD_TESTS OFF)
cordite_dependency_option(GLM_BUILD_INSTALL OFF)

# --- GLFW: windows and input ------------------------------------------------------------
cordite_declare_dependency(glfw
    URL https://github.com/glfw/glfw/releases/download/3.5.1/glfw-3.5.1.zip
    SHA256 ea79bc5feffc254c87291980c2d0bce9acebb68c4983b79f961dcd2cb8a611a0)
cordite_dependency_option(GLFW_BUILD_EXAMPLES OFF)
cordite_dependency_option(GLFW_BUILD_TESTS OFF)
cordite_dependency_option(GLFW_BUILD_DOCS OFF)
cordite_dependency_option(GLFW_INSTALL OFF)
# Match the static C runtime the rest of the build uses.
cordite_dependency_option(USE_MSVC_RUNTIME_LIBRARY_DLL OFF)

# --- Vulkan-Headers: the Vulkan API and its C++ bindings --------------------------------
# Must stay at the version NVRHI is developed against (NVRHI_VULKAN_HEADERS_GIT_TAG in
# its CMakeLists.txt). Declared before NVRHI so that it uses this copy.
cordite_declare_dependency(vulkan_headers
    URL https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/v1.4.352.tar.gz
    SHA256 4850909d22a8a9767c27daea2b972e49d7c298560573d5b6221ee50db9bf49db)

# --- NVRHI: rendering hardware interface over Vulkan ------------------------------------
# NVRHI has no releases; this is a commit of its main branch (2026-10-05).
cordite_declare_dependency(nvrhi
    URL https://github.com/NVIDIA-RTX/NVRHI/archive/6b96fb03e07539f08327aea76c56d55f1de9d906.tar.gz
    SHA256 f46c733ccc555fc457aaa3df510ee7df0ca089f5d1129d92903029e4fb67c560)
cordite_dependency_option(NVRHI_WITH_VULKAN ON)
cordite_dependency_option(NVRHI_WITH_DX11 OFF)
cordite_dependency_option(NVRHI_WITH_DX12 OFF)
cordite_dependency_option(NVRHI_WITH_VALIDATION ON)
cordite_dependency_option(NVRHI_FETCH_VULKAN_HEADERS OFF)
cordite_dependency_option(NVRHI_BUILD_SHARED OFF)
cordite_dependency_option(NVRHI_INSTALL OFF)

# --- stb: image decoding and encoding (single headers, compiled inside cordite_assets) --
# stb has no releases; this is a commit of its master branch.
cordite_declare_dependency(stb
    URL https://github.com/nothings/stb/archive/2c980bb59875b0d32144a71867fbdebb2f77cd20.tar.gz
    SHA256 9a955b1b49a4410088a2e0ee2a9c057c3c907d0c1d75454144cb980aca0ba515
    SOURCES_ONLY)

# --- CLI11: command-line parsing for the executables ------------------------------------
cordite_declare_dependency(cli11
    URL https://github.com/CLIUtils/CLI11/archive/refs/tags/v2.7.2.tar.gz
    SHA256 46eef3101da70852ec7af026e09d485ccee81813331c8c6052d39344443b83da)
cordite_dependency_option(CLI11_BUILD_TESTS OFF)
cordite_dependency_option(CLI11_BUILD_EXAMPLES OFF)
cordite_dependency_option(CLI11_BUILD_DOCS OFF)
cordite_dependency_option(CLI11_INSTALL OFF)
cordite_dependency_option(CLI11_PRECOMPILED OFF)

FetchContent_MakeAvailable(
    fmt spdlog nlohmann_json tl_expected enkits xxhash glm
    glfw vulkan_headers nvrhi stb cli11)

add_library(cordite_xxhash INTERFACE)
add_library(cordite::xxhash ALIAS cordite_xxhash)
target_include_directories(cordite_xxhash SYSTEM INTERFACE "${xxhash_SOURCE_DIR}")

add_library(cordite_stb INTERFACE)
add_library(cordite::stb ALIAS cordite_stb)
target_include_directories(cordite_stb SYSTEM INTERFACE "${stb_SOURCE_DIR}")

# --- Slang: shader compiler -------------------------------------------------------------
# Used as the official prebuilt release: building Slang from source takes far longer than
# the rest of the engine and needs tools the build otherwise avoids. Only the editor and
# development tools link it; exported games load precompiled shaders and do not need it.
set(CORDITE_SLANG_VERSION 2026.19)
set(cordite_slang_base_url
    "https://github.com/shader-slang/slang/releases/download/v${CORDITE_SLANG_VERSION}/slang-${CORDITE_SLANG_VERSION}")

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64|ARM64)$")
    set(cordite_slang_arch aarch64)
else()
    set(cordite_slang_arch x86_64)
endif()

if(WIN32)
    if(NOT cordite_slang_arch STREQUAL "x86_64")
        message(FATAL_ERROR "Cordite supports only x86-64 on Windows.")
    endif()
    set(cordite_slang_package windows-x86_64.zip)
    set(cordite_slang_sha256 fc922f214b5d115f663932e59e80e0f6b6cc6fae140979cf307da6459b6f8f33)
elseif(APPLE)
    if(cordite_slang_arch STREQUAL "aarch64")
        set(cordite_slang_package macos-aarch64.zip)
        set(cordite_slang_sha256 f77d89b91ec63698a7a8e5aa4316ce08b70d3211a236640ee4551f55be85d9ed)
    else()
        set(cordite_slang_package macos-x86_64.zip)
        set(cordite_slang_sha256 c5e18233c0b90cc651c792ec4c4e4a467a9a101ffb6301643aab89c016b48e42)
    endif()
else()
    if(cordite_slang_arch STREQUAL "aarch64")
        set(cordite_slang_package linux-aarch64.tar.gz)
        set(cordite_slang_sha256 eeefa4e52ca322cf3c037ee0d273b0ee68f4161fa422eda1bb704126073b69b4)
    else()
        set(cordite_slang_package linux-x86_64.tar.gz)
        set(cordite_slang_sha256 bd6cfc47b7353b2cffa36a866b1584749f76f4b2dbebf6dda26bc3fb40dc3c0e)
    endif()
endif()

cordite_declare_dependency(slang
    URL "${cordite_slang_base_url}-${cordite_slang_package}"
    SHA256 ${cordite_slang_sha256}
    SOURCES_ONLY)
FetchContent_MakeAvailable(slang)

# The release also ships a CMake package, but it only describes a Release configuration
# and exposes far more than we use, so the one library is imported directly.
add_library(cordite_slang SHARED IMPORTED GLOBAL)
add_library(cordite::slang ALIAS cordite_slang)
target_include_directories(cordite_slang SYSTEM INTERFACE "${slang_SOURCE_DIR}/include")

# CORDITE_SLANG_RUNTIME_FILES lists what must sit next to an executable that compiles
# shaders. On Linux and macOS the build RPATH points at the unpacked release instead.
set(CORDITE_SLANG_RUNTIME_FILES "")
if(WIN32)
    set_target_properties(cordite_slang PROPERTIES
        IMPORTED_LOCATION "${slang_SOURCE_DIR}/bin/slang.dll"
        IMPORTED_IMPLIB "${slang_SOURCE_DIR}/lib/slang.lib")
    foreach(library slang slang-compiler slang-glslang)
        list(APPEND CORDITE_SLANG_RUNTIME_FILES "${slang_SOURCE_DIR}/bin/${library}.dll")
    endforeach()
elseif(APPLE)
    set(cordite_slang_library "libslang-compiler.0.${CORDITE_SLANG_VERSION}.dylib")
    set_target_properties(cordite_slang PROPERTIES
        IMPORTED_LOCATION "${slang_SOURCE_DIR}/lib/${cordite_slang_library}"
        IMPORTED_SONAME "@rpath/${cordite_slang_library}")
else()
    set(cordite_slang_library "libslang-compiler.so.0.${CORDITE_SLANG_VERSION}")
    set_target_properties(cordite_slang PROPERTIES
        IMPORTED_LOCATION "${slang_SOURCE_DIR}/lib/${cordite_slang_library}"
        IMPORTED_SONAME "${cordite_slang_library}")
endif()

# Copies the shader compiler's runtime libraries next to the built executables.
if(CORDITE_SLANG_RUNTIME_FILES)
    add_custom_target(cordite_slang_runtime ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CORDITE_SLANG_RUNTIME_FILES}
            "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
        COMMENT "Copying the Slang runtime libraries"
        VERBATIM)
    set_target_properties(cordite_slang_runtime PROPERTIES FOLDER "Cordite/Build")
endif()

# --- doctest: unit test framework -------------------------------------------------------
if(CORDITE_BUILD_TESTS)
    cordite_declare_dependency(doctest
        URL https://github.com/doctest/doctest/archive/refs/tags/v2.5.3.tar.gz
        SHA256 174ebc4e769928959614789c5b4e9c3d0a0f81a62bb608756b127bfebfb21331)
    cordite_dependency_option(DOCTEST_WITH_TESTS OFF)
    cordite_dependency_option(DOCTEST_WITH_MAIN_IN_STATIC_LIB OFF)
    cordite_dependency_option(DOCTEST_NO_INSTALL ON)
    FetchContent_MakeAvailable(doctest)
endif()
