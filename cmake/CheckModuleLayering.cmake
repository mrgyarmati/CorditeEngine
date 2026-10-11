# Fails when an engine module includes a header from a module it does not depend on.
#
# Usage:
#   cmake -DENGINE_DIR=<repo>/engine/cordite
#         -DDEPENDENCY_TABLE="core=|math=core|platform=core,math"
#         -P CheckModuleLayering.cmake
#
# DEPENDENCY_TABLE lists each module's direct dependencies. A module may include its own
# headers and those of any module in its transitive dependency closure.

cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED ENGINE_DIR OR NOT DEFINED DEPENDENCY_TABLE)
    message(FATAL_ERROR "ENGINE_DIR and DEPENDENCY_TABLE must be defined.")
endif()

string(REPLACE "|" ";" entries "${DEPENDENCY_TABLE}")

set(modules "")
foreach(entry IN LISTS entries)
    string(FIND "${entry}" "=" separator)
    string(SUBSTRING "${entry}" 0 ${separator} module)
    math(EXPR value_start "${separator} + 1")
    string(SUBSTRING "${entry}" ${value_start} -1 dependencies)
    string(REPLACE "," ";" dependencies "${dependencies}")
    list(APPEND modules ${module})
    set(direct_${module} "${dependencies}")
endforeach()

# Computes the transitive dependency closure of a module into <out_var>.
function(resolve_closure module out_var)
    set(closure "")
    set(pending ${direct_${module}})
    while(pending)
        list(POP_FRONT pending current)
        if(NOT current IN_LIST closure)
            list(APPEND closure ${current})
            list(APPEND pending ${direct_${current}})
        endif()
    endwhile()
    set(${out_var} "${closure}" PARENT_SCOPE)
endfunction()

set(violations "")
foreach(module IN LISTS modules)
    resolve_closure(${module} allowed)
    list(APPEND allowed ${module})

    file(GLOB_RECURSE sources
        "${ENGINE_DIR}/${module}/*.h"
        "${ENGINE_DIR}/${module}/*.cpp"
        "${ENGINE_DIR}/${module}/*.inl")

    foreach(source IN LISTS sources)
        file(STRINGS "${source}" include_lines REGEX "^[ \t]*#[ \t]*include[ \t]*[\"<]cordite/")
        foreach(line IN LISTS include_lines)
            if(line MATCHES "cordite/([a-z_]+)/")
                set(included ${CMAKE_MATCH_1})
                if(NOT included IN_LIST allowed)
                    file(RELATIVE_PATH relative "${ENGINE_DIR}" "${source}")
                    string(APPEND violations
                        "  ${relative}: module '${module}' must not include from module '${included}'\n")
                endif()
            endif()
        endforeach()
    endforeach()
endforeach()

# A module directory that was never declared would silently escape the check above.
file(GLOB module_directories LIST_DIRECTORIES true RELATIVE "${ENGINE_DIR}" "${ENGINE_DIR}/*")
foreach(directory IN LISTS module_directories)
    if(IS_DIRECTORY "${ENGINE_DIR}/${directory}" AND NOT directory IN_LIST modules)
        string(APPEND violations "  ${directory}: directory is not a declared module (cordite_add_module)\n")
    endif()
endforeach()

if(violations)
    message(FATAL_ERROR "Module layering violations:\n${violations}"
        "Declare the dependency in the module's CMakeLists.txt, or move the code.")
endif()

list(LENGTH modules module_count)
message(STATUS "Module layering OK (${module_count} modules).")
