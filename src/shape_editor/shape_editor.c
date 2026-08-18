// ============================================================================
//  shape_editor.c  -  pixel-shape authoring state (see shape_editor.h)
//
//  EVERYTHING HERE IS SCREEN SPACE, and that is the whole layout story.
//  main.c runs Draw() inside BeginTextureMode (the 320x180 letterboxed target)
//  but runs Gui() AFTER EndTextureMode, against the real window. A pixel editor
//  needs the window's resolution - a 320-wide target cannot show a 128-cell grid
//  and a tool column at once - so the canvas is drawn in Gui() alongside the
//  chrome, and Draw() only clears. Mouse coordinates are therefore raw
//  GetMousePosition() with NO Screen2Target: that call converts INTO target
//  space, which is exactly the space this state does not use.
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
#include <stdlib.h>     // getenv, for the picker's "home" shortcut
#include <math.h>

static void Enter();
static void Exit();
static void Update();
static void Draw();
static void Gui();

                          /* Enter, Exit, Update, Draw, Gui, "Name" */
AppState app_state_shape_editor = { Enter, Exit, Update, Draw, Gui, "ShapeEditor" };

#define SE_PANEL_W    260.0f    // screen-space pixels, not target
#define SE_RH          24.0f
#define SE_GAP          6.0f
#define SE_TOP         12.0f
#define SE_UNDO_MAX    32
#define SE_STATUS_SECS  3.0f
#define SE_BROWSE_MAX  128      // image files listed by the picker
// Visible rows of the shape list, which scrolls. Kept SHORT on purpose: the
// tool column below it flows without a scrollbar (see the SAVE bar's comment),
// and the reference section's placement controls now sit at its tail, so every
// row spent here is a row that can push them off a short window.
#define SE_LIST_ROWS     6

// ---------------------------------------------------------------------------
//  State
// ---------------------------------------------------------------------------
static int   s_slot = ANIM_SHAPE_MISSING;   // shape being edited
static char  s_wanted[ANIM_SHAPE_NAME_MAX]; // what ShapeEditorOpen asked for
static int   s_ink = ANIM_PX_FILL;          // active brush value
// Brush DIAMETER in cells, 1..32. A round brush, so the stamp is a disc of this
// width; 1 is the single-cell pencil the editor started with.
static int   s_brush = 1;

static float s_zoom = 12.0f;                // screen pixels per cell
static Vector2 s_pan = { 0 };               // canvas top-left, screen space
static float s_listScroll = 0.0f;           // shape list, <= 0, in screen pixels

// The tool column scrolls as a whole: it holds more rows than a short window
// has height, and the rows that fell off the bottom were simply unreachable.
// Content height is measured by the flow itself, one frame behind - which is
// invisible, and far simpler than predicting the height of a variable panel.
static float s_panelScroll = 0.0f;          // <= 0, in screen pixels
static float s_panelContentH = 0.0f;        // measured at the end of last frame's flow

static char  s_status[96];
static float s_statusT = 0.0f;

// New/rename buffer + its edit flag (raygui text boxes are stateful this way).
static char  s_nameBuf[ANIM_SHAPE_NAME_MAX];
static bool  s_edName = false;
static int   s_newW = 24, s_newH = 16;
static bool  s_edNewW = false, s_edNewH = false;
static bool  s_edCurW = false, s_edCurH = false;
// Creating a shape is a modal question - name AND size, both needed before the
// slot exists. Inline in the column it read as three unrelated controls sitting
// next to the shape list, and the name box shared a lane with the list rows.
static bool  s_newOpen = false;

// Reference image, shown faintly under the grid to trace over.
static Texture2D s_refTex;
static Image     s_refImg;
static bool      s_refLoaded = false;
static float     s_refAlpha = 0.35f;
static float     s_threshold = 0.5f;
static bool      s_previewOn = false;       // live threshold preview vs the pixels
static bool      s_refInvert = false;       // bright ink on dark, or the reverse

// Where the reference sits, in GRID CELLS - not screen pixels. Cell space is
// what makes the reference stay glued to the grid through canvas zoom and pan:
// convert once at draw time and the placement survives every view change. It is
// also the space the sampler works in, so what you see traced is exactly what
// Convert reads.
//   scale 1.0 = the image fitted inside the grid with its ASPECT PRESERVED
//   offset    = cells away from that centred fit
static float     s_refScale = 1.0f;
static Vector2   s_refOff = { 0 };

// File picker. raylib has no native dialog and this repo pulls in no dialog
// library, so this is zen's File>Open list pointed at image files instead.
static bool  s_browseOpen = false;
static char  s_browseDir[512];
static char  s_browseList[SE_BROWSE_MAX][128];
static bool  s_browseIsDir[SE_BROWSE_MAX];
static int   s_browseCount = 0;
static float s_browseScroll = 0.0f;
static char  s_pathBuf[512];
static bool  s_edPath = false;

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

// True while any text field owns the keyboard, so shortcuts stay out of the way.
static bool Typing(void)
{
    return s_edName || s_edNewW || s_edNewH || s_edCurW || s_edCurH || s_edPath;
}

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
//  Brush
// ---------------------------------------------------------------------------
// Stamp the brush disc centred on (cx, cy). Returns true if any cell actually
// changed - the caller only invalidates the texture cache when it did.
//
// The undo push happens HERE rather than in the caller because it must fire on
// the first cell that really changes: pushing on mouse-press alone would record
// a step for a click that painted nothing (a stamp entirely off-grid, or over
// cells already holding this ink), and those empty steps eat the 32-deep ring.
static bool StampAt(AnimShapeDef *s, int cx, int cy, unsigned char v)
{
    // Even diameters have no centre cell, so the disc is offset by half a cell
    // and the radius is measured from cell centres. Odd diameters land square
    // on (cx, cy), which is what makes brush 1 an exact single-cell pencil.
    int   r    = s_brush / 2;
    float half = (s_brush % 2 == 0) ? 0.5f : 0.0f;
    float rr   = (s_brush * 0.5f) * (s_brush * 0.5f);
    bool  any  = false;

    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++)
        {
            float fx = (float)dx - half, fy = (float)dy - half;
            if (s_brush > 2 && fx*fx + fy*fy > rr) continue;   // round it off
            int px = cx + dx, py = cy + dy;
            if (px < 0 || py < 0 || px >= s->w || py >= s->h) continue;
            if (AnimShapePx(s, px, py) == v) continue;
            if (!s_strokeOpen) { UndoPush(); s_strokeOpen = true; }
            AnimShapeSetPx(s, px, py, v);
            any = true;
        }
    return any;
}

// ---------------------------------------------------------------------------
//  Canvas geometry (screen space)
// ---------------------------------------------------------------------------
// The region left over once the tool column is taken out.
static Rectangle WorkArea(void)
{
    Vector2 sc = ScreenStateSize();
    return (Rectangle){ SE_PANEL_W, 0.0f, sc.x - SE_PANEL_W, sc.y };
}

