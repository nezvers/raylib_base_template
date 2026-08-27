// ============================================================================
//  map_forge.c  -  FORGE - MAP (see map_forge.h)
//
//  EVERYTHING HERE IS SCREEN SPACE. main.c runs Draw() inside the small
//  letterboxed render target but runs Gui() AFTER EndTextureMode, against the
//  real window. A tool cannot live in 320 pixels, so the whole editor is drawn
//  in Gui() and Draw() is empty. Mouse coordinates are raw GetMousePosition()
//  with NO Screen2Target - that call converts INTO target space, which is
//  exactly the space this state does not use. The asset forge, the shape editor
//  and the showcase all make this same call for this same reason.
//
//  THE VIEWPORT IS A 3D PASS INSIDE Gui(). BeginMode3D is fine here; what is
//  not fine is BeginTextureMode, which does not nest - but Gui() runs outside
//  main.c's texture pass, so even that would be legal. We draw straight to the
//  window inside a scissor rect instead, which is cheaper and needs no target.
//
//  MEMORY. An SgmMap is ~256 KB desktop / ~66 KB Web - still smaller than an
//  SgaAsset, which is why the undo ring here can afford to be 24 deep where the
//  asset forge's is 8. It is still not stack data: s_doc and the ring are
//  file-static, house style.
//
//  UNDO GRANULARITY is the shape editor's stroke rule: a gesture opens on
//  mouse-press and the push happens once, at the moment the first real change
//  lands. One brush stroke is one Ctrl+Z, which is what a person means by
//  "undo that". s_gestureOpen is that latch.
// ============================================================================

#include "raylib.h"
#include "raygui.h"
#include "raymath.h"
#include "rlgl.h"       // rlViewport + an explicit pane-aspect frustum
#include "map_forge.h"
#include "../strategy_map/strategy_map_io.h"
#include "../strategy_map/strategy_map_catalog.h"
#include "../strategy_ui/strategy_ui.h"
#include "../screen_state/screen_state.h"
#include "../audio_state/audio_state.h"
#include "../examples/strategy_test/strategy_models.h"
#include "../examples/strategy_test/strategy_types.h"
#include <string.h>
#include <math.h>

static void Enter();
static void Exit();
static void Update();
static void Draw();
static void Gui();

                        /* Enter, Exit, Update, Draw, Gui, "Name" */
AppState app_state_map_forge = { Enter, Exit, Update, Draw, Gui, "MapForge" };

// ---------------------------------------------------------------------------
//  Metrics. Screen-space pixels, scaled by UiScale() against a 1280 design.
// ---------------------------------------------------------------------------
#define MF_LEFT_W       210.0f      // tool rail
#define MF_RIGHT_W      270.0f      // inspector
#define MF_HEADER_H      52.0f
#define MF_FOOTER_H      54.0f
#define MF_RH            22.0f      // one control row
#define MF_GAP            6.0f
#define MF_PAD           14.0f
#define MF_STATUS_SECS    3.0f

// Deeper than the asset forge's 8: an SgmMap is a fraction of an SgaAsset, and
// painting is a high-frequency edit where two undos is not enough.
#define MF_UNDO_MAX      24

// The size of a NEW map, and the point past which the inspector mentions travel
// time. No longer a capability limit: the runtime reads its extent from the map
// (StrategyWorld.groundHalfX/Z), so any authored size plays at its true size.
// It stays 50 because that is the field the game was tuned around - unit speeds
// and sight ranges all assume roughly this scale.
#define MF_GRID_PLAYABLE  50

// Authoring bounds. The upper bound is the build tier's cap (256 desktop /
// 128 Web), which the .sgm format is pinned to hold.
#define MF_GRID_MIN       16
#define MF_GRID_MAX       SGM_GRID_MAX

// ---------------------------------------------------------------------------
//  Tools
// ---------------------------------------------------------------------------
typedef enum {
    TOOL_TERRAIN = 0,
    TOOL_HEIGHT,
    TOOL_PASSABILITY,
    TOOL_PLACE,
    TOOL_START,
    TOOL_COUNT
} MapTool;

static const char *TOOL_NAME[TOOL_COUNT] = {
    "TERRAIN", "HEIGHT", "PASS", "PLACE", "START"
};

static const char *TERRAIN_NAME[SGM_TERRAIN_COUNT] = {
    "Ground", "Grass", "Dirt", "Shallow", "Water", "Rock", "Cliff", "Void"
};

// The look of each terrain in the viewport. Impassable kinds read darker and
// colder on purpose - "can I walk here" must be legible at a glance, without
// turning the passability overlay on.
static const Color TERRAIN_COL[SGM_TERRAIN_COUNT] = {
    {  96, 116,  84, 255 },     // ground   (the game's own ground green)
    {  84, 124,  72, 255 },     // grass
    { 126, 106,  76, 255 },     // dirt
    {  92, 140, 160, 255 },     // shallow
    {  46,  84, 122, 255 },     // water
    { 104, 104, 110, 255 },     // rock
    {  84,  80,  92, 255 },     // cliff
    {  14,  15,  20, 255 },     // void
};

// One elevation step, in world units. Small: the camera is a fixed-pitch RTS
// view, and tall steps turn a ridge into a wall that hides what is behind it.
#define MF_HEIGHT_STEP  0.35f

// The faction palette. The game ships two colours (blue/red); a map may author
// nine, so the other seven live here until the runtime grows to match.
static const Color FACTION_COL[SGM_FACTIONS_MAX] = {
    {  80, 140, 255, 255 },     // 0 blue   - the game's player colour
    { 230,  70,  70, 255 },     // 1 red    - the game's enemy colour
    {  90, 200, 110, 255 },     // 2 green
    { 235, 190,  70, 255 },     // 3 gold
    { 190, 110, 230, 255 },     // 4 violet
    { 240, 150,  60, 255 },     // 5 orange
    {  80, 210, 210, 255 },     // 6 teal
    { 240, 120, 180, 255 },     // 7 pink
    { 190, 195, 205, 255 },     // 8 silver
};

// ---------------------------------------------------------------------------
//  What PLACE can drop. The map stores family + numeric kind; these tables are
//  the editor's menu of them, and the one place the game's enums are named.
// ---------------------------------------------------------------------------
typedef struct {
    int         family;
    int         kind;
    const char *label;
} PlaceOption;

static const PlaceOption PLACE_OPTION[] = {
    { SGM_PLACE_BUILDING, BLD_TOWN_HALL, "Town Hall" },
    { SGM_PLACE_BUILDING, BLD_HOUSE,     "House"     },
    { SGM_PLACE_BUILDING, BLD_BARRACKS,  "Barracks"  },
    { SGM_PLACE_BUILDING, BLD_LOGGING,   "Logging"   },
    { SGM_PLACE_BUILDING, BLD_QUARRY,    "Quarry"    },
    { SGM_PLACE_BUILDING, BLD_FARM,      "Farm"      },
    { SGM_PLACE_BUILDING, BLD_CHANTRY,   "Chantry"   },
    { SGM_PLACE_BUILDING, BLD_FORESTRY,  "Forestry"  },
    { SGM_PLACE_UNIT,     KIND_WORKER,   "Worker"    },
    { SGM_PLACE_UNIT,     KIND_SOLDIER,  "Soldier"   },
    { SGM_PLACE_UNIT,     KIND_RANGED,   "Ranged"    },
    { SGM_PLACE_UNIT,     KIND_TEMPLAR,  "Templar"   },
    { SGM_PLACE_NODE,     NODE_TREE,     "Trees"     },
    { SGM_PLACE_NODE,     NODE_ROCK,     "Rocks"     },
    { SGM_PLACE_NODE,     NODE_WHEAT,    "Wheat"     },
};
#define PLACE_OPTION_COUNT ((int)(sizeof(PLACE_OPTION)/sizeof(PLACE_OPTION[0])))

// ---------------------------------------------------------------------------
//  State
// ---------------------------------------------------------------------------
static SgmMap   s_doc;                  // the map being edited
static bool     s_opened = false;       // an Open* ran before this Enter
static char     s_file[SGM_NAME_MAX];   // the file this saves over; "" = unsaved
static bool     s_dirty = false;

static int      s_tool = TOOL_TERRAIN;
static int      s_terrain = SGM_TERRAIN_GROUND;
static int      s_brush = 1;            // radius in tiles: 1 = a single tile
static int      s_placeOpt = 0;         // index into PLACE_OPTION
static int      s_faction = 0;          // the faction PLACE and START act on
static bool     s_showPass = false;     // passability overlay

