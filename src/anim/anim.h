// ============================================================================
//  anim.h  -  runtime-authored, signal-driven animation documents (NEW model)
//
//  This is a SECOND animation system that lives ALONGSIDE scene_anim.* (which
//  is untouched and still drives the current menu/platformer/strategy intros
//  and the transition outro). Where scene_anim hard-codes its phase tables in
//  C, this model is authored as DATA in the anim editor, saved to a .cfg and
//  loaded at runtime - no recompile to change an animation.
//
//  MODEL (a "timeline of tracks + signal edges"):
//    AnimDoc     one animation. Owns ELEMENTS on ONE shared clock (0..duration)
//    AnimElem    one animated thing: TEXT / SHAPE / GLOBAL. Has base (static)
//                properties + a set of TRACKS.
//    AnimTrack   one animated PROPERTY of an element (e.g. pos_y, alpha), as a
//                list of KEYFRAMES.
//    AnimKey     one keyframe { t (seconds), value, ease }. Between two keys the
//                value is eased from the left key's value to the right key's.
//    AnimSignal  a NAMED edge: "when signal <name> fires, play the doc in <dir>
//                over section [start,end]". The signal module (signal.*) maps a
//                SignalEmit(name) onto these.
//
//  Positions/sizes are FRACTIONS of the game canvas (ScreenStateTargetSize()),
//  exactly like AnimText in scene_anim.h, so a document is resolution
//  independent and previews in the editor at the same place it plays in game.
//
//  Everything is FIXED-CAPACITY (no heap): a whole AnimDoc is a plain value you
//  can memcpy (used by the editor's undo snapshots). All structs are built at
//  runtime (MSVC-clean: no file-scope address-of initializers).
// ============================================================================

#ifndef ANIM_H
#define ANIM_H

#include "raylib.h"
#include "../signal/signal.h"   // SignalParams (per-emit position parameter)
#include <stdbool.h>

// ---------------------------------------------------------------------------
//  Capacities. Because nothing here is heap-allocated, every one of these is an
//  ARRAY DIMENSION: raising one grows sizeof(AnimDoc) immediately, and grows it
//  MULTIPLICATIVELY, since the capacities nest (doc > elems > tracks > keys).
//
//  Measured sizes at the current values (x86-64; 4-byte float/int, Color = 4B):
//
//      AnimKey            16 B
//      AnimTrack         264 B   =  16 keys x 16 B  + prop/count
//      AnimElem        3,312 B   =  12 tracks x 264 B + base props
//      AnimSigTarget     140 B   =   8 keys x 16 B  + idx/prop/count
//      AnimSignal      4,524 B   =  32 targets x 140 B + name/length/terminal
//      AnimDoc        57,876 B   =  12 elems x 3,312 B + 4 signals x 4,524 B
//                                =  ~57 KB
//
//  WHICH KNOBS COST THE MOST, per +1 of each (all else unchanged):
//
//      ANIM_ELEMS_MAX      +3.3 KB/doc   elem is the single biggest unit
//      ANIM_SIGNALS_MAX    +3.4 KB/doc   a signal is bigger than an element
//      ANIM_TRACKS_MAX     +3.2 KB/doc   264 B x 12 elems - pays per element
//      ANIM_KEYS_MAX       +2.3 KB/doc   16 B x 12 tracks x 12 elems: the
//                                        deepest nesting, so the sleeper cost
//      ANIM_SIG_TARGETS_MAX +0.6 KB/doc  140 B x 4 signals
//
//  ...but sizeof(AnimDoc) is NOT the number that matters. Docs are held by
//  value in bulk, and those multipliers are what actually spend memory:
//
//      anim_editor undoBuf[UNDO_MAX=16]        16 x 52 KB = ~0.82 MB
//      anim_stage  slots[ANIM_STAGE_SLOTS_MAX=8] 8 x 52 KB = ~0.41 MB
//
//  So a change to any capacity above lands in the build multiplied by ~24.
//
//  WHY THIS MATTERS ON WEB: the Emscripten target links with a FIXED heap,
//  -sTOTAL_MEMORY=134217728 (128 MB) and no ALLOW_MEMORY_GROWTH (CMakeLists.txt).
//  There is no growing out of it at runtime - overshoot is an abort, not a
//  slowdown, and these buffers are static, so they are spent before main()
//  runs and sit alongside raylib, Box2D and the preloaded resource image.
//  Desktop has room to spare here; WASM is the binding constraint. Before
//  raising a capacity, multiply by ~24 and check it against that 128 MB.
//
//  Sanity-check a change with:
//    printf("%zu\n", sizeof(AnimDoc));
// ---------------------------------------------------------------------------
#define ANIM_NAME_MAX      32   // element / signal / doc name buffer
#define ANIM_TEXT_LEN_MAX  64   // a TEXT element's string buffer
#define ANIM_KEYS_MAX      16   // keyframes per track (deepest nesting: this
                                // one multiplies by tracks AND elems)