static Rectangle CanvasRect(void)
{
    AnimShapeDef *s = AnimShapePoolGet(s_slot);
    if (!s) return (Rectangle){ 0 };
    return (Rectangle){ s_pan.x, s_pan.y, s->w * s_zoom, s->h * s_zoom };
}

// Centres the shape in the work area. Called on select, resize, F, and resize
// of the window itself - anything that can move the box out from under it.
static void CanvasFit(void)
{
    AnimShapeDef *s = AnimShapePoolGet(s_slot);
    if (!s) return;
    Rectangle wa = WorkArea();
    float availW = wa.width - 48.0f, availH = wa.height - 72.0f;
    if (availW < 32.0f) availW = 32.0f;
    if (availH < 32.0f) availH = 32.0f;
    float zx = availW / (float)s->w, zy = availH / (float)s->h;
    s_zoom = (zx < zy ? zx : zy);
    if (s_zoom < 1.0f)  s_zoom = 1.0f;
    if (s_zoom > 48.0f) s_zoom = 48.0f;
    s_pan.x = wa.x + 24.0f + (availW - s->w * s_zoom) * 0.5f;
    s_pan.y = wa.y + 48.0f + (availH - s->h * s_zoom) * 0.5f;
}

// ---------------------------------------------------------------------------
//  Reference placement
// ---------------------------------------------------------------------------
// The reference's box in CELL space. Base is an aspect-preserving "contain" fit
// inside the grid - NOT the old stretch-to-grid, which distorted every image
// whose proportions were not the grid's and left nothing to centre. Scale grows
// the box about its own centre, then the offset slides it.
static Rectangle RefCellRect(void)
{
    AnimShapeDef *s = AnimShapePoolGet(s_slot);
    if (!s || !s_refLoaded) return (Rectangle){ 0, 0, 1, 1 };

    float gw = (float)s->w, gh = (float)s->h;
    float iw = (float)s_refImg.width, ih = (float)s_refImg.height;
    if (iw <= 0.0f || ih <= 0.0f) return (Rectangle){ 0, 0, gw, gh };

    float k = (gw / iw < gh / ih) ? gw / iw : gh / ih;   // contain
    float w = iw * k * s_refScale;
    float h = ih * k * s_refScale;
    return (Rectangle){ (gw - w)*0.5f + s_refOff.x,
                        (gh - h)*0.5f + s_refOff.y, w, h };
}

static void RefFit(void)    { s_refScale = 1.0f; s_refOff = (Vector2){ 0 }; }
static void RefCenter(void) { s_refOff = (Vector2){ 0 }; }

// Cell under the cursor, or false when the cursor is off the grid. Raw mouse:
// see the screen-space note in the file header.
static bool CellAtCursor(int *cx, int *cy)
{
    AnimShapeDef *s = AnimShapePoolGet(s_slot);
    if (!s) return false;
    Vector2 m = GetMousePosition();
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

    // Scroll the list so the selection is visible. With hundreds of slots the
    // shape opened from zen is usually nowhere near the top, and a selection
    // you cannot see reads as no selection at all. Rows are packed (unused slots
    // draw nothing), so the visual row is the count of used slots before this one.
    if (s)
    {
        int row = 0;
        for (int i = 0; i < slot; i++) if (AnimShapeIdValid(i)) row++;
        float rowH = SE_RH + 2.0f;
        float top = -s_listScroll;                      // first visible pixel
        float bot = top + rowH * SE_LIST_ROWS;
        if (row * rowH < top)             s_listScroll = -(row * rowH);
        else if ((row + 1) * rowH > bot)  s_listScroll = -((row + 1) * rowH - rowH * SE_LIST_ROWS);
    }
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
//
//  The cell -> image mapping goes through RefCellRect, the SAME box the draw
//  path uses, so moving or scaling the reference moves what Convert reads by
//  exactly as much. A cell outside the box has no source at all and is never
//  ink - that is what stops a shrunken reference from painting its margins.
// ---------------------------------------------------------------------------
static float CellCoverage(const Image *img, int cx, int cy, Rectangle r)
{
    if (r.width <= 0.0f || r.height <= 0.0f) return 0.0f;

    // Cell extents -> normalised position within the reference box -> pixels.
    int x0 = (int)(((float)cx     - r.x) / r.width  * img->width);
    int x1 = (int)(((float)(cx+1) - r.x) / r.width  * img->width);
    int y0 = (int)(((float)cy     - r.y) / r.height * img->height);
    int y1 = (int)(((float)(cy+1) - r.y) / r.height * img->height);
    // Wholly outside the reference: no source, so no ink.
    if (x1 <= 0 || y1 <= 0 || x0 >= img->width || y0 >= img->height) return 0.0f;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 <= x0) x1 = x0 + 1;
    if (y1 <= y0) y1 = y0 + 1;
    if (x1 > img->width)  x1 = img->width;
    if (y1 > img->height) y1 = img->height;
    if (x0 >= x1 || y0 >= y1) return 0.0f;

    float sum = 0.0f;
    int n = 0;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
        {
            Color c = GetImageColor(*img, x, y);
            // Luma, weighted by alpha so a transparent PNG's background does not
            // read as bright ink.
            float lum = (0.299f*c.r + 0.587f*c.g + 0.114f*c.b) / 255.0f;
            float a   = c.a / 255.0f;
            // Inverted: dark strokes on a white page are the ink. Alpha still
            // gates, so a transparent margin never becomes ink either way.
            sum += (s_refInvert ? (1.0f - lum) : lum) * a;
            n++;
        }
    return n ? sum / n : 0.0f;
}

