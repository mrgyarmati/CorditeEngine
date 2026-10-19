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

# Executables that link this module need the Slang runtime libraries beside them - and on
# Windows DXC's, through which Slang writes DXIL.
if(TARGET cordite_slang_runtime)
    add_dependencies(cordite_shader_compiler cordite_slang_runtime)
endif()

# What the shaders that DXC wrote were written by: a part of their version (SlangShaderCompiler.cpp),
# so that another DXC makes them anew. Empty where there is no DXC, and no DXIL.
target_compile_definitions(cordite_shader_compiler PRIVATE CORDITE_DXC_VERSION="${CORDITE_DXC_VERSION}")
