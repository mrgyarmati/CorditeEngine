# Fails when a test is written in a way that works until someone asks doctest for more.
#
# Usage:
#   cmake -DREPOSITORY_DIR=<repo> -P CheckTestSources.cmake
#
# One rule so far: the message of an assertion must not ask a Result for its error.
#
#     REQUIRE_MESSAGE(result.has_value(), result.error().message);    // wrong
#     REQUIRE_MESSAGE(result.has_value(), test::errorOf(result));     // right
#
# doctest builds the message of an assertion when the assertion fails - and also when it
# passes, if it was asked to list the assertions that pass (--success, -s). Asking a
# Result that holds a value for its error is undefined behavior; with -s, every suite
# that did so crashed in its first test.

cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED REPOSITORY_DIR)
    message(FATAL_ERROR "REPOSITORY_DIR must be defined.")
endif()

file(GLOB_RECURSE sources "${REPOSITORY_DIR}/tests/*.h" "${REPOSITORY_DIR}/tests/*.cpp")

set(violations "")
foreach(source IN LISTS sources)
    file(STRINGS "${source}" messages REGEX "_MESSAGE[ \t]*\\(.*\\.error\\(\\)")
    if(messages)
        file(RELATIVE_PATH relative "${REPOSITORY_DIR}" "${source}")
        list(LENGTH messages count)
        string(APPEND violations "  ${relative}: ${count} assertion message(s) ask a Result for its error\n")
    endif()
endforeach()

if(violations)
    message(FATAL_ERROR "Test sources that crash when doctest lists passing assertions (-s):\n${violations}"
        "Write test::errorOf(result) in the message instead of result.error().")
endif()

list(LENGTH sources source_count)
message(STATUS "Test sources OK (${source_count} files).")
