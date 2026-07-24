// *** DEPRECATED ***  This procedural scene-animation system (ZoomBoxes /
// crumbling text) is superseded by the editor-authored anim/ pipeline
// (anim.*, anim_stage.*, anim_signal.*). It survives only for strategy_test /
// platformer_test and SHOULD BE REMOVED in a future version.

// ============================================================================
//  scene_anim.c  -  playback for the data defined in scene_anim.h
//
//  Everything here reads phase tables through AnimPhaseAmount - there is not a
//  single raw `t < X` comparison in the file. The scene's data decides WHAT
//  happens WHEN and with WHICH easing; this file only knows HOW each effect is
//  drawn (slide math, crumble physics, zoom-box convergence).
//
//  Text metrics: glyphs are laid out once at SceneAnimStart with the same
//  advance formula DrawTextEx uses (advanceX*scale + spacing), so the rigid
//  DrawTextEx draw and the per-glyph crumble draw land on identical pixels.
// ============================================================================

#include "scene_anim.h"
#include "../../screen_state/screen_state.h"
#include <math.h>
#include <stddef.h>

// --- crumble tuning knobs (fractions of game size -> resolution-independent)
#define GRAVITY_FRAC        1.25f   // gravity, fraction of game height per s^2
#define LETTER_VY0_FRAC   (-0.08f)  // initial upward pop, game height/s
#define LETTER_VX_JIT_FRAC  0.20f   // +/- horizontal scatter, game width/s
#define LETTER_SPIN_JIT     260.0f  // +/- deg/s angular velocity

// --- zoom-box outro shape ----------------------------------------------------
#define ZOOM_GROUP_PHASE      0.85f  // shared phase the boxes converge on (settle)
#define ZOOM_REVERSE_PHASE    0.05f  // phase the grouped stack reverses back to
#define ZOOM_SETTLE_GAP1_FRAC 0.10f  // SP_ZOOM_SETTLE_GAP1: kept phase gap between
                                     //   successive boxes (box i sits i*gap deeper)

// ---------------------------------------------------------------------------
//  Phase table access (order-independent: rows are found by kind).
// ---------------------------------------------------------------------------
const AnimPhase *AnimPhaseFind(const AnimPhase *phases, int count, int kind)
{
    for (int i = 0; i < count; i++)
        if (phases && phases[i].kind == kind) return &phases[i];
    return NULL;
}

float AnimPhaseAmount(const AnimPhase *phases, int count, int kind,
                      float t, float missing)
{
    const AnimPhase *ph = AnimPhaseFind(phases, count, kind);
    if (!ph)             return missing;
    if (t <= ph->start)  return 0.0f;
    if (t >= ph->end)    return 1.0f;
    float p = (t - ph->start) / (ph->end - ph->start);
    return ph->ease ? ph->ease(p) : p;
}

// Latest .end in a table (0 for an empty/NULL table).
static float TableDuration(const AnimPhase *phases, int count)
{
    float d = 0.0f;
    for (int i = 0; i < count; i++)
        if (phases && phases[i].end > d) d = phases[i].end;
    return d;
}

// deterministic pseudo-random in [-1,1] from an int seed (repeatable scatter).
static float Rand11(int seed)
{
    float s = sinf((float)seed * 12.9898f) * 43758.5455f;
    return 2.0f * (s - floorf(s)) - 1.0f;
}

// The text's font size / DrawTextEx spacing for the current game canvas.
static float TextSizePx(const AnimText *at, Vector2 game)
{
    return fmaxf(1.0f, game.y * at->sizeFrac);
}
static float TextSpacing(float sizePx)
{
    return sizePx / (float)GetFontDefault().baseSize;
}

// The per-text phase table for the player's direction.
static void TextTable(const SceneAnimPlayer *p, const AnimText *at,
                      const AnimPhase **phases, int *count)
{
    *phases = (p->dir == ANIM_INTRO) ? at->intro      : at->outro;
    *count  = (p->dir == ANIM_INTRO) ? at->introCount : at->outroCount;
}

// ---------------------------------------------------------------------------
//  Text layout. Computed FRESH from the current game size every time it is
//  needed (never cached across frames): the render target can change size
//  after the player starts, and stale pixel positions would drift off-center.
//  This matches the old menu, which re-derived its layout in Draw each frame.
// ---------------------------------------------------------------------------
typedef struct {
    float startLeft;            // slide start (== restLeft when already on-screen)
    float exitLeft;             // TP_SLIDE_OUT target: nearest off-screen edge
    float restLeft, restTop;    // rest pose top-left
    float width, height;        // measured text box
} AnimTextPose;

