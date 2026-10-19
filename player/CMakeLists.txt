# The program of a game: what an export ships under the name of the game. It links the
# engine and nothing of the editor - no shader compiler: the shaders of a game come with
# it, compiled.
cordite_add_executable(cordite-player WINDOWED
    SOURCES
        Main.cpp
    LIBRARIES
        cordite::engine
        CLI11::CLI11)

# What it is made of goes with it: an export ships these notices beside it.
add_dependencies(cordite-player cordite_licenses)

# A game of macOS brings its Vulkan: MoltenVK beside the player, which an export puts into the
# Frameworks of the bundle (GameExport.cpp).
if(CORDITE_MOLTENVK_LIBRARY)
    add_custom_command(TARGET cordite-player POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CORDITE_MOLTENVK_LIBRARY}" "$<TARGET_FILE_DIR:cordite-player>"
        COMMENT "Copying MoltenVK beside the player"
        VERBATIM)
endif()
