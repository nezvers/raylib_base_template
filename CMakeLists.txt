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

    # Authored strategy assets. The data model and its binary IO are kept free
    # of UI calls precisely so they link headless here, the same way zen_groups.c
    # does. simple_save.h is header-only and its IMPLEMENTATION lives in an
    # example the tests do not link, so the suite compiles it itself.
    # strategy_asset_ease.c is the one file that bridges to src/anim/, so the
    # suite links the anim sources too - that bridge (baking a curve so the
    # asset stops depending on _easings.cfg) is exactly what needs testing.
    add_executable(sga_tests
        tests/sga_tests.c
        src/strategy_asset/strategy_asset.c
        src/strategy_asset/strategy_asset_io.c
        src/strategy_asset/strategy_asset_ease.c
        src/strategy_asset/strategy_bindings.c
        src/strategy_asset/strategy_catalog.c
        ${ANIM_TEST_SRC})
    target_link_libraries(sga_tests raylib)

    # map_tests - authored battlefields. src/strategy_map/ never calls raylib
    # UI (that is the whole reason it is split from the map forge), so the
    # suite links the module directly and needs no stubs.
    add_executable(map_tests
        tests/map_tests.c
        src/strategy_map/strategy_map.c
        src/strategy_map/strategy_map_io.c
        src/strategy_map/strategy_map_catalog.c)
    target_link_libraries(map_tests raylib)

    # path_tests - navigation and steering. src/strategy_path/ deliberately does
    # not include strategy_types.h (which would drag in the whole anim system
    # via strategy_asset.h), so it links here with no stubs at all. raylib is
    # linked for Vector2/Vector3 and the maths inlines only.
    # strategy_map.c rides along ONLY so the nav grid's tile<->world maths can
    # be cross-checked against SgmTileToWorld directly. The two are duplicated
    # (strategy_path/ must stay linkable without the map module), and a silent
    # drift between them would put every unit half a tile off its path.
    add_executable(path_tests
        tests/path_tests.c
        src/strategy_path/strategy_path_hash.c
        src/strategy_path/strategy_path_grid.c
        src/strategy_path/strategy_path_astar.c
        src/strategy_path/strategy_path_service.c
        src/strategy_path/strategy_path_flow.c
        src/strategy_map/strategy_map.c)
    target_link_libraries(path_tests raylib)
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

# ---------------------------------------------------------------------------
#  Anim document capacities (src/anim/anim.h)
#
#  Every one of these is an ARRAY DIMENSION in a fixed-capacity, heap-free
#  AnimDoc, and they NEST (doc > elems > tracks > keys), so raising them grows
#  sizeof(AnimDoc) multiplicatively. Docs are held BY VALUE in bulk (16 zen undo
#  slots + 8 anim_stage slots + export scratch, ~24 copies), so each change
#  lands in the build multiplied by ~24:
#
#      Web tier      AnimDoc  ~95 KB  ->  ~2.2 MB across the build
#      Desktop tier  AnimDoc ~949 KB  ->  ~22 MB across the build
#
#  Web CANNOT take the desktop tier: it links a FIXED 128 MB emscripten heap
#  (-sTOTAL_MEMORY above) with no ALLOW_MEMORY_GROWTH, and these buffers are
#  static, spent before main() runs, alongside raylib/Box2D/preloaded resources.
#  Overshoot is an abort, not a slowdown. So Web keeps the modest defaults
#  written in anim.h and desktop gets the raised ones from here.
#
#  A document authored at desktop capacities will therefore load TRUNCATED in
#  the web build; AnimDocLoad reports that (see AnimDocLoadTruncated) and the
#  zen editor surfaces it as a message rather than failing silently.
#
#  Plain variables, NOT cache entries - same reason as ANIM_SHAPE_POOL_MAX
#  above: reconfiguring one build dir from desktop to Web must recompute these,
#  not reuse a cached desktop value and blow the heap. An explicit -D still wins.
# ---------------------------------------------------------------------------
if (NOT "${PLATFORM}" STREQUAL "Web")
    if (NOT DEFINED ANIM_KEYS_MAX)    # anim.h Web default: 24
        set(ANIM_KEYS_MAX 64)
    endif()
    if (NOT DEFINED ANIM_TRACKS_MAX)  # anim.h Web default: 12
        set(ANIM_TRACKS_MAX 24)
    endif()
    if (NOT DEFINED ANIM_ELEMS_MAX)   # anim.h Web default: 12
        set(ANIM_ELEMS_MAX 36)
    endif()
    if (NOT DEFINED ANIM_STRINGS_MAX) # anim.h Web default: 24
        set(ANIM_STRINGS_MAX 64)
    endif()
    # MUST follow ANIM_ELEMS_MAX: anim.h requires ANIM_SIG_TARGETS_MAX >= it, or
    # elements past the 32nd cannot be driven by a signal at all (the archived
    # suite asserts this). Kept at the Web tier's ~1.33x ratio, because the
    # editor authors targets in property GROUPS and one authored track spends
    # several slots. Cheap next to the nested caps: ~0.6 KB/doc per +1.
    if (NOT DEFINED ANIM_SIG_TARGETS_MAX)   # anim.h Web default: 32
        set(ANIM_SIG_TARGETS_MAX 72)
    endif()