static AnimTextPose TextPose(const SceneAnimPlayer *p, int i, Vector2 game)
{
    const AnimText *at = &p->spec->texts[i];
    AnimTextPose ps;

    float sizePx  = TextSizePx(at, game);
    float spacing = TextSpacing(sizePx);

    Vector2 box = MeasureTextEx(GetFontDefault(), at->text, sizePx, spacing);
    ps.width    = box.x;
    ps.height   = sizePx;                                // one line
    ps.restLeft = game.x*at->posFrac.x - box.x*0.5f;     // posFrac.x = center
    ps.restTop  = game.y*at->posFrac.y;                  // posFrac.y = top

    // TP_SLIDE_IN start edge: if the rest pose is visible AND we're an OUTRO,
    // the text is on-screen right now - animate FROM where it is (no motion).
    // Otherwise (off-screen, or an intro where the text has not been shown
    // yet) slide in from the nearest screen edge.
    bool onScreen = (ps.restLeft + ps.width > 0.0f) && (ps.restLeft < game.x);
    float nearEdge = (ps.restLeft + ps.width*0.5f <= game.x*0.5f)
                         ? -ps.width         // nearest edge is the left one
                         : game.x;           // nearest edge is the right one
    if (onScreen && p->dir == ANIM_OUTRO)
        ps.startLeft = ps.restLeft;
    else
        ps.startLeft = nearEdge;
    ps.exitLeft = nearEdge;   // TP_SLIDE_OUT always leaves via the nearest edge
    return ps;
}

// ---------------------------------------------------------------------------
//  Rigid pose: the shared (dx, dy) offset from the rest pose at time t, i.e.
//  the combined TP_SLIDE_IN / TP_CENTER_X / TP_CENTER_Y motion. Used by both
//  Draw (to place the text) and Update (to seed crumble from the live pose).
//  Missing-row defaults all mean "sit at rest": slide missing -> 1 (arrived),
//  center missing -> 0 (never leaves rest).
// ---------------------------------------------------------------------------
static Vector2 RigidOffset(const SceneAnimPlayer *p, int i,
                           const AnimTextPose *ps, Vector2 game)
{
    const AnimPhase *tab; int n;
    TextTable(p, &p->spec->texts[i], &tab, &n);

    float slide    = AnimPhaseAmount(tab, n, TP_SLIDE_IN,  p->t, 1.0f);
    float slideOut = AnimPhaseAmount(tab, n, TP_SLIDE_OUT, p->t, 0.0f);
    float cxA      = AnimPhaseAmount(tab, n, TP_CENTER_X,  p->t, 0.0f);
    float cyA      = AnimPhaseAmount(tab, n, TP_CENTER_Y,  p->t, 0.0f);

    Vector2 off;
    off.x = (1.0f - slide) * (ps->startLeft - ps->restLeft)
          + slideOut * (ps->exitLeft - ps->restLeft)
          + cxA * (game.x*0.5f - (ps->restLeft + ps->width*0.5f));
    off.y = cyA * (game.y*0.5f - (ps->restTop + ps->height*0.5f));
    return off;
}

// Combined visibility of a text at time t (fade-in * fade-out * crumble fade).
static float TextAlpha(const SceneAnimPlayer *p, int i)
{
    const AnimText *at = &p->spec->texts[i];
    const AnimPhase *tab; int n;
    TextTable(p, at, &tab, &n);

    float in      = AnimPhaseAmount(tab, n, TP_FADE_IN,  p->t, 1.0f);
    float out     = AnimPhaseAmount(tab, n, TP_FADE_OUT, p->t, 0.0f);
    float crumble = AnimPhaseAmount(tab, n, TP_CRUMBLE,  p->t, 0.0f);
    return in * (1.0f - out) * (1.0f - crumble);
}

// ---------------------------------------------------------------------------
//  Start: claim a glyph-pool range per text (crumble particles) and (outro)
//  snapshot the zoom boxes' live phases. Positions are NOT computed here -
//  see TextPose above for why layout is always derived fresh.
// ---------------------------------------------------------------------------
void SceneAnimStart(SceneAnimPlayer *p, const SceneAnim *spec, AnimDir dir)
{
    p->spec = spec;
    p->dir  = dir;
    p->t    = 0.0f;
    p->letterTotal = 0;

    int textCount = spec->textCount;
    if (textCount > ANIM_TEXT_MAX) textCount = ANIM_TEXT_MAX;

    for (int i = 0; i < textCount; i++)
    {
        p->glyphs[i].letterStart = p->letterTotal;
        for (const char *c = spec->texts[i].text; *c; c++)
        {
            int cp = (unsigned char)*c;
            if (cp != ' ' && p->letterTotal < ANIM_LETTER_MAX)
            {
                AnimLetter *L = &p->letters[p->letterTotal++];
                L->codepoint = cp;
                L->seeded    = 0;
            }
        }
        p->glyphs[i].letterCount = p->letterTotal - p->glyphs[i].letterStart;
    }

    // Outro: freeze the boxes' CURRENT loop phases so the settle arc starts
    // exactly where the scene just drew them (continuity at hand-off).
    if (dir == ANIM_OUTRO && spec->boxes)
    {
        ZoomBoxes *b = spec->boxes;
        for (int i = 0; i < b->count && i < ZOOM_BOX_MAX; i++)
            b->captured[i] = fmodf(b->clock/b->period + (float)i/(float)b->count, 1.0f);
    }
}

