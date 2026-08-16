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

# Headless unit tests (desktop only; the tests stub ScreenStateTargetSize so no
# window/screen_state is needed).
#
# Two suites:
#   anim_tests - the ARCHIVED legacy suite. Frozen: build it and run it, but do
#                not edit or even open tests/archive/ - it is ~3k lines.
#   zen_tests  - where NEW checks go. Kept small on purpose.
set(ANIM_TEST_SRC
    src/anim/anim.c
    src/anim/anim_ease_custom.c
    src/anim/anim_io.c
    src/anim/anim_library.c
    src/anim/anim_stage.c
    src/anim/anim_scene.c
    src/anim/anim_shape_pool.c
    src/anim/signal.c
    src/anim/anim_signal.c
    src/easing/easing.c)

if (NOT "${PLATFORM}" STREQUAL "Web")
    add_executable(anim_tests
        tests/archive/anim_tests_archived.c
        ${ANIM_TEST_SRC})
    target_link_libraries(anim_tests raylib)

    # zen_groups.c pulls in the editor's group/selection helpers; the few UI
    # symbols it references are provided by the test file itself.
    add_executable(zen_tests
        tests/zen_tests.c
        src/anim_editor_zen/zen_groups.c
        ${ANIM_TEST_SRC})
    target_link_libraries(zen_tests raylib)
endif()

# Checks if OSX and links appropriate frameworks (Only required on MacOS)
if (APPLE)
    target_link_libraries(${PROJECT_NAME} "-framework IOKit")
    target_link_libraries(${PROJECT_NAME} "-framework Cocoa")
    target_link_libraries(${PROJECT_NAME} "-framework OpenGL")
endif()

# Custom pixel-shape slots. Each slot is a fixed 16 KB grid of static memory
# (see src/anim/anim_shape_pool.h), so this is a memory budget, not a GL limit.
# Web links a FIXED 128 MB emscripten heap with no ALLOW_MEMORY_GROWTH, and the
# pool is spent before main() runs - hence the low ceiling there. Override on the
# command line with -DANIM_SHAPE_POOL_MAX=<n>.
# A plain variable, NOT a cache entry: the platform default must be recomputed
# every configure, or reconfiguring one build dir from desktop to Web would keep
# the cached 512 and blow the emscripten heap. An explicit -D still wins, since
# that defines the variable before this runs.
if (NOT DEFINED ANIM_SHAPE_POOL_MAX)
    if ("${PLATFORM}" STREQUAL "Web")
        set(ANIM_SHAPE_POOL_MAX 8)
    else()
        set(ANIM_SHAPE_POOL_MAX 512)
    endif()
endif()
message(STATUS " - - - ANIM_SHAPE_POOL_MAX = ${ANIM_SHAPE_POOL_MAX}")
target_compile_definitions(${PROJECT_NAME} PUBLIC ANIM_SHAPE_POOL_MAX=${ANIM_SHAPE_POOL_MAX})
if (TARGET anim_tests)
    target_compile_definitions(anim_tests PUBLIC ANIM_SHAPE_POOL_MAX=${ANIM_SHAPE_POOL_MAX})
endif()
if (TARGET zen_tests)
    target_compile_definitions(zen_tests PUBLIC ANIM_SHAPE_POOL_MAX=${ANIM_SHAPE_POOL_MAX})
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