// Camera. Fixed pitch like the game's, but orbitable so an author can look
// behind a cliff.
// Square-on (yaw 0) at roughly the game's own camera pitch, so the map reads
// the way it will be PLAYED. A 45-degree default turned the grid into a
// diamond, which is a worse first impression and harder to line bases up on.
static float    s_yaw = 0.0f, s_pitch = 55.0f, s_zoom = 1.0f;
static Vector2  s_focus = { 0.0f, 0.0f };
static bool     s_orbit = false;

// Hover: which tile the cursor is over, and whether it is over the map at all.
static int      s_hoverX = -1, s_hoverZ = -1;
static bool     s_hoverValid = false;

static int      s_selPlace = -1;        // selected placement, or -1

static char     s_status[128];
static float    s_statusT = 0.0f;
static Color    s_statusCol;

// raygui text boxes are stateful: each needs its own "is being edited" flag.
static bool     s_edName = false, s_edDesc = false, s_edSaveName = false;

static AppState *s_return = NULL;

// Modals. Only one is ever open at a time; ESC closes it.
static bool     s_saveOpen = false;
static char     s_saveBuf[SGM_NAME_MAX];
static bool     s_confirmExit = false;  // unsaved-changes guard
static bool     s_openOpen = false;     // the open-a-map list
static float    s_openScroll = 0.0f;

// Undo ring. Whole-document snapshots: that is heavy in principle but an SgmMap
// is small, and it is the only scheme that survives a resize, which repoints
// every tile in the grid.
static SgmMap   s_undo[MF_UNDO_MAX];
static int      s_undoHead = 0, s_undoCount = 0;
static bool     s_gestureOpen = false;

#define MF_REDO_MAX 8
static SgmMap   s_redo[MF_REDO_MAX];
static int      s_redoCount = 0;

// The world's capacities, as the validator wants them.
static const SgmBudget MF_BUDGET = {
    .buildings = STRAT_MAX_BUILDINGS,
    .units     = STRAT_MAX_UNITS,
    .nodes     = STRAT_MAX_NODES,
};

static SgmReport s_report;

// ---------------------------------------------------------------------------
//  Small helpers
// ---------------------------------------------------------------------------
static void Status(const char *msg, Color c)
{
    TextCopy(s_status, msg);
    s_statusT = MF_STATUS_SECS;
    s_statusCol = c;
}

static void StatusOK(const char *m)   { Status(m, UI_COL_TEXT); }
static void StatusWarn(const char *m) { Status(m, UI_COL_WARN); }

// True while any text field owns the keyboard, so hotkeys stay out of the way.
static bool Typing(void)
{
    return s_edName || s_edDesc || s_edSaveName;
}

static bool ModalOpen(void)
{
    return s_saveOpen || s_confirmExit || s_openOpen;
}

static int ClampInt(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void Revalidate(void)
{
    SgmValidate(&s_doc, MF_BUDGET, BLD_TOWN_HALL, &s_report);
}

// ---------------------------------------------------------------------------
//  Undo
// ---------------------------------------------------------------------------
static void UndoPush(void)
{
    s_undo[s_undoHead] = s_doc;
    s_undoHead = (s_undoHead + 1) % MF_UNDO_MAX;
    if (s_undoCount < MF_UNDO_MAX) s_undoCount++;
    s_redoCount = 0;        // the classic branch rule
}

static void UndoPop(void)
{
    if (s_undoCount <= 0) { StatusWarn("nothing to undo"); return; }

    if (s_redoCount < MF_REDO_MAX) s_redo[s_redoCount++] = s_doc;

    s_undoHead = (s_undoHead - 1 + MF_UNDO_MAX) % MF_UNDO_MAX;
    s_doc = s_undo[s_undoHead];
    s_undoCount--;

    s_selPlace = -1;        // the document changed shape under it
    s_dirty = true;
    Revalidate();
    StatusOK("undo");
}

static void RedoPop(void)
{
    if (s_redoCount <= 0) { StatusWarn("nothing to redo"); return; }

    s_undo[s_undoHead] = s_doc;
    s_undoHead = (s_undoHead + 1) % MF_UNDO_MAX;
    if (s_undoCount < MF_UNDO_MAX) s_undoCount++;

    s_doc = s_redo[--s_redoCount];
    s_selPlace = -1;
    s_dirty = true;
    Revalidate();
    StatusOK("redo");
}

static void UndoClear(void)
{
    s_undoHead = s_undoCount = 0;
    s_redoCount = 0;
    s_gestureOpen = false;
}

static void GestureEnd(void)
{
    if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT) &&
        !IsMouseButtonDown(MOUSE_BUTTON_RIGHT)) s_gestureOpen = false;
}

// A one-shot edit that is its own gesture (a button, a key).
static void Touch(void)
{
    UndoPush();
    s_dirty = true;
    Revalidate();
}

// ---------------------------------------------------------------------------
//  Opening
// ---------------------------------------------------------------------------
static void ResetView(void)
{
    s_tool = TOOL_TERRAIN;
    s_terrain = SGM_TERRAIN_GROUND;
    s_brush = 1;
    s_placeOpt = 0;
    s_faction = 0;
    s_showPass = false;
    s_yaw = 0.0f; s_pitch = 55.0f; s_zoom = 1.0f;
    s_focus = (Vector2){ 0.0f, 0.0f };
    s_orbit = false;
    s_selPlace = -1;
    s_edName = s_edDesc = s_edSaveName = false;
    s_saveOpen = s_confirmExit = s_openOpen = false;
    UndoClear();
    Revalidate();
    s_opened = true;
}

// `stem` if the map folder has no such file, else "stem 2", "stem 3"... So a
// new document never collides with a saved one, and a remix never proposes to
// overwrite its source. Same helper, same shape as the asset forge's.
static void ProposeName(char *out, int cap, const char *stem)
{
    if (SgmMapNameFree(stem)) { TextCopy(out, stem); return; }
    for (int n = 2; n < 999; n++)
    {
        const char *t = TextFormat("%s %d", stem, n);
        if (SgmMapNameFree(t))
        {
            int i = 0;
            for (; (i < cap - 1) && t[i]; i++) out[i] = t[i];
            out[i] = '\0';
            return;
        }
    }
    TextCopy(out, stem);
}

void MapForgeOpenNew(void)
{
    char name[SGM_NAME_MAX];
    ProposeName(name, SGM_NAME_MAX, "new map");

    SgmMapInit(&s_doc, name, MF_GRID_PLAYABLE, MF_GRID_PLAYABLE);
    s_file[0] = '\0';       // never saved: SAVE must prompt for a name
    s_dirty = false;
    ResetView();
}

void MapForgeOpenMap(const SgmMap *m, bool remix)
{
    if (m == NULL) { MapForgeOpenNew(); return; }

    s_doc = *m;             // COPIED - the catalog is rebuilt on every save

    if (remix)
    {
        // A remix must not be able to overwrite its source, so it gets a free
        // name and NO file binding - saving it is therefore a create. Dirty
        // from the start: the copy itself is unsaved work, and without this a
        // remix closed straight away would leave nothing behind with no warning.
        char stem[SGM_NAME_MAX];
        TextCopy(stem, TextFormat("%s copy", m->name));
        ProposeName(s_doc.name, SGM_NAME_MAX, stem);
        s_file[0] = '\0';
        s_dirty = true;
    }
    else
    {
        TextCopy(s_file, m->name);
        s_dirty = false;
    }
    ResetView();
}

bool MapForgeOpenNamed(const char *name, bool remix)
{
    const SgmMap *m = SgmCatalogFind(name);
    if (m == NULL) return false;
    MapForgeOpenMap(m, remix);
    return true;
}

void MapForgeSetReturn(AppState *state) { s_return = state; }

// ---------------------------------------------------------------------------
//  Saving
// ---------------------------------------------------------------------------
static bool SaveAs(const char *name)
{
    if ((name == NULL) || (name[0] == '\0')) { StatusWarn("name required"); return false; }

    TextCopy(s_doc.name, name);
    if (!SgmMapSaveNamed(&s_doc, name)) { StatusWarn("save failed"); return false; }

    // A rename leaves the old file behind; drop it so the catalog does not show
    // both. Only after the new one is safely written.
    if (s_file[0] && (strncmp(s_file, name, SGM_NAME_MAX) != 0)) SgmMapDelete(s_file);

    TextCopy(s_file, name);
    s_dirty = false;
    SgmCatalogReload();
    StatusOK(TextFormat("saved %s", name));
    return true;
}

static void SaveOpen(void)
{
    TextCopy(s_saveBuf, s_file[0] ? s_file : s_doc.name);
    s_saveOpen = true;
    s_edSaveName = true;
}

