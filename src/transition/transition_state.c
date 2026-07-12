// ============================================================================
//  transition_state.c  -  MAIN MENU -> GAME transition animation
//
//  Played after PLAY/ENTER in the main menu, BEFORE the platformer starts.
//
//  The transition is DATA-DRIVEN: every beat is a row in the `phases[]` table
//  below (name, start, end, easing). The code never compares raw `t` values -
//  it asks PhaseAmount(PH_x) for that beat's eased 0..1 progress. To re-time the
//  whole thing, edit the table; to reuse it for another transition, copy the
//  table. Beats, in order:
//    CENTER  : the whole "MAIN MENU" word SLIDES in to its centered pose (rigid).
//    HOLD    : it rests centered for a beat.
//    CRUMBLE : each letter falls under gravity, spins (DrawTextPro), fades.
//    ZOOM    : the menu's zoom boxes settle (ease-out) then reverse in (ease-in).
//    ARTFADE : the rest of the menu art (sub-text, bar, lines, circle) fades.
//    BLACK   : the whole screen fades to black (fast); at its end we hand off.
//
//  The word's FINAL centered pose is computed to exactly match main_menu.c's
//  title (same font size fraction, same y, same MeasureText centering) so there
//  is NO snap at the seam when PLAY is pressed - it eases in from an offset and
//  lands precisely where the live menu had it.
//
//  State-machine note: AppStateTransition runs exit()->enter() synchronously and
//  there is NO deferred next-state queue. So this state owns its own clock and
//  calls AppStateTransition itself from Update() once BLACK completes.
//
//  Draw phases (see main.c): Draw() = game render-texture (scaled to window);
//  Gui() = real screen pixels on top. We do everything in Draw() (game space) so
//  it lines up with where the menu drew its title/art, and black covers it all.
// ============================================================================

#include "raylib.h"
#include "../app_state/app_state.h"
#include "../screen_state/screen_state.h"
#include "easing.h"    // single translation unit -> safe. easing.h has NO include
                       // guard and inline defs, so including it in a 2nd .c would
                       // cause duplicate-symbol link errors. Keep it only here.
#include <math.h>
#include <stddef.h>

// ---------------------------------------------------------------------------
//  PHASE TABLE - the "definable & repeatable" heart of the transition.
//  Each phase runs from `start` to `end` seconds and eases 0->1 by `ease`.
//  Overlaps are fine (e.g. BLACK starts before CRUMBLE fully ends).
// ---------------------------------------------------------------------------
typedef float (*EaseFn)(float);

typedef enum {
    PH_CENTER = 0,          // word slides in horizontally to its centered X
    PH_CENTER_VERTICAL,     // word rises/drops from rest Y to the vertical center
    PH_HOLD,                // rests centered
    PH_CRUMBLE,             // letters fall/spin/fade
    PH_ZOOM,                // zoom boxes settle then reverse
    PH_ARTFADE,             // non-title menu art fades out
    PH_BLACK,               // screen fades to black
    PH_COUNT
} PhaseId;

typedef struct {
    PhaseId id;
    float   start;   // seconds
    float   end;     // seconds
    EaseFn  ease;    // maps linear 0..1 -> eased 0..1 (from easing.h)
} Phase;

// Edit THIS to re-time the transition. Rows may appear in any order; they are
// indexed by .id (see PhaseAmount). Total length = the largest .end below.
// NOTE: rows are indexed by .id (PhaseAmount does &phases[id]), so keep them in
// the same order as the PhaseId enum.
static const Phase phases[] = {
    { PH_CENTER,          0.00f, 0.55f, sineEaseOutf },      // slide in horizontally
    { PH_CENTER_VERTICAL, 0.10f, 0.75f, sineEaseInOutf },    // move to vertical center
    { PH_HOLD,            0.75f, 1.05f, linearInterpolationf },
    { PH_CRUMBLE,         1.05f, 2.15f, cubicEaseInf },      // fall accelerates
    { PH_ZOOM,            0.00f, 2.45f, linearInterpolationf }, // shape in Draw (two sub-arcs)
    { PH_ARTFADE,         0.15f, 1.10f, linearInterpolationf },
    { PH_BLACK,           1.90f, 2.55f, linearInterpolationf }, // last .end -> hand off here
};

