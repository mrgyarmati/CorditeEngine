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

# --- EnTT: entity-component-system storage ----------------------------------------------
cordite_declare_dependency(entt
    URL https://github.com/skypjack/entt/archive/refs/tags/v3.16.0.tar.gz
    SHA256 7d7b4037b737992342049ffab14f22fa10243e01664f8c3a0657aa247ac52f71)
cordite_dependency_option(ENTT_BUILD_TESTING OFF)
cordite_dependency_option(ENTT_BUILD_DOCS OFF)
cordite_dependency_option(ENTT_INSTALL OFF)

# --- Dear ImGui: immediate-mode user interface (docking branch) -------------------------
# Has no CMake project; the cordite_imgui target below compiles the parts we use.
cordite_declare_dependency(imgui
    URL https://github.com/ocornut/imgui/archive/refs/tags/v1.92.9b-docking.tar.gz
    SHA256 90ded916bd57db2e0e171b6b098940a47c6f5042725dcdc67fb19940ca8bfdcc
    SOURCES_ONLY)

# --- cgltf: glTF 2.0 parsing (single header, compiled inside cordite_assets) -------------
cordite_declare_dependency(cgltf
    URL https://github.com/jkuhlmann/cgltf/archive/refs/tags/v1.15.tar.gz
    SHA256 84e352092e5cd6aab7f66de62ddb66beb5e6f18d412ca9d12950d7a55bfef25a
    SOURCES_ONLY)

# --- meshoptimizer: vertex welding and cache/fetch optimization of imported meshes ------
cordite_declare_dependency(meshoptimizer
    URL https://github.com/zeux/meshoptimizer/archive/refs/tags/v1.3.tar.gz
    SHA256 ca4f47d451201593904b9e3067fa54b65db94bc34ab5173864950862b748a145)
cordite_dependency_option(MESHOPT_BUILD_DEMO OFF)
cordite_dependency_option(MESHOPT_BUILD_GLTFPACK OFF)
cordite_dependency_option(MESHOPT_BUILD_SHARED_LIBS OFF)
cordite_dependency_option(MESHOPT_INSTALL OFF)

# --- MikkTSpace: the tangent basis every normal-map baker assumes ------------------------
# Has no releases and no CMake project; the cordite_mikktspace target below compiles it.
cordite_declare_dependency(mikktspace
    URL https://github.com/mmikk/MikkTSpace/archive/3e895b49d05ea07e4c2133156cfa94369e19e409.tar.gz
    SHA256 aeba65ddee85a679133d510d71d00b108f603752f90b15b9ecf7777033f6e351
    SOURCES_ONLY)

# --- Jolt Physics: rigid bodies, collision queries, character movement -------------------
# Its CMake project is in Build/. It is built like the engine around it: with exceptions
# and run-time type information - the engine derives from its interfaces - and for the
# instruction set every x86-64 processor has, not the newer ones Jolt would choose, so
# that a game does not stop on an older machine where its physics begins. It keeps to
# arithmetic that gives the same results on every compiler and platform, which is what
# lets a simulation be repeated. Its simulations on the GPU are left out: they need a
# shader compiler the build would have to find on the machine.
cordite_declare_dependency(joltphysics
    URL https://github.com/jrouwe/JoltPhysics/archive/refs/tags/v5.6.0.tar.gz
    SHA256 6e069ee0172478cc78182047aac87e5310ba14a67a53348ae14cc37801fd3f8e
    SOURCE_SUBDIR Build)
