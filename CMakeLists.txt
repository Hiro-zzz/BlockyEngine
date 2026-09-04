cmake_minimum_required(VERSION 3.21)
project(BlockyEngine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

if(MSVC)
    add_compile_options(
        /utf-8              # sources are UTF-8, and so is the exec charset
        /W4 /permissive-    # strict, conforming
        /Zc:preprocessor
        /EHsc
        /MP
    )
    # Keep the CRT static so scene executables are trivially portable.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    add_compile_options($<$<NOT:$<CONFIG:Debug>>:/O2>)
endif()

# ---------------------------------------------------------------- engine core
file(GLOB_RECURSE ENGINE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/engine/*.cpp"
    "${CMAKE_SOURCE_DIR}/engine/*.hpp")

add_library(blocky STATIC ${ENGINE_SOURCES})
target_include_directories(blocky PUBLIC "${CMAKE_SOURCE_DIR}")

# The viewport talks to Win32 and WGL directly, so these are the only
# libraries the engine links -- all of them ship with the OS.
if(WIN32)
    target_link_libraries(blocky PUBLIC opengl32 user32 gdi32)
endif()
source_group(TREE "${CMAKE_SOURCE_DIR}/engine" FILES ${ENGINE_SOURCES})

# ------------------------------------------------------------------- scenes
# Every scenes/<name>.cpp becomes its own executable: scene_<name>.exe
file(GLOB SCENE_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/scenes/*.cpp")
foreach(scene_src IN LISTS SCENE_SOURCES)
    get_filename_component(scene_name "${scene_src}" NAME_WE)
    add_executable(scene_${scene_name} "${scene_src}")
    target_link_libraries(scene_${scene_name} PRIVATE blocky)
    set_target_properties(scene_${scene_name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/scenes")
endforeach()

list(LENGTH SCENE_SOURCES scene_count)
message(STATUS "BlockyEngine: ${scene_count} scene(s) registered")

# --------------------------------------------------------------------- game
# Everything under game/ builds into one executable, unlike scenes/ where each
# file is its own. That is the difference the directory exists to mark: a scene
# renders one thing and exits, and a game has parts that have to know about
# each other.
file(GLOB_RECURSE GAME_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/game/*.cpp"
    "${CMAKE_SOURCE_DIR}/game/*.hpp")

if(GAME_SOURCES)
    add_executable(konstruct ${GAME_SOURCES})
    target_link_libraries(konstruct PRIVATE blocky)
    set_target_properties(konstruct PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    source_group(TREE "${CMAKE_SOURCE_DIR}/game" FILES ${GAME_SOURCES})

    list(LENGTH GAME_SOURCES game_count)
    message(STATUS "BlockyEngine: game target from ${game_count} file(s)")
endif()