// --- other tuning knobs -----------------------------------------------------
// (Horizontal slide has no knob: it respects the text's original position and
//  only slides in from the left edge when the original word is off-screen.)
#define GRAVITY_FRAC        1.25f   // gravity as a fraction of game height per s^2
#define LETTER_VY0_FRAC   (-0.08f)  // initial upward pop, fraction of game height/s
#define LETTER_VX_JIT_FRAC  0.20f   // +/- horizontal scatter, fraction of game width/s
#define LETTER_SPIN_JIT     260.0f  // +/- deg/s angular velocity

// Title metrics - MUST mirror main_menu.c Draw() so the centered pose matches.
#define TITLE_SIZE_FRAC     0.083f  // title font size as fraction of game height
#define TITLE_Y_FRAC        0.11f   // title top-left y as fraction of game height

// --- letter particles -------------------------------------------------------
#define MAX_LETTERS 16
typedef struct {
    int     codepoint;   // glyph to draw (0 = unused slot)
    float   restX;       // resting top-left x (centered pose), game space
    float   restY;       // resting top-left y
    // crumble state (seeded when CRUMBLE begins), integrated in Update():
    Vector2 pos;         // current top-left during crumble
    Vector2 vel;         // px/s
    float   rot;         // degrees
    float   rotVel;      // deg/s
    int     seeded;      // crumble velocities initialized?
} Letter;

static const char *TITLE = "MAIN MENU";

static float   t = 0.0f;               // clock: seconds since Enter()
static Letter  letters[MAX_LETTERS];
static int     letterCount = 0;

// Forward declares (same pattern as the other states).
static void Enter();
static void Exit();
static void Update();
static void Draw();
static void Gui();

                            /* Enter, Exit, Update, Draw, Gui, "Name" */
AppState app_state_transition = {Enter, Exit, Update, Draw, Gui, "Transition"};

// ---------------------------------------------------------------------------
//  PhaseAmount: eased 0..1 progress of a phase at the current clock `t`.
//  0 before it starts, 1 after it ends, eased in between. This is the ONLY way
//  the rest of the code reads timing - no raw `t < X` comparisons anywhere.
// ---------------------------------------------------------------------------
static float PhaseAmount(PhaseId id)
{
    const Phase *ph = &phases[id];   // table is indexed by id (see note above)
    if (t <= ph->start) return 0.0f;
    if (t >= ph->end)   return 1.0f;
    float p = (t - ph->start) / (ph->end - ph->start);
    return ph->ease ? ph->ease(p) : p;
}

// Total transition length = the latest phase end in the table.
static float TransitionDuration()
{
    float d = 0.0f;
    for (int i = 0; i < (int)(sizeof(phases)/sizeof(phases[0])); i++)
        if (phases[i].end > d) d = phases[i].end;
    return d;
}

// The menu's zoom box (from main_menu.c). `alpha` lets us fade it over the beats.
static void DrawZoomBox(float cx, float cy, Vector2 size, float boxT, float alpha)
{
    if (boxT < 0.05f) return;
    float W = size.x * boxT;
    float H = size.y * boxT;
    unsigned char a = (unsigned char)(255.0f * (1.0f - boxT) * alpha);
    Color c = { 130, 150, 180, a };
    DrawRectangleLines((int)(cx - W*0.5f), (int)(cy - H*0.5f), (int)W, (int)H, c);
}

// deterministic pseudo-random in [-1,1] from an int seed (repeatable scatter).
static float Rand11(int seed)
{
    float s = sinf((float)seed * 12.9898f) * 43758.5455f;
    return 2.0f * (s - floorf(s)) - 1.0f;
}

// Compute the RESTING (centered) top-left of each glyph, matching main_menu.c
// exactly: font size = game_h*TITLE_SIZE_FRAC, top y = game_h*TITLE_Y_FRAC, and
// horizontally centered by MeasureText width. Fills letters[]/letterCount.
static void LayoutRestingTitle(Vector2 game_size)
{
    Font  font   = GetFontDefault();
    float titleSize = fmaxf(1.0f, game_size.y * TITLE_SIZE_FRAC);
    float spacing   = titleSize / (float)font.baseSize;   // default-font spacing
    float scale     = titleSize / (float)font.baseSize;

    float titleWidth = (float)MeasureText(TITLE, (int)titleSize);
    float penX = game_size.x*0.5f - titleWidth*0.5f;      // menu's centered left edge
    float topY = game_size.y * TITLE_Y_FRAC;

    letterCount = 0;
    for (const char *p = TITLE; *p; p++)
    {
        int cp = (unsigned char)*p;
        GlyphInfo gi = GetGlyphInfo(font, cp);
        float advance = (gi.advanceX == 0 ? gi.image.width : gi.advanceX) * scale + spacing;

        if (cp != ' ' && letterCount < MAX_LETTERS)
        {
            Letter *L = &letters[letterCount++];
            L->codepoint = cp;
            L->restX = penX;
            L->restY = topY;
            L->seeded = 0;
        }
        penX += advance;
    }
}

