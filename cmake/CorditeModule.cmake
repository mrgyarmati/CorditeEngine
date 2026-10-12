# Helpers for declaring engine modules and their test suites.
#
# An engine module is a static library under engine/cordite/<name>. Modules list the other
# modules they may use in DEPENDS; the module_layering test fails the build when a module
# includes a header from a module outside its (transitive) dependencies.

# cordite_add_module(<name>
#     SOURCES <files...>
#     [DEPENDS <module names...>]
#     [PUBLIC_LIBRARIES <targets...>]
#     [PRIVATE_LIBRARIES <targets...>])
function(cordite_add_module name)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "" "SOURCES;DEPENDS;PUBLIC_LIBRARIES;PRIVATE_LIBRARIES")

    set(target cordite_${name})
    add_library(${target} STATIC ${arg_SOURCES})
    add_library(cordite::${name} ALIAS ${target})

    target_include_directories(${target} PUBLIC
        "${PROJECT_SOURCE_DIR}/engine"
        "${PROJECT_BINARY_DIR}/generated")

    set(module_targets "")
    foreach(dependency IN LISTS arg_DEPENDS)
        list(APPEND module_targets cordite::${dependency})
    endforeach()

    target_link_libraries(${target}
        PUBLIC cordite::build_options ${module_targets} ${arg_PUBLIC_LIBRARIES}
        PRIVATE ${arg_PRIVATE_LIBRARIES})

    set_target_properties(${target} PROPERTIES
        FOLDER "Cordite/Engine"
        COMPILE_WARNING_AS_ERROR ${CORDITE_WARNINGS_AS_ERRORS})

    set_property(GLOBAL APPEND PROPERTY CORDITE_MODULES ${name})
    set_property(GLOBAL PROPERTY CORDITE_MODULE_DEPENDS_${name} "${arg_DEPENDS}")
endfunction()

# cordite_add_executable(<name> SOURCES <files...> [LIBRARIES <targets...>])
function(cordite_add_executable name)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "" "SOURCES;LIBRARIES")

    add_executable(${name} ${arg_SOURCES})
    target_link_libraries(${name} PRIVATE cordite::build_options ${arg_LIBRARIES})
    set_target_properties(${name} PROPERTIES
        FOLDER "Cordite/Applications"
        COMPILE_WARNING_AS_ERROR ${CORDITE_WARNINGS_AS_ERRORS})
endfunction()

# cordite_add_test_suite(<name>
#     SOURCES <files...>
#     LIBRARIES <targets...>
#     [LABELS <ctest labels...>]
#     [WORKING_DIRECTORY <dir>]
#     [TIMEOUT <seconds>])
#
# Creates the executable cordite_<name>_tests and registers it with CTest as <name>.
# A suite that runs longer than TIMEOUT - five minutes, unless it says otherwise - is
# stopped and counts as failed: the limit is there to end a test that hangs, so a suite
# that has grown needs a larger one, not fewer tests.
# Suites that need a Vulkan device must carry the 'gpu' label so headless machines can
# skip them with: ctest -LE gpu
function(cordite_add_test_suite name)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "WORKING_DIRECTORY;TIMEOUT" "SOURCES;LIBRARIES;LABELS")

    set(target cordite_${name}_tests)
    add_executable(${target} ${arg_SOURCES})
    target_link_libraries(${target} PRIVATE cordite::test_support ${arg_LIBRARIES})
    set_target_properties(${target} PROPERTIES
        FOLDER "Cordite/Tests"
        COMPILE_WARNING_AS_ERROR ${CORDITE_WARNINGS_AS_ERRORS})

    if(NOT arg_WORKING_DIRECTORY)
        set(arg_WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
    endif()
    if(NOT arg_TIMEOUT)
        set(arg_TIMEOUT 300)
    endif()

    add_test(NAME ${name}
        COMMAND ${target}
        WORKING_DIRECTORY "${arg_WORKING_DIRECTORY}")
    set_tests_properties(${name} PROPERTIES LABELS "${arg_LABELS}" TIMEOUT ${arg_TIMEOUT})
    cordite_configure_gpu_test(${name} "${arg_LABELS}")
endfunction()

# Points tests labelled 'gpu' at the locally bootstrapped Vulkan validation layers,
# when present, so they run with full validation without a system-wide SDK install.
function(cordite_configure_gpu_test name labels)
    if("gpu" IN_LIST labels AND CORDITE_VULKAN_LAYER_PATH)
        set_tests_properties(${name} PROPERTIES
            ENVIRONMENT "VK_ADD_LAYER_PATH=${CORDITE_VULKAN_LAYER_PATH}")
    endif()
endfunction()

# Registers the module_layering test. Call after every module has been declared.
function(cordite_add_layering_test)
    get_property(modules GLOBAL PROPERTY CORDITE_MODULES)

    set(dependency_table "")
    foreach(module IN LISTS modules)
        get_property(dependencies GLOBAL PROPERTY CORDITE_MODULE_DEPENDS_${module})
        string(REPLACE ";" "," dependencies "${dependencies}")
        list(APPEND dependency_table "${module}=${dependencies}")
    endforeach()
    string(REPLACE ";" "|" dependency_table "${dependency_table}")

    add_test(NAME module_layering
        COMMAND ${CMAKE_COMMAND}
            "-DREPOSITORY_DIR=${PROJECT_SOURCE_DIR}"
            "-DDEPENDENCY_TABLE=${dependency_table}"
            -P "${PROJECT_SOURCE_DIR}/cmake/CheckModuleLayering.cmake")
    set_tests_properties(module_layering PROPERTIES LABELS "lint")
endfunction()
