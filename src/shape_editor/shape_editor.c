// ============================================================================
//  shape_editor.c  -  pixel-shape authoring state (see shape_editor.h)
//
//  Layout: a left tool column (raygui), the pixel canvas filling the rest. The
//  canvas is drawn in TARGET space via Screen2Target, like every other state -
//  the game renders to a letterboxed render target, so raw screen coordinates
//  would drift as soon as the window aspect changed.
//
//  UNDO is its own scheme, deliberately not zen's. ZenUndoPush snapshots the
//  whole AnimDoc, and pixels are not in the document at all. This ring stores
//  RIGHT-SIZED w*h copies (a 24x16 shape is 384 B) and pushes ONE STEP PER
//  STROKE, not per pixel: a drag across forty cells is one Ctrl+Z, which is what
//  a person means by "undo that".
// ============================================================================

#include "raylib.h"
#include "raygui.h"
#include "shape_editor.h"
#include "../anim/anim_shape_pool.h"
#include "../screen_state/screen_state.h"
#include "../audio_state/audio_state.h"
#include <string.h>
#include <math.h>

static void Enter();
static void Exit();
static void Update();
static void Draw();
static void Gui();

                          /* Enter, Exit, Update, Draw, Gui, "Name" */
AppState app_state_shape_editor = { Enter, Exit, Update, Draw, Gui, "ShapeEditor" };

#define SE_PANEL_W    200.0f
#define SE_RH          22.0f
#define SE_GAP          4.0f
#define SE_UNDO_MAX    32
#define SE_STATUS_SECS  3.0f

// ---------------------------------------------------------------------------
//  State
// ---------------------------------------------------------------------------
static int   s_slot = ANIM_SHAPE_MISSING;   // shape being edited
static char  s_wanted[ANIM_SHAPE_NAME_MAX]; // what ShapeEditorOpen asked for
static int   s_ink = ANIM_PX_FILL;          // active brush value

static float s_zoom = 12.0f;                // target-space pixels per cell
static Vector2 s_pan = { 0 };               // canvas top-left in target space

static char  s_status[96];
static float s_statusT = 0.0f;

// New/rename buffer + its edit flag (raygui text boxes are stateful this way).
static char  s_nameBuf[ANIM_SHAPE_NAME_MAX];
static bool  s_edName = false;
static int   s_newW = 24, s_newH = 16;

// Reference image, shown faintly under the grid to trace over.
static Texture2D s_refTex;
static Image     s_refImg;
static bool      s_refLoaded = false;
static float     s_refAlpha = 0.35f;
static float     s_threshold = 0.5f;
static bool      s_previewOn = false;       // live threshold preview vs the pixels

// Undo ring. Right-sized: only the cells the shape actually has.
typedef struct {
    unsigned char px[ANIM_SHAPE_GRID_MAX * ANIM_SHAPE_GRID_MAX];
    int w, h;
    bool used;
} SEUndo;
static SEUndo s_undo[SE_UNDO_MAX];
static int    s_undoHead = 0, s_undoCount = 0;
static bool   s_strokeOpen = false;         // a drag is in progress: no new push

static void Status(const char *msg) { TextCopy(s_status, msg); s_statusT = SE_STATUS_SECS; }

// ---------------------------------------------------------------------------
//  Undo
// ---------------------------------------------------------------------------
static void UndoPush(void)
{
    AnimShapeDef *s = AnimShapePoolGet(s_slot);
    if (!s) return;
    SEUndo *u = &s_undo[s_undoHead];
    memcpy(u->px, s->px, sizeof(u->px));
    u->w = s->w; u->h = s->h; u->used = true;
    s_undoHead = (s_undoHead + 1) % SE_UNDO_MAX;
    if (s_undoCount < SE_UNDO_MAX) s_undoCount++;
}

static void UndoPop(void)
{
    AnimShapeDef *s = AnimShapePoolGet(s_slot);
    if (!s || s_undoCount == 0) { Status("nothing to undo"); return; }
    s_undoHead = (s_undoHead - 1 + SE_UNDO_MAX) % SE_UNDO_MAX;
    s_undoCount--;
    SEUndo *u = &s_undo[s_undoHead];
    memcpy(s->px, u->px, sizeof(s->px));
    s->w = u->w; s->h = u->h;
    AnimShapePoolInvalidate(s_slot);
    Status("undo");
}