// ----------------------------------------------------------------------------
static void Enter()
{
    t = 0.0f;
    LayoutRestingTitle(ScreenStateTargetSize());
}

static void Exit() { /* nothing loaded */ }

// ----------------------------------------------------------------------------
//  Update: advance clock; once CRUMBLE has begun, seed + integrate particles.
// ----------------------------------------------------------------------------
static void Update()
{
    float dt = GetFrameTime();
    t += dt;

    Vector2 game = ScreenStateTargetSize();
    float crumble = PhaseAmount(PH_CRUMBLE);

    if (crumble > 0.0f)
    {
        float gravity = game.y * GRAVITY_FRAC;
        // By crumble time the vertical-center move is complete; seed from that
        // pose (the same vertDY the Draw uses at vertAmt=1) so letters fall from
        // where they visually rest, not from the original top.
        float titleSize = fmaxf(1.0f, game.y * TITLE_SIZE_FRAC);
        float restTopY  = letterCount ? letters[0].restY : game.y*TITLE_Y_FRAC;
        float vertDY    = game.y*0.5f - (restTopY + titleSize*0.5f);
        for (int i = 0; i < letterCount; i++)
        {
            Letter *L = &letters[i];
            if (!L->seeded)
            {
                // start crumbling from the resting, vertically-centered pose
                L->pos    = (Vector2){ L->restX, L->restY + vertDY };
                L->vel    = (Vector2){ Rand11(i*7 + 1) * game.x * LETTER_VX_JIT_FRAC,
                                       game.y * LETTER_VY0_FRAC };
                L->rot    = 0.0f;
                L->rotVel = Rand11(i*13 + 3) * LETTER_SPIN_JIT;
                L->seeded = 1;
            }
            L->vel.y += gravity * dt;
            L->pos.x += L->vel.x * dt;
            L->pos.y += L->vel.y * dt;
            L->rot   += L->rotVel * dt;
        }
    }

    if (t >= TransitionDuration())
        AppStateTransition(&app_state_platformer);   // hand off to the game
}

