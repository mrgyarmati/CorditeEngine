cordite_add_executable(cordite-editor
    SOURCES
        Main.cpp
    LIBRARIES
        cordite::editor
        CLI11::CLI11)

# An editor exports games: the program of one is built beside it.
add_dependencies(cordite-editor cordite-player)
