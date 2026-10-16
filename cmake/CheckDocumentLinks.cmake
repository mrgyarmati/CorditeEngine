# Checks that the links between the documents of the repository lead somewhere: every
# relative link of their Markdown names a file or a directory that is there. Links to the
# web, and to a place in the same page, are not followed.
#
#   cmake -DROOT=<repository> -P CheckDocumentLinks.cmake

cmake_minimum_required(VERSION 3.28)

if(NOT ROOT)
    message(FATAL_ERROR "CheckDocumentLinks.cmake needs -DROOT=<repository>")
endif()

file(GLOB_RECURSE documents RELATIVE "${ROOT}"
    "${ROOT}/*.md")
# What the repository writes and fetches is not its documents.
list(FILTER documents EXCLUDE REGEX "^(build|\\.tools|\\.git)/")
list(FILTER documents EXCLUDE REGEX "/(_deps|node_modules)/")

set(broken "")
set(checked 0)
foreach(document IN LISTS documents)
    file(READ "${ROOT}/${document}" text)
    get_filename_component(directory "${ROOT}/${document}" DIRECTORY)
    # A list of CMake does not end an element at a semicolon inside square brackets, which
    # every link has: where a link's target begins is marked first, and the brackets go.
    string(REPLACE "](" "@target@(" text "${text}")
    string(REPLACE "[" "" text "${text}")
    string(REPLACE "]" "" text "${text}")
    # [text](target) and [text](target#place), where the target is a path: no scheme, no
    # spaces, not only a place in the page.
    string(REGEX MATCHALL "@target@\\([^):#; ]+(#[^); ]*)?\\)" links "${text}")
    foreach(link IN LISTS links)
        string(REGEX REPLACE "^@target@\\(([^):#; ]+)(#[^); ]*)?\\)$" "\\1" target "${link}")
        math(EXPR checked "${checked} + 1")
        if(NOT EXISTS "${directory}/${target}")
            list(APPEND broken "${document}: ${target}")
        endif()
    endforeach()
endforeach()

if(broken)
    list(JOIN broken "\n  " list)
    message(FATAL_ERROR "links that lead nowhere:\n  ${list}")
endif()
list(LENGTH documents count)
message(STATUS "${checked} links of ${count} documents lead somewhere")