// ----------------------------------------------------------------------------
//  Draw: GAME SPACE. Menu art (fading), sliding/crumbling title, fade to black.
// ----------------------------------------------------------------------------
static void Draw()
{
    Vector2 size = ScreenStateTargetSize();
    float cx = size.x * 0.5f;
    float cy = size.y * 0.5f;
    Font  font = GetFontDefault();

    // ---- menu art fade (everything but the title) --------------------------
    float artAlpha = 1.0f - PhaseAmount(PH_ARTFADE);
    if (artAlpha > 0.0f)
    {
        // Mirror main_menu.c's resolution-independent art so there is no snap.
        int descrSize = (int)fmaxf(1.0f, size.y*0.028f);
        const char *descr = "place of all the buttons";
        int descrWidth = MeasureText(descr, descrSize);
        DrawText(descr, (int)(cx - descrWidth*0.5f), (int)(size.y*0.19f), descrSize,
                 Fade(RAYWHITE, artAlpha));

        float bar_w = size.x*0.31f;
        int   bar_h = (int)fmaxf(1.0f, size.y*0.006f);
        DrawRectangle((int)(cx - bar_w*0.5f), (int)(size.y*0.24f), (int)bar_w, bar_h,
                      Fade(SKYBLUE, artAlpha));

        DrawLine(0, 0, (int)size.x, (int)size.y, Fade((Color){60,70,90,255}, artAlpha));
        DrawLine((int)size.x, 0, 0, (int)size.y, Fade((Color){60,70,90,255}, artAlpha));

        float bob   = sinf(t*2.0f) * size.y*0.056f;
        float bob_y = cy + size.y*0.17f + bob;
        float bob_r = size.y*0.042f;
        DrawCircle((int)cx, (int)bob_y, bob_r, Fade(ORANGE, artAlpha));
        DrawCircleLines((int)cx, (int)bob_y, bob_r, Fade(RAYWHITE, artAlpha));
    }

    // ---- zoom boxes: settle (ease-out) then reverse inward (ease-in) --------
    // PH_ZOOM linear-progresses 0..1 across the whole zoom window; we split it
    // into two arcs here (settle for the first ~55%, reverse for the rest).
    float z = PhaseAmount(PH_ZOOM);
    float boxT;
    if (z < 0.55f) boxT = 0.45f + (0.85f - 0.45f) * sineEaseOutf(z / 0.55f);
    else           boxT = 0.85f + (0.05f - 0.85f) * cubicEaseInf((z - 0.55f) / 0.45f);

    float boxAlpha = 0.35f + 0.65f * artAlpha;
    for (int i = 0; i < 3; i++)
    {
        float bt = boxT - (float)i * 0.12f;     // stagger inward
        if (bt > 0.0f) DrawZoomBox(cx, cy, size, bt, boxAlpha);
    }

    // ---- title: SLIDE in to center (rigid), then CRUMBLE per-letter ---------
    float titleSize = fmaxf(1.0f, size.y * TITLE_SIZE_FRAC);
    float spacing   = titleSize / (float)font.baseSize;

    // ---- horizontal slide -------------------------------------------------
    // Respect the text's ORIGINAL position: the word slides from where it
    // actually is (the menu's centered X) to its resting X - so when it's already
    // on-screen there is NO horizontal motion. Only if the original word is fully
    // off-screen (left of 0 or past the right edge) do we slide it in from the
    // left edge instead.
    float centerAmt = PhaseAmount(PH_CENTER);              // 0..1 horizontal slide
    float restLeft  = letterCount ? letters[0].restX : cx; // word's resting left edge
    float wordWidth = (float)MeasureText(TITLE, (int)titleSize);
    float restRight = restLeft + wordWidth;

    float startLeft = restLeft;                            // on-screen: don't move
    if (restRight < 0.0f || restLeft > size.x)             // original is off-screen
        startLeft = -wordWidth;                            // -> come in from the left edge

    float slideDX = (1.0f - centerAmt) * (startLeft - restLeft); // 0 at rest

    // Vertical move: rest (top y = restY) -> glyph CENTER at the screen mid.
    // As a shared top-left delta = (target center y) - (rest center y). Uses the
    // title height (all glyphs share it) so the word moves rigidly.
    float vertAmt   = PhaseAmount(PH_CENTER_VERTICAL);     // 0..1 vertical progress
    float restTopY  = letterCount ? letters[0].restY : size.y*TITLE_Y_FRAC;
    float vertDY    = vertAmt * (size.y*0.5f - (restTopY + titleSize*0.5f));

    float crumble   = PhaseAmount(PH_CRUMBLE);
    float titleAlpha = 1.0f - crumble;                     // fades as it crumbles

    if (titleAlpha > 0.0f)
    {
        for (int i = 0; i < letterCount; i++)
        {
            Letter *L = &letters[i];
            char buf[2] = { (char)L->codepoint, 0 };
            Vector2 gsz = MeasureTextEx(font, buf, titleSize, spacing);
            Vector2 origin = { gsz.x*0.5f, gsz.y*0.5f };   // spin about glyph center

            // Before crumble: rigid word at rest + shared horizontal & vertical
            // offsets. During crumble: per-letter integrated pos/rot (from Update).
            float topX = L->seeded ? L->pos.x : L->restX + slideDX;
            float topY = L->seeded ? L->pos.y : L->restY + vertDY;
            float rot  = L->seeded ? L->rot  : 0.0f;

            Vector2 posCenter = { topX + origin.x, topY + origin.y };
            DrawTextPro(font, buf, posCenter, origin, rot, titleSize, spacing,
                        Fade(RAYWHITE, titleAlpha));
        }
    }

    // ---- fade to black (fast); full at hand-off ----------------------------
    float black = PhaseAmount(PH_BLACK);
    if (black > 0.0f)
        DrawRectangle(0, 0, (int)size.x, (int)size.y, Fade(BLACK, black));
}

// Nothing in screen space.
static void Gui() { }
