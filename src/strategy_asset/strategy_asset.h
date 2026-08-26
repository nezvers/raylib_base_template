// ============================================================================
//  strategy_asset.h  -  authored strategy assets ("SGA"), saved as binary
//
//  strategy_models.c describes the game's art as const part tables compiled
//  into the binary. That is the right home for the shipped models and it stays
//  exactly as it is - CLOSED. This module is the other half: assets a user
//  BUILDS at runtime in the forge and saves to a file, which the showcase then
//  lists beside the built-ins.
//
//  An SgaPart is deliberately a superset of ModelPart (strategy_models.h), so
//  remixing a built-in is a field copy plus a colour-role translation and never
//  a re-modelling job. What it adds is what authoring needs: a name, a
//  visibility flag, a generalised tint policy, and - for animation - the
//  ability to be a PATH rather than geometry.
//
//  TINT, and why ColorRole was not enough. The built-in ColorRole enum is six
//  values that really say "fixed, faction, or faction at one of four hard-coded
//  brightness deltas". Authored parts need the in-between: a part that takes
//  SOME of the faction colour so it reads as the same army while staying darker
//  than its neighbours. So the policy here is a mode plus two numbers, and the
//  six legacy roles map onto it exactly (StrategyAssetTintFromRole).
//
//  STANDALONE FILES. A .sga carries its own easing curves, baked in at save
//  (see SgaEase). An asset moved to another machine, or loaded after
//  anims/_easings.cfg was edited or deleted, still animates the way it was
//  authored. Nothing here reads that file at load time.
//
//  MEMORY: everything is fixed-capacity and heap-free, house style, so an
//  SgaAsset is a plain value the forge's undo ring can memcpy. That makes the
//  capacities below ARRAY DIMENSIONS, and they NEST (asset > parts > keys), so
//  CMake tiers them: Web keeps the small values written here, desktop gets the
//  raised ones. Web links a FIXED 128 MB emscripten heap with no
//  ALLOW_MEMORY_GROWTH, where overshooting is an abort at load, not a slowdown.
//
//  DO NOT PUT AN SgaAsset ON THE STACK. It is ~68 KB on the Web tier and
//  ~450 KB on the desktop one, against a 1 MB stack on Web (-sSTACK_SIZE) and
//  8 MB typical on desktop - one local is survivable, two or three are not, and
//  the failure is a segfault at the declaration rather than anywhere near the
//  code that looks wrong. Make them static, file-scope, or MemAlloc'd.
// ============================================================================

#ifndef STRATEGY_ASSET_H
#define STRATEGY_ASSET_H

#include "raylib.h"
#include <stdbool.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
//  Capacities. The #ifndef-guarded ones are raised by CMakeLists.txt on every
//  non-Web build; the values here are the WEB tier.
// ---------------------------------------------------------------------------
#define SGA_NAME_MAX        32      // asset name / part name buffer
#define SGA_SUBTYPE_MAX     24      // "soldier", "quarry", "oak" ...

#ifndef SGA_PARTS_MAX
#define SGA_PARTS_MAX       24      // parts per asset (geometry AND paths)
#endif
#ifndef SGA_KEYS_MAX
#define SGA_KEYS_MAX        12      // keyframes per part, per state
#endif
#ifndef SGA_EASES_MAX
#define SGA_EASES_MAX       16      // distinct easing curves baked per asset
#endif
#ifndef SGA_ASSETS_MAX
#define SGA_ASSETS_MAX      64      // .sga files held in memory at once
#endif

#define SGA_EASE_NAME_MAX   24      // matches ANIM_CUSTOM_NAME_MAX
#define SGA_EASE_PTS_MAX     8      // matches ANIM_EASE_PTS_MAX

// ---------------------------------------------------------------------------
//  Taxonomy. BOTH fields are mandatory on save - they are how an asset is
//  found and filtered later.
//
//  A category NEVER restricts where an asset may be used. It is a label for
//  finding things, not a type system: binding a "resource/tree" asset to the
//  town hall is a supported thing to do, not a mistake to prevent.
// ---------------------------------------------------------------------------
typedef enum {
    SGA_BUILDING = 0,
    SGA_UNIT,
    SGA_RESOURCE,
    SGA_CATEGORY_COUNT
} SgaCategory;

const char *StrategyAssetCategoryName(int cat);     // bad id -> "BUILDING"

