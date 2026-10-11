cordite_add_module(engine
    SOURCES
        Engine.cpp
        Engine.h
        EngineComponents.cpp
        EngineComponents.h
    DEPENDS
        core
        math
        platform
        assets
        world
        rhi
        shaders
        shader_compiler
        renderer
        ui)

# Development builds read shader sources straight from the repository, so edits are
# picked up without a copy step. Shipping builds get their shaders from the game package.
target_compile_definitions(cordite_engine PRIVATE
    $<$<NOT:$<CONFIG:Release>>:CORDITE_ENGINE_SHADER_DIR="${PROJECT_SOURCE_DIR}/shaders">)
