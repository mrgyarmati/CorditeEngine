# Development tools: programs that help make the engine and its samples. They are not
# part of the editor and not of exported games.

# Generates the textures and models of the showcase project (see its file comment).
cordite_add_executable(cordite-sample-assets
    SOURCES
        sample-assets/Main.cpp
    LIBRARIES
        cordite::assets)
set_target_properties(cordite-sample-assets PROPERTIES FOLDER "Cordite/Tools")
