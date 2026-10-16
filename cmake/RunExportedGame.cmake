# Makes a game of a project with the editor, as a person would, and plays it from where it
# was put, as a player would: nothing of the build is beside it there, so what it needs and
# did not get is found here.
#
#   cmake -DEDITOR=<cordite-editor> -DPROJECT=<project directory> -DDESTINATION=<directory>
#         -DPROGRAM=<file name of the game's program> -DSCREENSHOT=<png> -P RunExportedGame.cmake

foreach(argument EDITOR PROJECT DESTINATION PROGRAM SCREENSHOT)
    if(NOT DEFINED ${argument})
        message(FATAL_ERROR "RunExportedGame.cmake needs -D${argument}=...")
    endif()
endforeach()

file(REMOVE_RECURSE "${DESTINATION}")
execute_process(
    COMMAND "${EDITOR}" --project "${PROJECT}" --export "${DESTINATION}"
    RESULT_VARIABLE exported)
if(NOT exported EQUAL 0)
    message(FATAL_ERROR "exporting ${PROJECT} failed with ${exported}")
endif()

set(game "${DESTINATION}/${PROGRAM}")
if(NOT EXISTS "${game}")
    message(FATAL_ERROR "the export did not write ${game}")
endif()
# The settings of a person who plays, kept away from the game, which an export replaces.
set(settings "${DESTINATION}-settings/settings.json")
file(REMOVE_RECURSE "${DESTINATION}-settings")
execute_process(
    COMMAND "${game}" --headless --validation --frames 60 --fixed-time-step 0.0166667 --width 640 --height 360
        --settings "${settings}" --screenshot "${SCREENSHOT}"
    WORKING_DIRECTORY "${DESTINATION}"
    RESULT_VARIABLE played)
if(NOT played EQUAL 0)
    message(FATAL_ERROR "the game made of ${PROJECT} failed with ${played}")
endif()
if(NOT EXISTS "${SCREENSHOT}")
    message(FATAL_ERROR "the game did not save its last frame")
endif()
# Written the first time, with the defaults: what the command line said was for one start.
if(NOT EXISTS "${settings}")
    message(FATAL_ERROR "the game did not write the settings of its player to ${settings}")
endif()
file(READ "${settings}" written)
if(NOT written MATCHES "\"format\": \"cordite.settings\"" OR NOT written MATCHES "\"width\": 1280")
    message(FATAL_ERROR "the game did not write the default settings:\n${written}")
endif()