static void UndoClear(void) { s_undoHead = 0; s_undoCount = 0; s_strokeOpen = false; }

// ---------------------------------------------------------------------------
//  Canvas geometry (target space)
// ---------------------------------------------------------------------------
static Rectangle CanvasRect(void)
{
    AnimShapeDef *s = AnimShapePoolGet(s_slot);
    if (!s) return (Rectangle){ 0 };
    return (Rectangle){ s_pan.x, s_pan.y, s->w * s_zoom, s->h * s_zoom };
}

// Centres the shape in the space left of the tool panel.
static void CanvasFit(void)
{
    AnimShapeDef *s = AnimShapePoolGet(s_slot);
    if (!s) return;
    Vector2 t = ScreenStateTargetSize();
    float availW = t.x - SE_PANEL_W - 40.0f, availH = t.y - 80.0f;
    float zx = availW / (float)s->w, zy = availH / (float)s->h;
    s_zoom = (zx < zy ? zx : zy);
    if (s_zoom < 1.0f) s_zoom = 1.0f;
    if (s_zoom > 40.0f) s_zoom = 40.0f;
    s_pan.x = SE_PANEL_W + 20.0f + (availW - s->w * s_zoom) * 0.5f;
    s_pan.y = 60.0f + (availH - s->h * s_zoom) * 0.5f;
}

// Cell under the cursor, or false when the cursor is off the grid.
static bool CellAtCursor(int *cx, int *cy)
{
    AnimShapeDef *s = AnimShapePoolGet(s_slot);
    if (!s) return false;
    Vector2 m = Screen2Target(GetMousePosition());
    Rectangle c = CanvasRect();
    if (!CheckCollisionPointRec(m, c)) return false;
    *cx = (int)((m.x - c.x) / s_zoom);
    *cy = (int)((m.y - c.y) / s_zoom);
    return (*cx >= 0 && *cy >= 0 && *cx < s->w && *cy < s->h);
}

// ---------------------------------------------------------------------------
//  Shape selection
// ---------------------------------------------------------------------------
static void SelectSlot(int slot)
{
    s_slot = slot;
    UndoClear();
    AnimShapeDef *s = AnimShapePoolGet(slot);
    if (s) { TextCopy(s_nameBuf, s->name); CanvasFit(); }
}

static void SelectFirst(void)
{
    for (int i = 0; i < ANIM_SHAPE_POOL_MAX; i++)
        if (AnimShapeIdValid(i)) { SelectSlot(i); return; }
    s_slot = ANIM_SHAPE_MISSING;
}

void ShapeEditorOpen(const char *name)
{
    TextCopy(s_wanted, name ? name : "");
}

// ---------------------------------------------------------------------------
//  PNG -> pixels: box-downsample, then threshold
//
//  Each grid cell averages the source region it covers (so detail between
//  sample points still counts, unlike point sampling), and a cell whose average
//  LUMINANCE-times-ALPHA clears the threshold becomes ink. The outline is
//  derived afterwards rather than sampled: any fill cell touching an empty
//  4-neighbour is the boundary, which is the cheapest rule that produces a
//  usable two-tone shape to hand-fix.
// ---------------------------------------------------------------------------
static float CellCoverage(const Image *img, int cx, int cy, int gw, int gh)
{
    int x0 = (int)((float)cx      / gw * img->width);
    int x1 = (int)((float)(cx+1)  / gw * img->width);
    int y0 = (int)((float)cy      / gh * img->height);
    int y1 = (int)((float)(cy+1)  / gh * img->height);
    if (x1 <= x0) x1 = x0 + 1;
    if (y1 <= y0) y1 = y0 + 1;
    if (x1 > img->width)  x1 = img->width;
    if (y1 > img->height) y1 = img->height;

    float sum = 0.0f;
    int n = 0;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
        {
            Color c = GetImageColor(*img, x, y);
            // Luma, weighted by alpha so a transparent PNG's background does not
            // read as bright ink.
            float lum = (0.299f*c.r + 0.587f*c.g + 0.114f*c.b) / 255.0f;
            sum += lum * (c.a / 255.0f);
            n++;
        }
    return n ? sum / n : 0.0f;
}