endif()

# Applied to the game AND both test executables, so the tests exercise the same
# capacities the desktop game ships with.
function(anim_apply_caps target)
    foreach(cap ANIM_KEYS_MAX ANIM_TRACKS_MAX ANIM_ELEMS_MAX ANIM_STRINGS_MAX
                ANIM_SIG_TARGETS_MAX)
        if (DEFINED ${cap})
            target_compile_definitions(${target} PUBLIC ${cap}=${${cap}})
        endif()
    endforeach()
endfunction()

if ("${PLATFORM}" STREQUAL "Web")
    message(STATUS " - - - anim capacities: WEB tier (anim.h defaults)")
else()
    message(STATUS " - - - anim capacities: keys=${ANIM_KEYS_MAX} tracks=${ANIM_TRACKS_MAX} elems=${ANIM_ELEMS_MAX} strings=${ANIM_STRINGS_MAX} sigTargets=${ANIM_SIG_TARGETS_MAX}")
endif()

anim_apply_caps(${PROJECT_NAME})
if (TARGET anim_tests)
    anim_apply_caps(anim_tests)
endif()
if (TARGET zen_tests)
    anim_apply_caps(zen_tests)
endif()

# ---------------------------------------------------------------------------
#  Authored strategy asset capacities (src/strategy_asset/strategy_asset.h)
#
#  Same shape, and the same reason, as the anim caps above: an SgaAsset is a
#  fixed-capacity, heap-free value the forge holds by value in an undo ring, so
#  every one of these is an ARRAY DIMENSION and they NEST (asset > parts >
#  keys-per-state). Web keeps the smaller values written in the header, because
#  it links a FIXED 128 MB emscripten heap with no ALLOW_MEMORY_GROWTH, where
#  overshooting is an abort at load rather than a slowdown.
#
#  A desktop-authored .sga can therefore be too big for the Web build. That is
#  expected and REPORTED, not silent - see SgaLoadTrunc in strategy_asset_io.h.
#
#  Plain variables, NOT cache entries - same reason as the caps above.
# ---------------------------------------------------------------------------
if (NOT "${PLATFORM}" STREQUAL "Web")
    if (NOT DEFINED SGA_PARTS_MAX)    # strategy_asset.h Web default: 24
        set(SGA_PARTS_MAX 64)
    endif()
    if (NOT DEFINED SGA_KEYS_MAX)     # strategy_asset.h Web default: 12
        set(SGA_KEYS_MAX 32)
    endif()
    if (NOT DEFINED SGA_EASES_MAX)    # strategy_asset.h Web default: 16
        set(SGA_EASES_MAX 32)
    endif()
    if (NOT DEFINED SGA_ASSETS_MAX)   # strategy_asset.h Web default: 64
        set(SGA_ASSETS_MAX 128)
    endif()
endif()

# ---------------------------------------------------------------------------
#  Map capacities. Same two-tier scheme, same reasoning: Web keeps the small
#  values written in strategy_map.h, desktop gets the raised ones. The .sgm
#  format itself is pinned to the DESKTOP tier (SGM_FILE_GRID in
#  strategy_map_io.c), so a desktop-authored map always fits on disk and a Web
#  build reports what it had to crop instead of misreading it.
# ---------------------------------------------------------------------------
if (NOT "${PLATFORM}" STREQUAL "Web")
    if (NOT DEFINED SGM_GRID_MAX)     # strategy_map.h Web default: 128
        set(SGM_GRID_MAX 256)
    endif()
    if (NOT DEFINED SGM_PLACES_MAX)   # strategy_map.h Web default: 128
        set(SGM_PLACES_MAX 512)
    endif()
    if (NOT DEFINED SGM_MAPS_MAX)     # strategy_map.h Web default: 32
        set(SGM_MAPS_MAX 64)
    endif()
endif()

# ---------------------------------------------------------------------------
#  Strategy runtime capacities. Same two-tier scheme again: Web keeps the small
#  value in strategy_types.h, desktop gets the raised one. Unlike the map and
#  asset caps this one is a PERFORMANCE guard rather than a memory guard - 10000
#  units is only 2.4 MB of Unit structs, well inside the 128 MB Web heap, but a
#  browser cannot draw that many procedural units at any framerate.
#
#  NOTE these are plain variables, NOT cache entries, for the same reason as
#  every other cap block here: reconfiguring one build dir from desktop to Web
#  would keep a cached 10000 and make the Web build unusable. An explicit -D
#  still wins, since that defines the variable before this runs.
# ---------------------------------------------------------------------------
if (NOT "${PLATFORM}" STREQUAL "Web")
    if (NOT DEFINED STRAT_MAX_UNITS)  # strategy_types.h Web default: 256
        set(STRAT_MAX_UNITS 10000)
    endif()
endif()

