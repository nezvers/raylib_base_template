cmake_minimum_required(VERSION 3.24...3.30)
IF (DEFINED PROJECT_NAME)
    project(${PROJECT_NAME})
    MESSAGE(STATUS " - - - Project name is defined")
ELSE( DEFINED PROJECT_NAME )
    project(raylib-game-template)
ENDIF (DEFINED PROJECT_NAME)

include(FetchContent)

# Generate compile_commands.json
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Dependencies
function(add_git_dependency libName gitURL gitTag)
    FetchContent_Declare(${libName}
        GIT_REPOSITORY ${gitURL}
        GIT_TAG        ${gitTag}
        GIT_SHALLOW    TRUE
        GIT_PROGRESS   TRUE
    )
    FetchContent_MakeAvailable(${libName})
    target_compile_options(${libName} PRIVATE "-w")
endfunction()

## RAYLIB
set(RAYLIB_VERSION 6.0)

FetchContent_Declare(
    raylib
    DOWNLOAD_EXTRACT_TIMESTAMP OFF
    URL https://github.com/raysan5/raylib/archive/refs/tags/${RAYLIB_VERSION}.tar.gz
    FIND_PACKAGE_ARGS
)

FetchContent_MakeAvailable(raylib)

## Box2D
set(BOX2D_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(BOX2D_SAMPLES OFF CACHE BOOL "" FORCE)
set(BOX2D_VALIDATE OFF CACHE BOOL "" FORCE)
# Disable box2d SIMD: avoids -msimd128 flag mismatch with raylib/app objects on Web,
# and keeps the static lib portable across desktop archs.
set(BOX2D_DISABLE_SIMD ON CACHE BOOL "" FORCE)
add_git_dependency(
    box2d
    https://github.com/erincatto/box2d.git
    v3.1.1
)

# Our Project
add_executable(${PROJECT_NAME})
# set_property(TARGET ${PROJECT_NAME} PROPERTY C_STANDARD 99)
add_subdirectory(src)
include_directories(${CMAKE_SOURCE_DIR}/include)

set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${PROJECT_NAME})

set_property(TARGET ${PROJECT_NAME} PROPERTY VS_DEBUGGER_WORKING_DIRECTORY $<TARGET_FILE_DIR:${PROJECT_NAME}>)

if ("${PLATFORM}" STREQUAL "Web")
    add_custom_command(
        TARGET ${PROJECT_NAME} PRE_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/resources $<TARGET_FILE_DIR:${PROJECT_NAME}>/../resources
    )
    #DEPENDS ${PROJECT_NAME}
else()
    add_custom_command(
        TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/resources $<TARGET_FILE_DIR:${PROJECT_NAME}>/resources
    )
    #DEPENDS ${PROJECT_NAME}
endif()

#set(raylib_VERBOSE 1)
target_link_libraries(${PROJECT_NAME} raylib)
target_link_libraries(${PROJECT_NAME} box2d)

# Web Configurations
if ("${PLATFORM}" STREQUAL "Web")
    # Tell Emscripten to build an <project>.html file.
    set_target_properties(${PROJECT_NAME} PROPERTIES SUFFIX ".html")
    target_link_options(${PROJECT_NAME} PUBLIC
        -sUSE_GLFW=3
        -sTOTAL_MEMORY=134217728       # 128MB heap
        -sSTACK_SIZE=1048576           # 1MB stack
        -sFORCE_FILESYSTEM=1
        -sASSERTIONS=1
        --preload-file ${CMAKE_SOURCE_DIR}/resources@resources
        --shell-file ${CMAKE_SOURCE_DIR}/src/minshell.html)
endif()

if(WIN32)
    # Win socket for enet
    target_link_libraries(${PROJECT_NAME} ws2_32 winmm)
    if(MSVC)
        target_compile_options(enet_static PRIVATE /W3)
    endif()
endif()

# Headless unit tests for the anim / signal modules (desktop only; the test
# stubs ScreenStateTargetSize so no window/screen_state is needed).
if (NOT "${PLATFORM}" STREQUAL "Web")
    add_executable(anim_tests
        tests/anim_tests.c
        src/anim/anim.c
        src/anim/anim_io.c
        src/anim/anim_library.c
        src/anim/anim_stage.c
        src/anim/anim_scene.c
        src/anim/signal.c
        src/anim/anim_signal.c
        src/easing/easing.c)
    target_link_libraries(anim_tests raylib)
endif()

# Checks if OSX and links appropriate frameworks (Only required on MacOS)
if (APPLE)
    target_link_libraries(${PROJECT_NAME} "-framework IOKit")
    target_link_libraries(${PROJECT_NAME} "-framework Cocoa")
    target_link_libraries(${PROJECT_NAME} "-framework OpenGL")
endif()

# Setting RESOURCES_PATH
if (${PLATFORM} STREQUAL "Web")
  # Wasm builds have resources directory copied next to it
    target_compile_definitions(${PROJECT_NAME} PUBLIC RESOURCES_PATH="./resources/")
elseif(CMAKE_BUILD_TYPE STREQUAL "Debug")
  SET(CMAKE_CXX_FLAGS  "${CMAKE_CXX_FLAGS} -O0 -DDEBUG")
  SET(CMAKE_C_FLAGS  "${CMAKE_C_FLAGS} -O0 -DDEBUG")
  # Set the resources path macro to the absolute path on the dev machine
  target_compile_definitions(${PROJECT_NAME} PUBLIC RESOURCES_PATH="${CMAKE_CURRENT_SOURCE_DIR}/resources/")
else()
  # Set the resources path macro in release mode to a relative path that assumes the assets folder is in the same directory as the game executable
  target_compile_definitions(${PROJECT_NAME} PUBLIC RESOURCES_PATH="resources/")
  # TODO: Copy "resources" directory next to output

    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/resources
            $<TARGET_FILE_DIR:${PROJECT_NAME}>/resources
    )
endif()