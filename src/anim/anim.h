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
#include "../include/easing.h"   // EaseFn typedef + easing function bodies
#include <stdbool.h>

// --- capacities (kept small; a document is copied by value for undo) --------
#define ANIM_NAME_MAX      32   // element / signal / doc name buffer
#define ANIM_TEXT_LEN_MAX  64   // a TEXT element's string buffer
#define ANIM_KEYS_MAX       8   // keyframes per track
#define ANIM_TRACKS_MAX     6   // tracks (animated properties) per element
#define ANIM_ELEMS_MAX      8   // elements per document
#define ANIM_SIGNALS_MAX    6   // signals per document

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
    AP_T_POS_Y,         // text top Y,    fraction of game height
    AP_T_SIZE,          // font size,     fraction of game height
    AP_T_ALPHA,         // 0..1 opacity
    AP_T_ROT,           // whole-text rotation, degrees
    AP_T_CRUMBLE,       // 0..1 per-glyph crumble amount (0 = intact)

    // SHAPE (100..) ---------------------------------------------------------
    AP_S_POS_X = 100,   // shape center X, fraction of game width
    AP_S_POS_Y,         // shape center Y, fraction of game height
    AP_S_W,             // width,  fraction of game width
    AP_S_H,             // height, fraction of game height
    AP_S_ALPHA,         // 0..1 opacity
    AP_S_ROT,           // rotation, degrees

    // GLOBAL (200..) --------------------------------------------------------
    AP_G_FADE = 200,    // whole-screen fade-to-color amount, 0..1
} AnimPropKind;

// A single keyframe on the shared clock.
typedef struct {
    float  t;           // seconds
    float  value;       // property value AT this keyframe (absolute)
    EaseFn ease;        // easing used on the segment ENDING at this key; NULL=linear
} AnimKey;

// One animated property = an ordered list of keyframes.
typedef struct {
    int     prop;                   // an AP_* value
    AnimKey keys[ANIM_KEYS_MAX];
    int     keyCount;
} AnimTrack;

typedef enum { AE_TEXT = 0, AE_SHAPE, AE_GLOBAL } AnimElemKind;
typedef enum { SHAPE_RECT = 0, SHAPE_CIRCLE } AnimShapeKind;

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
    Color   color;
    Vector2 posFrac;                   // center (text: x=center, y=top edge)
    Vector2 sizeFrac;                  // x=width/size, y=height (shape height)
    int     shapeKind;                 // AnimShapeKind (AE_SHAPE only)

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
AnimTrack *AnimElemAddTrack(AnimElem *e, int prop);         // NULL if full/dupe
AnimTrack *AnimElemFindTrack(AnimElem *e, int prop);        // NULL if absent
AnimKey *AnimTrackAddKey(AnimTrack *tr, float t, float value, EaseFn ease);
void AnimTrackSortKeys(AnimTrack *tr);          // keep keys ascending in t

// ---------------------------------------------------------------------------
//  Evaluation
// ---------------------------------------------------------------------------
// Value of a track at time t: before the first key -> first value; after the
// last -> last value; between two keys -> eased interpolation (the RIGHT key's
// ease shapes the segment). Empty track -> `missing`.
float AnimTrackEval(const AnimTrack *tr, float t, float missing);

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
