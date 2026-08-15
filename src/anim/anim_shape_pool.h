#ifndef ANIM_SHAPE_POOL_H
#define ANIM_SHAPE_POOL_H

// ============================================================================
//  anim_shape_pool.h  -  global pool of hand-authored pixel shapes
//
//  A pixel shape is a small 2-bit bitmap: every cell is EMPTY, FILL or OUTLINE.
//  It stores NO COLOUR AT ALL. That is the whole point: a SHAPE_CUSTOM element
//  paints its FILL cells with the element's animatable AP_S_COLOR and its
//  OUTLINE cells with AP_S_OUTLINE_COLOR, so a custom shape is exactly as
//  recolourable and as animatable as a rect or a circle. Baking a palette into
//  the shape would break that; there is deliberately nowhere to put one.
//
//  The pool is GLOBAL and lives outside any AnimDoc, in the same spirit as the
//  shared easing set (anim_ease_custom.c). It must stay out of AnimDoc: the zen
//  editor keeps a 16-deep ring of whole-document undo snapshots, so 256 KB of
//  pixels inside the doc would cost 4 MB of undo.
//
//  MEMORY: ANIM_SHAPE_GRID_MAX^2 bytes per slot (16 KB, spent whether the shape
//  is 128x128 or 4x4) x ANIM_SHAPE_POOL_MAX slots, plus two cached textures per
//  slot once drawn. The slot count is a BUILD-TIME knob set by CMake:
//
//      Web      8 slots =  128 KB    desktop   512 slots = 8.4 MB
//
//  Web is pinned low because Emscripten links a FIXED 128 MB heap with no
//  ALLOW_MEMORY_GROWTH (CMakeLists.txt), and this array is static - it is spent
//  before main() runs, so overshooting is an abort at load, not a slow game. On
//  desktop the same 8.4 MB is zero-filled BSS and costs nothing worth counting.
//  Nothing about the number is a GL or shader limit: shapes are CPU-baked into
//  two ALPHA stencils and drawn with two DrawTexturePro calls.
//
//  DOCUMENTS REFERENCE SHAPES BY NAME, never by slot index (see anim_io.c's
//  shape_ref lines). Slot indices are a runtime convenience only - they are how
//  a stepped AP_S_SHAPE keyframe addresses a shape while the editor runs - and
//  they are free to change between runs as files are added or removed.
//
//  FILE FORMAT  (one shape per file, "<name>.shp"), `#` comments:
//
//      shape fangs_upper       # must match the file's basename
//      size 12 4               # w h, both <= ANIM_SHAPE_GRID_MAX
//      rows                    # exactly h rows follow, each exactly w chars
//      .###....###.
//      .#OO#..#OO#.
//      .#OO#..#OO#.
//      ..##....##..
//      end
//
//  '.' empty, '#' fill, 'O' outline. One token per row keeps the fscanf house
//  style while staying diffable - a changed pixel shows up as a changed picture.
//  Unknown leading keys are skipped, so newer files stay readable by older
//  builds.
// ============================================================================

#include <stdbool.h>
#include "raylib.h"     // Texture2D

#define ANIM_SHAPE_GRID_MAX  128    // max cells per side
#ifndef ANIM_SHAPE_POOL_MAX
    // CMake supplies this: 8 on Web, 512 elsewhere. The fallback matches Web,
    // so a build that forgets the define is small rather than 8 MB by accident.
    #define ANIM_SHAPE_POOL_MAX 8   // pool slots (16 KB of pixels each)
#endif
// How far a SAVED shape_ref index may reach, independent of this build's pool
// size. A .cfg written by a 512-slot desktop build carries indices a 8-slot web
// build could never allocate, but its shape_ref lines still name shapes that web
// build may well have - so parsing must not reject the index before the NAME is
// consulted. Fixed, so the same file parses identically on every platform.
#define ANIM_SHAPE_REF_MAX   512
#define ANIM_SHAPE_NAME_MAX   24    // matches ANIM_CUSTOM_NAME_MAX
#define ANIM_SHAPE_MISSING   (-1)   // "no shape" / "name did not resolve"

// Cell values. The numbers are the .cfg-visible truth via AP_S_SHAPE keys only
// indirectly (keys hold slot indices, not cell values), but the pixel bytes are
// written as characters, so these are free to change - keep them anyway.
enum { ANIM_PX_EMPTY = 0, ANIM_PX_FILL = 1, ANIM_PX_OUTLINE = 2 };

