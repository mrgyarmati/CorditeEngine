configure_file(
    cordite/core/Version.h.in
    "${PROJECT_BINARY_DIR}/generated/cordite/core/Version.h"
    @ONLY)

# Modules are listed in dependency order: a module may only depend on modules above it.
add_subdirectory(cordite/core)
add_subdirectory(cordite/math)
add_subdirectory(cordite/platform)
add_subdirectory(cordite/assets)
add_subdirectory(cordite/world)
add_subdirectory(cordite/animation)
add_subdirectory(cordite/physics)
add_subdirectory(cordite/audio)
add_subdirectory(cordite/scripting)
add_subdirectory(cordite/rhi)
add_subdirectory(cordite/shaders)
add_subdirectory(cordite/shader_compiler)
add_subdirectory(cordite/renderer)
add_subdirectory(cordite/ui)
add_subdirectory(cordite/engine)
add_subdirectory(cordite/editor)