#define ANIM_TRACKS_MAX    12   // tracks (animated properties) per element
                                // (>= the largest per-kind property count, so
                                //  every property of an element is trackable)
#define ANIM_ELEMS_MAX     12   // elements per document
#define ANIM_SIGNALS_MAX    4   // signals per document (a signal is the single
                                // largest sub-struct, ~3.4 KB each)

// Seconds of smooth-loop blend a document gets when nothing says otherwise -
// new documents and .cfg files written before the field existed (see
// AnimDoc.loopSmooth / loopBlend).
#define ANIM_LOOP_BLEND_DEFAULT 0.3f

// ---------------------------------------------------------------------------
//  Easing, as a plain id (keeps AnimKey pointer-free: memcpy/fscanf-clean).
//  Order matches the stable .cfg names in anim.c's table - do not reorder.
// ---------------------------------------------------------------------------
typedef enum {
    ANIM_EASE_LINEAR = 0,
    ANIM_EASE_SINE_IN,  ANIM_EASE_SINE_OUT,  ANIM_EASE_SINE_INOUT,
    ANIM_EASE_QUAD_IN,  ANIM_EASE_QUAD_OUT,  ANIM_EASE_QUAD_INOUT,
    ANIM_EASE_CUBIC_IN, ANIM_EASE_CUBIC_OUT, ANIM_EASE_CUBIC_INOUT,
    ANIM_EASE_EXPO_IN,  ANIM_EASE_EXPO_OUT,
    ANIM_EASE_BACK_IN,  ANIM_EASE_BACK_OUT,
    ANIM_EASE_ELASTIC_OUT, ANIM_EASE_BOUNCE_OUT,
    ANIM_EASE_COUNT
} AnimEase;

const char *AnimEaseName(int ease);           // id -> "sineOut"; bad id -> "linear"
int         AnimEaseByName(const char *name); // name -> id; unknown -> LINEAR
float       AnimEaseApply(int ease, float p); // eased p; LINEAR/bad id -> p
int         AnimEaseCount(void);              // == ANIM_EASE_COUNT

// ---------------------------------------------------------------------------
//  Which PROPERTY a track drives. The ranges are disjoint per element type so
//  a prop pasted onto the wrong element type can't silently match (same idea
//  as scene_anim's GP_/TP_/SP_ ranges).
//
//  Values are documented in canvas fractions unless noted; a track's keyframe
//  `value` is that property's absolute target at that keyframe.
// ---------------------------------------------------------------------------
typedef enum {
    // TEXT (0..) ------------------------------------------------------------
    AP_T_POS_X = 0,     // text center X, fraction of game width
    AP_T_POS_Y,         // text center Y, fraction of game height
    AP_T_SIZE,          // font size,     fraction of game height
    AP_T_ALPHA,         // 0..1 opacity
    AP_T_ROT,           // whole-text rotation, degrees
    AP_T_CRUMBLE,       // 0..1 per-glyph crumble amount (0 = intact)
    AP_T_COLOR,         // RGBA tint; keys use AnimKey.cval, not value

    // SHAPE (100..) ---------------------------------------------------------
    AP_S_POS_X = 100,   // shape center X, fraction of game width
    AP_S_POS_Y,         // shape center Y, fraction of game height
    AP_S_W,             // width,  fraction of game width
    AP_S_H,             // height, fraction of game height
    AP_S_ALPHA,         // 0..1 opacity
    AP_S_ROT,           // rotation, degrees
    AP_S_COLOR,         // RGB fill; keys use AnimKey.cval, not value
    AP_S_OUTLINE_COLOR, // RGB outline; keys use AnimKey.cval, not value
    AP_S_OUTLINE,       // outline thickness, fraction of game height (0 = off)
    AP_S_OUTLINE_ALPHA, // 0..1 outline opacity (AP_S_ALPHA is the FILL opacity)
    AP_S_SCALE,         // uniform size MULTIPLIER on top of w/h (1 = authored
                        // size). The one-track way to grow/shrink a shape
                        // without keeping w and h in proportion by hand.

    // GLOBAL (200..) --------------------------------------------------------
    AP_G_FADE = 200,    // whole-screen fade-to-color amount, 0..1
    AP_G_COLOR,         // RGBA fade-to colour; keys use AnimKey.cval
    AP_G_BG_ALPHA,      // 0..1 opacity of the scene background fill
    AP_G_BG_COLOR,      // RGB scene background; keys use AnimKey.cval
} AnimPropKind;