// Fills `out` (gw*gh at the ANIM_SHAPE_GRID_MAX stride) from the reference.
static void Downsample(unsigned char *out, int gw, int gh, float thresh)
{
    Rectangle r = RefCellRect();
    for (int y = 0; y < gh; y++)
        for (int x = 0; x < gw; x++)
            out[y*ANIM_SHAPE_GRID_MAX + x] =
                (CellCoverage(&s_refImg, x, y, r) >= thresh) ? ANIM_PX_FILL
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

// ---------------------------------------------------------------------------
//  Bulk ink
// ---------------------------------------------------------------------------
// Rewrites every INKED cell to `v`, leaving empty cells empty - so it changes
// which ink a shape uses without changing the shape. One undo step for the lot,
// like clear and convert. The common case is a shape traced or converted as
// fill-plus-derived-rim that only ever needed one of the two.
static void ConvertAllInk(unsigned char v)
{
    AnimShapeDef *s = AnimShapePoolGet(s_slot);
    if (!s) return;

    bool any = false;
    for (int y = 0; y < s->h && !any; y++)
        for (int x = 0; x < s->w && !any; x++)
        {
            unsigned char c = AnimShapePx(s, x, y);
            if (c != ANIM_PX_EMPTY && c != v) any = true;
        }
    if (!any) { Status("nothing to convert"); return; }

    UndoPush();
    for (int y = 0; y < s->h; y++)
        for (int x = 0; x < s->w; x++)
            if (AnimShapePx(s, x, y) != ANIM_PX_EMPTY) AnimShapeSetPx(s, x, y, v);
    AnimShapePoolInvalidate(s_slot);
    Status(v == ANIM_PX_FILL ? "all ink -> fill" : "all ink -> rim");
}

// Step the current shape's grid by one cell. The value boxes above these can do
// the same job, but only by committing a whole typed number - which is the wrong
// gesture for "one column narrower" and cannot be repeated by eye.
static void ResizeCur(int dw, int dh)
{
    AnimShapeDef *s = AnimShapePoolGet(s_slot);
    if (!s) return;
    int nw = s->w + dw, nh = s->h + dh;
    if (nw < 1) nw = 1;
    if (nh < 1) nh = 1;
    if (nw > ANIM_SHAPE_GRID_MAX) nw = ANIM_SHAPE_GRID_MAX;
    if (nh > ANIM_SHAPE_GRID_MAX) nh = ANIM_SHAPE_GRID_MAX;
    if (nw == s->w && nh == s->h) return;
    UndoPush();
    AnimShapeResize(s, nw, nh);     // clears its own texture cache
    CanvasFit();
    Status(TextFormat("%dx%d", nw, nh));
}

static void LoadReference(const char *path)
{
    if (!path || !path[0]) return;
    if (!FileExists(path)) { Status("no such file"); return; }
    if (!IsFileExtension(path, ".png") && !IsFileExtension(path, ".jpg") &&
        !IsFileExtension(path, ".jpeg") && !IsFileExtension(path, ".bmp") &&
        !IsFileExtension(path, ".tga"))
    { Status("need .png / .jpg / .bmp / .tga"); return; }

    Image img = LoadImage(path);
    if (img.width <= 0 || img.height <= 0)
    { Status("could not decode that image"); return; }

    // Only drop the old one once the new one is known good, so a bad pick does
    // not leave the editor with nothing to trace.
    if (s_refLoaded) { UnloadTexture(s_refTex); UnloadImage(s_refImg); }
    s_refImg = img;
    s_refTex = LoadTextureFromImage(s_refImg);
    s_refLoaded = true;
    RefFit();               // a new image starts fitted and centred, not where the last one was
    TextCopy(s_pathBuf, path);
    Status(TextFormat("reference %dx%d", s_refImg.width, s_refImg.height));
}

// ---------------------------------------------------------------------------
//  File picker
// ---------------------------------------------------------------------------
static void BrowseScan(const char *dir)
{
    s_browseCount = 0;
    s_browseScroll = 0.0f;
    if (!DirectoryExists(dir)) return;
    TextCopy(s_browseDir, dir);

    FilePathList fl = LoadDirectoryFiles(dir);
    // Directories first, then images - the order a person scans a file list in.
    for (unsigned int pass = 0; pass < 2 && s_browseCount < SE_BROWSE_MAX; pass++)
        for (unsigned int i = 0; i < fl.count && s_browseCount < SE_BROWSE_MAX; i++)
        {
            bool isDir = DirectoryExists(fl.paths[i]);
            if ((pass == 0) != isDir) continue;
            const char *base = GetFileName(fl.paths[i]);
            if (base[0] == '.') continue;                  // hidden entries
            if (!isDir && !IsFileExtension(fl.paths[i], ".png") &&
                !IsFileExtension(fl.paths[i], ".jpg") &&
                !IsFileExtension(fl.paths[i], ".jpeg") &&
                !IsFileExtension(fl.paths[i], ".bmp") &&
                !IsFileExtension(fl.paths[i], ".tga")) continue;
            TextCopy(s_browseList[s_browseCount], base);
            s_browseIsDir[s_browseCount] = isDir;
            s_browseCount++;
        }
    UnloadDirectoryFiles(fl);
}

static void BrowseOpen(void)
{
    s_browseOpen = true;
    s_edPath = false;
    // Reopen where the last pick came from, else the working directory.
    const char *start = s_browseDir[0] ? s_browseDir : GetWorkingDirectory();
    BrowseScan(start);
}

static void BrowseGui(void)
{
    if (!s_browseOpen) return;

    Vector2 sc = ScreenStateSize();
    float mw = 520.0f, mh = 420.0f;
    if (mw > sc.x - 40.0f) mw = sc.x - 40.0f;
    if (mh > sc.y - 40.0f) mh = sc.y - 40.0f;
    Rectangle m = { (sc.x - mw)*0.5f, (sc.y - mh)*0.5f, mw, mh };

    DrawRectangle(0, 0, (int)sc.x, (int)sc.y, (Color){ 0, 0, 0, 140 });
    DrawRectangleRec(m, (Color){ 40, 42, 48, 255 });
    DrawRectangleLinesEx(m, 1.0f, (Color){ 90, 94, 104, 255 });
    GuiLabel((Rectangle){ m.x+14, m.y+8, mw-28, 20 }, "OPEN REFERENCE IMAGE");

    // Current directory, and a typable path for anywhere the list cannot reach.
    GuiLabel((Rectangle){ m.x+14, m.y+30, mw-28, 18 },
             TextFormat("dir: %s", s_browseDir));

    Rectangle up = { m.x+14, m.y+52, 60.0f, SE_RH };
    if (GuiButton(up, "up"))
    {
        const char *parent = GetPrevDirectoryPath(s_browseDir);
        if (parent && parent[0]) BrowseScan(parent);
    }
    if (GuiButton((Rectangle){ m.x+80, m.y+52, 70.0f, SE_RH }, "home"))
    {
        const char *h = getenv("HOME");
        if (h && h[0]) BrowseScan(h);
    }
    if (GuiButton((Rectangle){ m.x+156, m.y+52, 70.0f, SE_RH }, "cwd"))
        BrowseScan(GetWorkingDirectory());

    // The list.
    Rectangle list = { m.x+14, m.y+84, mw-28, mh-84-84 };
    DrawRectangleRec(list, (Color){ 28, 29, 34, 255 });
    DrawRectangleLinesEx(list, 1.0f, (Color){ 70, 74, 84, 255 });
    if (CheckCollisionPointRec(GetMousePosition(), list))
        s_browseScroll += GetMouseWheelMove() * 28.0f;
    float rowH = 22.0f;
    float maxScroll = s_browseCount * rowH - list.height;
    if (maxScroll < 0.0f) maxScroll = 0.0f;
    if (s_browseScroll < -maxScroll) s_browseScroll = -maxScroll;
    if (s_browseScroll > 0.0f) s_browseScroll = 0.0f;

    int clicked = -1;
    BeginScissorMode((int)list.x, (int)list.y, (int)list.width, (int)list.height);
    float y = list.y + s_browseScroll;
    for (int i = 0; i < s_browseCount; i++)
    {
        Rectangle r = { list.x+2, y, list.width-4, rowH };
        if (y + rowH >= list.y && y <= list.y + list.height)
            if (GuiButton(r, s_browseIsDir[i]
                             ? TextFormat("#01# %s", s_browseList[i])
                             : TextFormat("#12# %s", s_browseList[i])))
                clicked = i;
        y += rowH;
    }
    EndScissorMode();
    if (s_browseCount == 0)
        GuiLabel((Rectangle){ list.x+8, list.y+6, list.width-16, 20 },
                 "(no images or folders here)");

    // Typed path, for pasting somewhere the list does not go.
    Rectangle pb = { m.x+14, m.y+mh-72, mw-28-70, SE_RH };
    if (GuiTextBox(pb, s_pathBuf, sizeof(s_pathBuf), s_edPath)) s_edPath = !s_edPath;
    if (GuiButton((Rectangle){ pb.x+pb.width+6, pb.y, 64.0f, SE_RH }, "load") ||
        (s_edPath && (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER))))
    {
        AudioPlayButton();
        s_edPath = false;
        if (DirectoryExists(s_pathBuf)) BrowseScan(s_pathBuf);
        else { LoadReference(s_pathBuf); s_browseOpen = false; }
    }

    if (GuiButton((Rectangle){ m.x+mw-90, m.y+mh-36, 78.0f, 26.0f }, "Cancel"))
    { AudioPlayButton(); s_browseOpen = false; }

    if (clicked >= 0)
    {
        AudioPlayButton();
        const char *full = TextFormat("%s/%s", s_browseDir, s_browseList[clicked]);
        if (s_browseIsDir[clicked]) BrowseScan(full);
        else { LoadReference(full); s_browseOpen = false; }
    }
}

