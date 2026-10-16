# The program of a game: what an export ships under the name of the game. It links the
# engine and nothing of the editor - no shader compiler: the shaders of a game come with
# it, compiled.
cordite_add_executable(cordite-player
    SOURCES
        Main.cpp
    LIBRARIES
        cordite::engine
        CLI11::CLI11)