// A single keyframe on the shared clock.
typedef struct {
    float t;            // seconds
    float value;        // property value AT this keyframe (absolute)
    Color cval;         // RGB AT this keyframe (AP_*_COLOR tracks only; the
                        // alpha channel is ignored - alpha has its own track)
    int   ease;         // AnimEase id; eases the segment ENDING at this key
} AnimKey;

// One animated property = an ordered list of keyframes.
typedef struct {
    int     prop;                   // an AP_* value
    AnimKey keys[ANIM_KEYS_MAX];
    int     keyCount;
} AnimTrack;

typedef enum { AE_TEXT = 0, AE_SHAPE, AE_GLOBAL } AnimElemKind;
// Shape geometry, all center-anchored on posFrac and rotated about the center.
// Both axes are multiplied by AP_S_SCALE before use, so a single scale track
// grows a shape about its center without disturbing its aspect ratio.
// sizeFrac meaning per kind:
//   RECT     w x h box
//   CIRCLE   ellipse with axes w x h
//   SQUARE   side = game.y * h for BOTH pixel axes (stays square; w unused)
//   RHOMBUS  diamond with diagonals w (horizontal) and h (vertical)
//   TRIANGLE isosceles pointing up: apex at -h/2, base w wide at +h/2
//   LINE     segment of length game.x * w, thickness game.y * h; fill colour
// Order is the stable .cfg representation (anim_io.c) - do not reorder.
typedef enum {
    SHAPE_RECT = 0, SHAPE_CIRCLE, SHAPE_SQUARE,
    SHAPE_RHOMBUS, SHAPE_TRIANGLE, SHAPE_LINE,
    SHAPE_KIND_COUNT
} AnimShapeKind;