// ---------------------------------------------------------------------------
//  New-shape modal
// ---------------------------------------------------------------------------
static void NewShapeOpen(void)
{
    s_newOpen = true;
    s_edName = s_edNewW = s_edNewH = false;

    // Propose a name nothing in the pool holds. The buffer arrives carrying the
    // SELECTED shape's name (SelectSlot puts it there), so offering it unchanged
    // would open the dialog pre-loaded with the one name guaranteed to be taken.
    for (int i = 1; i < ANIM_SHAPE_POOL_MAX + 2; i++)
    {
        const char *cand = TextFormat("shape_%d", i);
        if (AnimShapePoolFindByName(cand) == ANIM_SHAPE_MISSING)
        { TextCopy(s_nameBuf, cand); break; }
    }
}

static void NewShapeCreate(void)
{
    int slot = AnimShapePoolAdd(s_nameBuf, s_newW, s_newH);
    if (slot == ANIM_SHAPE_MISSING)
    {
        // Two very different problems used to wear one message: a full pool is a
        // build limit to work around, a bad name is a typo to fix. The modal
        // stays open either way - both are fixed right here.
        Status(AnimShapeNameOk(s_nameBuf)
               ? TextFormat("pool full (%d/%d slots)",
                            AnimShapePoolCount(), ANIM_SHAPE_POOL_MAX)
               : "name taken, empty, or has spaces / # / slashes");
        return;
    }
    s_newOpen = false;
    s_edName = s_edNewW = s_edNewH = false;
    SelectSlot(slot);
    Status("created - save to keep it");
}

static void NewShapeGui(void)
{
    if (!s_newOpen) return;

    Vector2 sc = ScreenStateSize();
    float mw = 320.0f, mh = 210.0f;
    if (mw > sc.x - 40.0f) mw = sc.x - 40.0f;
    if (mh > sc.y - 40.0f) mh = sc.y - 40.0f;
    Rectangle m = { (sc.x - mw)*0.5f, (sc.y - mh)*0.5f, mw, mh };

    DrawRectangle(0, 0, (int)sc.x, (int)sc.y, (Color){ 0, 0, 0, 140 });
    DrawRectangleRec(m, (Color){ 40, 42, 48, 255 });
    DrawRectangleLinesEx(m, 1.0f, (Color){ 90, 94, 104, 255 });
    GuiLabel((Rectangle){ m.x+14, m.y+8, mw-28, 20 }, "NEW SHAPE");

    float x = m.x + 14.0f, w = mw - 28.0f;
    float y = m.y + 34.0f;
    float hw = (w - SE_GAP) * 0.5f;

    GuiLabel((Rectangle){ x, y, w, 18.0f }, "name  (no spaces, # or slashes)");
    y += 20.0f;
    if (GuiTextBox((Rectangle){ x, y, w, SE_RH }, s_nameBuf, ANIM_SHAPE_NAME_MAX, s_edName))
        s_edName = !s_edName;
    y += SE_RH + SE_GAP + 4.0f;

    GuiLabel((Rectangle){ x, y, w, 18.0f }, "size in cells");
    y += 20.0f;
    GuiLabel((Rectangle){ x, y, 16.0f, SE_RH }, "w");
    if (GuiValueBox((Rectangle){ x + 18.0f, y, hw - 18.0f, SE_RH }, NULL,
                    &s_newW, 1, ANIM_SHAPE_GRID_MAX, s_edNewW))
        s_edNewW = !s_edNewW;
    GuiLabel((Rectangle){ x + hw + SE_GAP, y, 16.0f, SE_RH }, "h");
    if (GuiValueBox((Rectangle){ x + hw + SE_GAP + 18.0f, y, hw - 18.0f, SE_RH }, NULL,
                    &s_newH, 1, ANIM_SHAPE_GRID_MAX, s_edNewH))
        s_edNewH = !s_edNewH;
    y += SE_RH + SE_GAP + 6.0f;

    // ENTER creates, so the whole dialog can be driven from the name box. Only
    // while a field owns the keyboard would ENTER otherwise do nothing at all.
    bool enter = IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER);
    if (GuiButton((Rectangle){ x, y, hw, SE_RH }, "create") || enter)
    { AudioPlayButton(); s_edName = s_edNewW = s_edNewH = false; NewShapeCreate(); }
    if (GuiButton((Rectangle){ x + hw + SE_GAP, y, hw, SE_RH }, "cancel"))
    {
        AudioPlayButton();
        s_newOpen = false;
        s_edName = s_edNewW = s_edNewH = false;
    }
}