static void DoSave(void)
{
    if (s_file[0]) SaveAs(s_file);      // known file: straight over it
    else           SaveOpen();          // never saved: prompt
}

static void CloseForge(void)
{
    // The map browser is the only route in, and it always sets the return, so
    // the fallback is a safety net rather than a real path. It points at the
    // browser too: arriving here without a return set still means the forge was
    // opened on some map, and MAPS is where that map is now listed.
    AppStateTransition(s_return ? s_return : &app_state_strategy_showcase);
}

// ---------------------------------------------------------------------------
//  Viewport: camera, picking, drawing
// ---------------------------------------------------------------------------
static Camera3D ForgeCamera(Rectangle vp)
{
    // Frame the whole grid by default.
    //
    // fovy is the VERTICAL field of view, so fitting the map means solving both
    // axes and taking the looser one: the horizontal half-angle shrinks with
    // the viewport's aspect, and a portrait window (or a narrow pane between
    // two panels) would otherwise crop the map's sides off. The map is also
    // tilted away from the camera, so its apparent depth is the grid depth
    // times cos(pitch) plus the height it stands up - close enough to use the
    // larger side for both and let the margin absorb the rest.
    float aspect = (vp.height > 1.0f) ? (vp.width/vp.height) : 1.0f;
    float fovy = 45.0f*DEG2RAD;
    float pitch = s_pitch*DEG2RAD;

    // The map is TILTED away from the camera, so its depth on screen is
    // foreshortened by cos(pitch) - a 50-tile-deep grid seen at 55 degrees
    // covers about 29 tiles vertically, not 50. Budgeting the full depth
    // against the vertical fov is what pushed the camera absurdly far back in
    // a tall narrow pane and left most of it empty.
    float halfV = (float)s_doc.gridH*0.5f*cosf(pitch);
    float halfH = (float)s_doc.gridW*0.5f;

    float distV = halfV/tanf(fovy*0.5f);
    // tan(fovx/2) = tan(fovy/2)*aspect
    float distH = halfH/(tanf(fovy*0.5f)*aspect);

    float dist = ((distV > distH) ? distV : distH)*1.25f*s_zoom;   // 1.25 = margin
    if (dist < 6.0f) dist = 6.0f;

    float yaw = s_yaw*DEG2RAD;

    Vector3 target = { s_focus.x, 0.0f, s_focus.y };
    Vector3 offset = {
        cosf(pitch)*sinf(yaw)*dist,
        sinf(pitch)*dist,
        cosf(pitch)*cosf(yaw)*dist,
    };

    Camera3D cam = { 0 };
    cam.up = (Vector3){ 0.0f, 1.0f, 0.0f };
    cam.fovy = 45.0f;
    cam.projection = CAMERA_PERSPECTIVE;
    cam.target = target;
    cam.position = Vector3Add(target, offset);
    return cam;
}

// Cast the cursor onto the y=0 plane and convert to a tile. Screen space, so
// the ray is built against the VIEWPORT rect rather than the whole window -
// GetScreenToWorldRayEx assumes the camera fills the size it is given, so the
// mouse must be expressed relative to the viewport's own origin.
static bool MouseTile(Rectangle vp, Camera3D cam, int *outX, int *outZ)
{
    Vector2 mp = GetMousePosition();
    if (!CheckCollisionPointRec(mp, vp)) return false;

    Vector2 local = { mp.x - vp.x, mp.y - vp.y };
    Ray ray = GetScreenToWorldRayEx(local, cam, (int)vp.width, (int)vp.height);

    if (fabsf(ray.direction.y) < 0.0001f) return false;
    float t = -ray.position.y/ray.direction.y;
    if (t < 0.0f) return false;

    Vector3 hit = Vector3Add(ray.position, Vector3Scale(ray.direction, t));
    int tx = 0, tz = 0;
    SgmWorldToTile(&s_doc, hit.x, hit.z, &tx, &tz);
    if (!SgmInBounds(&s_doc, tx, tz)) return false;

    *outX = tx;
    *outZ = tz;
    return true;
}

// Is this tile inside the current brush, centred on (cx,cz)? A round brush
// reads as a brush; a square one reads as a fill tool.
static bool InBrush(int x, int z, int cx, int cz)
{
    if (s_brush <= 1) return (x == cx) && (z == cz);
    float dx = (float)(x - cx), dz = (float)(z - cz);
    float r = (float)s_brush - 0.5f;
    return (dx*dx + dz*dz) <= r*r;
}

static float TileTop(const SgmTile *t)
{
    return (float)t->height*MF_HEIGHT_STEP;
}

static void DrawTiles(void)
{
    for (int z = 0; z < s_doc.gridH; z++)
    {
        for (int x = 0; x < s_doc.gridW; x++)
        {
            const SgmTile *t = SgmTileAtConst(&s_doc, x, z);
            if (t->terrain == SGM_TERRAIN_VOID) continue;   // a hole draws nothing

            Vector3 c = SgmTileToWorld(&s_doc, x, z);
            float top = TileTop(t);

            Color col = TERRAIN_COL[t->terrain];

            // The passability overlay tints rather than replaces, so terrain
            // stays readable underneath it.
            if (s_showPass)
            {
                bool pass = (t->flags & SGM_TILE_PASSABLE) != 0;
                bool build = (t->flags & SGM_TILE_BUILDABLE) != 0;
                Color over = pass ? (build ? (Color){ 90, 220, 120, 255 }
                                           : (Color){ 230, 200, 90, 255 })
                                  : (Color){ 230, 80, 80, 255 };
                col = ColorLerp(col, over, 0.45f);
            }

            // A raised tile is a box from the ground up, so the side faces show
            // the elevation. A flat tile is a single quad-thick slab, which is
            // far cheaper and reads identically from this pitch.
            float h = (top > 0.001f) ? top : 0.02f;
            Vector3 centre = { c.x, h*0.5f, c.z };
            DrawCube(centre, 1.0f, h, 1.0f, col);

            // Only outline raised tiles: an outline on every flat tile is the
            // "too aggressive gridlines" complaint all over again.
            if (top > 0.001f)
                DrawCubeWires(centre, 1.0f, h, 1.0f, Fade(BLACK, 0.25f));
        }
    }
}

// The authored grid, drawn at the map's own opacity so the author sees exactly
// what the player will. This is also the renderer the game will use in Phase 5 -
// raylib's DrawGrid cannot do this, which is the whole reason gridlines read as
// too aggressive today.
static float GridStyleAlpha(int style)
{
    switch (style)
    {
        case SGM_GRID_OFF:    return 0.0f;
        case SGM_GRID_SUBTLE: return 0.10f;
        case SGM_GRID_NORMAL: return 0.22f;
        case SGM_GRID_STRONG: return 0.40f;
        default:              return 0.22f;
    }
}

static void DrawMapGrid(void)
{
    float a = GridStyleAlpha(s_doc.gridStyle);
    if (a <= 0.0f) return;

    Color line = Fade((Color){ 220, 230, 245, 255 }, a);
    float y = 0.03f;        // just clear of the tile tops, to avoid z-fighting
    float x0 = -(float)s_doc.gridW*0.5f, x1 = (float)s_doc.gridW*0.5f;
    float z0 = -(float)s_doc.gridH*0.5f, z1 = (float)s_doc.gridH*0.5f;

    for (int x = 0; x <= s_doc.gridW; x++)
    {
        float wx = x0 + (float)x;
        DrawLine3D((Vector3){ wx, y, z0 }, (Vector3){ wx, y, z1 }, line);
    }
    for (int z = 0; z <= s_doc.gridH; z++)
    {
        float wz = z0 + (float)z;
        DrawLine3D((Vector3){ x0, y, wz }, (Vector3){ x1, y, wz }, line);
    }
}

static const StrategyModel *PlaceModel(const SgmPlacement *p)
{
    switch (p->family)
    {
        case SGM_PLACE_BUILDING: return StrategyBuildingModel((BuildingKind)p->kind);
        case SGM_PLACE_UNIT:     return StrategyUnitModel((UnitKind)p->kind);
        case SGM_PLACE_NODE:     return StrategyNodeModel((NodeKind)p->kind);
        default:                 return NULL;
    }
}

