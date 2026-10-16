# Checks that programs are built for the Windows subsystem: one built for the console
# subsystem opens a console window beside its own when it is started with a double click.
#
#   cmake "-DPROGRAMS=<file>;<file>" -P CheckWindowsSubsystem.cmake
#
# The subsystem is a field of the optional header of a PE file. At 0x3C the file says where
# its PE header begins; the optional header follows the four bytes of "PE\0\0" and the 20
# of the file header, and holds the subsystem 68 bytes in: 2 is the Windows subsystem, 3
# the console.

cmake_minimum_required(VERSION 3.28)

# Reads `length` bytes of `file` at `offset`, as hexadecimal digits.
function(read_bytes file offset length result)
    file(READ "${file}" bytes OFFSET ${offset} LIMIT ${length} HEX)
    set(${result} "${bytes}" PARENT_SCOPE)
endfunction()

# The number that little-endian bytes, as hexadecimal digits, make.
function(number_of bytes result)
    string(LENGTH "${bytes}" position)
    set(digits "")
    while(position GREATER 0)
        math(EXPR position "${position} - 2")
        string(SUBSTRING "${bytes}" ${position} 2 byte)
        string(APPEND digits "${byte}")
    endwhile()
    math(EXPR value "0x${digits}")
    set(${result} ${value} PARENT_SCOPE)
endfunction()

if(NOT PROGRAMS)
    message(FATAL_ERROR "CheckWindowsSubsystem.cmake needs -DPROGRAMS=<file>;<file>")
endif()
foreach(program IN LISTS PROGRAMS)
    read_bytes("${program}" 60 4 headerBytes)
    number_of("${headerBytes}" header)
    read_bytes("${program}" ${header} 4 signature)
    if(NOT signature STREQUAL "50450000")
        message(FATAL_ERROR "${program} is no PE file")
    endif()
    math(EXPR subsystemOffset "${header} + 4 + 20 + 68")
    read_bytes("${program}" ${subsystemOffset} 2 subsystemBytes)
    number_of("${subsystemBytes}" subsystem)
    if(NOT subsystem EQUAL 2)
        message(FATAL_ERROR
            "${program} is built for subsystem ${subsystem}, not for the Windows subsystem (2): "
            "a console window opens beside it")
    endif()
    message(STATUS "${program}: the Windows subsystem")
endforeach()