// One animated thing. Which base fields matter depends on `kind`:
//   AE_TEXT   : text, color, and base posFrac/sizeFrac (defaults when no track)
//   AE_SHAPE  : shapeKind, color, base posFrac + sizeFrac(w)/ h via base fields
//   AE_GLOBAL : color (the fade colour) + bgColor (scene background, drawn
//               behind every element); position/size unused
typedef struct {
    AnimElemKind kind;
    char         name[ANIM_NAME_MAX];

    // base (static) properties - used as the value for any property that has
    // NO track, and as the starting point the editor seeds new tracks from.
    char    text[ANIM_TEXT_LEN_MAX];   // AE_TEXT only
    Color   color;                     // fill / text / fade colour
    Vector2 posFrac;                   // center (both text and shapes)
    Vector2 sizeFrac;                  // x=width/size, y=height (shape height)
    float   scaleFrac;                 // AE_SHAPE: rest-pose AP_S_SCALE (1 = as
                                       // authored); multiplies sizeFrac
    float   rotBase;                   // AE_TEXT/AE_SHAPE: rest-pose rotation in
                                       // degrees (AP_*_ROT with no track). 0 =
                                       // upright. Lets a static shape be rotated
                                       // (and a line be authored by endpoints).
    int     shapeKind;                 // AnimShapeKind (AE_SHAPE only)
    Color   outlineColor;              // AE_SHAPE: outline tint (rest pose)
    float   outlineFrac;               // AE_SHAPE: outline thickness, fraction
                                       // of game height; 0 = no outline
    Color   bgColor;                   // AE_GLOBAL: scene background fill; the
                                       // alpha channel is the rest-pose
                                       // AP_G_BG_ALPHA (0 = no background)

    // AUTHORING FLAGS (do not change what a track stores; they change how the
    // base geometry is interpreted at draw time / edited in the inspector).
    bool    sizeAbsolute;              // AE_TEXT/AE_SHAPE: when true, sizeFrac
                                       // (and text size, outline thickness) are
                                       // ABSOLUTE PIXELS instead of canvas
                                       // fractions - a fixed size regardless of
                                       // how the canvas rescales. Position is
                                       // always a fraction. Default false.
    bool    cornerMode;               // AE_SHAPE: inspector-only editing mode.
                                       // When true the inspector authors the
                                       // shape by two opposite corners (line:
                                       // two endpoints) instead of center+size;
                                       // storage stays center+size. Default false.
    bool    outlineCrisp;             // AE_SHAPE + circle: draw the outline as a
                                       // smooth ring (DrawRing) instead of the
                                       // faceted polygon loop, giving a stable
                                       // crisp edge. Ignored for non-circle
                                       // shapes. Default false (polygon /
                                       // "crawling").
                                       // NOTE: crisp mode removes the chord/cap
                                       // shimmer, but a residual sub-pixel crawl
                                       // remains because shapes draw at float
                                       // positions with MSAA off. Worth exploring
                                       // as a different style: enable MSAA 4x
                                       // (FLAG_MSAA_4X_HINT in main.c) to smooth
                                       // all edges app-wide, or pixel-snap the
                                       // crisp draw to match the baked look.

    AnimTrack tracks[ANIM_TRACKS_MAX];
    int       trackCount;
} AnimElem;

typedef enum { ANIM_FWD = 0, ANIM_REV } AnimPlayDir;

// These two are capacities like the ones at the top of the file, and carry the
// same cost rules - see the memory notes there before raising either.
#define ANIM_SIG_TARGETS_MAX 32   // (element, property) pairs a signal drives
                                  // (>= ANIM_ELEMS_MAX so every element can be
                                  //  driven, with several properties each)
                                  // 140 B each; the bulk of AnimSignal's 4.5 KB
                                  // The editor authors targets in property
                                  // GROUPS (see AnimPropGroup in anim_io.h), so
                                  // one authored track spends up to
                                  // ANIM_GROUP_PROPS slots - hence 32, not 24.
#define ANIM_SIG_KEYS_MAX    8    // keyframes per target

// One property a signal drives, on one element.
//
// Key TIMES ARE NORMALIZED 0..1 (a fraction of the owning signal's `length`),
// so changing the length rescales the whole signal instead of stranding keys
// past its end. Everything else matches a timeline key: `value` for scalar
// props, `cval` for AP_*_COLOR props, `ease` shapes the segment ENDING at it.
//
// There is an IMPLICIT key at u=0 holding whatever the element looked like
// when the signal fired (captured by AnimSignalPlayerStart), which is what
// makes a signal a smooth transition FROM the live scene rather than a jump.
typedef struct {
    int     elemIdx;                    // index into AnimDoc.elems
    int     prop;                       // an AP_* valid for that element's kind
    AnimKey keys[ANIM_SIG_KEYS_MAX];    // key.t is 0..1, NOT seconds
    int     keyCount;
} AnimSigTarget;

// ---------------------------------------------------------------------------
//  Mouse-position parameter binding (the signal modal's "--params--" section)
// ---------------------------------------------------------------------------
// A signal that consumes the emit's position (AnimSignal.usesPos) drives one or
// more POSITION SLOTS from that mouse position. A slot is a point on an element:
// the center of a text/shape, or - for a corners-mode shape, which is authored
// by two opposite corners - either corner P0 / P1. Each binding is an authored
// track: its keys ease the slot from the live pose to (mouse + per-key offset)
// at the key's normalized time u, exactly like a signal target eases a value.
//
// Driving a corner recomputes the element's stored center+size so that corner
// lands on the target while the OTHER corner holds its base value (see
// AnimCornersToGeom / PosParamEval); driving the center simply translates.
typedef struct { float t; float offX, offY; int ease; } AnimPosKey; // t = u 0..1