// ---------------------------------------------------------------------------
//  Update: advance the clock; once a text's TP_CRUMBLE begins, seed its
//  glyphs from the CURRENT rigid pose and integrate gravity/spin.
// ---------------------------------------------------------------------------
void SceneAnimUpdate(SceneAnimPlayer *p, float dt)
{
    if (!p->spec) return;
    p->t += dt;

    Vector2 game    = ScreenStateTargetSize();
    float   gravity = game.y * GRAVITY_FRAC;

    int textCount = p->spec->textCount;
    if (textCount > ANIM_TEXT_MAX) textCount = ANIM_TEXT_MAX;

    for (int i = 0; i < textCount; i++)
    {
        const AnimText *at = &p->spec->texts[i];
        const AnimPhase *tab; int n;
        TextTable(p, at, &tab, &n);
        if (AnimPhaseAmount(tab, n, TP_CRUMBLE, p->t, 0.0f) <= 0.0f) continue;

        AnimTextPose ps  = TextPose(p, i, game);
        Vector2      off = RigidOffset(p, i, &ps, game);  // pose the instant it broke

        // Seed each glyph from its live rigid position: pen-walk the string
        // with the exact DrawTextEx advance so glyphs break from where the
        // rigid draw last showed them.
        float sizePx  = TextSizePx(at, game);
        float spacing = TextSpacing(sizePx);
        float scale   = sizePx / (float)GetFontDefault().baseSize;
        float penX    = ps.restLeft;
        int   k       = 0;

        for (const char *c = at->text; *c; c++)
        {
            int cp = (unsigned char)*c;
            GlyphInfo gi = GetGlyphInfo(GetFontDefault(), cp);
            float advance = (gi.advanceX == 0 ? gi.image.width : gi.advanceX) * scale
                          + spacing;
            if (cp != ' ' && k < p->glyphs[i].letterCount)
            {
                AnimLetter *L = &p->letters[p->glyphs[i].letterStart + k];
                if (!L->seeded)
                {
                    int seed  = p->glyphs[i].letterStart + k;  // unique across texts
                    L->pos    = (Vector2){ penX + off.x, ps.restTop + off.y };
                    L->vel    = (Vector2){ Rand11(seed*7 + 1) * game.x * LETTER_VX_JIT_FRAC,
                                           game.y * LETTER_VY0_FRAC };
                    L->rot    = 0.0f;
                    L->rotVel = Rand11(seed*13 + 3) * LETTER_SPIN_JIT;
                    L->seeded = 1;
                }
                L->vel.y += gravity * dt;
                L->pos.x += L->vel.x * dt;
                L->pos.y += L->vel.y * dt;
                L->rot   += L->rotVel * dt;
                k++;
            }
            penX += advance;
        }
    }
}

// Done once the clock passes the latest .end across every table of this
// direction (texts + global + shapes).
bool SceneAnimDone(const SceneAnimPlayer *p)
{
    if (!p->spec) return true;
    const SceneAnim *s = p->spec;

    float d = 0.0f, td;
    if (p->dir == ANIM_INTRO)
        d = TableDuration(s->introGlobal, s->introGlobalCount);
    else
    {
        d = TableDuration(s->outroGlobal, s->outroGlobalCount);
        td = TableDuration(s->outroShape, s->outroShapeCount);
        if (td > d) d = td;
    }
    for (int i = 0; i < s->textCount; i++)
    {
        const AnimText *at = &s->texts[i];
        const AnimPhase *tab; int n;
        TextTable(p, at, &tab, &n);
        td = TableDuration(tab, n);
        if (td > d) d = td;
    }
    return p->t >= d;
}

float SceneAnimGlobalAmount(const SceneAnimPlayer *p, int kind)
{
    if (!p->spec) return 0.0f;
    const AnimPhase *tab = (p->dir == ANIM_INTRO) ? p->spec->introGlobal
                                                  : p->spec->outroGlobal;
    int n = (p->dir == ANIM_INTRO) ? p->spec->introGlobalCount
                                   : p->spec->outroGlobalCount;
    // Missing GP_UNFADE_BLACK must mean "no black overlay" (1 = fully unfaded);
    // every other global effect rests at 0.
    float missing = (kind == GP_UNFADE_BLACK) ? 1.0f : 0.0f;
    return AnimPhaseAmount(tab, n, kind, p->t, missing);
}

