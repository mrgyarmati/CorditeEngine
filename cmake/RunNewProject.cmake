# Makes new projects with the editor as a person does - from the command line, and with
# File > New project, which is the command project.new - and plays the game that is exported
# of one, from where it was put: a project begins with a scene that runs.
#
#   cmake -DEDITOR=<cordite-editor> -DDIRECTORY=<directory to work in>
#         -DPROGRAM=<file name of the game's program> -P RunNewProject.cmake

foreach(argument EDITOR DIRECTORY PROGRAM)
    if(NOT DEFINED ${argument})
        message(FATAL_ERROR "RunNewProject.cmake needs -D${argument}=...")
    endif()
endforeach()

file(REMOVE_RECURSE "${DIRECTORY}")
file(MAKE_DIRECTORY "${DIRECTORY}")

# From the command line.
execute_process(
    COMMAND "${EDITOR}" --new-project "${DIRECTORY}/first" --name "Smoke Test" --headless --frames 2
    RESULT_VARIABLE made)
if(NOT made EQUAL 0 OR NOT EXISTS "${DIRECTORY}/first/project.cordite")
    message(FATAL_ERROR "cordite-editor --new-project failed with ${made}")
endif()

# With the command, from an editor that has a project open: it ends, and another editor
# begins on the new project, with the frames that are left.
file(WRITE "${DIRECTORY}/new-project.json"
    "[{\"command\": \"project.new\", \"params\": {\"path\": \"${DIRECTORY}/second\", \"name\": \"Second\"}}]")
execute_process(
    COMMAND "${EDITOR}" --project "${DIRECTORY}/first" --headless --validation --exec "${DIRECTORY}/new-project.json"
        --frames 2 --screenshot "${DIRECTORY}/second.png"
    RESULT_VARIABLE switched
    ERROR_VARIABLE log)
if(NOT switched EQUAL 0)
    message(FATAL_ERROR "project.new failed with ${switched}:\n${log}")
endif()
if(NOT EXISTS "${DIRECTORY}/second/project.cordite" OR NOT EXISTS "${DIRECTORY}/second.png")
    message(FATAL_ERROR "project.new made no project, or the editor that opened it drew nothing")
endif()
if(NOT log MATCHES "opening the project")
    message(FATAL_ERROR "the editor did not open the new project:\n${log}")
endif()

# A new project is a game.
execute_process(
    COMMAND "${EDITOR}" --project "${DIRECTORY}/first" --export "${DIRECTORY}/game"
    RESULT_VARIABLE exported)
if(NOT exported EQUAL 0)
    message(FATAL_ERROR "exporting the new project failed with ${exported}")
endif()
execute_process(
    COMMAND "${DIRECTORY}/game/${PROGRAM}" --headless --validation --frames 30 --fixed-time-step 0.0166667
        --width 640 --height 360 --screenshot "${DIRECTORY}/game.png"
    WORKING_DIRECTORY "${DIRECTORY}/game"
    RESULT_VARIABLE played)
if(NOT played EQUAL 0 OR NOT EXISTS "${DIRECTORY}/game.png")
    message(FATAL_ERROR "the game of the new project failed with ${played}")
endif()
