# Files compiled into a target, and the numbers of files.
#
# cordite_embed_file(<target> <function> <file>)
#
# Adds to <target> a source that defines, in the namespace cordite::embedded,
#
#     std::span<const unsigned char> <function>();    // the bytes of <file>
#
# which a source of the target declares for itself, as cordite/ui/UiFonts.cpp does. The
# source is written at build time by CorditeEmbedFile.cmake, and again whenever <file>
# changes; a program has the bytes wherever it runs, without looking for a file. It is
# for files of up to a few megabytes: compilers take every byte as an element of an
# array.

set(CORDITE_EMBED_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/CorditeEmbedFile.cmake")
set(CORDITE_EMBED_NUMBERS_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/CorditeEmbedNumbers.cmake")

function(cordite_embed_file target function file)
    if(NOT EXISTS "${file}")
        message(FATAL_ERROR "cordite_embed_file: ${file} does not exist")
    endif()
    set(output "${CMAKE_CURRENT_BINARY_DIR}/embedded/${function}.cpp")
    add_custom_command(
        OUTPUT "${output}"
        COMMAND "${CMAKE_COMMAND}" "-DINPUT=${file}" "-DOUTPUT=${output}" "-DSYMBOL=${function}"
                -P "${CORDITE_EMBED_SCRIPT}"
        DEPENDS "${file}" "${CORDITE_EMBED_SCRIPT}"
        COMMENT "Embedding ${file}"
        VERBATIM)
    target_sources(${target} PRIVATE "${output}")
endfunction()

# cordite_embed_numbers(<target> <function> <file> <array> <count>)
#
# Adds to <target> a source that defines, in the namespace cordite::embedded,
#
#     std::span<const double> <function>();    // the numbers of <array> in <file>
#
# for data that comes as an array of a JavaScript file - `var <array> = [1, 0.5, 2e-05];`
# - as cordite/renderer/RectLights.cpp has its fit. There must be <count> numbers in the
# array, and nothing else, or the build stops: CorditeEmbedNumbers.cmake checks it as it
# writes the source.
function(cordite_embed_numbers target function file array count)
    if(NOT EXISTS "${file}")
        message(FATAL_ERROR "cordite_embed_numbers: ${file} does not exist")
    endif()
    set(output "${CMAKE_CURRENT_BINARY_DIR}/embedded/${function}.cpp")
    add_custom_command(
        OUTPUT "${output}"
        COMMAND "${CMAKE_COMMAND}" "-DINPUT=${file}" "-DARRAY=${array}" "-DCOUNT=${count}" "-DOUTPUT=${output}"
                "-DSYMBOL=${function}" -P "${CORDITE_EMBED_NUMBERS_SCRIPT}"
        DEPENDS "${file}" "${CORDITE_EMBED_NUMBERS_SCRIPT}"
        COMMENT "Embedding the array ${array} of ${file}"
        VERBATIM)
    target_sources(${target} PRIVATE "${output}")
endfunction()
