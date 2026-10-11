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

FetchContent_MakeAvailable(fmt spdlog nlohmann_json tl_expected enkits xxhash glm)

add_library(cordite_xxhash INTERFACE)
add_library(cordite::xxhash ALIAS cordite_xxhash)
target_include_directories(cordite_xxhash SYSTEM INTERFACE "${xxhash_SOURCE_DIR}")

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