# Navigation / steering capacities (src/strategy_path/). Same two-tier scheme:
# Web keeps the small header defaults, desktop is raised here.
#
# SP_HASH_ITEMS_MAX MUST BE >= STRAT_MAX_UNITS. The spatial hash holds one entry
# per live unit; if it is short, the overflow is REFUSED rather than crashing -
# which reads in-game as units at the back of a crowd walking through each other.
# It is derived from STRAT_MAX_UNITS above rather than typed twice, so raising
# the unit cap cannot leave the hash behind.
#
# A plain variable, NOT a cache entry: the platform default must be recomputed
# on every configure, or a desktop->Web reconfigure keeps the big desktop value.
if (NOT "${PLATFORM}" STREQUAL "Web")
    if (NOT DEFINED SP_GRID_MAX)         # strategy_path.h Web default: 128
        set(SP_GRID_MAX 256)
    endif()
    if (NOT DEFINED SP_HASH_CELLS_MAX)   # strategy_path.h Web default: 16384
        set(SP_HASH_CELLS_MAX 65536)
    endif()
    if (NOT DEFINED SP_HASH_ITEMS_MAX)   # strategy_path.h Web default: 256
        set(SP_HASH_ITEMS_MAX ${STRAT_MAX_UNITS})
    endif()
    if (NOT DEFINED SP_PATH_MAX)         # strategy_path.h Web default: 12
        set(SP_PATH_MAX 24)
    endif()
    if (NOT DEFINED SP_REQUESTS_MAX)     # strategy_path.h Web default: 64
        set(SP_REQUESTS_MAX 256)
    endif()
    if (NOT DEFINED SP_FLOW_FIELDS_MAX)  # strategy_path.h Web default: 4
        set(SP_FLOW_FIELDS_MAX 16)
    endif()
endif()

function(sp_apply_caps target)
    foreach(cap SP_GRID_MAX SP_HASH_CELLS_MAX SP_HASH_ITEMS_MAX
                SP_PATH_MAX SP_REQUESTS_MAX SP_FLOW_FIELDS_MAX)
        if (DEFINED ${cap})
            target_compile_definitions(${target} PUBLIC ${cap}=${${cap}})
        endif()
    endforeach()
endfunction()

function(strat_apply_caps target)
    foreach(cap STRAT_MAX_UNITS)
        if (DEFINED ${cap})
            target_compile_definitions(${target} PUBLIC ${cap}=${${cap}})
        endif()
    endforeach()
endfunction()

function(sgm_apply_caps target)
    foreach(cap SGM_GRID_MAX SGM_PLACES_MAX SGM_MAPS_MAX)
        if (DEFINED ${cap})
            target_compile_definitions(${target} PUBLIC ${cap}=${${cap}})
        endif()
    endforeach()
endfunction()

function(sga_apply_caps target)
    foreach(cap SGA_PARTS_MAX SGA_KEYS_MAX SGA_EASES_MAX SGA_ASSETS_MAX)
        if (DEFINED ${cap})
            target_compile_definitions(${target} PUBLIC ${cap}=${${cap}})
        endif()
    endforeach()
endfunction()

if ("${PLATFORM}" STREQUAL "Web")
    message(STATUS " - - - strategy asset capacities: WEB tier (header defaults)")
else()
    message(STATUS " - - - strategy asset capacities: parts=${SGA_PARTS_MAX} keys=${SGA_KEYS_MAX} eases=${SGA_EASES_MAX} assets=${SGA_ASSETS_MAX}")
endif()

sga_apply_caps(${PROJECT_NAME})
if (TARGET sga_tests)
    sga_apply_caps(sga_tests)
endif()

if ("${PLATFORM}" STREQUAL "Web")
    message(STATUS " - - - strategy map capacities: WEB tier (header defaults)")
else()
    message(STATUS " - - - strategy map capacities: grid=${SGM_GRID_MAX} places=${SGM_PLACES_MAX} maps=${SGM_MAPS_MAX}")
endif()

sgm_apply_caps(${PROJECT_NAME})
if (TARGET map_tests)
    sgm_apply_caps(map_tests)
endif()
# path_tests links strategy_map.c for the tile<->world cross-check, so it needs
# the same SGM caps - otherwise it would compare a 256-grid nav module against a
# 128-grid map module and the mismatch would look like a maths bug.
if (TARGET path_tests)
    sgm_apply_caps(path_tests)
endif()

if ("${PLATFORM}" STREQUAL "Web")
    message(STATUS " - - - strategy runtime capacities: WEB tier (header defaults)")
else()
    message(STATUS " - - - strategy runtime capacities: units=${STRAT_MAX_UNITS}")
endif()

strat_apply_caps(${PROJECT_NAME})

if ("${PLATFORM}" STREQUAL "Web")
    message(STATUS " - - - strategy path capacities: WEB tier (header defaults)")
else()
    message(STATUS " - - - strategy path capacities: grid=${SP_GRID_MAX} hashCells=${SP_HASH_CELLS_MAX} hashItems=${SP_HASH_ITEMS_MAX} path=${SP_PATH_MAX} requests=${SP_REQUESTS_MAX} fields=${SP_FLOW_FIELDS_MAX}")
endif()

sp_apply_caps(${PROJECT_NAME})
if (TARGET path_tests)
    sp_apply_caps(path_tests)
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