// ---------------------------------------------------------------------------
//  Primitives. Same set and same semantics as PartKind in strategy_models.h,
//  restated here so this module does not drag the game's headers into the
//  headless tests. SGA_PATH is the one addition: it draws nothing.
// ---------------------------------------------------------------------------
typedef enum {
    SGA_CUBE = 0,       // size = w/h/l, centered on offset
    SGA_CUBE_WIRES,     // wire pass, drawn over a CUBE of the same size
    SGA_SPHERE,         // r0 = radius, centered on offset
    SGA_CYLINDER,       // r0 -> r1 over h, `sides` segments; a cone when r0 = 0
    SGA_CYLINDER_EX,    // bar from offset to offset + size
    SGA_LINE,           // line from offset to offset + size
    SGA_PATH,           // INVISIBLE: a motion path other parts travel along
    SGA_KIND_COUNT
} SgaPartKind;

const char *StrategyAssetKindName(int kind);        // bad id -> "CUBE"

// ---------------------------------------------------------------------------
//  Colour policy
// ---------------------------------------------------------------------------
typedef enum {
    SGA_TINT_NONE = 0,      // .color verbatim; the faction never reaches it
    SGA_TINT_PARTIAL,       // faction colour pulled toward .color by `tintAmount`
    SGA_TINT_FULL,          // the faction colour itself
    SGA_TINT_COUNT
} SgaTintMode;

const char *StrategyAssetTintName(int mode);        // bad id -> "FIXED"

// ---------------------------------------------------------------------------
//  Motion paths
//
//  One continuous shape control instead of a shape enum. radiusX/radiusZ give
//  circle (equal), ellipse (unequal) and line (one near zero); `squareness`
//  blends that profile toward a rectangle, so "flattened", "elliptical" and
//  "squarish" are all reachable by dragging rather than by switching modes -
//  and nothing jumps as you cross between them. `rotation` then turns the whole
//  loop on all three axes.
// ---------------------------------------------------------------------------
typedef struct {
    Vector3 center;             // LOCAL, from the model's ground origin
    float   radiusX, radiusZ;   // equal = circle, unequal = ellipse, ~0 = line
    float   squareness;         // 0 = ellipse .. 1 = rectangle
    Vector3 rotation;           // euler degrees, applied X then Y then Z
} SgaPath;

// Point at u (0..1 around the loop). u wraps, so 1.25 == 0.25.
Vector3 StrategyPathPoint(const SgaPath *path, float u);

// ---------------------------------------------------------------------------
//  Animation states. IDLE is the default and may be empty - a still model is a
//  perfectly good idle, which is what every built-in effectively is today.
// ---------------------------------------------------------------------------
typedef enum {
    SGA_STATE_IDLE = 0,
    SGA_STATE_MOVING,
    SGA_STATE_DAMAGED,
    SGA_STATE_ATTACKING,
    SGA_STATE_DIE,
    SGA_STATE_HEALED,
    SGA_STATE_COUNT
} SgaState;

const char *StrategyAssetStateName(int state);      // bad id -> "IDLE"

// ---------------------------------------------------------------------------
//  Several states at once
//
//  A unit can truthfully be walking, swinging and freshly wounded in the same
//  frame. Because animation is authored PER PART, those do not have to fight:
//  if MOVING drives the legs and IDLE drives the head, both play at once and
//  the model reads as one motion rather than a slideshow of poses.
//
//  A conflict is narrower than it sounds - it is two ACTIVE states with keys on
//  the SAME part. Only then does priority decide, and the loser is silenced for
//  that part alone, not for the whole model. This is why an unconflicted part
//  keeps playing its own state no matter how low that state ranks.
//
//  The ladder is FIXED and lives in code, not in the file: it encodes urgency
//  (dying outranks being hit, which outranks swinging) and every asset wants
//  the same answer. Keeping it out of the format also means no version bump.
// ---------------------------------------------------------------------------
typedef struct {
    int32_t state;      // SgaState
    float   time;       // seconds into THIS state; each state clocks separately
    bool    active;
} SgaStateSlot;

typedef struct {
    // Indexed BY state, so a state can never be listed twice with two clocks -
    // the ambiguity is designed out rather than checked for.
    SgaStateSlot slot[SGA_STATE_COUNT];
} SgaStateSet;

// Higher wins a contested part: DIE > DAMAGED > HEALED > ATTACKING > MOVING > IDLE.
int  StrategyAssetStatePriority(int state);