cordite_dependency_option(USE_STATIC_MSVC_RUNTIME_LIBRARY ON)
cordite_dependency_option(OVERRIDE_CXX_FLAGS OFF)
cordite_dependency_option(ENABLE_ALL_WARNINGS OFF)
cordite_dependency_option(INTERPROCEDURAL_OPTIMIZATION OFF)
cordite_dependency_option(CPP_EXCEPTIONS_ENABLED ON)
cordite_dependency_option(CPP_RTTI_ENABLED ON)
cordite_dependency_option(CROSS_PLATFORM_DETERMINISTIC ON)
cordite_dependency_option(USE_SSE4_1 OFF)
cordite_dependency_option(USE_SSE4_2 OFF)
cordite_dependency_option(USE_AVX OFF)
cordite_dependency_option(USE_AVX2 OFF)
cordite_dependency_option(USE_AVX512 OFF)
cordite_dependency_option(USE_LZCNT OFF)
cordite_dependency_option(USE_TZCNT OFF)
cordite_dependency_option(USE_F16C OFF)
cordite_dependency_option(USE_FMADD OFF)
cordite_dependency_option(DEBUG_RENDERER_IN_DEBUG_AND_RELEASE OFF)
cordite_dependency_option(PROFILER_IN_DEBUG_AND_RELEASE OFF)
cordite_dependency_option(ENABLE_OBJECT_STREAM OFF)
cordite_dependency_option(ENABLE_INSTALL OFF)
cordite_dependency_option(FLOATING_POINT_EXCEPTIONS_ENABLED OFF)
cordite_dependency_option(JPH_USE_DX12 OFF)
cordite_dependency_option(JPH_USE_VK OFF)
cordite_dependency_option(JPH_USE_MTL OFF)
cordite_dependency_option(JPH_USE_CPU_COMPUTE OFF)

# --- Lua: the language gameplay scripts are written in ----------------------------------
# Published as an archive of C sources with a makefile; the cordite_lua target below
# compiles them.
cordite_declare_dependency(lua
    URL https://www.lua.org/ftp/lua-5.4.8.tar.gz
    SHA256 4f18ddae154e793e46eeab727c59ef1c0c0c2b744e7b94219710d76f530629ae
    SOURCES_ONLY)

# --- Inter: the typeface of the interface (SIL Open Font License 1.1) ------------------
# Two of its files are compiled into the ui module: extras/ttf/Inter-Regular.ttf and
# Inter-SemiBold.ttf (see engine/cordite/ui/CMakeLists.txt).
cordite_declare_dependency(inter
    URL https://github.com/rsms/inter/releases/download/v4.1/Inter-4.1.zip
    SHA256 9883fdd4a49d4fb66bd8177ba6625ef9a64aa45899767dde3d36aa425756b11e
    SOURCES_ONLY)

# --- JetBrains Mono: the typeface of what is typed and logged (SIL Open Font License 1.1)
# One file is compiled into the ui module: fonts/ttf/JetBrainsMonoNL-Regular.ttf, the
# version without ligatures - what is typed is shown as it was typed.
cordite_declare_dependency(jetbrains_mono
    URL https://github.com/JetBrains/JetBrainsMono/releases/download/v2.304/JetBrainsMono-2.304.zip
    SHA256 6f6376c6ed2960ea8a963cd7387ec9d76e3f629125bc33d1fdcd7eb7012f7bbf
    SOURCES_ONLY)

# --- Lucide: the icons of the interface, as a font (ISC) ----------------------------
# The package lucide-static as the npm registry has it; font/lucide.ttf is compiled into
# the ui module, and font/lucide.css says which code point each icon has.
cordite_declare_dependency(lucide
    URL https://registry.npmjs.org/lucide-static/-/lucide-static-0.469.0.tgz
    SHA256 c4d7d11a4c5ebd5e6dcb21c50f7fb65aeda9a48e97827d1eb4d7638fd1ec694b
    SOURCES_ONLY)

# --- ltc_code: the fit of GGX by linearly transformed cosines (BSD-2-Clause) ------------
# The tables of Heitz, Dupuy, Hill and Neubelt, "Real-Time Polygonal-Light Shading with
# Linearly Transformed Cosines" (2016), that what a rectangle of light gives a shiny surface
# is worked out with (renderer/RectLights.h). Nothing of it is built: the renderer compiles
# in the numbers of fit/results/ltc.js. Its authors ask that the paper be cited where it is
# used; docs/third-party.md does.
cordite_declare_dependency(ltc_code
    URL https://github.com/selfshadow/ltc_code/archive/31e5e96b54f98f33098f8503003119ba2231a1c6.tar.gz
    SHA256 4c18b3a70ce42f23739ec5fb50e113725e8c0f9453923e70409e8315c0ac620c
    SOURCES_ONLY)

