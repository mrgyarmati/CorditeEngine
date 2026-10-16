cordite_add_module(engine
    SOURCES
        Engine.cpp
        Engine.h
        EngineComponents.cpp
        EngineComponents.h
        Game.cpp
        Game.h
        GameSettings.cpp
        GameSettings.h
        ModelInstance.cpp
        ModelInstance.h
        Project.cpp
        Project.h
        SceneAssets.cpp
        SceneAssets.h
        SceneChange.cpp
        SceneChange.h
    DEPENDS
        core
        math
        platform
        assets
        world
        animation
        physics
        audio
        scripting
        rhi
        shaders
        renderer
        ui)

# Development builds read shader sources straight from the repository, so edits are
# picked up without a copy step. Shipping builds get their shaders from the game package.
target_compile_definitions(cordite_engine PRIVATE
    $<$<NOT:$<CONFIG:Release>>:CORDITE_ENGINE_SHADER_DIR="${PROJECT_SOURCE_DIR}/shaders">)