// ---------------------------------------------------------------------------
//  State lifecycle
// ---------------------------------------------------------------------------
static void Enter()
{
    AnimShapePoolLoadAll(ANIM_SHAPE_RES_DIR, ANIM_SHAPE_USER_DIR);

    // Before the selection, not after: SelectSlot scrolls the list to reveal
    // what it picked, and clearing afterwards would undo that.
    s_listScroll = 0.0f;

    int want = s_wanted[0] ? AnimShapePoolFindByName(s_wanted) : ANIM_SHAPE_MISSING;
    if (want >= 0) SelectSlot(want); else SelectFirst();
    s_wanted[0] = '\0';

    if (s_slot == ANIM_SHAPE_MISSING) TextCopy(s_nameBuf, "new_shape");
    s_status[0] = '\0';
    s_statusT = 0.0f;
    s_edName = s_edNewW = s_edNewH = s_edCurW = s_edCurH = s_edPath = false;
    s_previewOn = false;
    s_browseOpen = false;
    s_newOpen = false;
    s_panelScroll = 0.0f;
    CanvasFit();
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

    // Dropped file: still supported, and it fills the picker's path box too so
    // the two routes agree on where the reference came from.
    if (IsFileDropped())
    {
        FilePathList fl = LoadDroppedFiles();
        if (fl.count > 0) LoadReference(fl.paths[0]);
        UnloadDroppedFiles(fl);
    }

    if (IsKeyPressed(KEY_ESCAPE))
    {
        // ESC unwinds one layer at a time: modals, then the state.
        if (s_browseOpen) { s_browseOpen = false; return; }
        if (s_newOpen)    { s_newOpen = false; s_edName = s_edNewW = s_edNewH = false; return; }
        AppStateTransition(&app_state_main_menu);
        return;
    }
    if (s_browseOpen || s_newOpen || Typing()) return;  // modal / typing: no shortcuts

    bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    if (ctrl && IsKeyPressed(KEY_Z)) UndoPop();
    if (ctrl && IsKeyPressed(KEY_S) && AnimShapePoolGet(s_slot))
        Status(AnimShapePoolSaveOne(s_slot, ANIM_SHAPE_USER_DIR)
               ? "saved to shapes/" : "could not write shapes/");
    if (IsKeyPressed(KEY_ONE))   s_ink = ANIM_PX_EMPTY;
    if (IsKeyPressed(KEY_TWO))   s_ink = ANIM_PX_FILL;
    if (IsKeyPressed(KEY_THREE)) s_ink = ANIM_PX_OUTLINE;

    // Photoshop's brush-size keys, because the hand is already on the canvas.
    if (IsKeyPressed(KEY_LEFT_BRACKET)  && s_brush >  1) s_brush--;
    if (IsKeyPressed(KEY_RIGHT_BRACKET) && s_brush < 32) s_brush++;
    if (IsKeyPressed(KEY_F))     CanvasFit();

    Rectangle wa = WorkArea();
    bool overCanvas = CheckCollisionPointRec(GetMousePosition(), wa);

    // ALT is the reference-image modifier, for the whole gesture set: while it
    // is held the canvas does not zoom, the brush does not paint, and the same
    // wheel-and-drag the hand already knows moves the REFERENCE instead. A
    // modifier rather than a mode, because aligning a reference is a handful of
    // nudges between strokes - a mode you must leave again would be worse.
    // With no reference loaded ALT means nothing, and swallowing the wheel then
    // would just look broken.
    bool alt = (IsKeyDown(KEY_LEFT_ALT) || IsKeyDown(KEY_RIGHT_ALT))
             && s_refLoaded && AnimShapePoolGet(s_slot) != NULL;
    if (alt)
    {
        float rw = GetMouseWheelMove();
        if (rw != 0.0f && overCanvas)
        {
            // Scale about the cursor, so the detail being aligned stays under
            // the pointer. Cell space throughout: find the cursor's cell, scale,
            // then push the offset by however far that cell's image point moved.
            Rectangle c = CanvasRect();
            Vector2 m = GetMousePosition();
            Vector2 cell = { (m.x - c.x) / s_zoom, (m.y - c.y) / s_zoom };
            Rectangle before = RefCellRect();
            float k = 1.0f + rw * 0.08f;
            s_refScale *= k;
            if (s_refScale < 0.05f) s_refScale = 0.05f;
            if (s_refScale > 8.0f)  s_refScale = 8.0f;
            Rectangle after = RefCellRect();
            // Where the cursor sat inside the box before, it must sit after.
            if (before.width > 0.0f && before.height > 0.0f)
            {
                float u = (cell.x - before.x) / before.width;
                float v = (cell.y - before.y) / before.height;
                s_refOff.x += cell.x - (after.x + u * after.width);
                s_refOff.y += cell.y - (after.y + v * after.height);
            }
        }
        if (IsMouseButtonDown(MOUSE_BUTTON_LEFT) && overCanvas)
        {
            Vector2 d = GetMouseDelta();
            s_refOff.x += d.x / s_zoom;     // screen pixels -> cells
            s_refOff.y += d.y / s_zoom;
        }
        s_strokeOpen = false;               // an alt-drag is never a paint stroke
    }

    // Zoom about the cursor, so the cell under it stays put - panning by hand
    // after every zoom step is the thing that makes a pixel editor tiring.
    float wheel = alt ? 0.0f : GetMouseWheelMove();
    if (wheel != 0.0f && overCanvas)
    {
        Vector2 m = GetMousePosition();
        float before = s_zoom;
        s_zoom += wheel * (s_zoom < 8.0f ? 1.0f : 2.0f);
        if (s_zoom < 1.0f)  s_zoom = 1.0f;
        if (s_zoom > 48.0f) s_zoom = 48.0f;
        if (s_zoom != before)
        {
            float k = s_zoom / before;
            s_pan.x = m.x - (m.x - s_pan.x) * k;
            s_pan.y = m.y - (m.y - s_pan.y) * k;
        }
    }

    // Middle-drag pans.
    if (IsMouseButtonDown(MOUSE_BUTTON_MIDDLE))
    {
        Vector2 d = GetMouseDelta();
        s_pan.x += d.x;
        s_pan.y += d.y;
    }

    AnimShapeDef *s = AnimShapePoolGet(s_slot);
    if (!s) return;

    // Painting. Left paints the active ink, right erases - and the undo push
    // happens ONCE on press, so the whole drag is a single step.
    bool left  = !alt && IsMouseButtonDown(MOUSE_BUTTON_LEFT);
    bool right = !alt && IsMouseButtonDown(MOUSE_BUTTON_RIGHT);
    if (!left && !right) s_strokeOpen = false;
    else if (overCanvas)
    {
        int cx, cy;
        if (CellAtCursor(&cx, &cy))
        {
            unsigned char v = right ? ANIM_PX_EMPTY : (unsigned char)s_ink;
            if (StampAt(s, cx, cy, v))
            {
                AnimShapePoolInvalidate(s_slot);
            }
        }
    }
}

// The canvas lives in Gui() (screen space); Draw() runs inside the 320x180
// target and would render it at a sixth of the size. Clear only.
static void Draw()
{
    ClearBackground((Color){ 24, 25, 30, 255 });
}

