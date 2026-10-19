# Writes a C++ source that holds the numbers of an array of a JavaScript file (see
# CorditeEmbed.cmake):
#
#     cmake -DINPUT=<file> -DARRAY=<name> -DCOUNT=<n> -DOUTPUT=<source> -DSYMBOL=<name>
#           -P CorditeEmbedNumbers.cmake
#
# The array is the text between `var <name> = [` and the `]` after it. Everything in it
# must be a number, written as C++ reads one, or what separates numbers - and there must
# be <n> of them: what the file says is checked here, where a mistake stops the build,
# not where a program would run on it.

foreach(variable INPUT ARRAY COUNT OUTPUT SYMBOL)
    if(NOT DEFINED ${variable})
        message(FATAL_ERROR "CorditeEmbedNumbers.cmake needs -D${variable}=...")
    endif()
endforeach()

file(READ "${INPUT}" text)
set(opening "var ${ARRAY} = [")
string(FIND "${text}" "${opening}" start)
if(start EQUAL -1)
    message(FATAL_ERROR "${INPUT} has no array named ${ARRAY}")
endif()
string(LENGTH "${opening}" openingLength)
math(EXPR start "${start} + ${openingLength}")
string(SUBSTRING "${text}" ${start} -1 rest)
string(FIND "${rest}" "]" end)
if(end EQUAL -1)
    message(FATAL_ERROR "the array ${ARRAY} of ${INPUT} does not end")
endif()
string(SUBSTRING "${rest}" 0 ${end} body)

# What is left once the numbers and what separates them are taken out must be nothing.
set(number "-?[0-9]+(\\.[0-9]+)?([eE][-+]?[0-9]+)?")
string(REGEX MATCHALL "${number}" numbers "${body}")
string(REGEX REPLACE "${number}" "" leftover "${body}")
string(REGEX REPLACE "[, \t\r\n]" "" leftover "${leftover}")
if(NOT leftover STREQUAL "")
    message(FATAL_ERROR "the array ${ARRAY} of ${INPUT} holds what is not a number: ${leftover}")
endif()
list(LENGTH numbers found)
if(NOT found EQUAL COUNT)
    message(FATAL_ERROR "the array ${ARRAY} of ${INPUT} has ${found} numbers, not ${COUNT}")
endif()
list(JOIN numbers ",\n" numbers)

file(WRITE "${OUTPUT}.part"
    "// Written by cmake/CorditeEmbedNumbers.cmake from the array ${ARRAY} of ${INPUT}.\n"
    "// Do not edit.\n"
    "#include <span>\n"
    "\n"
    "namespace cordite::embedded {\n"
    "namespace {\n"
    "\n"
    "const double kNumbers[] = {\n"
    "${numbers}\n"
    "};\n"
    "static_assert(sizeof(kNumbers) / sizeof(kNumbers[0]) == ${COUNT});\n"
    "\n"
    "} // namespace\n"
    "\n"
    "std::span<const double> ${SYMBOL}();\n"
    "\n"
    "std::span<const double> ${SYMBOL}() {\n"
    "    return kNumbers;\n"
    "}\n"
    "\n"
    "} // namespace cordite::embedded\n")
# Put in place whole: a build that is stopped half way leaves no half of a source behind.
file(RENAME "${OUTPUT}.part" "${OUTPUT}")
