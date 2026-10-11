# Shared test harness: the doctest entry point plus fixtures used by every suite.
add_library(cordite_test_support STATIC
    support/TestMain.cpp
    support/TestSupport.cpp
    support/TestSupport.h
    support/MathTestSupport.h)
add_library(cordite::test_support ALIAS cordite_test_support)

target_include_directories(cordite_test_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(cordite_test_support PUBLIC doctest::doctest cordite::core)
target_compile_definitions(cordite_test_support PRIVATE
    CORDITE_TEST_TEMP_DIR="${PROJECT_BINARY_DIR}/test-tmp"
    CORDITE_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
set_target_properties(cordite_test_support PROPERTIES
    FOLDER "Cordite/Tests"
    COMPILE_WARNING_AS_ERROR ${CORDITE_WARNINGS_AS_ERRORS})

cordite_add_test_suite(core
    SOURCES
        unit/core/AssertTests.cpp
        unit/core/BinaryStreamTests.cpp
        unit/core/ConfigTests.cpp
        unit/core/FileSystemTests.cpp
        unit/core/HashTests.cpp
        unit/core/JobSystemTests.cpp
        unit/core/JsonTests.cpp
        unit/core/LogTests.cpp
        unit/core/MemoryTests.cpp
        unit/core/PlatformTests.cpp
        unit/core/ResultTests.cpp
        unit/core/StringUtilsTests.cpp
        unit/core/TimeTests.cpp
        unit/core/UuidTests.cpp
    LIBRARIES
        cordite::core)

cordite_add_test_suite(math
    SOURCES
        unit/math/ColorTests.cpp
        unit/math/CurveTests.cpp
        unit/math/EncodingTests.cpp
        unit/math/FrustumTests.cpp
        unit/math/GeometryTests.cpp
        unit/math/MathJsonTests.cpp
        unit/math/ProjectionTests.cpp
        unit/math/RandomTests.cpp
        unit/math/TransformTests.cpp
    LIBRARIES
        cordite::math)

cordite_add_layering_test()
