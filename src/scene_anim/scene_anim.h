// ============================================================================
//  scene_anim.h  -  data-driven scene text / global / shape animations
//
//  A scene (app state) DEFINES its animated content as plain data:
//    - AnimText[]   : everything needed to DrawText + how it animates in/out
//    - AnimPhase[]  : timing rows (kind, start, end, easing) - any order
//    - SceneAnim    : bundles texts + global beats + shapes + scene art
//  and a SceneAnimPlayer PLAYS a spec in one direction:
//    - ANIM_INTRO : entering the scene (texts slide/fade IN to their rest pose)
//    - ANIM_OUTRO : leaving the scene  (texts center/crumble/fade OUT)
//
//  A finished INTRO and an unstarted OUTRO both render the exact rest pose,
//  so a state calls SceneAnimDrawTexts() every frame forever - that IS the
//  normal text draw. No second DrawText path = no duplicated declarations.
//
//  Phase kinds are prefixed by scope:
//    GP_*  global (whole screen)      - looked up in SceneAnim.introGlobal/outroGlobal
//    TP_*  text  (one AnimText)       - looked up in AnimText.intro/outro
//    SP_*  shape (zoom rectangles)    - looked up in SceneAnim.outroShape
//  Rows may appear in ANY order inside a table: they are found by kind,
//  never by array index. A missing row = "that effect stays at rest".
//
//  See main_menu.c for the integration example (intro embedded in the state)
//  and transition_state.c for the generic outro player app-state.
// ============================================================================

#ifndef SCENE_ANIM_H
#define SCENE_ANIM_H

#include "raylib.h"
#include "easing.h"     // EaseFn typedef + the easing functions (bodies in easing.c)
#include <stdbool.h>

// ---------------------------------------------------------------------------
//  Phase kinds. The numeric ranges are deliberately DISJOINT (0/100/200) so a
//  kind pasted into the wrong table can never silently match another scope.
// ---------------------------------------------------------------------------
typedef enum {                  // GLOBAL - whole-screen beats
    GP_FADE_BLACK = 0,          // outro: screen -> black (hand-off when done)
    GP_UNFADE_BLACK,            // intro: black -> screen
    GP_ART_FADE,                // outro: scene art alpha 1 -> 0 (fed to drawArt)
} GlobalPhaseKind;

typedef enum {                  // TEXT - per AnimText beats
    TP_SLIDE_IN = 100,          // off-screen edge -> rest pose (no motion if the
                                //   text is already on-screen when an OUTRO starts)
    TP_CENTER_X,                // rest -> horizontally centered on screen
    TP_CENTER_Y,                // rest -> vertically centered on screen
    TP_FADE_IN,                 // alpha 0 -> 1 (intro)
    TP_FADE_OUT,                // alpha 1 -> 0 (outro)
    TP_CRUMBLE,                 // per-glyph fall/spin/fade particles (outro)
} TextPhaseKind;

typedef enum {                  // SHAPE - zoom rectangles
    SP_ZOOM_SETTLE = 200,       // boxes keep zooming while their gap shrinks to 0
                                //   (they fully OVERLAY on one shared phase)
    SP_ZOOM_SETTLE_GAP1,        // alternative settle: boxes converge but KEEP a
                                //   predefined gap between each rectangle
                                //   (ZOOM_SETTLE_GAP1_FRAC in scene_anim.c).
                                //   Use one settle kind per scene, not both.
    SP_ZOOM_REVERSE,            // the grouped boxes reverse inward together
} ShapePhaseKind;

// ---------------------------------------------------------------------------
//  One animation beat: WHAT (kind), WHEN (start..end seconds on the play
//  clock) and HOW it moves (ease; NULL = linear). This is the whole exposed
//  parameter surface - re-time a transition by editing these rows only.
// ---------------------------------------------------------------------------
typedef struct {
    int    kind;                // a GP_* / TP_* / SP_* value
    float  start, end;          // seconds
    EaseFn ease;                // from easing.h; NULL = linear
} AnimPhase;

// Order-independent lookup: find a row by kind (NULL if absent) and read its
// eased 0..1 progress at time t (`missing` is returned when the row is absent
// - the caller picks the value that means "at rest" for that kind).
const AnimPhase *AnimPhaseFind(const AnimPhase *phases, int count, int kind);
float AnimPhaseAmount(const AnimPhase *phases, int count, int kind,
                      float t, float missing);