#define ANIM_SIG_POS_MAX  4       // position bindings a signal may drive
typedef struct {
    int        elemIdx;                     // index into AnimDoc.elems
    int        slot;                        // 0 = center; corners: 0 = P0, 1 = P1
    AnimPosKey keys[ANIM_SIG_KEYS_MAX];
    int        keyCount;
} AnimSigPosParam;

// ---------------------------------------------------------------------------
//  Sequence offset (the signal modal's "--sequence--" section)
// ---------------------------------------------------------------------------
// The same document is often played several times at once (the scene table
// lists one row per instance - see anim_scene.h) and each instance carries a
// sequence number `seq`. When a signal opts in (AnimSignal.usesSeq), it adds
// `seq * seqMult * env(u)` to each of its seqTargets' scalar properties, ON TOP
// of whatever else drives them, where env(u) is the 0..1 envelope keyed by
// seqKeys (eased, implicit 0 at u=0, held after the last key). This fans the
// instances apart at a chosen beat - three boxes offset in size at u=0.55.
#define ANIM_SIG_SEQ_TARGETS 8    // (elem,prop) pairs one sequence offsets
#define ANIM_SIG_SEQ_KEYS    8
typedef struct { int elemIdx; int prop; } AnimSigSeqTarget;  // scalar props only
typedef struct { float t; float amt; int ease; } AnimSeqKey; // t = u; amt = 0..1

// A named transition: firing signal `name` eases every target from its live
// value into the target's keys, over `length` seconds. length <= 0 snaps
// instantly to the final key (an "instant trigger").
//
// `terminal` marks the signal as an ENDING: a runtime instance playing this doc
// (see anim_stage.h) stops itself once this signal's player has run its full
// `length`, so a looping animation winds down through the authored transition
// instead of being cut off mid-cycle. It means nothing to the editor preview,
// where the playhead belongs to the user.
typedef struct {
    char          name[ANIM_NAME_MAX];
    float         length;               // seconds; 0 = instant
    bool          terminal;             // completing this signal ends playback

    // Does this signal CONSUME the emit's position parameter (SignalParams.pos)?
    // Authored per signal, because whether a placed transition makes sense is a
    // property of the animation, not of the firing site: a ripple wants to play
    // where the mouse clicked, a screen wipe does not. When false, an emit
    // carrying a position is ignored and the authored motion plays in place.
    // When true, the signal's `posParams` bindings ease their slots to the
    // emitted mouse position (see AnimSigPosParam / PosParamEval).
    bool          usesPos;

    // Does this signal consume the instance's sequence number? When true it adds
    // `seq * seqMult * env(u)` to each seqTarget's property (see AnimSeqKey).
    bool          usesSeq;
    float         seqMult;              // per-instance multiplier, -30..+30

    AnimSigTarget    targets[ANIM_SIG_TARGETS_MAX];
    int              targetCount;
    AnimSigPosParam  posParams[ANIM_SIG_POS_MAX];
    int              posParamCount;
    AnimSigSeqTarget seqTargets[ANIM_SIG_SEQ_TARGETS];
    int              seqTargetCount;
    AnimSeqKey       seqKeys[ANIM_SIG_SEQ_KEYS];
    int              seqKeyCount;
} AnimSignal;