// Clears the set, then turns one state on. The common case, and it keeps
// callers from having to remember that the array is indexed by state.
void StrategyAssetStateSetInit(SgaStateSet *set);
void StrategyAssetStateSetAdd(SgaStateSet *set, int state, float time);


// One keyframe. `ease` shapes the segment ENDING at this key - the same rule
// AnimTrack uses, so the two editors never disagree about what a curve means.
typedef struct {
    float   t;          // seconds
    float   u;          // position along the bound path, 0..1
    Vector3 offset;     // ADDITIVE offset from the part's rest position
    Vector3 rot;        // ADDITIVE rotation, degrees
    Vector3 scale;      // MULTIPLICATIVE scale; {1,1,1} is rest
    int32_t ease;       // index into SgaAsset.eases, or -1 for linear
} SgaKey;

// What one part does during one state.
typedef struct {
    int32_t pathPart;               // part index of the SGA_PATH to follow, -1 none
    int32_t keyCount;
    SgaKey  keys[SGA_KEYS_MAX];
} SgaPartAnim;

// ---------------------------------------------------------------------------
//  Baked easing.
//
//  Referenced BY NAME, never by runtime id: a custom ease id is
//  ANIM_EASE_COUNT + slot, and which slot a curve lands in depends on what
//  _easings.cfg happened to contain that run. Saving an id would silently
//  repoint the curve to a different shape on the next load. anim_io.c hit this
//  exact problem with shape references and solved it the same way.
//
//  `ptCount` > 0 means the knots below ARE the curve - evaluated directly, with
//  no lookup into the global easing set. That is what makes a .sga standalone.
//  `ptCount` == 0 means resolve `name` against the builtin table instead.
// ---------------------------------------------------------------------------
typedef struct {
    float x, y;         // knot
    float ix, iy;       // in-handle  offset from the knot
    float ox, oy;       // out-handle offset from the knot
} SgaEasePt;            // layout-identical to AnimEasePt, on purpose

typedef struct {
    char      name[SGA_EASE_NAME_MAX];
    int32_t   ptCount;                      // 0 = builtin by name; else knots
    SgaEasePt pts[SGA_EASE_PTS_MAX];
} SgaEase;

// ---------------------------------------------------------------------------
//  A part
// ---------------------------------------------------------------------------
typedef struct {
    char        name[SGA_NAME_MAX];
    int32_t     kind;           // SgaPartKind
    int32_t     visible;        // int, not bool: this struct goes to disk

    Vector3     offset;         // LOCAL, from the model's ground origin
    Vector3     size;           // CUBE: w/h/l. CYLINDER_EX / LINE: end offset.
    float       r0, r1, h;      // CYLINDER: radii + height. SPHERE: r0 = radius.
    int32_t     sides;

    int32_t     tintMode;       // SgaTintMode
    float       tintAmount;     // PARTIAL: 0 = all .color .. 1 = all faction
    float       brightness;     // applied after the blend, -1 .. +1
    Color       color;          // the part's own colour (4 bytes, packed)

    SgaPath     path;           // SGA_PATH only
    SgaPartAnim anim[SGA_STATE_COUNT];
} SgaPart;

// ---------------------------------------------------------------------------
//  An asset
// ---------------------------------------------------------------------------
typedef struct {
    char    name[SGA_NAME_MAX];
    char    subtype[SGA_SUBTYPE_MAX];
    int32_t category;               // SgaCategory

    int32_t partCount;
    SgaPart parts[SGA_PARTS_MAX];

    int32_t easeCount;
    SgaEase eases[SGA_EASES_MAX];

    float   duration[SGA_STATE_COUNT];  // seconds per state; 0 = static

    // Measured from the parts, never hand-typed - the built-ins' hand-measured
    // height/radius drift the moment a part moves. StrategyAssetMeasure() owns
    // these; they exist so the showcase's ModelCamera can frame a preview.
    float   height, radius;
} SgaAsset;

// ---------------------------------------------------------------------------
//  Authoring
// ---------------------------------------------------------------------------
void StrategyAssetInit(SgaAsset *a, const char *name);  // one default part
void StrategyAssetMeasure(SgaAsset *a);                 // refresh height/radius

int  StrategyAssetAddPart(SgaAsset *a, int kind);       // new part index, or -1
bool StrategyAssetRemovePart(SgaAsset *a, int index);   // repoints path refs
bool StrategyAssetMovePart(SgaAsset *a, int index, int delta);  // draw order
int  StrategyAssetDuplicatePart(SgaAsset *a, int index);

