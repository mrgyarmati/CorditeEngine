# Writes a C++ source that holds the bytes of a file (see CorditeEmbed.cmake):
#
#     cmake -DINPUT=<file> -DOUTPUT=<source> -DSYMBOL=<name> -P CorditeEmbedFile.cmake

foreach(variable INPUT OUTPUT SYMBOL)
    if(NOT DEFINED ${variable})
        message(FATAL_ERROR "CorditeEmbedFile.cmake needs -D${variable}=...")
    endif()
endforeach()

file(READ "${INPUT}" bytes HEX)
string(LENGTH "${bytes}" digits)
if(digits EQUAL 0)
    # An array of no elements is not C++.
    message(FATAL_ERROR "${INPUT} is empty; there is nothing to embed")
endif()

# 0x.. for every byte, sixteen to a line (CMake has no {16} in its expressions).
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${bytes}")
string(REPEAT "0x..," 16 line)
string(REGEX REPLACE "(${line})" "\\1\n" bytes "${bytes}")

file(WRITE "${OUTPUT}.part"
    "// Written by cmake/CorditeEmbedFile.cmake from ${INPUT}. Do not edit.\n"
    "#include <span>\n"
    "\n"
    "namespace cordite::embedded {\n"
    "namespace {\n"
    "\n"
    "const unsigned char kBytes[] = {\n"
    "${bytes}\n"
    "};\n"
    "\n"
    "} // namespace\n"
    "\n"
    "std::span<const unsigned char> ${SYMBOL}();\n"
    "\n"
    "std::span<const unsigned char> ${SYMBOL}() {\n"
    "    return kBytes;\n"
    "}\n"
    "\n"
    "} // namespace cordite::embedded\n")
# Put in place whole: a build that is stopped half way leaves no half of a source behind.
file(RENAME "${OUTPUT}.part" "${OUTPUT}")