// One animation document = the unit the editor edits and the .cfg stores.
typedef struct {
    char       name[ANIM_NAME_MAX];
    float      duration;                    // clock length (>= last keyframe)

    // Intro / outro trim on the shared clock. [0,introEnd) is the INTRO: it
    // plays once when playback starts and is skipped on every loop after that.
    // [outroStart,duration] is the OUTRO: trimmed - never played, never shown.
    // A loop cycle is therefore [introEnd, outroStart). Read them through
    // AnimDocIntroEnd/AnimDocOutroStart, which clamp and handle the unset case.
    float      introEnd;                    // 0 = no intro
    float      outroStart;                  // <= 0 means "unset" -> duration

    // SMOOTH LOOP. Keyframes stop at the last key, so a looping document holds
    // its final pose to the end of the cycle and then SNAPS back to the loop
    // start. With loopSmooth on, the last `loopBlend` seconds of every cycle
    // ease each property into the pose it has at the loop start, so the wrap is
    // continuous - the loop's last key lerps into the loop's first key. Off
    // gives the old hard restart (which a strobe or a hard cut may want).
    // Only LOOPING playback blends: a one-shot play shows its tail as authored.
    bool       loopSmooth;
    float      loopBlend;                   // seconds; clamped to the cycle len

    AnimElem   elems[ANIM_ELEMS_MAX];
    int        elemCount;
    AnimSignal signals[ANIM_SIGNALS_MAX];
    int        signalCount;
} AnimDoc;

// ---------------------------------------------------------------------------
//  Authoring / defaults
// ---------------------------------------------------------------------------
void AnimDocInit(AnimDoc *doc);                 // empty doc, sane duration/name
void AnimElemInit(AnimElem *e, AnimElemKind kind);   // one element w/ base props
AnimElem *AnimDocAddElem(AnimDoc *doc, AnimElemKind kind);   // NULL if full
void AnimDocRemoveElem(AnimDoc *doc, int idx);               // shift down over idx

// Reorder: swap element `idx` with its neighbour `delta` slots away (-1 up,
// +1 down). Element order IS draw order, so this is the z-order control.
// No-op if idx or the destination is out of range.
void AnimDocMoveElem(AnimDoc *doc, int idx, int delta);

// Insert a copy of element `idx` directly after it (tracks and keys included),
// with its name uniquified ("title" -> "title_2"). NULL if idx bad or doc full.
AnimElem *AnimDocDuplicateElem(AnimDoc *doc, int idx);

// Give element `idx` a name no OTHER element in the doc uses, by appending
// "_2", "_3", ... Used after duplicating and after inserting a library element.
void AnimDocUniquifyElemName(AnimDoc *doc, int idx);
AnimTrack *AnimElemAddTrack(AnimElem *e, int prop);         // NULL if full/dupe
AnimTrack *AnimElemFindTrack(AnimElem *e, int prop);        // NULL if absent
void AnimElemRemoveTrack(AnimElem *e, int idx);             // shift down over idx
AnimKey *AnimTrackAddKey(AnimTrack *tr, float t, float value, int ease);
void AnimTrackRemoveKey(AnimTrack *tr, int idx);
void AnimTrackSortKeys(AnimTrack *tr);          // keep keys ascending in t

// Move key `idx` to time t, keeping keys sorted; returns the key's NEW index
// (enables continuous timeline drags across neighbours). -1 if idx invalid.
int AnimTrackSetKeyTime(AnimTrack *tr, int idx, float t);

// Auto-key core: if a key lies within `eps` of t, update its value (ease kept);
// otherwise insert a new LINEAR key at t. NULL if the track is full.
AnimKey *AnimTrackWriteKeyAt(AnimTrack *tr, float t, float value, float eps);

// Colour-track variants of AddKey/WriteKeyAt (AP_*_COLOR: keys carry cval).
AnimKey *AnimTrackAddColorKey(AnimTrack *tr, float t, Color c, int ease);
AnimKey *AnimTrackWriteColorKeyAt(AnimTrack *tr, float t, Color c, float eps);

// True for the AP_*_COLOR properties (keys carry cval instead of value).
bool AnimPropIsColor(int prop);

// Editor slider range for a property (e.g. fractions 0..1, rotation -360..360).
float AnimPropMin(int prop);
float AnimPropMax(int prop);

// ---------------------------------------------------------------------------
//  Corner-mode geometry (shared by the inspector's corner sliders and the
//  runtime, so a corner authored in the editor re-anchors identically at play)
// ---------------------------------------------------------------------------
// A corners-mode shape is STORED as center+size but AUTHORED by two opposite
// corners. AnimCornersToGeom converts a corner pair back to the stored form:
// p0/p1 are the corner points in canvas fractions, scaleFrac divides back out
// (the renderer multiplies size by AP_S_SCALE) so w/h are the base size.
void    AnimCornersToGeom(Vector2 p0, Vector2 p1, float scaleFrac,
                          float *cx, float *cy, float *w, float *h);
