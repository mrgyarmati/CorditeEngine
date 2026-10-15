# Files compiled into a target.
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
