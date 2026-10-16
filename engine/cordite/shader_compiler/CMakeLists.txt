cordite_add_module(shader_compiler
    SOURCES
        EngineShaderCache.cpp
        EngineShaderCache.h
        SlangShaderCompiler.cpp
        SlangShaderCompiler.h
    DEPENDS
        core
        shaders
    PRIVATE_LIBRARIES
        cordite::slang)

# Executables that link this module need the Slang runtime libraries beside them.
if(TARGET cordite_slang_runtime)
    add_dependencies(cordite_shader_compiler cordite_slang_runtime)
endif()