// ---------------------------------------------------------------------------
//  Canvas, in screen space
// ---------------------------------------------------------------------------
static void DrawCanvas(void)
{
    Rectangle wa = WorkArea();
    DrawRectangleRec(wa, (Color){ 24, 25, 30, 255 });

    AnimShapeDef *s = AnimShapePoolGet(s_slot);
    if (!s)
    {
        GuiLabel((Rectangle){ wa.x + 24.0f, wa.y + 24.0f, wa.width - 48.0f, 24.0f },
                 "No shape selected - name one on the left and press '+ create shape'.");
        return;
    }

    Rectangle c = CanvasRect();

    // Clip to the work area so a zoomed-in canvas cannot spill under the panel.
    BeginScissorMode((int)wa.x, (int)wa.y, (int)wa.width, (int)wa.height);

    // Reference image first, faint, in its own placed box - converted from cell
    // space here and nowhere else, so a traced pixel lands where the reference
    // shows it at any zoom, pan or reference offset.
    Rectangle refCells = RefCellRect();
    Rectangle refScreen = { c.x + refCells.x * s_zoom, c.y + refCells.y * s_zoom,
                            refCells.width * s_zoom,  refCells.height * s_zoom };
    if (s_refLoaded && s_refAlpha > 0.0f)
    {
        Rectangle src = { 0, 0, (float)s_refTex.width, (float)s_refTex.height };
        DrawTexturePro(s_refTex, src, refScreen, (Vector2){ 0, 0 }, 0.0f,
                       Fade(WHITE, s_refAlpha));
    }

    // A dark plate behind the cells, so EMPTY reads as "nothing here" rather
    // than as the page colour - and so the reference shows through only faintly.
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
                          (int)s_zoom + 1, (int)s_zoom + 1,
                          Fade(col, usePreview ? 0.6f : 1.0f));
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

    // While ALT is held the reference is what the mouse acts on, so show its
    // box - otherwise a drag that moves something invisible past the grid edge
    // is impossible to aim.
    if (s_refLoaded &&
        (IsKeyDown(KEY_LEFT_ALT) || IsKeyDown(KEY_RIGHT_ALT)))
        DrawRectangleLinesEx(refScreen, 2.0f, Fade(ORANGE, 0.8f));

    // Cursor cell, so the brush lands where the eye expects.
    int hx, hy;
    if (!s_browseOpen && CellAtCursor(&hx, &hy))
    {
        // Outline every cell the brush would touch, so the disc is visible
        // before it is committed - a 32-wide brush is otherwise a guess.
        int   r    = s_brush / 2;
        float half = (s_brush % 2 == 0) ? 0.5f : 0.0f;
        float rr   = (s_brush * 0.5f) * (s_brush * 0.5f);
        for (int dy = -r; dy <= r; dy++)
            for (int dx = -r; dx <= r; dx++)
            {
                float fx = (float)dx - half, fy = (float)dy - half;
                if (s_brush > 2 && fx*fx + fy*fy > rr) continue;
                int px = hx + dx, py = hy + dy;
                if (px < 0 || py < 0 || px >= s->w || py >= s->h) continue;
                DrawRectangle((int)(c.x + px*s_zoom), (int)(c.y + py*s_zoom),
                              (int)s_zoom + 1, (int)s_zoom + 1, Fade(SKYBLUE, 0.25f));
            }
        DrawRectangleLinesEx((Rectangle){ c.x + hx*s_zoom, c.y + hy*s_zoom,
                                          s_zoom, s_zoom }, 2.0f, SKYBLUE);
        GuiLabel((Rectangle){ wa.x + 12.0f, wa.y + 8.0f, 200.0f, 20.0f },
                 TextFormat("%d, %d", hx, hy));
    }

    EndScissorMode();

    GuiLabel((Rectangle){ wa.x + wa.width - 380.0f, wa.y + 8.0f, 370.0f, 20.0f },
             s_refLoaded
             ? TextFormat("%dx%d   zoom %.0fx   wheel zoom / MMB pan / F fit / ALT ref",
                          s->w, s->h, s_zoom)
             : TextFormat("%dx%d   zoom %.0fx   wheel zoom / MMB pan / F fit",
                          s->w, s->h, s_zoom));
}

