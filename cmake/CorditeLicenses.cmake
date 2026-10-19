# The notices of the software the programs are made of, which go wherever a program goes:
# the licenses of what the engine links - MIT, BSD, zlib, Apache, the OFL of the typefaces
# of its interface - all ask for their text to accompany a copy. Every build copies them
# into licenses/ beside the programs, with docs/third-party.md as the list of them, as it
# copies the shaders; an export ships that directory with the game.
#
# A library that the engine starts to link adds its notice here in the same change. Lua and
# MikkTSpace keep their notices in a header, not in a file of their own; those are in
# cmake/licenses/, worded as their headers word them. Slang, which only the editor links,
# and doctest, which only the tests do, are not shipped with a game.

set(CORDITE_SHIPPED_LICENSES
    "cgltf|${cgltf_SOURCE_DIR}/LICENSE"
    "cli11|${cli11_SOURCE_DIR}/LICENSE"
    "enkits|${enkits_SOURCE_DIR}/License.txt"
    "entt|${entt_SOURCE_DIR}/LICENSE"
    "fmt|${fmt_SOURCE_DIR}/LICENSE"
    "glfw|${glfw_SOURCE_DIR}/LICENSE.md"
    "glm|${glm_SOURCE_DIR}/copying.txt"
    "imgui|${imgui_SOURCE_DIR}/LICENSE.txt"
    "inter|${inter_SOURCE_DIR}/LICENSE.txt"
    "jetbrains-mono|${jetbrains_mono_SOURCE_DIR}/OFL.txt"
    "jolt-physics|${joltphysics_SOURCE_DIR}/LICENSE"
    "ltc-code|${ltc_code_SOURCE_DIR}/LICENSE"
    "lua|${PROJECT_SOURCE_DIR}/cmake/licenses/lua.txt"
    "lucide|${lucide_SOURCE_DIR}/LICENSE"
    "meshoptimizer|${meshoptimizer_SOURCE_DIR}/LICENSE.md"
    "mikktspace|${PROJECT_SOURCE_DIR}/cmake/licenses/mikktspace.txt"
    "miniaudio|${miniaudio_SOURCE_DIR}/LICENSE"
    "nlohmann-json|${nlohmann_json_SOURCE_DIR}/LICENSE.MIT"
    "nvrhi|${nvrhi_SOURCE_DIR}/LICENSE.txt"
    "spdlog|${spdlog_SOURCE_DIR}/LICENSE"
    "stb|${stb_SOURCE_DIR}/LICENSE"
    "tl-expected|${tl_expected_SOURCE_DIR}/COPYING"
    "vulkan-headers|${vulkan_headers_SOURCE_DIR}/LICENSE.md"
    "xxhash|${xxhash_SOURCE_DIR}/LICENSE")
# A game of macOS brings MoltenVK in its bundle, and its notice with it.
if(APPLE)
    list(APPEND CORDITE_SHIPPED_LICENSES "moltenvk|${moltenvk_SOURCE_DIR}/LICENSE")
endif()
# On Windows the program draws with Direct3D 12 as well, and has its GUIDs compiled in.
if(WIN32)
    list(APPEND CORDITE_SHIPPED_LICENSES "directx-headers|${directx_headers_SOURCE_DIR}/LICENSE")
endif()

set(cordite_license_directory "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/licenses")
set(cordite_license_copies "")
foreach(entry IN LISTS CORDITE_SHIPPED_LICENSES)
    string(REPLACE "|" ";" parts "${entry}")
    list(GET parts 0 name)
    list(GET parts 1 file)
    if(NOT EXISTS "${file}")
        message(FATAL_ERROR "The notice of ${name} is not at ${file}; correct cmake/CorditeLicenses.cmake.")
    endif()
    list(APPEND cordite_license_copies
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${file}" "${cordite_license_directory}/${name}.txt")
endforeach()

add_custom_target(cordite_licenses ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${cordite_license_directory}"
    ${cordite_license_copies}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${PROJECT_SOURCE_DIR}/docs/third-party.md"
        "${cordite_license_directory}/THIRD-PARTY.md"
    COMMENT "Copying the notices of the libraries"
    VERBATIM)
set_target_properties(cordite_licenses PROPERTIES FOLDER "Cordite/Build")
