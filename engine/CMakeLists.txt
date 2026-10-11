configure_file(
    cordite/core/Version.h.in
    "${PROJECT_BINARY_DIR}/generated/cordite/core/Version.h"
    @ONLY)

# Modules are listed in dependency order: a module may only depend on modules above it.
add_subdirectory(cordite/core)
add_subdirectory(cordite/math)
