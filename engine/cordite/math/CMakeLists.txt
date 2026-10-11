cordite_add_module(math
    SOURCES
        Color.cpp
        Color.h
        Curve.h
        Encoding.h
        Frustum.cpp
        Frustum.h
        Geometry.cpp
        Geometry.h
        Math.h
        MathJson.cpp
        MathJson.h
        MathReflection.cpp
        MathReflection.h
        Projection.cpp
        Projection.h
        Random.cpp
        Random.h
        Transform.cpp
        Transform.h
    DEPENDS
        core
    PUBLIC_LIBRARIES
        glm::glm)

# These must be identical in every translation unit that includes glm, so they are part
# of the module's public interface rather than set in a header.
target_compile_definitions(cordite_math PUBLIC
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_FORCE_CTOR_INIT
    GLM_ENABLE_EXPERIMENTAL)