// ---------------------------------------------------------------------------
//  AnimText: ALL the information needed to draw one text plus its intro and
//  outro phase tables. Positions/sizes are fractions of the game canvas
//  (ScreenStateTargetSize) so the layout is resolution-independent.
// ---------------------------------------------------------------------------
typedef struct {
    const char *text;
    float       sizeFrac;       // font size as fraction of game HEIGHT
    Vector2     posFrac;        // anchor: x = CENTER of the text, y = TOP edge,
                                //   both as fractions of game size
    Color       color;
    const AnimPhase *intro; int introCount;   // TP_* rows played on scene enter
    const AnimPhase *outro; int outroCount;   // TP_* rows played on scene exit
} AnimText;

// ---------------------------------------------------------------------------
//  ZoomBoxes: the looping zooming rectangles. The instance lives in the SCENE
//  (one clock shared by the scene's ambient loop and the outro player), so the
//  outro starts exactly where the scene just drew them - no snap at hand-off.
// ---------------------------------------------------------------------------
#define ZOOM_BOX_MAX 8
typedef struct {
    int   count;                    // how many boxes (e.g. 3)
    float period;                   // seconds for one box to grow 0 -> 1
    float clock;                    // advanced by ZoomBoxesUpdate every frame
    float captured[ZOOM_BOX_MAX];   // per-box phase snapshot taken at outro start
} ZoomBoxes;

void ZoomBoxesUpdate(ZoomBoxes *b, float dt);
void ZoomBoxesDrawLoop(const ZoomBoxes *b);   // ambient staggered loop (scene idle)
// Outro draw. Two settle variants (a scene defines ONE of the two rows):
//   `settle`     (SP_ZOOM_SETTLE)      - boxes converge onto one shared phase
//                                        (gaps shrink to zero, full overlay)
//   `settleGap1` (SP_ZOOM_SETTLE_GAP1) - boxes converge but keep a predefined
//                                        gap between each rectangle
// then `reverse` (SP_ZOOM_REVERSE) zooms the settled stack back inward
// together (a kept gap stays kept). Amounts are the eased phase progress;
// alpha fades the lines.
void ZoomBoxesDrawOutro(const ZoomBoxes *b, float settle, float settleGap1,
                        float reverse, float alpha);

// ---------------------------------------------------------------------------
//  SceneAnim: one per scene - everything the intro player and the outro
//  player need. All pointers reference the scene's own static data.
// ---------------------------------------------------------------------------
typedef struct {
    AnimText  *texts;       int textCount;
    const AnimPhase *introGlobal; int introGlobalCount;  // GP_* rows (enter)
    const AnimPhase *outroGlobal; int outroGlobalCount;  // GP_* rows (exit)
    const AnimPhase *outroShape;  int outroShapeCount;   // SP_* rows (exit)
    ZoomBoxes *boxes;                                    // NULL = no zoom boxes
    // Scene decor that isn't text or boxes (bars/lines/circles). `alpha` is
    // 1 while the scene is live and 1-GP_ART_FADE during the outro; `time`
    // keeps the scene's own clock running so idle motion doesn't snap.
    void (*drawArt)(float alpha, float time);
} SceneAnim;

// ---------------------------------------------------------------------------
//  Player. Treat the fields as internal - they are only public so states can
//  hold a player by value (file-scope static, no allocation).
// ---------------------------------------------------------------------------
typedef enum { ANIM_INTRO, ANIM_OUTRO } AnimDir;

#define ANIM_TEXT_MAX   8       // max texts per scene
#define ANIM_LETTER_MAX 64      // glyph-particle pool shared by all texts

typedef struct {                // one glyph of a crumbling text
    int     codepoint;
    Vector2 pos, vel;           // integrated during TP_CRUMBLE (top-left, px/s)
    float   rot, rotVel;        // degrees, deg/s
    int     seeded;             // crumble state initialized?
} AnimLetter;

typedef struct {                // one text's glyph-particle range in letters[]
    int letterStart, letterCount;
} AnimTextGlyphs;

typedef struct {
    const SceneAnim *spec;
    AnimDir        dir;
    float          t;           // play clock, seconds since Start
    AnimTextGlyphs glyphs[ANIM_TEXT_MAX];
    AnimLetter     letters[ANIM_LETTER_MAX];
    int            letterTotal;
} SceneAnimPlayer;

void  SceneAnimStart(SceneAnimPlayer *p, const SceneAnim *spec, AnimDir dir);
void  SceneAnimUpdate(SceneAnimPlayer *p, float dt);  // clock + crumble physics
bool  SceneAnimDone(const SceneAnimPlayer *p);        // t past the latest .end
void  SceneAnimDrawTexts(SceneAnimPlayer *p);         // every text, animated pose
// Eased 0..1 of a GP_* / SP_* row for the player's direction (rest if absent).
float SceneAnimGlobalAmount(const SceneAnimPlayer *p, int kind);
float SceneAnimShapeAmount(const SceneAnimPlayer *p, int kind);

#endif // SCENE_ANIM_H