static void DrawPlacements(void)
{
    for (int i = 0; i < s_doc.placeCount; i++)
    {
        const SgmPlacement *p = &s_doc.places[i];
        const StrategyModel *model = PlaceModel(p);
        if (model == NULL) continue;

        const SgmTile *t = SgmTileAtConst(&s_doc, p->tileX, p->tileZ);
        Vector3 pos = SgmTileToWorld(&s_doc, p->tileX, p->tileZ);
        if (t != NULL) pos.y = TileTop(t);

        // Neutral placements (nodes, wildlife) go through the game's guarded
        // tint; owned ones use the map's nine-colour palette, which the game
        // does not have yet.
        int faction = (p->faction == SGM_FACTION_NEUTRAL) ? FACTION_NEUTRAL
                                                          : (int)p->faction;
        StrategyModelDraw(model, faction, pos, 0.0f, 1.0f);

        if (i == s_selPlace)
        {
            DrawCircle3D((Vector3){ pos.x, pos.y + 0.05f, pos.z }, 0.75f,
                         (Vector3){ 1.0f, 0.0f, 0.0f }, 90.0f, UI_COL_ACCENT);
        }
    }
}

static void DrawStarts(void)
{
    for (int f = 0; f < s_doc.factionCount; f++)
    {
        int sx = s_doc.starts[f].tileX, sz = s_doc.starts[f].tileZ;
        if (!SgmInBounds(&s_doc, sx, sz)) continue;

        const SgmTile *t = SgmTileAtConst(&s_doc, sx, sz);
        Vector3 c = SgmTileToWorld(&s_doc, sx, sz);
        float top = TileTop(t);
        Color col = FACTION_COL[ClampInt(s_doc.starts[f].colorIndex, 0, SGM_FACTIONS_MAX - 1)];

        // A banner: visible from across the map, and unmistakably not a
        // building. The ring on the ground is what marks the actual tile.
        Vector3 bot = { c.x, top, c.z };
        Vector3 tip = { c.x, top + 2.4f, c.z };
        DrawLine3D(bot, tip, col);
        DrawCube((Vector3){ c.x + 0.35f, top + 2.15f, c.z }, 0.7f, 0.45f, 0.05f, col);
        DrawCircle3D((Vector3){ c.x, top + 0.06f, c.z }, 1.6f,
                     (Vector3){ 1.0f, 0.0f, 0.0f }, 90.0f, col);

        // The faction's index, so nine banners stay tellable apart.
        DrawCube((Vector3){ c.x, top + 0.06f, c.z }, 0.3f, 0.02f, 0.3f, col);
    }
}

static void DrawHoverBrush(void)
{
    if (!s_hoverValid) return;
    if ((s_tool == TOOL_PLACE) || (s_tool == TOOL_START))
    {
        const SgmTile *t = SgmTileAtConst(&s_doc, s_hoverX, s_hoverZ);
        Vector3 c = SgmTileToWorld(&s_doc, s_hoverX, s_hoverZ);
        float top = (t != NULL) ? TileTop(t) : 0.0f;
        DrawCubeWires((Vector3){ c.x, top + 0.05f, c.z }, 1.0f, 0.1f, 1.0f, UI_COL_ACCENT);
        return;
    }

    for (int z = 0; z < s_doc.gridH; z++)
    {
        for (int x = 0; x < s_doc.gridW; x++)
        {
            if (!InBrush(x, z, s_hoverX, s_hoverZ)) continue;
            const SgmTile *t = SgmTileAtConst(&s_doc, x, z);
            Vector3 c = SgmTileToWorld(&s_doc, x, z);
            DrawCubeWires((Vector3){ c.x, TileTop(t) + 0.05f, c.z },
                          1.0f, 0.1f, 1.0f, Fade(UI_COL_ACCENT, 0.85f));
        }
    }
}

static void ViewportDraw(Rectangle vp)
{
    DrawRectangleRec(vp, (Color){ 12, 14, 18, 255 });

    // Scissor so the 3D pass cannot paint over the panels around it.
    BeginScissorMode((int)vp.x, (int)vp.y, (int)vp.width, (int)vp.height);

    // FLUSH FIRST. Anything already batched but not yet drawn would otherwise
    // be flushed later, under the viewport transform set below, and land
    // stretched across the pane. (The asset forge documents the same trap.)
    rlDrawRenderBatchActive();

    // rlViewport aims the 3D pass at the pane. GL's origin is bottom-left,
    // hence the flipped y.
    rlViewport((int)vp.x, (int)(GetScreenHeight() - vp.y - vp.height),
               (int)vp.width, (int)vp.height);

    Camera3D cam = ForgeCamera(vp);

    // BeginMode3D is NOT usable here: it builds the frustum from
    // CORE.Window.currentFbo - the whole WINDOW - and ignores rlViewport
    // entirely. The pane is a different aspect from the window, so the map
    // would be rendered through a wider frustum than the one MouseTile picks
    // with (GetScreenToWorldRayEx, given the pane's size). The two frustums
    // agree only along the pane's centre axis and diverge linearly toward its
    // edges, which is exactly how the cursor-to-tile mismatch presented: right
    // in the middle, progressively wrong at the outskirts.
    //
    // So build the projection explicitly from the PANE's aspect and drive the
    // view matrix by hand. Everything between here and the pop is what
    // BeginMode3D/EndMode3D would have done.
    float aspect = (vp.height > 1.0f) ? (vp.width/vp.height) : 1.0f;

    rlMatrixMode(RL_PROJECTION);
    rlPushMatrix();
    rlLoadIdentity();
    {
        double top   = rlGetCullDistanceNear()*tan(cam.fovy*0.5*DEG2RAD);
        double right = top*aspect;
        rlFrustum(-right, right, -top, top,
                  rlGetCullDistanceNear(), rlGetCullDistanceFar());
    }

    rlMatrixMode(RL_MODELVIEW);
    rlLoadIdentity();
    rlMultMatrixf(MatrixToFloat(MatrixLookAt(cam.position, cam.target, cam.up)));
    rlEnableDepthTest();

        DrawTiles();
        DrawMapGrid();
        DrawPlacements();
        DrawStarts();
        DrawHoverBrush();

    rlDrawRenderBatchActive();
    rlMatrixMode(RL_PROJECTION);
    rlPopMatrix();
    rlMatrixMode(RL_MODELVIEW);
    rlLoadIdentity();
    rlDisableDepthTest();

    // Restore the full-window viewport, or every later Gui() draw lands inside
    // the pane's little rectangle. Flush first, for the same reason as above.
    rlDrawRenderBatchActive();
    rlViewport(0, 0, GetScreenWidth(), GetScreenHeight());
    EndScissorMode();

    DrawRectangleLinesEx(vp, 1.0f, UI_COL_LINE);
}

// ---------------------------------------------------------------------------
//  Viewport input
// ---------------------------------------------------------------------------
// Apply the active tool at (cx,cz). Returns true when it actually changed the
// document, so the caller only opens an undo gesture for real edits - hovering
// with the mouse down over already-painted tiles must not stack undo steps.
static bool ApplyTool(int cx, int cz, bool erase)
{
    bool changed = false;

    switch (s_tool)
    {
        case TOOL_TERRAIN:
        {
            int want = erase ? SGM_TERRAIN_GROUND : s_terrain;
            for (int z = 0; z < s_doc.gridH; z++)
            {
                for (int x = 0; x < s_doc.gridW; x++)
                {
                    if (!InBrush(x, z, cx, cz)) continue;
                    SgmTile *t = SgmTileAt(&s_doc, x, z);
                    if (t->terrain == want) continue;
                    SgmPaintTerrain(&s_doc, x, z, want);
                    changed = true;
                }
            }
        } break;

        case TOOL_HEIGHT:
        {
            int delta = erase ? -1 : +1;
            for (int z = 0; z < s_doc.gridH; z++)
            {
                for (int x = 0; x < s_doc.gridW; x++)
                {
                    if (!InBrush(x, z, cx, cz)) continue;
                    SgmTile *t = SgmTileAt(&s_doc, x, z);
                    int nh = ClampInt((int)t->height + delta, 0, SGM_HEIGHT_MAX);
                    if (nh == t->height) continue;
                    t->height = (uint8_t)nh;
                    changed = true;
                }
            }
        } break;

        case TOOL_PASSABILITY:
        {
            // Left paints PASSABLE+BUILDABLE, right paints blocked. The terrain
            // underneath is untouched - that separation is the whole reason a
            // ford or a walkable ridge can be authored at all.
            uint8_t want = erase ? 0 : (SGM_TILE_PASSABLE | SGM_TILE_BUILDABLE);
            for (int z = 0; z < s_doc.gridH; z++)
            {
                for (int x = 0; x < s_doc.gridW; x++)
                {
                    if (!InBrush(x, z, cx, cz)) continue;
                    SgmTile *t = SgmTileAt(&s_doc, x, z);
                    if (t->flags == want) continue;
                    t->flags = want;
                    changed = true;
                }
            }
        } break;

        case TOOL_PLACE:
        {
            if (erase)
            {
                int hit = SgmPlaceAt(&s_doc, cx, cz);
                if (hit >= 0) { SgmPlaceRemove(&s_doc, hit); s_selPlace = -1; changed = true; }
                break;
            }

            // One placement per tile: stacking two buildings on a tile is never
            // what was meant, and the game would spawn them inside each other.
            if (SgmPlaceAt(&s_doc, cx, cz) >= 0) break;

            const PlaceOption *o = &PLACE_OPTION[s_placeOpt];
            int faction = (o->family == SGM_PLACE_NODE) ? SGM_FACTION_NEUTRAL : s_faction;
            int idx = SgmPlaceAdd(&s_doc, o->family, o->kind, faction, cx, cz);
            if (idx >= 0) { s_selPlace = idx; changed = true; }
            else StatusWarn("map is full");
        } break;

        case TOOL_START:
        {
            if (erase) break;
            if ((s_doc.starts[s_faction].tileX == cx) &&
                (s_doc.starts[s_faction].tileZ == cz)) break;
            s_doc.starts[s_faction].tileX = (int16_t)cx;
            s_doc.starts[s_faction].tileZ = (int16_t)cz;
            changed = true;
        } break;

        default: break;
    }

    return changed;
}