// Both fields are mandatory. Returns false and fills `why` (may be NULL) when
// the asset is not yet saveable, so the UI can say WHICH field is missing
// rather than leaving a button dead with no explanation.
bool StrategyAssetValid(const SgaAsset *a, const char **why);

// ---------------------------------------------------------------------------
//  Keyframes
//
//  Keys are kept SORTED BY TIME at all times. Evaluation walks them in order
//  and would read a wrong segment otherwise, so insert/move re-sort rather than
//  trusting the caller - a key dragged past its neighbour on a timeline is the
//  normal case, not an error.
// ---------------------------------------------------------------------------
// Insert a key at time t, seeded from the pose already showing at t so adding a
// key never makes the model jump. Returns the new key's index, or -1 if full.
// An existing key at (almost) the same time is REPLACED, not duplicated.
int  StrategyAssetAddKey(SgaAsset *a, int partIndex, int state, float t);
bool StrategyAssetRemoveKey(SgaAsset *a, int partIndex, int state, int keyIndex);

// Move a key in time, re-sorting. Returns the index the key ENDED UP at, since
// a drag past a neighbour changes it.
int  StrategyAssetMoveKey(SgaAsset *a, int partIndex, int state, int keyIndex,
                          float newT);

// Longest key time across every part in a state - what the state's duration has
// to cover. 0 when nothing is animated.
float StrategyAssetStateExtent(const SgaAsset *a, int state);

// True when any part has keys in this state.
bool StrategyAssetStateHasKeys(const SgaAsset *a, int state);

// Legacy ColorRole -> tint policy. Reproduces the built-in's colour exactly,
// including the four brightness deltas in strategy_models.c's PartColor.
void StrategyAssetTintFromRole(int role, int32_t *mode, float *amount,
                               float *brightness);

// ---------------------------------------------------------------------------
//  Evaluation + drawing
// ---------------------------------------------------------------------------
// The faction palette lives in strategy_world.c, which owns the whole
// simulation. Rather than link that in - and drag it into the headless tests -
// the app installs the lookup once at startup. Unset, every faction resolves to
// a neutral grey, which is what a test wants and what a preview can live with.
void StrategyAssetSetFactionTint(Color (*fn)(int faction));

// Resolved colour of one part for `faction` at `alpha`. Faction may be
// FACTION_NEUTRAL; the tint resolves through the same guard the showcase uses.
Color StrategyAssetPartColor(const SgaPart *p, int faction, float alpha);

// Where a part sits at time `t` in `state`. Fills the offset/rotation/scale the
// draw pass applies on top of the part's rest pose.
void StrategyAssetPartPose(const SgaAsset *a, int partIndex, int state, float t,
                           Vector3 *outOffset, Vector3 *outRot, Vector3 *outScale);

// Eased progress for one of the asset's BAKED curves. index < 0, or an index
// past easeCount, is linear.
float StrategyAssetEase(const SgaAsset *a, int index, float p);

// Draw the asset. MUST be called between BeginMode3D/EndMode3D.
// `state`/`time` pose it; pass SGA_STATE_IDLE and 0 for the rest pose.
// `alpha` is 0..1 over the whole model, which is what the forge's ghosted
// reference model rides on.
void StrategyAssetDraw(const SgaAsset *a, int faction, Vector3 pos, float yawDeg,
                       float alpha, int state, float time);

// Draw with several states live at once. Each part independently plays the
// highest-priority ACTIVE state that has keys on it; a part no active state
// animates holds its rest pose. This is the entry point the game draws through.
void StrategyAssetDrawStates(const SgaAsset *a, int faction, Vector3 pos,
                             float yawDeg, float alpha, const SgaStateSet *set);

// Which state should drive this part, or -1 for "none active animates it", in
// which case the part holds its rest pose. Exposed for the tests, which check
// the resolution rules directly rather than through a draw call.
int  StrategyAssetResolvePartState(const SgaAsset *a, int partIndex,
                                   const SgaStateSet *set);

// Draw one path as a wire loop - the forge's direct-manipulation handle. Not
// part of the model; never called by the showcase.
void StrategyAssetDrawPath(const SgaPath *path, Vector3 pos, float yawDeg, Color c);

#endif // STRATEGY_ASSET_H