// The element's rest-pose corner `slot` (0 = P0 = center-halfsize, 1 = P1) in
// canvas fractions, derived from its base center/size/scale.
Vector2 AnimGeomToCorner(const AnimElem *e, int slot);

// ---------------------------------------------------------------------------
//  Evaluation
// ---------------------------------------------------------------------------
// Value of a track at time t: before the first key -> first value; after the
// last -> last value; between two keys -> eased interpolation (the RIGHT key's
// ease shapes the segment). Empty track -> `missing`.
float AnimTrackEval(const AnimTrack *tr, float t, float missing);

// Bracketing keys [i0, i1] at time t (clamped: before first -> 0/0, after last
// -> last/last; between keys -> the segment ends). false if the track is empty.
bool AnimTrackSegment(const AnimTrack *tr, float t, int *i0, int *i1);

// Colour of a colour track at time t (per-channel eased RGB mix; overshoot-
// safe). Alpha always comes from `missing` - colour tracks are RGB only.
// Empty/NULL track -> `missing`.
Color AnimTrackEvalColor(const AnimTrack *tr, float t, Color missing);

// Colour of a SPECIFIC colour property at time t: that prop's track if present,
// else its base colour (AP_S_OUTLINE_COLOR -> e->outlineColor, else e->color).
Color AnimElemColorProp(const AnimElem *e, int prop, float t);

// Element's PRIMARY colour at time t (fill/text/fade), via AnimElemColorProp.
Color AnimElemColor(const AnimElem *e, float t);

// Value of property `prop` on element e at time t. If e has no track for prop,
// returns the element's BASE value for that property (rest pose).
float AnimElemProp(const AnimElem *e, int prop, float t);

// Latest keyframe time across the whole document (what duration should cover).
float AnimDocMaxKeyTime(const AnimDoc *doc);

// Trim accessors - always use these instead of reading the raw fields, so the
// clamping (and the "outroStart unset -> duration" default that keeps old .cfg
// files and zeroed docs working) lives in exactly one place.
float AnimDocOutroStart(const AnimDoc *doc);  // end of the played section
float AnimDocIntroEnd(const AnimDoc *doc);    // 0..outroStart
float AnimDocPlayLen(const AnimDoc *doc);     // outroStart - introEnd (loop len)

// Draw the whole document at clock time t, in GAME space (call inside Draw()).
void AnimDocDraw(const AnimDoc *doc, float t);

// ---------------------------------------------------------------------------
//  Signal player: plays ONE AnimSignal as a transition off the LIVE scene.
//
//  On Start, each target's current value is captured; while playing, the
//  target eases from that captured value into the target's own keys (whose
//  times are 0..1 fractions of sig->length). This is what lets a signal be
//  fired at any moment and still blend smoothly from whatever is on screen.
//
//  A signal player OVERRIDES the document's own timeline for exactly the
//  (element, property) pairs it targets; everything else keeps animating
//  normally. Pass one to AnimDocDrawEx to see it.
// ---------------------------------------------------------------------------
typedef struct {
    const AnimSignal *sig;
    float clock;                                // 0..sig->length
    bool  playing;
    // value of each target at fire time (the implicit key at u=0)
    float fromValue[ANIM_SIG_TARGETS_MAX];
    Color fromColor[ANIM_SIG_TARGETS_MAX];
    // Per-emit params captured at Start (see SignalParams). hasPos feeds the
    // signal's posParams bindings, so the same authored transition can be fired
    // at a runtime location. Zeroed = no override.
    SignalParams param;

    // This instance's sequence number, feeding the signal's sequence offset
    // (AnimSignal.usesSeq / seqMult). It belongs to the OWNER of the player (the
    // stage slot / the editor preview), not to an emit: AnimSignalPlayerStart
    // deliberately leaves it alone, so it is set once when the player is created
    // and survives every re-fire.
    int seq;

    // Live pose of each posParams binding's element, captured at fire time - the
    // pose a Mouse-Position binding eases FROM (so it never teleports on fire).
    // For a corner binding, px/py/w/h/scale reconstruct both live corners; the
    // undriven corner is held here while the driven one eases to the mouse.
    struct { float px, py, w, h, scale; bool corner; } fromPose[ANIM_SIG_POS_MAX];
} AnimSignalPlayer;