typedef struct
{
    char name[ANIM_SHAPE_NAME_MAX];
    int  w, h;                              // 1..ANIM_SHAPE_GRID_MAX
    unsigned char px[ANIM_SHAPE_GRID_MAX * ANIM_SHAPE_GRID_MAX];  // row-major
    bool builtin;       // came from the read-only resources root
    bool used;          // slot occupied

    // Render cache - built lazily on first draw, never persisted. Two alpha
    // stencils (fill mask, outline mask) so each can be tinted with its own
    // live animated colour. See AnimShapePoolTextures.
    Texture2D fillTex, lineTex;
    bool texValid;
} AnimShapeDef;

// The two roots, in load order, and the whole of the web/desktop split.
//
//   resources/shapes/  ships with the build and is PRELOADED into the web VFS,
//                      so it is the only place a web build can see shapes from.
//                      Keep it under the web slot count (8) - anything past that
//                      is skipped at load (see AnimShapePoolSkipped).
//   shapes/            CWD-relative and writable, created on demand, exactly
//                      like ZEN_ANIM_DIR. This is where the editor saves, and
//                      where the desktop build's hundreds of shapes live.
//
// A user shape overrides a shipped one of the same name, keeping its slot. On
// web the user root is MEMFS: authoring works for the session and does not
// survive a reload - the same known limitation as anims/.
#ifdef RESOURCES_PATH
    #define ANIM_SHAPE_RES_DIR   RESOURCES_PATH "shapes/"
#else
    #define ANIM_SHAPE_RES_DIR   "resources/shapes/"   // headless test build
#endif
#define ANIM_SHAPE_USER_DIR      "shapes/"

// -- persistence -------------------------------------------------------------
// Loads every "*.shp" under both roots, wiping the pool first. Reads the
// read-only resources root FIRST, then the writable user root - a user shape
// overrides a builtin of the same name. Returns the number of shapes loaded.
// Either root may be NULL or missing.
int  AnimShapePoolLoadAll(const char *resRoot, const char *userRoot);
// How many .shp files the LAST AnimShapePoolLoadAll could not fit, because the
// pool filled up. Silence here is how a web build loses shapes without saying
// so, which is exactly what the editor surfaces.
int  AnimShapePoolSkipped(void);
// Writes one slot to "<userRoot>/<name>.shp". Marks the slot non-builtin,
// since it now has a user copy that shadows any shipped one.
bool AnimShapePoolSaveOne(int slot, const char *userRoot);
// Deletes the slot's user file (never a builtin's resource file) and frees the
// slot. Returns false if the slot is unused.
bool AnimShapePoolDelete(int slot, const char *userRoot);

// -- slots -------------------------------------------------------------------
// Claims a free slot with a blank w x h grid. Returns the slot, or
// ANIM_SHAPE_MISSING when the pool is full or the name is invalid/taken.
int  AnimShapePoolAdd(const char *name, int w, int h);
AnimShapeDef *AnimShapePoolGet(int slot);              // NULL if unused
int  AnimShapePoolFindByName(const char *name);        // slot or ANIM_SHAPE_MISSING
// Iteration for dropdowns: walk [0, ANIM_SHAPE_POOL_MAX) and skip ids where
// AnimShapeIdValid is false. Sparse, exactly like AnimEaseIdValid.
bool AnimShapeIdValid(int slot);
int  AnimShapePoolCount(void);                         // used slots
// True when the name fits a slot, is whitespace-free and is not already taken.
bool AnimShapeNameOk(const char *name);

// -- pixels ------------------------------------------------------------------
// Bounds-checked accessors; Get returns ANIM_PX_EMPTY outside the grid.
unsigned char AnimShapePx(const AnimShapeDef *s, int x, int y);
void AnimShapeSetPx(AnimShapeDef *s, int x, int y, unsigned char v);
// Changes the grid size, preserving the overlapping top-left region. Invalidates
// the texture cache.
void AnimShapeResize(AnimShapeDef *s, int w, int h);

// -- render cache (Phase 3; needs a GL context, so draw-path only) -----------
// Bakes the two stencils on first call and hands back borrowed textures. Safe
// to call every frame. Returns false when the slot is unused.
bool AnimShapePoolTextures(int slot, Texture2D *fill, Texture2D *line);
void AnimShapePoolInvalidate(int slot);   // after any pixel/size edit
void AnimShapePoolUnloadTextures(void);   // call before closing the window

#endif