# --- miniaudio: sound - decoding, mixing, placing in space, and the devices that play it --
# One header and one source; the cordite_miniaudio target below compiles the source.
cordite_declare_dependency(miniaudio
    URL https://github.com/mackron/miniaudio/archive/refs/tags/0.11.25.tar.gz
    SHA256 b900edcffe979816e2560a0580b9b1216d674b4f17fbadeca8f777a7f8ab0274
    SOURCES_ONLY)

FetchContent_MakeAvailable(
    fmt spdlog nlohmann_json tl_expected enkits xxhash glm
    glfw vulkan_headers nvrhi stb cli11 entt imgui
    cgltf meshoptimizer mikktspace joltphysics lua miniaudio
    inter jetbrains_mono lucide ltc_code)

# Jolt checks itself where the engine does: in the configurations with assertions. They
# are reported through the engine's (see cordite/physics/PhysicsWorld.cpp).
target_compile_definitions(Jolt PUBLIC $<$<NOT:$<CONFIG:Release>>:JPH_ENABLE_ASSERTS>)
set_target_properties(Jolt PROPERTIES FOLDER "ThirdParty")

add_library(cordite_xxhash INTERFACE)
add_library(cordite::xxhash ALIAS cordite_xxhash)
target_include_directories(cordite_xxhash SYSTEM INTERFACE "${xxhash_SOURCE_DIR}")

add_library(cordite_stb INTERFACE)
add_library(cordite::stb ALIAS cordite_stb)
target_include_directories(cordite_stb SYSTEM INTERFACE "${stb_SOURCE_DIR}")

add_library(cordite_cgltf INTERFACE)
add_library(cordite::cgltf ALIAS cordite_cgltf)
target_include_directories(cordite_cgltf SYSTEM INTERFACE "${cgltf_SOURCE_DIR}")

add_library(cordite_mikktspace STATIC "${mikktspace_SOURCE_DIR}/mikktspace.c")
add_library(cordite::mikktspace ALIAS cordite_mikktspace)
target_include_directories(cordite_mikktspace SYSTEM PUBLIC "${mikktspace_SOURCE_DIR}")
set_target_properties(cordite_mikktspace PROPERTIES FOLDER "ThirdParty")

# miniaudio with its decoders for WAV, FLAC and MP3 and every way of playing sound the
# platform has - each looked for when the program runs, so that building needs none of
# them, and neither does a machine that has no sound. Its own CMake file is not used:
# it searches the machine for codecs and for other libraries to build against, and a
# build of Cordite is the same wherever it runs.
#
# Its resource manager is left out. It reads files by name on a thread of its own; here
# the asset manager reads files, and what plays a sound is handed samples. Whoever
# includes miniaudio.h has to see the definitions its source was compiled with, because
# they change what is in its structures: that is why they are PUBLIC.
add_library(cordite_miniaudio STATIC "${miniaudio_SOURCE_DIR}/miniaudio.c")
add_library(cordite::miniaudio ALIAS cordite_miniaudio)
target_include_directories(cordite_miniaudio SYSTEM PUBLIC "${miniaudio_SOURCE_DIR}")
target_compile_definitions(cordite_miniaudio PUBLIC MA_NO_RESOURCE_MANAGER)
if(UNIX)
    # What miniaudio says it needs: threads and the mathematics library, and on Linux
    # what loads a library while the program runs. Nothing more on macOS either.
    find_package(Threads REQUIRED)
    target_link_libraries(cordite_miniaudio PUBLIC Threads::Threads ${CMAKE_DL_LIBS} m)
endif()
set_target_properties(cordite_miniaudio PROPERTIES FOLDER "ThirdParty")