static void ViewportInput(Rectangle vp)
{
    Vector2 mp = GetMousePosition();
    bool over = CheckCollisionPointRec(mp, vp) && !ModalOpen();

    Camera3D cam = ForgeCamera(vp);
    s_hoverValid = over && MouseTile(vp, cam, &s_hoverX, &s_hoverZ);

    // -- orbit / pan --------------------------------------------------------
    // MIDDLE drags orbit, RIGHT+SHIFT pans. Right alone is the erase gesture,
    // so panning takes a modifier rather than stealing it.
    if (over && IsMouseButtonPressed(MOUSE_BUTTON_MIDDLE)) s_orbit = true;
    if (!IsMouseButtonDown(MOUSE_BUTTON_MIDDLE)) s_orbit = false;

    if (s_orbit)
    {
        Vector2 d = GetMouseDelta();
        s_yaw   -= d.x*0.35f;
        s_pitch  = Clamp(s_pitch + d.y*0.25f, 12.0f, 88.0f);
    }

    bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    if (over && shift && IsMouseButtonDown(MOUSE_BUTTON_RIGHT))
    {
        Vector2 d = GetMouseDelta();
        // Pan along the camera's own axes, so dragging right moves the map
        // right whatever the yaw is.
        float yaw = s_yaw*DEG2RAD;
        float k = 0.05f*s_zoom*(float)s_doc.gridW/50.0f;
        s_focus.x -= (d.x*cosf(yaw) - d.y*sinf(yaw))*k;
        s_focus.y -= (d.x*sinf(yaw) + d.y*cosf(yaw))*k;
    }

    if (over)
    {
        float wheel = GetMouseWheelMove();
        if (wheel != 0.0f) s_zoom = Clamp(s_zoom - wheel*0.08f, 0.25f, 2.5f);
    }

    // -- painting -----------------------------------------------------------
    if (!s_hoverValid || s_orbit || shift) { GestureEnd(); return; }

    bool paint = IsMouseButtonDown(MOUSE_BUTTON_LEFT);
    bool erase = IsMouseButtonDown(MOUSE_BUTTON_RIGHT);

    if (paint || erase)
    {
        // Select on press with PLACE, so clicking an existing placement picks
        // it rather than silently refusing to stack another on it.
        if ((s_tool == TOOL_PLACE) && paint && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
        {
            int hit = SgmPlaceAt(&s_doc, s_hoverX, s_hoverZ);
            if (hit >= 0) s_selPlace = hit;
        }

        // THE SNAPSHOT MUST PRECEDE THE EDIT, or undo restores the document
        // as it already is. But the gesture must only open when something
        // really changes - a drag across already-painted tiles must not stack
        // ring slots. So snapshot first, apply, and roll the snapshot back if
        // nothing moved.
        bool opened = s_gestureOpen;
        if (!opened) UndoPush();

        if (ApplyTool(s_hoverX, s_hoverZ, erase && !paint))
        {
            s_gestureOpen = true;
            s_dirty = true;
            Revalidate();
        }
        else if (!opened)
        {
            // Nothing changed: give the ring slot back.
            s_undoHead = (s_undoHead - 1 + MF_UNDO_MAX) % MF_UNDO_MAX;
            if (s_undoCount > 0) s_undoCount--;
        }
    }

    GestureEnd();
}

// ---------------------------------------------------------------------------
//  Left rail: tools and their options
// ---------------------------------------------------------------------------
static void ToolRailGui(Rectangle pane, float s, int fs, int fsSmall)
{
    DrawRectangleRec(pane, UI_COL_PANEL);
    DrawRectangleRec((Rectangle){ pane.x + pane.width - 1.0f, pane.y, 1.0f, pane.height },
                     UI_COL_LINE);

    float pad = MF_PAD*s;
    float x = pane.x + pad;
    float w = pane.width - 2.0f*pad;
    float y = pane.y + pad;
    float rh = MF_RH*s;
    float gap = MF_GAP*s;

    DrawText("TOOL", (int)x, (int)y, fsSmall, UI_COL_TEXT_DIM);
    y += (float)fsSmall + 4.0f*s;

    // One per row: five chips across 210px would be unreadable.
    for (int i = 0; i < TOOL_COUNT; i++)
    {
        Rectangle r = { x, y, w, rh + 4.0f*s };
        const char *labels[1] = { TOOL_NAME[i] };
        if (UiChips(r, labels, 1, (i == s_tool) ? 0 : -1, fs, UI_COL_ACCENT) == 0)
        {
            s_tool = i;
            s_selPlace = -1;
        }
        y += rh + 4.0f*s + 3.0f*s;
    }

    y += gap;
    DrawRectangleRec((Rectangle){ x, y, w, 1.0f }, UI_COL_LINE);
    y += gap;

    // -- per-tool options ---------------------------------------------------
    if ((s_tool == TOOL_TERRAIN) || (s_tool == TOOL_HEIGHT) || (s_tool == TOOL_PASSABILITY))
    {
        DrawText("BRUSH", (int)x, (int)y, fsSmall, UI_COL_TEXT_DIM);
        y += (float)fsSmall + 4.0f*s;

        float bf = (float)s_brush;
        Rectangle sr = { x, y, w, rh };
        if (UiSlider(sr, "size", &bf, 1.0f, 8.0f, fsSmall)) s_brush = (int)(bf + 0.5f);
        y += rh + gap;
    }

    if (s_tool == TOOL_TERRAIN)
    {
        DrawText("TERRAIN", (int)x, (int)y, fsSmall, UI_COL_TEXT_DIM);
        y += (float)fsSmall + 4.0f*s;

        for (int i = 0; i < SGM_TERRAIN_COUNT; i++)
        {
            Rectangle r = { x, y, w, rh };
            bool on = (i == s_terrain);
            bool hot = !UiModalBlocks() && CheckCollisionPointRec(GetMousePosition(), r);

            DrawRectangleRec(r, on ? Fade(UI_COL_ACCENT, 0.20f)
                                   : (hot ? UI_COL_PANEL_HI : UI_COL_PANEL));
            DrawRectangleLinesEx(r, 1.0f, on ? UI_COL_ACCENT
                                             : (hot ? UI_COL_LINE_HI : UI_COL_LINE));
            // A swatch, because the name alone does not say what it looks like.
            DrawRectangleRec((Rectangle){ r.x + 4.0f*s, r.y + 4.0f*s,
                                          14.0f*s, r.height - 8.0f*s }, TERRAIN_COL[i]);
            DrawText(TERRAIN_NAME[i], (int)(r.x + 24.0f*s),
                     (int)(r.y + (r.height - (float)fsSmall)*0.5f), fsSmall,
                     on ? UI_COL_TEXT : UI_COL_TEXT_DIM);

            UiTip(r, SgmTerrainBlocks(i) ? "impassable" : "walkable");
            if (hot && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
            { AudioPlayButton(); s_terrain = i; }

            y += rh + 2.0f*s;
        }
    }
    else if (s_tool == TOOL_HEIGHT)
    {
        DrawText("left raises, right lowers", (int)x, (int)y, fsSmall, UI_COL_TEXT_DIM);
        y += (float)fsSmall + gap;
    }
    else if (s_tool == TOOL_PASSABILITY)
    {
        DrawText("left = walkable", (int)x, (int)y, fsSmall, UI_COL_TEXT_DIM);
        y += (float)fsSmall + 2.0f*s;
        DrawText("right = blocked", (int)x, (int)y, fsSmall, UI_COL_TEXT_DIM);
        y += (float)fsSmall + gap;
    }
    else if (s_tool == TOOL_PLACE)
    {
        DrawText("PLACE", (int)x, (int)y, fsSmall, UI_COL_TEXT_DIM);
        y += (float)fsSmall + 4.0f*s;

        for (int i = 0; i < PLACE_OPTION_COUNT; i++)
        {
            Rectangle r = { x, y, w, rh };
            bool on = (i == s_placeOpt);
            bool hot = !UiModalBlocks() && CheckCollisionPointRec(GetMousePosition(), r);

            DrawRectangleRec(r, on ? Fade(UI_COL_ACCENT, 0.20f)
                                   : (hot ? UI_COL_PANEL_HI : UI_COL_PANEL));
            DrawRectangleLinesEx(r, 1.0f, on ? UI_COL_ACCENT
                                             : (hot ? UI_COL_LINE_HI : UI_COL_LINE));
            DrawText(PLACE_OPTION[i].label, (int)(r.x + 6.0f*s),
                     (int)(r.y + (r.height - (float)fsSmall)*0.5f), fsSmall,
                     on ? UI_COL_TEXT : UI_COL_TEXT_DIM);

            if (hot && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
            { AudioPlayButton(); s_placeOpt = i; }

            y += rh + 2.0f*s;
        }
    }
    else if (s_tool == TOOL_START)
    {
        DrawText("click to move the", (int)x, (int)y, fsSmall, UI_COL_TEXT_DIM);
        y += (float)fsSmall + 2.0f*s;
        DrawText("selected faction's start", (int)x, (int)y, fsSmall, UI_COL_TEXT_DIM);
        y += (float)fsSmall + gap;
    }
}

// ---------------------------------------------------------------------------
//  Right pane: map properties, factions, budget, validation
// ---------------------------------------------------------------------------
static void InspectorGui(Rectangle pane, float s, int fs, int fsSmall)
{
    DrawRectangleRec(pane, UI_COL_PANEL);
    DrawRectangleRec((Rectangle){ pane.x, pane.y, 1.0f, pane.height }, UI_COL_LINE);

    float pad = MF_PAD*s;
    float x = pane.x + pad;
    float w = pane.width - 2.0f*pad;
    float y = pane.y + pad;
    float rh = MF_RH*s;
    float gap = MF_GAP*s;

    // -- identity -----------------------------------------------------------
    DrawText("MAP", (int)x, (int)y, fsSmall, UI_COL_TEXT_DIM);
    y += (float)fsSmall + 4.0f*s;

    GuiSetStyle(DEFAULT, TEXT_SIZE, fs);
    if (GuiTextBox((Rectangle){ x, y, w, rh }, s_doc.name, SGM_NAME_MAX, s_edName))
    { s_edName = !s_edName; s_dirty = true; }
    y += rh + 4.0f*s;

    if (GuiTextBox((Rectangle){ x, y, w, rh }, s_doc.desc, SGM_DESC_MAX, s_edDesc))
    { s_edDesc = !s_edDesc; s_dirty = true; }
    y += rh + gap;

    // -- extent -------------------------------------------------------------
    DrawText(TextFormat("SIZE  %d x %d", s_doc.gridW, s_doc.gridH),
             (int)x, (int)y, fsSmall, UI_COL_TEXT_DIM);
    y += (float)fsSmall + 4.0f*s;
    {
        float gw = (float)s_doc.gridW;
        Rectangle r = { x, y, w, rh };
        // Full authoring range: the runtime only plays 50x50 today, but large
        // maps must be authorable now so pathfinding has something to stress.
        if (UiSlider(r, "tiles", &gw, (float)MF_GRID_MIN, (float)MF_GRID_MAX, fsSmall))
        {
            int n = ClampInt((int)(gw + 0.5f), MF_GRID_MIN, MF_GRID_MAX);
            if (n != s_doc.gridW)
            {
                Touch();
                SgmMapResize(&s_doc, n, n);
                Revalidate();
            }
        }
        y += rh + gap;

        // The runtime now takes its extent from the map (world.groundHalf*), so
        // a large map plays at its authored size and there is nothing to warn
        // about. What IS worth saying is that a big field is a long walk, since
        // movement is still a straight-line lerp with no pathfinding.
        if (s_doc.gridW > MF_GRID_PLAYABLE || s_doc.gridH > MF_GRID_PLAYABLE)
        {
            DrawText("large field - long travel times", (int)x, (int)y,
                     fsSmall, UI_COL_TEXT_DIM);
            y += (float)fsSmall + 4.0f*s;
        }
    }

    // -- factions -----------------------------------------------------------
    DrawRectangleRec((Rectangle){ x, y, w, 1.0f }, UI_COL_LINE);
    y += gap;

    DrawText(TextFormat("FACTIONS  %d", s_doc.factionCount),
             (int)x, (int)y, fsSmall, UI_COL_TEXT_DIM);
    y += (float)fsSmall + 4.0f*s;
    {
        float fc = (float)s_doc.factionCount;
        Rectangle r = { x, y, w, rh };
        if (UiSlider(r, "count", &fc, 1.0f, (float)SGM_FACTIONS_MAX, fsSmall))
        {
            int n = ClampInt((int)(fc + 0.5f), 1, SGM_FACTIONS_MAX);
            if (n != s_doc.factionCount)
            {
                Touch();
                s_doc.factionCount = n;
                if (s_faction >= n) s_faction = n - 1;
                Revalidate();
            }
        }
        y += rh + 4.0f*s;
    }

    // Which faction PLACE and START act on. A swatch grid: nine chips of text
    // would not fit, and colour is how they are told apart on the map anyway.
    {
        float cw = (w - 8.0f*4.0f)/5.0f;
        for (int f = 0; f < s_doc.factionCount; f++)
        {
            int row = f/5, col = f%5;
            Rectangle r = { x + (float)col*(cw + 8.0f), y + (float)row*(rh + 4.0f*s), cw, rh };
            bool on = (f == s_faction);
            bool hot = !UiModalBlocks() && CheckCollisionPointRec(GetMousePosition(), r);

            DrawRectangleRec(r, FACTION_COL[f]);
            DrawRectangleLinesEx(r, on ? 2.0f : 1.0f,
                                 on ? UI_COL_TEXT : (hot ? UI_COL_LINE_HI : UI_COL_LINE));
            const char *num = TextFormat("%d", f + 1);
            DrawText(num, (int)(r.x + (cw - (float)MeasureText(num, fsSmall))*0.5f),
                     (int)(r.y + (rh - (float)fsSmall)*0.5f), fsSmall, BLACK);

            UiTip(r, TextFormat("faction %d", f + 1));
            if (hot && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
            { AudioPlayButton(); s_faction = f; }
        }
        int rows = (s_doc.factionCount + 4)/5;
        y += (float)rows*(rh + 4.0f*s) + gap;
    }

    // -- budget -------------------------------------------------------------
    // Shown as it FILLS rather than reported at load: the node cap in
    // particular is one authors hit (the shipped map already uses 32 of 48).
    DrawRectangleRec((Rectangle){ x, y, w, 1.0f }, UI_COL_LINE);
    y += gap;
    DrawText("BUDGET", (int)x, (int)y, fsSmall, UI_COL_TEXT_DIM);
    y += (float)fsSmall + 4.0f*s;
    {
        struct { const char *label; int used, cap; } rows[3] = {
            { "buildings", SgmPlaceCountOf(&s_doc, SGM_PLACE_BUILDING), MF_BUDGET.buildings },
            { "units",     SgmPlaceCountOf(&s_doc, SGM_PLACE_UNIT),     MF_BUDGET.units     },
            { "nodes",     SgmPlaceCountOf(&s_doc, SGM_PLACE_NODE),     MF_BUDGET.nodes     },
        };
        for (int i = 0; i < 3; i++)
        {
            float frac = (rows[i].cap > 0) ? (float)rows[i].used/(float)rows[i].cap : 0.0f;
            if (frac > 1.0f) frac = 1.0f;
            Color bar = (rows[i].used > rows[i].cap) ? UI_COL_WARN
                      : (frac > 0.8f) ? (Color){ 235, 200, 90, 255 }
                                      : UI_COL_ACCENT;

            DrawRectangleRec((Rectangle){ x, y, w, rh*0.62f }, UI_COL_BG);
            DrawRectangleRec((Rectangle){ x, y, w*frac, rh*0.62f }, Fade(bar, 0.55f));
            DrawText(TextFormat("%s  %d/%d", rows[i].label, rows[i].used, rows[i].cap),
                     (int)(x + 4.0f*s), (int)(y + 1.0f*s), fsSmall, UI_COL_TEXT);
            y += rh*0.62f + 3.0f*s;
        }
        y += gap;
    }

    // -- grid look ----------------------------------------------------------
    DrawRectangleRec((Rectangle){ x, y, w, 1.0f }, UI_COL_LINE);
    y += gap;
    DrawText("GRIDLINES", (int)x, (int)y, fsSmall, UI_COL_TEXT_DIM);
    y += (float)fsSmall + 4.0f*s;
    {
        const char *labels[SGM_GRID_STYLE_COUNT] = { "Off", "Subtle", "Normal", "Strong" };
        int picked = UiChips((Rectangle){ x, y, w, rh }, labels, SGM_GRID_STYLE_COUNT,
                             s_doc.gridStyle, fsSmall, UI_COL_ACCENT);
        if ((picked >= 0) && (picked != s_doc.gridStyle))
        {
            Touch();
            s_doc.gridStyle = picked;
        }
        y += rh + gap;
    }

    // -- validation ---------------------------------------------------------
    DrawRectangleRec((Rectangle){ x, y, w, 1.0f }, UI_COL_LINE);
    y += gap;

    if (s_report.errors == 0)
    {
        DrawText((s_report.count > 0) ? "PLAYABLE (with notes)" : "PLAYABLE",
                 (int)x, (int)y, fsSmall, (Color){ 120, 220, 140, 255 });
    }
    else
    {
        DrawText(TextFormat("NOT PLAYABLE (%d)", s_report.errors),
                 (int)x, (int)y, fsSmall, UI_COL_WARN);
    }
    y += (float)fsSmall + 4.0f*s;

    for (int i = 0; (i < s_report.count) && (y < pane.y + pane.height - rh); i++)
    {
        const SgmIssue *is = &s_report.items[i];
        Color c = is->warning ? UI_COL_TEXT_DIM : UI_COL_WARN;
        const char *line = (is->faction >= 0)
            ? TextFormat("- f%d: %s", is->faction + 1, SgmIssueText(is))
            : TextFormat("- %s", SgmIssueText(is));
        DrawText(line, (int)x, (int)y, fsSmall, c);
        y += (float)fsSmall + 2.0f*s;
    }
}

// ---------------------------------------------------------------------------
//  Header / footer
// ---------------------------------------------------------------------------
static void HeaderGui(Rectangle bar, float s, int fs, int fsSmall)
{
    DrawRectangleRec(bar, UI_COL_PANEL);
    DrawRectangleRec((Rectangle){ 0.0f, bar.height - 1.0f, bar.width, 1.0f }, UI_COL_LINE);

    float pad = MF_PAD*s;
    DrawText("FORGE - MAP", (int)pad, (int)(bar.y + 8.0f*s), fs, UI_COL_ACCENT);

    float x = pad + (float)MeasureText("FORGE - MAP", fs) + 12.0f*s;
    DrawText(TextFormat("%s%s", s_doc.name, s_dirty ? " *" : ""),
             (int)x, (int)(bar.y + 9.0f*s), fsSmall, UI_COL_TEXT);

    // Hover readout: which tile, what is on it. The only place the author can
    // read exact coordinates, which matters when lining two bases up.
    if (s_hoverValid)
    {
        const SgmTile *t = SgmTileAtConst(&s_doc, s_hoverX, s_hoverZ);
        const char *info = TextFormat("tile %d,%d   %s   h%d   %s",
                                      s_hoverX, s_hoverZ,
                                      TERRAIN_NAME[t->terrain], t->height,
                                      (t->flags & SGM_TILE_PASSABLE) ? "walkable" : "blocked");
        DrawText(info, (int)pad, (int)(bar.y + 8.0f*s + (float)fs + 4.0f*s),
                 fsSmall, UI_COL_TEXT_DIM);
    }

    // Status, right-aligned.
    if (s_statusT > 0.0f)
    {
        int tw = MeasureText(s_status, fsSmall);
        DrawText(s_status, (int)(bar.width - pad - (float)tw),
                 (int)(bar.y + 9.0f*s), fsSmall, s_statusCol);
    }
}

static void FooterGui(Rectangle bar, float s, int fs, int fsSmall)
{
    DrawRectangleRec(bar, UI_COL_PANEL);
    DrawRectangleRec((Rectangle){ 0.0f, bar.y, bar.width, 1.0f }, UI_COL_LINE);

    float pad = MF_PAD*s;
    float bh = 26.0f*s;
    float by = bar.y + (bar.height - bh)*0.5f;
    float bw = 82.0f*s;
    float gap = MF_GAP*s;
    float x = pad;

    if (UiButton((Rectangle){ x, by, bw, bh }, "NEW", true,
                 "Start a new blank map.", fsSmall))
    {
        MapForgeOpenNew();
        StatusOK("new map");
        return;
    }
    x += bw + gap;

    if (UiButton((Rectangle){ x, by, bw, bh }, "OPEN", true,
                 "Open a saved map.", fsSmall))
    {
        SgmCatalogReload();
        s_openOpen = true;
        s_openScroll = 0.0f;
    }
    x += bw + gap;

    if (UiButtonEx((Rectangle){ x, by, bw, bh }, "SAVE", true,
                   "Save this map (Ctrl+S).", fsSmall, true)) DoSave();
    x += bw + gap;

    if (UiButton((Rectangle){ x, by, bw + 20.0f*s, bh }, "SAVE AS", true,
                 "Save under a new name.", fsSmall)) SaveOpen();
    x += bw + 20.0f*s + gap;

    // Passability overlay: the one view toggle that changes what the author is
    // looking at rather than where they look from.
    if (UiButtonEx((Rectangle){ x, by, bw + 30.0f*s, bh }, "PASS VIEW", true,
                   "Tint tiles by walkable / buildable / blocked.", fsSmall, s_showPass))
        s_showPass = !s_showPass;
    x += bw + 30.0f*s + gap;

    if (UiButton((Rectangle){ x, by, bw, bh }, "UNDO", s_undoCount > 0,
                 "Undo (Ctrl+Z).", fsSmall)) UndoPop();
    x += bw + gap;

    if (UiButton((Rectangle){ x, by, bw, bh }, "REDO", s_redoCount > 0,
                 "Redo (Ctrl+Y).", fsSmall)) RedoPop();

    // BACK, right-aligned: leaving is not something to hit by accident.
    float backW = 90.0f*s;
    if (UiButton((Rectangle){ bar.width - pad - backW, by, backW, bh }, "BACK", true,
                 "Leave the forge.", fsSmall))
    {
        if (s_dirty) s_confirmExit = true;
        else         CloseForge();
    }
    (void)fs;
}

// ---------------------------------------------------------------------------
//  Modals
// ---------------------------------------------------------------------------
static void ModalsGui(float s, int fs, int fsSmall)
{
    if (!ModalOpen()) return;

    UiSetInModal(true);

    float bh = 26.0f*s;
    float gap = MF_GAP*s;

    if (s_saveOpen)
    {
        Rectangle m = UiModalFrame("SAVE MAP AS", "The file name is the map's identity.",
                                   s, fs, 380.0f*s, 150.0f*s);
        float x = m.x + 16.0f*s;
        float w = m.width - 32.0f*s;
        float y = m.y + 62.0f*s;

        GuiSetStyle(DEFAULT, TEXT_SIZE, fs);
        if (GuiTextBox((Rectangle){ x, y, w, bh }, s_saveBuf, SGM_NAME_MAX, s_edSaveName))
            s_edSaveName = !s_edSaveName;
        y += bh + gap;

        if (UiButtonEx((Rectangle){ x, y, 110.0f*s, bh }, "SAVE", s_saveBuf[0] != '\0',
                       NULL, fsSmall, true))
        {
            if (SaveAs(s_saveBuf)) { s_saveOpen = false; s_edSaveName = false; }
        }
        if (UiButton((Rectangle){ x + 118.0f*s, y, 110.0f*s, bh }, "CANCEL", true,
                     NULL, fsSmall))
        { s_saveOpen = false; s_edSaveName = false; }
    }
    else if (s_confirmExit)
    {
        Rectangle m = UiModalFrame("UNSAVED CHANGES",
                                   "Leave the forge without saving?", s, fs,
                                   380.0f*s, 130.0f*s);
        float x = m.x + 16.0f*s;
        float y = m.y + 70.0f*s;

        if (UiButtonEx((Rectangle){ x, y, 110.0f*s, bh }, "SAVE", true, NULL, fsSmall, true))
        {
            if (s_file[0])
            {
                if (SaveAs(s_file)) { UiSetInModal(false); CloseForge(); return; }
            }
            else { s_confirmExit = false; SaveOpen(); }
        }
        if (UiButton((Rectangle){ x + 118.0f*s, y, 110.0f*s, bh }, "DISCARD", true,
                     NULL, fsSmall))
        { s_confirmExit = false; UiSetInModal(false); CloseForge(); return; }
        if (UiButton((Rectangle){ x + 236.0f*s, y, 110.0f*s, bh }, "CANCEL", true,
                     NULL, fsSmall))
            s_confirmExit = false;
    }
    else if (s_openOpen)
    {
        Rectangle m = UiModalFrame("OPEN MAP", NULL, s, fs, 420.0f*s, 320.0f*s);
        float x = m.x + 16.0f*s;
        float w = m.width - 32.0f*s;
        float y = m.y + 46.0f*s;

        int count = SgmCatalogCount();
        const SgmMap *maps = SgmCatalogMaps();

        if (count == 0)
        {
            DrawText("no maps saved yet", (int)x, (int)y, fsSmall, UI_COL_TEXT_DIM);
        }
        else
        {
            Rectangle list = { x, y, w, m.height - 100.0f*s };
            BeginScissorMode((int)list.x, (int)list.y, (int)list.width, (int)list.height);
            UiSetClip(list);

            float rowH = 28.0f*s;
            float ry = list.y - s_openScroll;
            for (int i = 0; i < count; i++)
            {
                Rectangle r = { list.x, ry, list.width, rowH - 3.0f*s };
                if ((r.y + r.height >= list.y) && (r.y <= list.y + list.height))
                {
                    bool hot = UiClipAllows(GetMousePosition()) &&
                               CheckCollisionPointRec(GetMousePosition(), r);
                    DrawRectangleRec(r, hot ? UI_COL_PANEL_HI : UI_COL_BG);
                    DrawRectangleLinesEx(r, 1.0f, hot ? UI_COL_LINE_HI : UI_COL_LINE);
                    DrawText(maps[i].name, (int)(r.x + 8.0f*s),
                             (int)(r.y + (r.height - (float)fsSmall)*0.5f),
                             fsSmall, UI_COL_TEXT);
                    DrawText(TextFormat("%dx%d  %df", maps[i].gridW, maps[i].gridH,
                                        maps[i].factionCount),
                             (int)(r.x + r.width - 90.0f*s),
                             (int)(r.y + (r.height - (float)fsSmall)*0.5f),
                             fsSmall, UI_COL_TEXT_DIM);

                    if (hot && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
                    {
                        AudioPlayButton();
                        // Copy BEFORE closing: MapForgeOpenMap resets the view,
                        // and the catalog pointer must still be alive here.
                        // Never a remix - this list is "open the file I saved",
                        // so it stays bound to that file and SAVE writes over it.
                        MapForgeOpenMap(&maps[i], false);
                        s_openOpen = false;
                        StatusOK("opened");
                        UiSetClip((Rectangle){ 0 });
                        EndScissorMode();
                        UiSetInModal(false);
                        return;
                    }
                }
                ry += rowH;
            }

            UiSetClip((Rectangle){ 0 });
            EndScissorMode();

            if (CheckCollisionPointRec(GetMousePosition(), list))
            {
                float content = (float)count*rowH;
                s_openScroll -= GetMouseWheelMove()*30.0f;
                float maxScroll = content - list.height;
                if (maxScroll < 0.0f) maxScroll = 0.0f;
                s_openScroll = Clamp(s_openScroll, 0.0f, maxScroll);
            }
        }

        if (UiButton((Rectangle){ x, m.y + m.height - 42.0f*s, 110.0f*s, bh },
                     "CANCEL", true, NULL, fsSmall))
            s_openOpen = false;
    }

    UiSetInModal(false);
}

// ---------------------------------------------------------------------------
//  Keyboard
// ---------------------------------------------------------------------------
static void Keyboard(void)
{
    if (Typing()) return;

    bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);

    if (ctrl && IsKeyPressed(KEY_S)) { if (!ModalOpen()) DoSave(); return; }
    if (ctrl && IsKeyPressed(KEY_Z)) { UndoPop(); return; }
    if (ctrl && IsKeyPressed(KEY_Y)) { RedoPop(); return; }

    if (IsKeyPressed(KEY_ESCAPE))
    {
        if (ModalOpen()) { s_saveOpen = s_confirmExit = s_openOpen = false; return; }
        if (s_dirty) s_confirmExit = true;
        else         CloseForge();
        return;
    }

    if (ModalOpen()) return;

    // Tool hotkeys, in rail order.
    if (IsKeyPressed(KEY_ONE))   s_tool = TOOL_TERRAIN;
    if (IsKeyPressed(KEY_TWO))   s_tool = TOOL_HEIGHT;
    if (IsKeyPressed(KEY_THREE)) s_tool = TOOL_PASSABILITY;
    if (IsKeyPressed(KEY_FOUR))  s_tool = TOOL_PLACE;
    if (IsKeyPressed(KEY_FIVE))  s_tool = TOOL_START;

    if (IsKeyPressed(KEY_P)) s_showPass = !s_showPass;

    // Brush size on the bracket keys, the convention every paint tool uses.
    if (IsKeyPressed(KEY_LEFT_BRACKET))  s_brush = ClampInt(s_brush - 1, 1, 8);
    if (IsKeyPressed(KEY_RIGHT_BRACKET)) s_brush = ClampInt(s_brush + 1, 1, 8);

    // DELETE removes the selected placement.
    if (IsKeyPressed(KEY_DELETE) && (s_selPlace >= 0))
    {
        Touch();
        SgmPlaceRemove(&s_doc, s_selPlace);
        s_selPlace = -1;
        Revalidate();
    }
}

// ---------------------------------------------------------------------------
//  AppState hooks
// ---------------------------------------------------------------------------
static void Enter()
{
    ScreenState *ss = ScreenStateGet();
    ss->clear_color = UI_COL_BG;

    SgmCatalogLoad();

    // Normally opened through MapForgeOpen*, which runs BEFORE the transition.
    // Reaching Enter with nothing opened means something jumped here directly -
    // give it a blank map rather than whatever the last session left behind.
    if (!s_opened) MapForgeOpenNew();

    s_statusT = 0.0f;
    Revalidate();
}

static void Exit()
{
    // Cleared so the NEXT Enter cannot silently reopen this document. Every
    // real entry sets it again through an Open* call.
    s_opened = false;
}

static void Update()
{
    if (s_statusT > 0.0f) s_statusT -= GetFrameTime();
}

static void Draw()
{
    // Deliberately empty: everything is screen space, drawn in Gui().
    // See the file header.
}

static void Gui()
{
    Vector2 screen = ScreenStateSize();
    float s = UiScale();
    int fs      = (int)(14.0f*s);
    int fsSmall = (int)(11.0f*s);
    if (fs < 10) fs = 10;
    if (fsSmall < 8) fsSmall = 8;

    // Publish this frame's modal state BEFORE any widget - every one of them
    // gates its hit testing on it.
    UiFrameBegin(ModalOpen());

    DrawRectangle(0, 0, (int)screen.x, (int)screen.y, UI_COL_BG);

    float headerH = MF_HEADER_H*s;
    float footerH = MF_FOOTER_H*s;
    float leftW   = MF_LEFT_W*s;
    float rightW  = MF_RIGHT_W*s;

    Rectangle header = { 0.0f, 0.0f, screen.x, headerH };
    Rectangle footer = { 0.0f, screen.y - footerH, screen.x, footerH };
    Rectangle left   = { 0.0f, headerH, leftW, screen.y - headerH - footerH };
    Rectangle right  = { screen.x - rightW, headerH, rightW, screen.y - headerH - footerH };
    Rectangle vp     = { leftW, headerH, screen.x - leftW - rightW,
                         screen.y - headerH - footerH };

    // Input BEFORE draw, so the hover highlight matches the frame it is drawn
    // on rather than lagging it by one.
    if (!ModalOpen()) ViewportInput(vp);
    else              s_hoverValid = false;

    ViewportDraw(vp);
    ToolRailGui(left, s, fs, fsSmall);
    InspectorGui(right, s, fs, fsSmall);
    HeaderGui(header, s, fs, fsSmall);
    FooterGui(footer, s, fs, fsSmall);

    // A transition may have happened inside the footer (BACK) - anything drawn
    // after it would land on top of the frame the NEW state already painted.
    if (!AppStateIsCurrent(&app_state_map_forge)) return;

    ModalsGui(s, fs, fsSmall);
    Keyboard();

    if (!AppStateIsCurrent(&app_state_map_forge)) return;

    UiTipDraw(fsSmall);
}