// Begin `sig`, capturing each target's live value from `doc` at `docTime`.
// A NULL sig (or one with no targets) leaves the player idle. `params` (may be
// NULL for none) is stored and applied by AnimSignalPlayerEval - a position
// param eases the signal's posParams bindings toward param.pos.
void AnimSignalPlayerStart(AnimSignalPlayer *p, const AnimSignal *sig,
                           const AnimDoc *doc, float docTime,
                           const SignalParams *params);
void AnimSignalPlayerUpdate(AnimSignalPlayer *p, float dt);
bool AnimSignalPlayerDone(const AnimSignalPlayer *p);

// Does this player currently drive (elemIdx, prop)? If so fill whichever of
// outValue / outColor is non-NULL (per AnimPropIsColor(prop)) and return true.
// Both may be NULL to just test. False when idle or not a target.
bool AnimSignalPlayerEval(const AnimSignalPlayer *p, int elemIdx, int prop,
                          float *outValue, Color *outColor);

// The additive sequence offset this playing signal applies to (elemIdx, prop):
// seq * seqMult * envelope(u), or 0 when the signal has no sequence, seq is 0,
// or the prop is not a seqTarget. Stacked ON TOP of the value by AnimElemProp,
// so it layers over a track OR another signal target. Scalar props only.
float AnimSignalPlayerSeqOffset(const AnimSignalPlayer *p, int elemIdx, int prop);

// Draw the document with an optional signal player layered on top (NULL = the
// plain AnimDocDraw behaviour).
void AnimDocDrawEx(const AnimDoc *doc, float t, const AnimSignalPlayer *ovr);

// As AnimDocDrawEx, but tells the evaluator whether this playback LOOPS. Only a
// looping playback wraps, so only it gets the doc's smooth-loop blend (see
// AnimDoc.loopSmooth); AnimDocDrawEx is exactly this with looping = false.
// Call it from anything that owns a looping clock (anim_stage, the editor's
// preview) so the last loopBlend seconds ease back into the loop-start pose.
void AnimDocDrawLoop(const AnimDoc *doc, float t, const AnimSignalPlayer *ovr,
                     bool looping);

// The smooth-loop window on its own, for code that reads properties instead of
// drawing them (and for headless tests, which cannot call a draw). Between
// Begin and End, AnimElemProp / AnimElemColorProp blend the last loopBlend
// seconds of the cycle into the loop-start pose, exactly as during a draw.
// Begin(doc, false) - or a NULL doc - is the same as no blend at all.
// ALWAYS pair them: the window is file-scope state, like the signal override.
void AnimLoopBlendBegin(const AnimDoc *doc, bool looping);
void AnimLoopBlendEnd(void);

// ---------------------------------------------------------------------------
//  Player: plays a doc (or a section of it) in one direction on its own clock.
//  Forward  -> local clock 0..len, sampled at (sectionStart + clock).
//  Reverse  -> sampled at (sectionEnd - clock): the same doc runs backwards.
// ---------------------------------------------------------------------------
typedef struct {
    const AnimDoc *doc;
    int   dir;                  // AnimPlayDir
    float secStart, secEnd;     // section being played
    float clock;                // 0..(secEnd-secStart)
    bool  playing;
    bool  loop;
    bool  introDone;            // set on the first loop wrap: later cycles
                                // restart at the doc's introEnd, not at 0
} AnimPlayer;

void  AnimPlayerStart(AnimPlayer *p, const AnimDoc *doc, int dir,
                      float secStart, float secEnd);   // play a section
void  AnimPlayerStartAll(AnimPlayer *p, const AnimDoc *doc, int dir); // whole doc
void  AnimPlayerUpdate(AnimPlayer *p, float dt);
bool  AnimPlayerDone(const AnimPlayer *p);
float AnimPlayerSampleTime(const AnimPlayer *p);   // doc-clock time to draw at
void  AnimPlayerDraw(const AnimPlayer *p);         // AnimDocDraw at sample time

#endif // ANIM_H