// Fills `out` (gw*gh at the ANIM_SHAPE_GRID_MAX stride) from the reference.
static void Downsample(unsigned char *out, int gw, int gh, float thresh)
{
    for (int y = 0; y < gh; y++)
        for (int x = 0; x < gw; x++)
            out[y*ANIM_SHAPE_GRID_MAX + x] =
                (CellCoverage(&s_refImg, x, y, gw, gh) >= thresh) ? ANIM_PX_FILL
                                                                  : ANIM_PX_EMPTY;

    // Boundary pass: a fill cell with an empty (or off-grid) 4-neighbour is rim.
    // Read from a copy so a cell promoted to outline does not make its neighbour
    // one too, which would eat the whole shape.
    static unsigned char src[ANIM_SHAPE_GRID_MAX * ANIM_SHAPE_GRID_MAX];
    memcpy(src, out, sizeof(src));
    for (int y = 0; y < gh; y++)
        for (int x = 0; x < gw; x++)
        {
            if (src[y*ANIM_SHAPE_GRID_MAX + x] != ANIM_PX_FILL) continue;
            bool edge = (x == 0 || y == 0 || x == gw-1 || y == gh-1)
                || src[y*ANIM_SHAPE_GRID_MAX + (x-1)] == ANIM_PX_EMPTY
                || src[y*ANIM_SHAPE_GRID_MAX + (x+1)] == ANIM_PX_EMPTY
                || src[(y-1)*ANIM_SHAPE_GRID_MAX + x] == ANIM_PX_EMPTY
                || src[(y+1)*ANIM_SHAPE_GRID_MAX + x] == ANIM_PX_EMPTY;
            if (edge) out[y*ANIM_SHAPE_GRID_MAX + x] = ANIM_PX_OUTLINE;
        }
}

static void ApplyDownsample(void)
{
    AnimShapeDef *s = AnimShapePoolGet(s_slot);
    if (!s || !s_refLoaded) return;
    UndoPush();                     // one step for the whole conversion
    Downsample(s->px, s->w, s->h, s_threshold);
    AnimShapePoolInvalidate(s_slot);
    Status("converted from reference");
}

static void LoadReference(const char *path)
{
    if (s_refLoaded)
    {
        UnloadTexture(s_refTex);
        UnloadImage(s_refImg);
        s_refLoaded = false;
    }
    if (!path || !FileExists(path)) { Status("no such image"); return; }
    s_refImg = LoadImage(path);
    if (s_refImg.width <= 0) { Status("could not read that image"); return; }
    s_refTex = LoadTextureFromImage(s_refImg);
    s_refLoaded = true;
    Status(TextFormat("reference %dx%d loaded", s_refImg.width, s_refImg.height));
}

// ---------------------------------------------------------------------------
//  State lifecycle
// ---------------------------------------------------------------------------
static void Enter()
{
    AnimShapePoolLoadAll(ANIM_SHAPE_RES_DIR, ANIM_SHAPE_USER_DIR);

    int want = s_wanted[0] ? AnimShapePoolFindByName(s_wanted) : ANIM_SHAPE_MISSING;
    if (want >= 0) SelectSlot(want); else SelectFirst();
    s_wanted[0] = '\0';

    if (s_slot == ANIM_SHAPE_MISSING) TextCopy(s_nameBuf, "new_shape");
    s_status[0] = '\0';
    s_statusT = 0.0f;
    s_edName = false;
    s_previewOn = false;
}

static void Exit()
{
    if (s_refLoaded)
    {
        UnloadTexture(s_refTex);
        UnloadImage(s_refImg);
        s_refLoaded = false;
    }
    // The stencils rebake lazily wherever they are next drawn; dropping them
    // here keeps the handles owned by whoever is running.
    AnimShapePoolUnloadTextures();
    UndoClear();
}

