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
#include <stdbool.h>

// --- capacities (kept small; a document is copied by value for undo) --------
#define ANIM_NAME_MAX      32   // element / signal / doc name buffer
#define ANIM_TEXT_LEN_MAX  64   // a TEXT element's string buffer
#define ANIM_KEYS_MAX      16   // keyframes per track
#define ANIM_TRACKS_MAX    10   // tracks (animated properties) per element
#define ANIM_ELEMS_MAX     12   // elements per document
#define ANIM_SIGNALS_MAX    8   // signals per document

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

    // GLOBAL (200..) --------------------------------------------------------
    AP_G_FADE = 200,    // whole-screen fade-to-color amount, 0..1
    AP_G_COLOR,         // RGBA fade-to colour; keys use AnimKey.cval
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
//   AE_GLOBAL : color (the fade colour); position/size unused
typedef struct {
    AnimElemKind kind;
    char         name[ANIM_NAME_MAX];

    // base (static) properties - used as the value for any property that has
    // NO track, and as the starting point the editor seeds new tracks from.
    char    text[ANIM_TEXT_LEN_MAX];   // AE_TEXT only
    Color   color;                     // fill / text / fade colour
    Vector2 posFrac;                   // center (both text and shapes)
    Vector2 sizeFrac;                  // x=width/size, y=height (shape height)
    int     shapeKind;                 // AnimShapeKind (AE_SHAPE only)
    Color   outlineColor;              // AE_SHAPE: outline tint (rest pose)
    float   outlineFrac;               // AE_SHAPE: outline thickness, fraction
                                       // of game height; 0 = no outline

    AnimTrack tracks[ANIM_TRACKS_MAX];
    int       trackCount;
} AnimElem;

typedef enum { ANIM_FWD = 0, ANIM_REV } AnimPlayDir;

// A named edge: firing signal `name` plays the doc in `dir` over the section.
typedef struct {
    char  name[ANIM_NAME_MAX];
    int   dir;                  // AnimPlayDir
    float sectionStart, sectionEnd;   // seconds; whole doc = [0, duration]
} AnimSignal;

// One animation document = the unit the editor edits and the .cfg stores.
typedef struct {
    char       name[ANIM_NAME_MAX];
    float      duration;                    // clock length (>= last keyframe)
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

// Draw the whole document at clock time t, in GAME space (call inside Draw()).
void AnimDocDraw(const AnimDoc *doc, float t);

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
} AnimPlayer;

void  AnimPlayerStart(AnimPlayer *p, const AnimDoc *doc, int dir,
                      float secStart, float secEnd);   // play a section
void  AnimPlayerStartAll(AnimPlayer *p, const AnimDoc *doc, int dir); // whole doc
void  AnimPlayerUpdate(AnimPlayer *p, float dt);
bool  AnimPlayerDone(const AnimPlayer *p);
float AnimPlayerSampleTime(const AnimPlayer *p);   // doc-clock time to draw at
void  AnimPlayerDraw(const AnimPlayer *p);         // AnimDocDraw at sample time

#endif // ANIM_H