float SceneAnimShapeAmount(const SceneAnimPlayer *p, int kind)
{
    if (!p->spec) return 0.0f;
    return AnimPhaseAmount(p->spec->outroShape, p->spec->outroShapeCount,
                           kind, p->t, 0.0f);
}

// ---------------------------------------------------------------------------
//  Draw: every text at its animated pose. Rigid texts use one DrawTextEx;
//  a crumbling text switches to its per-glyph particles (DrawTextPro spins
//  each glyph about its own center).
// ---------------------------------------------------------------------------
void SceneAnimDrawTexts(SceneAnimPlayer *p)
{
    if (!p->spec) return;
    Vector2 game = ScreenStateTargetSize();
    Font    font = GetFontDefault();

    int textCount = p->spec->textCount;
    if (textCount > ANIM_TEXT_MAX) textCount = ANIM_TEXT_MAX;

    for (int i = 0; i < textCount; i++)
    {
        const AnimText *at = &p->spec->texts[i];

        float alpha = TextAlpha(p, i);
        if (alpha <= 0.0f) continue;

        AnimTextPose ps = TextPose(p, i, game);

        float sizePx  = TextSizePx(at, game);
        float spacing = TextSpacing(sizePx);
        Color col     = Fade(at->color, alpha);

        // seeded == the crumble has begun for this text (set in Update)
        bool crumbling = p->glyphs[i].letterCount > 0
                      && p->letters[p->glyphs[i].letterStart].seeded;

        if (!crumbling)
        {
            Vector2 off = RigidOffset(p, i, &ps, game);
            DrawTextEx(font, at->text,
                       (Vector2){ ps.restLeft + off.x, ps.restTop + off.y },
                       sizePx, spacing, col);
        }
        else
        {
            for (int k = 0; k < p->glyphs[i].letterCount; k++)
            {
                const AnimLetter *L = &p->letters[p->glyphs[i].letterStart + k];
                char buf[2] = { (char)L->codepoint, 0 };
                Vector2 gsz    = MeasureTextEx(font, buf, sizePx, spacing);
                Vector2 origin = { gsz.x*0.5f, gsz.y*0.5f };  // spin about center
                Vector2 center = { L->pos.x + origin.x, L->pos.y + origin.y };
                DrawTextPro(font, buf, center, origin, L->rot, sizePx, spacing, col);
            }
        }
    }
}

// ---------------------------------------------------------------------------
//  Zoom boxes.
// ---------------------------------------------------------------------------
void ZoomBoxesUpdate(ZoomBoxes *b, float dt)
{
    b->clock += dt;
}

// One box outline at loop phase `t` (0 = tiny center, 1 = screen-sized+gone).
static void DrawZoomBox(Vector2 game, float t, float alpha)
{
    if (t < 0.05f || t >= 1.0f) return;   // matches the small DARKGRAY seed box
    float W = game.x * t;
    float H = game.y * t;
    unsigned char a = (unsigned char)(255.0f * (1.0f - t) * alpha);
    Color c = { 130, 150, 180, a };
    DrawRectangleLines((int)((game.x - W)*0.5f), (int)((game.y - H)*0.5f),
                       (int)W, (int)H, c);
}

void ZoomBoxesDrawLoop(const ZoomBoxes *b)
{
    Vector2 game = ScreenStateTargetSize();
    float base = b->clock / b->period;
    for (int i = 0; i < b->count && i < ZOOM_BOX_MAX; i++)
        DrawZoomBox(game, fmodf(base + (float)i/(float)b->count, 1.0f), 1.0f);
}

void ZoomBoxesDrawOutro(const ZoomBoxes *b, float settle, float settleGap1,
                        float reverse, float alpha)
{
    Vector2 game = ScreenStateTargetSize();
    for (int i = 0; i < b->count && i < ZOOM_BOX_MAX; i++)
    {
        // Settle: each box lerps from its captured live phase toward its
        // target - at settle=0 it is exactly where the scene loop drew it.
        //   SP_ZOOM_SETTLE      -> everyone shares ZOOM_GROUP_PHASE (overlay)
        //   SP_ZOOM_SETTLE_GAP1 -> box i stops i*gap short, keeping the gap
        // (a scene defines one of the two rows; the other amount stays 0)
        float phase = b->captured[i];
        phase += settle     * (ZOOM_GROUP_PHASE - b->captured[i]);
        phase += settleGap1 * ((ZOOM_GROUP_PHASE - (float)i*ZOOM_SETTLE_GAP1_FRAC)
                               - b->captured[i]);
        // Reverse: the settled stack zooms back inward together; each box
        // keeps whatever offset it settled at, so a kept gap stays kept.
        phase += reverse * (ZOOM_REVERSE_PHASE - ZOOM_GROUP_PHASE);
        DrawZoomBox(game, phase, alpha);
    }
}