// ---------------------------------------------------------------------------
//  Gui: the tool column, then the canvas, then the picker on top
// ---------------------------------------------------------------------------
static void Gui()
{
    Vector2 sc = ScreenStateSize();

    DrawCanvas();                       // behind the panel and the modal

    float x = 12.0f, y = SE_TOP, w = SE_PANEL_W - 24.0f;
    float hw = (w - SE_GAP) * 0.5f;

    GuiPanel((Rectangle){ 0, 0, SE_PANEL_W, sc.y }, NULL);

    // A modal eats input for everything under it.
    bool modal = s_browseOpen || s_newOpen;
    if (modal) GuiLock();

    if (GuiButton((Rectangle){ x, y, w, SE_RH }, "< BACK TO MENU"))
    { AudioPlayButton(); GuiUnlock(); AppStateTransition(&app_state_main_menu); return; }
    y += SE_RH + SE_GAP;

    // -- the scrolling body --------------------------------------------------
    // Everything between BACK and SAVE lives in a scissored, wheel-scrolled box.
    // BACK and SAVE stay pinned outside it: they are the two controls that must
    // never be scrolled out of reach.
    //
    // Scissoring alone is not enough. raygui reads the mouse from the rectangle
    // it is passed, and a scissor only clips the DRAWING - so a control scrolled
    // out of the box still answers clicks aimed at whatever is visible in its
    // place. Hence the lock whenever the cursor is outside the box.
    Vector2 mouse = GetMousePosition();
    float wheel = modal ? 0.0f : GetMouseWheelMove();
    Rectangle body = { 0, y, SE_PANEL_W, sc.y - 26.0f - SE_RH - SE_GAP*2 - y };
    if (body.height < SE_RH) body.height = SE_RH;
    bool overBody = CheckCollisionPointRec(mouse, body);

    // Exempted while a field owns the keyboard: a locked raygui control ignores
    // keys as well as clicks, so locking here would stop a value box accepting
    // digits the moment the cursor drifted onto the canvas.
    bool bodyLock = !modal && !overBody && !Typing();
    if (bodyLock) GuiLock();
    BeginScissorMode((int)body.x, (int)body.y, (int)body.width, (int)body.height);

    float flowTop = y + s_panelScroll;
    y = flowTop;

    // -- the colour-modularity banner ---------------------------------------
    // Not advice to follow, a fact about the format: there is nowhere in a .shp
    // to put a colour. Saying so here is what stops someone hunting for the
    // colour picker that does not exist.
    GuiLabel((Rectangle){ x, y, w, 18.0f }, "Shapes store NO colour - fill");
    y += 15.0f;
    GuiLabel((Rectangle){ x, y, w, 18.0f }, "and rim are tinted by the");
    y += 15.0f;
    GuiLabel((Rectangle){ x, y, w, 18.0f }, "element, animatable in zen.");
    y += 22.0f;

    // -- shape list ----------------------------------------------------------
    // The pool holds up to ANIM_SHAPE_POOL_MAX shapes (8 on web, 512 on desktop),
    // so this cannot be a flowing column - it is a fixed box that scrolls, the
    // same construction as the file picker's list below.
    GuiLine((Rectangle){ x, y, w, 10.0f }, "shapes");
    y += 14.0f;
    GuiLabel((Rectangle){ x, y, w, 16.0f },
             TextFormat("%d / %d slots used", AnimShapePoolCount(), ANIM_SHAPE_POOL_MAX));
    y += 17.0f;
    if (AnimShapePoolSkipped() > 0)
    {
        // The pool filled before every .shp on disk got in. Silence here is how
        // a web build loses shapes it looks like it should have.
        GuiLabel((Rectangle){ x, y, w, 16.0f },
                 TextFormat("! %d file(s) did not fit", AnimShapePoolSkipped()));
        y += 17.0f;
    }

    float rowH = SE_RH + 2.0f;
    Rectangle list = { x, y, w, rowH * SE_LIST_ROWS };
    DrawRectangleRec(list, (Color){ 28, 29, 34, 255 });
    DrawRectangleLinesEx(list, 1.0f, (Color){ 70, 74, 84, 255 });
    bool overList = CheckCollisionPointRec(mouse, list) && overBody;
    if (overList) { s_listScroll += wheel * rowH; wheel = 0.0f; }   // the list eats it
    float listMax = AnimShapePoolCount() * rowH - list.height;
    if (listMax < 0.0f) listMax = 0.0f;
    if (s_listScroll < -listMax) s_listScroll = -listMax;
    if (s_listScroll > 0.0f) s_listScroll = 0.0f;

    // Same reason as the body's lock, one level in: a row straddling the box
    // edge is drawn clipped but still reads the mouse over its FULL height, so
    // its bottom half used to swallow clicks meant for whatever sat below the
    // list - which is how clicking the name field selected a shape instead.
    bool listLock = !GuiIsLocked() && !overList;
    if (listLock) GuiLock();
    // Intersected with the body, and the body's scissor restored after: raylib's
    // EndScissorMode turns the test off rather than popping back to the outer
    // rectangle, so scissors do not nest on their own.
    Rectangle lclip = GetCollisionRec(list, body);
    BeginScissorMode((int)lclip.x, (int)lclip.y, (int)lclip.width, (int)lclip.height);
    float ry = list.y + s_listScroll;
    for (int i = 0; i < ANIM_SHAPE_POOL_MAX; i++)
    {
        AnimShapeDef *sd = AnimShapePoolGet(i);
        if (!sd) continue;
        if (ry + SE_RH >= list.y && ry <= list.y + list.height)
        {
            bool on = (i == s_slot);
            GuiToggle((Rectangle){ list.x + 2.0f, ry, w - 28.0f, SE_RH },
                      TextFormat("%s  %dx%d", sd->name, sd->w, sd->h), &on);
            if (on && i != s_slot) { AudioPlayButton(); SelectSlot(i); }
            if (i == s_slot &&
                GuiButton((Rectangle){ list.x + w - 24.0f, ry, 22.0f, SE_RH }, "#143#"))
            {
                AudioPlayButton();
                AnimShapePoolDelete(i, ANIM_SHAPE_USER_DIR);
                SelectFirst();
                Status("shape deleted");
            }
        }
        ry += rowH;
    }
    EndScissorMode();
    if (listLock) GuiUnlock();
    BeginScissorMode((int)body.x, (int)body.y, (int)body.width, (int)body.height);
    if (bodyLock) GuiLock();       // GuiUnlock is global; put the body's lock back
    y += list.height + SE_GAP;

    if (GuiButton((Rectangle){ x, y, w, SE_RH }, "+ new shape..."))
    { AudioPlayButton(); NewShapeOpen(); }
    y += SE_RH + SE_GAP + 4.0f;

    AnimShapeDef *s = AnimShapePoolGet(s_slot);
    if (s)
    {
        // -- brush -----------------------------------------------------------
        GuiLine((Rectangle){ x, y, w, 10.0f }, "brush  (1/2/3)");
        y += 14.0f;
        float bw = (w - SE_GAP*2) / 3.0f;
        const char *inkName[3] = { "empty", "fill", "rim" };
        for (int i = 0; i < 3; i++)
        {
            bool on = (s_ink == i);
            GuiToggle((Rectangle){ x + i*(bw + SE_GAP), y, bw, SE_RH }, inkName[i], &on);
            if (on) s_ink = i;
        }
        y += SE_RH + 2.0f;

        // Brush size. GuiSlider is float-only, so round to a whole cell count -
        // a 2.5-cell brush has no meaning on a pixel grid.
        float bsz = (float)s_brush;
        GuiSlider((Rectangle){ x + 46.0f, y, w - 46.0f - 30.0f, SE_RH },
                  "size", TextFormat("%d", s_brush), &bsz, 1.0f, 32.0f);
        s_brush = (int)(bsz + 0.5f);
        if (s_brush < 1)  s_brush = 1;
        if (s_brush > 32) s_brush = 32;
        y += SE_RH + 2.0f;
        GuiLabel((Rectangle){ x, y, w, 18.0f }, "right-drag erases   [ ] size");
        y += 20.0f;

        // -- size of the CURRENT shape ---------------------------------------
        GuiLine((Rectangle){ x, y, w, 10.0f }, "size");
        y += 14.0f;
        int cw = s->w, ch = s->h;
        GuiLabel((Rectangle){ x, y, 16.0f, SE_RH }, "w");
        if (GuiValueBox((Rectangle){ x + 18.0f, y, hw - 18.0f, SE_RH }, NULL,
                        &cw, 1, ANIM_SHAPE_GRID_MAX, s_edCurW))
            s_edCurW = !s_edCurW;
        GuiLabel((Rectangle){ x + hw + SE_GAP, y, 16.0f, SE_RH }, "h");
        if (GuiValueBox((Rectangle){ x + hw + SE_GAP + 18.0f, y, hw - 18.0f, SE_RH }, NULL,
                        &ch, 1, ANIM_SHAPE_GRID_MAX, s_edCurH))
            s_edCurH = !s_edCurH;
        // Resize only once the box is committed, or a half-typed "1" of "128"
        // would crop the shape to a single column on the way through.
        if ((cw != s->w || ch != s->h) && !s_edCurW && !s_edCurH)
        { UndoPush(); AnimShapeResize(s, cw, ch); CanvasFit(); }
        y += SE_RH + 2.0f;

        // One cell at a time, each pair sitting under the field it steps. Sized
        // from the FIELD width, not the column width, so the two halves keep
        // their gap instead of colliding in the middle.
        float qw = (hw - 18.0f - SE_GAP) * 0.5f;
        if (GuiButton((Rectangle){ x + 18.0f, y, qw, SE_RH }, "-")) ResizeCur(-1, 0);
        if (GuiButton((Rectangle){ x + 18.0f + qw + SE_GAP, y, qw, SE_RH }, "+")) ResizeCur(1, 0);
        if (GuiButton((Rectangle){ x + hw + SE_GAP + 18.0f, y, qw, SE_RH }, "-")) ResizeCur(0, -1);
        if (GuiButton((Rectangle){ x + hw + SE_GAP + 18.0f + qw + SE_GAP, y, qw, SE_RH }, "+"))
            ResizeCur(0, 1);
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

        // Ink of the WHOLE shape at once - see ConvertAllInk. Sits with clear
        // rather than with the brush: these act on the shape, not on the stroke.
        if (GuiButton((Rectangle){ x, y, hw, SE_RH }, "all -> fill"))
        { AudioPlayButton(); ConvertAllInk(ANIM_PX_FILL); }
        if (GuiButton((Rectangle){ x + hw + SE_GAP, y, hw, SE_RH }, "all -> rim"))
        { AudioPlayButton(); ConvertAllInk(ANIM_PX_OUTLINE); }
        y += SE_RH + SE_GAP;

        if (GuiButton((Rectangle){ x, y, w, SE_RH }, "undo (^Z)")) UndoPop();
        y += SE_RH + SE_GAP + 4.0f;

        // -- reference image -------------------------------------------------
        GuiLine((Rectangle){ x, y, w, 10.0f }, "reference image");
        y += 14.0f;
        if (GuiButton((Rectangle){ x, y, w, SE_RH }, "#5# open image..."))
        { AudioPlayButton(); BrowseOpen(); }
        y += SE_RH + 2.0f;
        GuiLabel((Rectangle){ x, y, w, 18.0f },
                 s_refLoaded ? GetFileName(s_pathBuf) : "or drop a file here");
        y += 20.0f;

        if (s_refLoaded)
        {
            GuiSlider((Rectangle){ x + 34.0f, y, w - 34.0f, SE_RH }, "fade", NULL,
                      &s_refAlpha, 0.0f, 1.0f);
            y += SE_RH + 2.0f;
            GuiSlider((Rectangle){ x + 34.0f, y, w - 34.0f, SE_RH }, "thr", NULL,
                      &s_threshold, 0.0f, 1.0f);
            y += SE_RH + 2.0f;

            // Placement. The sliders and the ALT gestures write the same two
            // values, so either route can finish what the other started.
            GuiSlider((Rectangle){ x + 34.0f, y, w - 34.0f - 34.0f, SE_RH }, "scale",
                      TextFormat("%.2f", s_refScale), &s_refScale, 0.05f, 8.0f);
            y += SE_RH + 2.0f;
            // Offsets range over the grid either way, which is enough to push
            // any part of the image onto any part of the grid.
            float ext = (float)(s->w > s->h ? s->w : s->h);
            GuiSlider((Rectangle){ x + 34.0f, y, w - 34.0f - 34.0f, SE_RH }, "off x",
                      TextFormat("%.1f", s_refOff.x), &s_refOff.x, -ext, ext);
            y += SE_RH + 2.0f;
            GuiSlider((Rectangle){ x + 34.0f, y, w - 34.0f - 34.0f, SE_RH }, "off y",
                      TextFormat("%.1f", s_refOff.y), &s_refOff.y, -ext, ext);
            y += SE_RH + 2.0f;

            float tw = (w - SE_GAP*2) / 3.0f;
            if (GuiButton((Rectangle){ x, y, tw, SE_RH }, "fit"))
            { AudioPlayButton(); RefFit(); }
            if (GuiButton((Rectangle){ x + tw + SE_GAP, y, tw, SE_RH }, "center"))
            { AudioPlayButton(); RefCenter(); }
            if (GuiButton((Rectangle){ x + (tw + SE_GAP)*2, y, tw, SE_RH }, "reset"))
            { AudioPlayButton(); RefFit(); s_refAlpha = 0.35f; s_threshold = 0.5f; }
            y += SE_RH + 2.0f;
            GuiLabel((Rectangle){ x, y, w, 18.0f }, "ALT+drag move   ALT+wheel scale");
            y += 20.0f;
            GuiCheckBox((Rectangle){ x, y, 16.0f, 16.0f }, " preview", &s_previewOn);
            GuiCheckBox((Rectangle){ x + hw + SE_GAP, y, 16.0f, 16.0f },
                        " dark ink", &s_refInvert);
            y += SE_RH;
            if (GuiButton((Rectangle){ x, y, w, SE_RH }, "convert -> pixels"))
            { AudioPlayButton(); ApplyDownsample(); }
            y += SE_RH + SE_GAP;

            if (GuiButton((Rectangle){ x, y, w, SE_RH }, "unload reference"))
            {
                AudioPlayButton();
                UnloadTexture(s_refTex);
                UnloadImage(s_refImg);
                s_refLoaded = false;
                s_previewOn = false;
                Status("reference unloaded");
            }
            y += SE_RH + SE_GAP;
        }
    }

    // -- end of the scrolling body -------------------------------------------
    s_panelContentH = y - flowTop;
    EndScissorMode();
    if (bodyLock) GuiUnlock();

    // Scrolled here rather than before the flow because the content height is
    // only known once the flow has run - which is also why a wheel notch lands
    // on the next frame. Clamping against last frame's height is close enough:
    // the column only changes height when a section appears or disappears.
    float panelMax = s_panelContentH - body.height;
    if (panelMax < 0.0f) panelMax = 0.0f;
    if (wheel != 0.0f && overBody) s_panelScroll += wheel * 40.0f;
    if (s_panelScroll < -panelMax) s_panelScroll = -panelMax;
    if (s_panelScroll > 0.0f) s_panelScroll = 0.0f;

    if (panelMax > 0.0f)
    {
        // A plain indicator, not a grabbable bar: the wheel is the only way to
        // scroll this column and drawing a handle would promise otherwise.
        float frac = body.height / s_panelContentH;
        float bh = body.height * frac;
        float bt = body.y + (-s_panelScroll / s_panelContentH) * body.height;
        DrawRectangle((int)(SE_PANEL_W - 5.0f), (int)bt, 3, (int)bh,
                      (Color){ 110, 116, 130, 200 });
    }

    // -- save bar, PINNED to the bottom of the panel -------------------------
    // Outside the scrolling body on purpose. The body scrolls now, so nothing is
    // unreachable there, but SAVE should never need a scroll to find - anchoring
    // to sc.y keeps it in the same place at any window height.
    if (s)
    {
        float sy = sc.y - 26.0f - SE_RH - SE_GAP;
        if (GuiButton((Rectangle){ x, sy, w, SE_RH },
                      TextFormat("#2# SAVE  %s  (^S)", s->name)))
        {
            AudioPlayButton();
            Status(AnimShapePoolSaveOne(s_slot, ANIM_SHAPE_USER_DIR)
                   ? "saved to shapes/" : "could not write shapes/");
        }
    }

    if (s_statusT > 0.0f && s_status[0])
        GuiLabel((Rectangle){ x, sc.y - 26.0f, SE_PANEL_W - 24.0f, SE_RH }, s_status);

    if (modal) GuiUnlock();
    if (s_browseOpen) BrowseGui();
    if (s_newOpen)    NewShapeGui();
}