# Lua, without what reaches the machine a script runs on: the libraries for files, for
# the operating system, for loading code and for looking into the interpreter are not
# compiled, so no script can be given them - and neither are the two programs of the
# distribution, nor the file that opens all libraries at once. What reads and writes
# chunks that are compiled already belongs to the core and cannot be left out; the
# engine loads text only, and takes string.dump() away (ScriptRuntime.cpp).
#
# It is compiled as C++. An error in a script leaves the functions it passes through by
# an exception then, which runs their destructors on the way; compiled as C it would
# jump over them. Whoever includes its headers includes them as C++ as well - through
# engine/cordite/scripting/Lua.h, not lua.hpp, which says they are C.
#
# Lua is configured through engine/cordite/scripting/LuaConfig.h, which lua.h includes
# wherever it is used (LUA_USER_H) - in Lua's own sources as in the engine's. That header
# routes the checks of Lua's API into the engine's assertions, hence the link to core.
set(cordite_lua_sources)
foreach(cordite_lua_source
        lapi lauxlib lbaselib lcode lcorolib lctype ldebug ldo ldump lfunc lgc llex lmathlib lmem
        lobject lopcodes lparser lstate lstring lstrlib ltable ltablib ltm lundump lutf8lib lvm lzio)
    list(APPEND cordite_lua_sources "${lua_SOURCE_DIR}/src/${cordite_lua_source}.c")
endforeach()
set_source_files_properties(${cordite_lua_sources} PROPERTIES LANGUAGE CXX)
add_library(cordite_lua STATIC ${cordite_lua_sources})
add_library(cordite::lua ALIAS cordite_lua)
target_compile_features(cordite_lua PRIVATE cxx_std_20)
target_include_directories(cordite_lua SYSTEM PUBLIC "${lua_SOURCE_DIR}/src")
target_include_directories(cordite_lua PUBLIC "${PROJECT_SOURCE_DIR}/engine")
target_compile_definitions(cordite_lua PUBLIC LUA_USER_H="cordite/scripting/LuaConfig.h")
target_link_libraries(cordite_lua
    PUBLIC cordite::build_configuration
    PRIVATE $<LINK_ONLY:cordite::core>)
set_target_properties(cordite_lua PROPERTIES FOLDER "ThirdParty" LINKER_LANGUAGE CXX)
# Its warnings are its authors' to read.
if(MSVC)
    target_compile_options(cordite_lua PRIVATE /w)
else()
    target_compile_options(cordite_lua PRIVATE -w)
endif()

# Dear ImGui is configured through engine/cordite/ui/ImGuiConfig.h, which every file that
# includes imgui.h picks up through IMGUI_USER_CONFIG - ImGui's own sources included.
# That header routes ImGui's assertions into the engine's, hence the link to core.
add_library(cordite_imgui STATIC
    "${imgui_SOURCE_DIR}/imgui.cpp"
    "${imgui_SOURCE_DIR}/imgui_demo.cpp"
    "${imgui_SOURCE_DIR}/imgui_draw.cpp"
    "${imgui_SOURCE_DIR}/imgui_tables.cpp"
    "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
    "${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp")
add_library(cordite::imgui ALIAS cordite_imgui)
target_compile_features(cordite_imgui PUBLIC cxx_std_20)
target_include_directories(cordite_imgui SYSTEM PUBLIC "${imgui_SOURCE_DIR}")
target_include_directories(cordite_imgui PUBLIC "${PROJECT_SOURCE_DIR}/engine")
target_compile_definitions(cordite_imgui PUBLIC IMGUI_USER_CONFIG="cordite/ui/ImGuiConfig.h")
# The GLFW backend loads X11 with dlopen() on Linux.
target_link_libraries(cordite_imgui
    PUBLIC cordite::build_configuration
    PRIVATE glfw $<LINK_ONLY:cordite::core> ${CMAKE_DL_LIBS})
set_target_properties(cordite_imgui PROPERTIES FOLDER "ThirdParty")

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