static void Update()
{
    if (s_statusT > 0.0f) s_statusT -= GetFrameTime();

    if (IsKeyPressed(KEY_ESCAPE)) { AppStateTransition(&app_state_main_menu); return; }
    if (s_edName) return;                       // typing a name: no shortcuts

    if ((IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL)) &&
        IsKeyPressed(KEY_Z)) UndoPop();
    if (IsKeyPressed(KEY_ONE))   s_ink = ANIM_PX_EMPTY;
    if (IsKeyPressed(KEY_TWO))   s_ink = ANIM_PX_FILL;
    if (IsKeyPressed(KEY_THREE)) s_ink = ANIM_PX_OUTLINE;
    if (IsKeyPressed(KEY_F))     CanvasFit();

    float wheel = GetMouseWheelMove();
    if (wheel != 0.0f)
    {
        s_zoom += wheel;
        if (s_zoom < 1.0f)  s_zoom = 1.0f;
        if (s_zoom > 40.0f) s_zoom = 40.0f;
    }

    AnimShapeDef *s = AnimShapePoolGet(s_slot);
    if (!s) return;

    // Painting. Left paints the active ink, right erases - and the undo push
    // happens ONCE on press, so the whole drag is a single step.
    bool left  = IsMouseButtonDown(MOUSE_BUTTON_LEFT);
    bool right = IsMouseButtonDown(MOUSE_BUTTON_RIGHT);
    if (!left && !right) s_strokeOpen = false;
    else
    {
        int cx, cy;
        if (CellAtCursor(&cx, &cy))
        {
            unsigned char v = right ? ANIM_PX_EMPTY : (unsigned char)s_ink;
            if (AnimShapePx(s, cx, cy) != v)
            {
                if (!s_strokeOpen) { UndoPush(); s_strokeOpen = true; }
                AnimShapeSetPx(s, cx, cy, v);
                AnimShapePoolInvalidate(s_slot);
            }
        }
    }
}

// ---------------------------------------------------------------------------
//  Draw: the canvas. Chrome is raygui and lives in Gui().
// ---------------------------------------------------------------------------
static void Draw()
{
    ClearBackground((Color){ 24, 25, 30, 255 });

    AnimShapeDef *s = AnimShapePoolGet(s_slot);
    if (!s) return;
    Rectangle c = CanvasRect();

    // Reference image first, faint, stretched over the same box the grid covers
    // so a traced pixel lands where the reference shows it.
    if (s_refLoaded && s_refAlpha > 0.0f)
    {
        Rectangle src = { 0, 0, (float)s_refTex.width, (float)s_refTex.height };
        DrawTexturePro(s_refTex, src, c, (Vector2){ 0, 0 }, 0.0f,
                       Fade(WHITE, s_refAlpha));
    }

    // A mid grey behind the cells, so EMPTY reads as "nothing here" rather than
    // as the page colour - and so the reference shows through only where asked.
    DrawRectangleRec(c, Fade((Color){ 18, 19, 23, 255 }, s_refLoaded ? 0.25f : 0.85f));

    // Live threshold preview: what Convert would write, without writing it.
    static unsigned char preview[ANIM_SHAPE_GRID_MAX * ANIM_SHAPE_GRID_MAX];
    bool usePreview = s_previewOn && s_refLoaded;
    if (usePreview) Downsample(preview, s->w, s->h, s_threshold);

    for (int y = 0; y < s->h; y++)
        for (int x = 0; x < s->w; x++)
        {
            unsigned char v = usePreview ? preview[y*ANIM_SHAPE_GRID_MAX + x]
                                         : AnimShapePx(s, x, y);
            if (v == ANIM_PX_EMPTY) continue;
            // Deliberately NEUTRAL: white fill, grey rim. These are stand-ins for
            // whatever colour the element supplies - see the banner in Gui().
            Color col = (v == ANIM_PX_FILL) ? RAYWHITE : (Color){ 150, 155, 165, 255 };
            DrawRectangle((int)(c.x + x*s_zoom), (int)(c.y + y*s_zoom),
                          (int)s_zoom + 1, (int)s_zoom + 1, Fade(col, usePreview ? 0.6f : 1.0f));
        }

    // Grid lines only when a cell is big enough for them to mean something.
    if (s_zoom >= 5.0f)
    {
        Color g = Fade(WHITE, 0.12f);
        for (int x = 0; x <= s->w; x++)
            DrawLineV((Vector2){ c.x + x*s_zoom, c.y },
                      (Vector2){ c.x + x*s_zoom, c.y + c.height }, g);
        for (int y = 0; y <= s->h; y++)
            DrawLineV((Vector2){ c.x, c.y + y*s_zoom },
                      (Vector2){ c.x + c.width, c.y + y*s_zoom }, g);
    }
    DrawRectangleLinesEx(c, 1.0f, (Color){ 90, 95, 110, 255 });

    // Cursor cell, so the brush lands where the eye expects.
    int hx, hy;
    if (CellAtCursor(&hx, &hy))
        DrawRectangleLinesEx((Rectangle){ c.x + hx*s_zoom, c.y + hy*s_zoom,
                                          s_zoom, s_zoom }, 2.0f, SKYBLUE);
}

// ---------------------------------------------------------------------------
//  Gui: the tool column
// ---------------------------------------------------------------------------
static void Gui()
{
    Vector2 t = ScreenStateTargetSize();
    float x = 10.0f, y = 10.0f, w = SE_PANEL_W - 20.0f;

    GuiPanel((Rectangle){ 0, 0, SE_PANEL_W, t.y }, NULL);

    if (GuiButton((Rectangle){ x, y, w, SE_RH }, "< BACK TO MENU"))
    { AudioPlayButton(); AppStateTransition(&app_state_main_menu); return; }
    y += SE_RH + SE_GAP * 2;

    // -- the colour-modularity banner ---------------------------------------
    // Not advice to follow, a fact about the format: there is nowhere in a .shp
    // to put a colour. Saying so here is what stops someone hunting for the
    // colour picker that does not exist.
    GuiLabel((Rectangle){ x, y, w, SE_RH }, "Shapes store NO colour.");
    y += SE_RH - 4.0f;
    GuiLabel((Rectangle){ x, y, w, SE_RH }, "Fill + outline are tinted");
    y += SE_RH - 6.0f;
    GuiLabel((Rectangle){ x, y, w, SE_RH }, "by the element, and stay");
    y += SE_RH - 6.0f;
    GuiLabel((Rectangle){ x, y, w, SE_RH }, "animatable in zen.");
    y += SE_RH + SE_GAP;

    // -- shape list ----------------------------------------------------------
    GuiLine((Rectangle){ x, y, w, 8 }, "shapes");
    y += 12.0f;
    for (int i = 0; i < ANIM_SHAPE_POOL_MAX; i++)
    {
        AnimShapeDef *sd = AnimShapePoolGet(i);
        if (!sd) continue;
        bool on = (i == s_slot);
        Rectangle rr = { x, y, w - 24.0f, SE_RH };
        GuiToggle(rr, TextFormat("%s %dx%d", sd->name, sd->w, sd->h), &on);
        if (on && i != s_slot) { AudioPlayButton(); SelectSlot(i); }
        if (GuiButton((Rectangle){ x + w - 20.0f, y, 20.0f, SE_RH }, "#143#") &&
            i == s_slot)
        {
            AudioPlayButton();
            AnimShapePoolDelete(i, ANIM_SHAPE_USER_DIR);
            SelectFirst();
            Status("shape deleted");
        }
        y += SE_RH + 2.0f;
    }
    y += SE_GAP;

    // -- new shape -----------------------------------------------------------
    GuiLine((Rectangle){ x, y, w, 8 }, "new");
    y += 12.0f;
    if (GuiTextBox((Rectangle){ x, y, w, SE_RH }, s_nameBuf, ANIM_SHAPE_NAME_MAX, s_edName))
        s_edName = !s_edName;
    y += SE_RH + SE_GAP;

    float hw = (w - SE_GAP) * 0.5f;
    float fw = (float)s_newW, fh = (float)s_newH;
    GuiSpinner((Rectangle){ x + 26.0f, y, hw - 26.0f, SE_RH }, "w ", &s_newW, 1,
               ANIM_SHAPE_GRID_MAX, false);
    GuiSpinner((Rectangle){ x + hw + SE_GAP + 26.0f, y, hw - 26.0f, SE_RH }, "h ",
               &s_newH, 1, ANIM_SHAPE_GRID_MAX, false);
    (void)fw; (void)fh;
    y += SE_RH + SE_GAP;

    if (GuiButton((Rectangle){ x, y, w, SE_RH }, "+ create shape"))
    {
        AudioPlayButton();
        int slot = AnimShapePoolAdd(s_nameBuf, s_newW, s_newH);
        if (slot == ANIM_SHAPE_MISSING)
            Status("pool full, or name taken/invalid");
        else { SelectSlot(slot); Status("created - save to keep it"); }
    }
    y += SE_RH + SE_GAP * 2;

    AnimShapeDef *s = AnimShapePoolGet(s_slot);
    if (s)
    {
        // -- brush -----------------------------------------------------------
        GuiLine((Rectangle){ x, y, w, 8 }, "brush  (1/2/3)");
        y += 12.0f;
        float bw = (w - SE_GAP*2) / 3.0f;
        const char *inkName[3] = { "empty", "fill", "rim" };
        for (int i = 0; i < 3; i++)
        {
            bool on = (s_ink == i);
            GuiToggle((Rectangle){ x + i*(bw + SE_GAP), y, bw, SE_RH }, inkName[i], &on);
            if (on) s_ink = i;
        }
        y += SE_RH + SE_GAP;
        GuiLabel((Rectangle){ x, y, w, SE_RH }, "right-drag erases");
        y += SE_RH;

        // -- size / view -----------------------------------------------------
        int cw = s->w, ch = s->h;
        GuiSpinner((Rectangle){ x + 26.0f, y, hw - 26.0f, SE_RH }, "w ", &cw, 1,
                   ANIM_SHAPE_GRID_MAX, false);
        GuiSpinner((Rectangle){ x + hw + SE_GAP + 26.0f, y, hw - 26.0f, SE_RH }, "h ",
                   &ch, 1, ANIM_SHAPE_GRID_MAX, false);
        if (cw != s->w || ch != s->h)
        { UndoPush(); AnimShapeResize(s, cw, ch); CanvasFit(); }
        y += SE_RH + SE_GAP;

        if (GuiButton((Rectangle){ x, y, hw, SE_RH }, "fit (F)")) CanvasFit();
        if (GuiButton((Rectangle){ x + hw + SE_GAP, y, hw, SE_RH }, "clear"))
        {
            AudioPlayButton();
            UndoPush();
            memset(s->px, ANIM_PX_EMPTY, sizeof(s->px));
            AnimShapePoolInvalidate(s_slot);
            Status("cleared");
        }
        y += SE_RH + SE_GAP;

        if (GuiButton((Rectangle){ x, y, hw, SE_RH }, "undo (^Z)")) UndoPop();
        if (GuiButton((Rectangle){ x + hw + SE_GAP, y, hw, SE_RH }, "SAVE"))
        {
            AudioPlayButton();
            Status(AnimShapePoolSaveOne(s_slot, ANIM_SHAPE_USER_DIR)
                   ? "saved to shapes/" : "could not write shapes/");
        }
        y += SE_RH + SE_GAP * 2;

        // -- reference image -------------------------------------------------
        GuiLine((Rectangle){ x, y, w, 8 }, "reference png");
        y += 12.0f;
        GuiLabel((Rectangle){ x, y, w, SE_RH }, s_refLoaded ? "loaded - drop to replace"
                                                            : "drop a .png here");
        y += SE_RH;

        if (s_refLoaded)
        {
            GuiSlider((Rectangle){ x + 32.0f, y, w - 64.0f, SE_RH }, "fade", NULL,
                      &s_refAlpha, 0.0f, 1.0f);
            y += SE_RH + SE_GAP;
            GuiSlider((Rectangle){ x + 32.0f, y, w - 64.0f, SE_RH }, "thr", NULL,
                      &s_threshold, 0.0f, 1.0f);
            y += SE_RH + SE_GAP;
            GuiCheckBox((Rectangle){ x, y, 16.0f, 16.0f }, " preview", &s_previewOn);
            y += SE_RH;
            if (GuiButton((Rectangle){ x, y, w, SE_RH }, "convert -> pixels"))
            { AudioPlayButton(); ApplyDownsample(); }
            y += SE_RH + SE_GAP;
        }
    }

    // Dropped file: the reference image. Handled here rather than in Update
    // because it only ever affects this panel's state.
    if (IsFileDropped())
    {
        FilePathList fl = LoadDroppedFiles();
        if (fl.count > 0) LoadReference(fl.paths[0]);
        UnloadDroppedFiles(fl);
    }

    if (s_statusT > 0.0f && s_status[0])
        GuiLabel((Rectangle){ x, t.y - 26.0f, t.x - x, SE_RH }, s_status);
}
