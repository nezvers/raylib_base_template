// ============================================================================
//  strategy_showcase.c  -  gallery / inspector / map for the strategy assets
//
//  EVERYTHING IS DRAWN IN Gui(), AND THAT IS THE WHOLE LAYOUT STORY.
//  main.c runs Draw() inside BeginTextureMode on the 320x180 letterboxed
//  target (screen_state.c: 1920/6) and runs Gui() AFTER EndTextureMode against
//  the real window. A wall of rotating 3D previews at 320x180 is mush, so the
//  entire screen is built in Gui() at window resolution and Draw() only clears.
//  The shape editor made the same call for the same reason.
//
//  Mouse coordinates are therefore raw GetMousePosition() with NO
//  Screen2Target - that call converts INTO target space, the space this state
//  deliberately does not use.
//
//  Because Gui() runs outside main.c's render-texture pass, this state can own
//  a full-resolution RenderTexture2D and BeginTextureMode into it there.
//  BeginTextureMode does not nest, so that is only ever safe in Gui().
//
//  THE MODELS ARE NOT THE GAME'S RENDERER. strategy_world.c still draws the
//  battlefield with its own hand-written code; this screen draws from the part
//  tables in strategy_models.c. The two are kept separate on purpose so the
//  gallery cannot regress how the game looks.
//
//  ROTATION: every tile free-spins. Grab one and it follows the mouse; let go
//  and it eases back into the free spin, picking the shortest way round rather
//  than unwinding the long way. autoYaw keeps advancing while you drag, so the
//  model rejoins the spin where it would have been, not where you left it.
// ============================================================================

#include "raylib.h"
#include "raygui.h"
#include "raymath.h"
#include "rlgl.h"           // rlViewport: aim a 3D pass at one tile
#include "strategy_showcase.h"
#include "../strategy_test/strategy_models.h"
#include "../../strategy_asset/strategy_asset.h"
#include "../../strategy_asset/strategy_asset_io.h"
#include "../../strategy_asset/strategy_bindings.h"
#include "../../strategy_asset/strategy_catalog.h"
#include "../../strategy_forge/strategy_forge.h"
#include "../strategy_test/strategy_world.h"
#include "../strategy_test/strategy_defs.h"
#include "../../screen_state/screen_state.h"
#include "../../settings_state/settings_state.h"
#include "../../audio_state/audio_state.h"
#include "easing.h"
#include <math.h>
#include <string.h>

static void Enter();
static void Exit();
static void Update();
static void Draw();
static void Gui();

static void ShowcaseInspect(int index);

                                  /* Enter, Exit, Update, Draw, Gui, "Name" */
AppState app_state_strategy_showcase = {Enter, Exit, Update, Draw, Gui,
                                        "StrategyShowcase"};

// ============================================================================
//  Catalog: one flat list behind all three views
//
//  Flat is what makes "next asset" seamless in the inspector - stepping off
//  the last unit lands on the first building instead of dead-ending, and the
//  gallery still groups by category because each entry remembers its own.
// ============================================================================
typedef enum {
    CAT_UNIT = 0, CAT_BUILDING, CAT_NODE,
    CAT_CUSTOM,             // authored .sga files - see strategy_asset.h
    CAT_COUNT
} AssetCategory;

// A catalog entry is EITHER a built-in (compiled part table, addressed by
// `kind`) OR an authored asset (a loaded .sga, addressed by `asset`). The two
// never mix: `asset` is NULL for every built-in, and that NULL is the test
// every accessor branches on.
typedef struct {
    AssetCategory cat;
    int           kind;         // UnitKind / BuildingKind / NodeKind
    const SgaAsset *asset;      // CAT_CUSTOM only; NULL for built-ins. Borrowed
                                //   from strategy_catalog.c - never freed here.
} CatalogEntry;

#define CATALOG_MAX (UNIT_KIND_COUNT + BLD_COUNT + NODE_KIND_COUNT + SGA_ASSETS_MAX)

typedef enum {
    VIEW_GALLERY = 0, VIEW_INSPECT, VIEW_MAP,
    VIEW_BIND,          // which asset stands in for which game role
} ShowcaseView;

// -- Rotation state, one per catalog entry -----------------------------------
typedef struct {
    float autoYaw;      // where the free spin is, always advancing
    float userYaw;      // where the hand put it
    float blend;        // 1 = fully hand-held, 0 = fully free
    float dragDist;     // total |mouse dx| this grab: tells a click from a turn
} SpinState;

// -- Tuning ------------------------------------------------------------------
#define SPIN_SPEED       22.0f   // deg/sec of free rotation
#define SPIN_HOVER_BOOST  2.2f   // hovering speeds a tile up: "grab me"
#define DRAG_SENS         0.55f  // deg per pixel of horizontal drag
#define RELEASE_TIME      0.75f  // seconds to ease back into the free spin
#define CLICK_SLOP        4.0f   // px of drag still treated as a click, not a turn

#define TIP_LINES 4
#define TIP_TEXT_MAX 192

// -- Palette -----------------------------------------------------------------
// Dark editorial ground; the models stay the brightest thing on screen.
#define COL_BG        (Color){  18,  20,  26, 255 }
#define COL_PANEL     (Color){  26,  29,  37, 255 }
#define COL_PANEL_HI  (Color){  33,  37,  47, 255 }
#define COL_LINE      (Color){  52,  57,  70, 255 }
#define COL_LINE_HI   (Color){ 110, 120, 145, 255 }
#define COL_TEXT      (Color){ 232, 236, 245, 255 }
#define COL_TEXT_DIM  (Color){ 138, 146, 166, 255 }
#define COL_TIP_BG    (Color){  24,  26,  31, 245 }
#define COL_TIP_LINE  (Color){ 100, 104, 116, 255 }

// One accent per category - the "colored separation for types". Used on the
// section rule, the tile's top edge and the header label, never as a fill.
static const Color categoryAccent[CAT_COUNT] = {
    {  90, 190, 255, 255 },     // units      - cyan
    { 255, 170,  70, 255 },     // buildings  - amber
    { 120, 220, 130, 255 },     // resources  - green
    { 200, 150, 255, 255 },     // authored   - violet
};
static const char *categoryName[CAT_COUNT] = {
    "UNITS", "BUILDINGS", "RESOURCES", "AUTHORED"
};

// ============================================================================
//  State
// ============================================================================
static struct {
    ShowcaseView view;

    CatalogEntry catalog[CATALOG_MAX];
    int          catalogCount;
    int          catStart[CAT_COUNT];    // first catalog index of each category
    int          catCount[CAT_COUNT];

    SpinState    spin[CATALOG_MAX];
    int          dragTile;               // catalog index being dragged, -1 none
    int          hoverTile;

    int          faction;                // 0, 1 or FACTION_NEUTRAL
    int          inspectIndex;

    // Which animation state the inspector is playing, and its clock. Only
    // authored assets have anything but IDLE; a built-in ignores both.
    int          inspectState;
    float        stateClock;

    Vector2      scroll;
    float        contentH;

    // Map view: orbit around the battlefield center.
    float        mapYaw, mapPitch, mapZoom;
    bool         mapDragging;
    SpinState    mapPanelSpin;           // the small panel slowly turns too

    RenderTexture2D preview;             // scratch target for every 3D preview
    int             previewW, previewH;

    // Tooltip recorded this frame, painted last (see ShowcaseTipDraw).
    char         tipText[TIP_TEXT_MAX];

    // Authored assets, loaded from disk on Enter. Held by value because a
    // CatalogEntry points INTO this array - a reload rebuilds both together
    // (CatalogBuild), so no entry can outlive the asset it names.
    // The assets themselves live in strategy_catalog.c, shared with the forge
    // and the live game. Holding a private copy here would be a second ~74 MB
    // array (desktop tier) of the same models - and would drift the moment one
    // screen reloaded and the other did not.

    // Catalog index awaiting a delete confirmation, -1 for none. Deleting is
    // the one destructive thing the gallery can do, so it always asks.
    int          confirmDelete;

    // -- Filter --------------------------------------------------------------
    // The catalog itself is NEVER reordered or shortened by a filter - every
    // index the rest of this file holds (inspectIndex, dragTile, confirmDelete,
    // the spin array) would shift under it. Instead `shown` is the list of
    // catalog indices that pass the filter, and the gallery walks that. A
    // filtered-out asset is still fully addressable, which is what lets the
    // inspector step through everything while the grid shows a subset.
    int          shown[CATALOG_MAX];
    int          shownCount;
    int          shownStart[CAT_COUNT];  // first SHOWN index of each category
    int          shownCat[CAT_COUNT];    // how many of that category survived

    bool         catFilter[CAT_COUNT];   // all false = show everything
    char         search[64];
    bool         searchEdit;

    // -- Binding view --------------------------------------------------------
    // Which role row is open for picking, as (family, role); -1 for none. The
    // picker is a list of every asset, so it is drawn as an overlay rather
    // than inline - a row that grew by twenty entries would push the rest of
    // the table off screen.
    int          bindFamily, bindRole;
    float        bindScroll;
    float        bindPickScroll;
} sc;

// ============================================================================
//  Tooltip
//
//  raygui's own tooltips only fire on FOCUSED controls, and a disabled control
//  never becomes focused - which is exactly backwards for WIP buttons, where
//  the tooltip is the only thing explaining why the button does nothing. So
//  it is hand-rolled: widgets record a hovered rect, and ShowcaseTipDraw
//  paints the last one recorded, after everything else. (The zen editor
//  solves this the same way, but writes into its own singleton.)
// ============================================================================
static void ShowcaseTip(Rectangle r, const char *text)
{
    if ((text == NULL) || (text[0] == '\0')) return;
    if (!CheckCollisionPointRec(GetMousePosition(), r)) return;

    int n = (int)sizeof(sc.tipText) - 1;
    int i = 0;
    for (; (i < n) && text[i]; i++) sc.tipText[i] = text[i];
    sc.tipText[i] = '\0';
}

// Greedy word wrap. Returns line count, widest line width through outW.
static int TipWrap(const char *text, float maxW, int fontSize,
                   char lines[TIP_LINES][TIP_TEXT_MAX], float *outW)
{
    int n = 0;
    float widest = 0.0f;
    const char *p = text;
    lines[0][0] = '\0';

    while (*p && (n < TIP_LINES))
    {
        char cur[TIP_TEXT_MAX] = { 0 };
        int len = 0;
        while (*p == ' ') p++;
        const char *lineStart = p;

        while (*p)
        {
            const char *ws = p;
            while (*p && (*p != ' ')) p++;
            int wl = (int)(p - ws);
            int sep = len ? 1 : 0;
            if ((len + sep + wl) >= (int)sizeof(cur)) { p = ws; break; }

            char trial[TIP_TEXT_MAX];
            for (int i = 0; i < len; i++) trial[i] = cur[i];
            if (sep) trial[len] = ' ';
            for (int i = 0; i < wl; i++) trial[len + sep + i] = ws[i];
            trial[len + sep + wl] = '\0';

            if ((MeasureText(trial, fontSize) > maxW) && (len > 0)) { p = ws; break; }
            for (int i = 0; i <= (len + sep + wl); i++) cur[i] = trial[i];
            len += sep + wl;
            while (*p == ' ') p++;
        }

        if (len == 0)   // a single unbreakable word: take it whole
        {
            p = lineStart;
            while (*p && (*p != ' ') && (len < (int)sizeof(cur) - 1)) cur[len++] = *p++;
            cur[len] = '\0';
        }

        TextCopy(lines[n], cur);
        float lw = (float)MeasureText(lines[n], fontSize);
        if (lw > widest) widest = lw;
        n++;
    }

    *outW = widest;
    return (n > 0) ? n : 1;
}

static void ShowcaseTipDraw(int fontSize)
{
    if (!sc.tipText[0]) return;

    Vector2 screen = ScreenStateSize();
    float pad = 8.0f;
    float lh = (float)fontSize + 5.0f;
    float maxW = screen.x*0.4f;
    if (maxW > 380.0f) maxW = 380.0f;

    char lines[TIP_LINES][TIP_TEXT_MAX];
    float textW = 0.0f;
    int n = TipWrap(sc.tipText, maxW, fontSize, lines, &textW);

    float w = textW + 2.0f*pad;
    float h = (float)n*lh + 2.0f*pad;

    // Flip rather than clip, so a tip near an edge stays fully readable.
    Vector2 mp = GetMousePosition();
    float x = mp.x + 16.0f;
    float y = mp.y + 20.0f;
    if ((x + w) > (screen.x - 8.0f)) x = mp.x - 16.0f - w;
    if ((y + h) > (screen.y - 8.0f)) y = mp.y - 8.0f - h;
    if (x < 4.0f) x = 4.0f;
    if (y < 4.0f) y = 4.0f;

    Rectangle box = { x, y, w, h };
    DrawRectangleRec(box, COL_TIP_BG);
    DrawRectangleLinesEx(box, 1.0f, COL_TIP_LINE);
    for (int i = 0; i < n; i++)
    {
        DrawText(lines[i], (int)(x + pad), (int)(y + pad + (float)i*lh),
                 fontSize, COL_TEXT);
    }

    sc.tipText[0] = '\0';   // consumed; re-recorded next frame
}

// A button that is deliberately dead, and says so on hover.
static void WipButton(Rectangle r, const char *label, const char *why)
{
    GuiDisable();
    GuiButton(r, label);
    GuiEnable();

    // A dim "not yet" marker in the corner, so the state reads without hovering.
    DrawRectangleLinesEx(r, 1.0f, Fade(COL_LINE, 0.8f));
    ShowcaseTip(r, why);
}

// A live action button in the showcase's own palette. Disabled is a real state
// here rather than a placeholder: DELETE is off for built-ins, and the tooltip
// is the only place that can say why - a disabled raygui control never takes
// focus, so it never shows a raygui tooltip.
// Set while the confirm modal paints its own controls, so those stay live while
// everything behind them goes inert. Guarding on "a modal is open" alone would
// kill the modal's own buttons too.
static bool sc_inModal = false;

static bool SheetButton(Rectangle r, const char *label, bool enabled,
                        const char *tip, int fontSize)
{
    Vector2 mp = GetMousePosition();
    bool blocked = (sc.confirmDelete >= 0) && !sc_inModal;
    bool hot = enabled && !blocked && CheckCollisionPointRec(mp, r);

    // Enabled must read as enabled at REST, not only under the cursor - an
    // available action drawn in the dim text colour is indistinguishable from
    // the disabled one beside it until you happen to hover.
    DrawRectangleRec(r, enabled ? (hot ? COL_PANEL_HI : Fade(COL_PANEL, 0.75f))
                                : Fade(COL_PANEL, 0.4f));
    DrawRectangleLinesEx(r, 1.0f, enabled ? (hot ? COL_LINE_HI : COL_LINE)
                                          : Fade(COL_LINE, 0.5f));
    Color tc = enabled ? COL_TEXT : Fade(COL_TEXT_DIM, 0.35f);
    DrawText(label,
             (int)(r.x + (r.width - (float)MeasureText(label, fontSize))*0.5f),
             (int)(r.y + (r.height - (float)fontSize)*0.5f), fontSize, tc);

    ShowcaseTip(r, tip);
    if (hot && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) { AudioPlayButton(); return true; }
    return false;
}

// ============================================================================
//  Catalog + model access
// ============================================================================
static void CatalogBuild(void)
{
    sc.catalogCount = 0;

    sc.catStart[CAT_UNIT] = sc.catalogCount;
    for (int k = 0; k < UNIT_KIND_COUNT; k++)
        sc.catalog[sc.catalogCount++] = (CatalogEntry){ CAT_UNIT, k, NULL };
    sc.catCount[CAT_UNIT] = sc.catalogCount - sc.catStart[CAT_UNIT];

    sc.catStart[CAT_BUILDING] = sc.catalogCount;
    for (int k = 0; k < BLD_COUNT; k++)
        sc.catalog[sc.catalogCount++] = (CatalogEntry){ CAT_BUILDING, k, NULL };
    sc.catCount[CAT_BUILDING] = sc.catalogCount - sc.catStart[CAT_BUILDING];

    sc.catStart[CAT_NODE] = sc.catalogCount;
    for (int k = 0; k < NODE_KIND_COUNT; k++)
        sc.catalog[sc.catalogCount++] = (CatalogEntry){ CAT_NODE, k, NULL };
    sc.catCount[CAT_NODE] = sc.catalogCount - sc.catStart[CAT_NODE];

    // Authored assets last, so the built-in sections keep the indices they
    // always had and a newly saved asset never renumbers the rest.
    sc.catStart[CAT_CUSTOM] = sc.catalogCount;
    for (int k = 0; (k < StrategyCatalogCount()) && (sc.catalogCount < CATALOG_MAX); k++)
        sc.catalog[sc.catalogCount++] =
            (CatalogEntry){ CAT_CUSTOM, k, &StrategyCatalogAssets()[k] };
    sc.catCount[CAT_CUSTOM] = sc.catalogCount - sc.catStart[CAT_CUSTOM];
}

// Defined below, with the rest of the filter: it reads the entry accessors,
// which in turn need the catalog this file builds first.
static void FilterApply(void);

// Rescans the asset directory and rebuilds the catalog around it. The two must
// happen together: a CatalogEntry holds a pointer into the shared catalog.
static void CatalogReload(void)
{
    StrategyCatalogReload();
    CatalogBuild();
    FilterApply();      // the filter is over the catalog, so it is stale now
}

// The built-in part table behind an entry, or NULL when the entry is an
// authored asset (which has no StrategyModel at all - see EntryAsset).
static const StrategyModel *EntryModel(int index)
{
    const CatalogEntry *e = &sc.catalog[index];
    switch (e->cat)
    {
        case CAT_UNIT:     return StrategyUnitModel((UnitKind)e->kind);
        case CAT_BUILDING: return StrategyBuildingModel((BuildingKind)e->kind);
        case CAT_NODE:     return StrategyNodeModel((NodeKind)e->kind);
        default:           return NULL;
    }
}

static const SgaAsset *EntryAsset(int index)
{
    return sc.catalog[index].asset;
}

// Display name, from whichever half of the entry owns it.
static const char *EntryName(int index)
{
    const SgaAsset *a = EntryAsset(index);
    if (a) return a->name;
    const StrategyModel *m = EntryModel(index);
    return m ? m->name : "";
}

// The extents a preview camera frames. Both kinds measure the same two numbers;
// authored assets keep theirs derived (StrategyAssetMeasure) rather than typed.
static void EntryExtents(int index, float *height, float *radius)
{
    const SgaAsset *a = EntryAsset(index);
    if (a) { *height = a->height; *radius = a->radius; return; }

    const StrategyModel *m = EntryModel(index);
    *height = m ? m->height : 1.0f;
    *radius = m ? m->radius : 0.5f;
}

// Resource nodes belong to the terrain, not to anyone - the faction switcher
// must not pretend otherwise, so they always draw neutral.
static int EntryFaction(int index)
{
    const CatalogEntry *e = &sc.catalog[index];

    // An authored asset says which faction it belongs to through its own
    // category label - a "resource" reads as terrain wherever it came from.
    if (e->cat == CAT_CUSTOM)
    {
        const SgaAsset *a = e->asset;
        if (a && (a->category == SGA_RESOURCE)) return FACTION_NEUTRAL;
        return sc.faction;
    }

    if (e->cat == CAT_NODE) return FACTION_NEUTRAL;
    if ((e->cat == CAT_UNIT) &&
        ((e->kind == KIND_ANIMAL_WEAK) || (e->kind == KIND_ANIMAL_STRONG)))
    {
        return FACTION_NEUTRAL;     // animals are neutral by nature
    }
    return sc.faction;
}

// ============================================================================
//  Forge entry points
//
//  The forge is opened BEFORE the transition, never after: AppStateTransition
//  runs the new state's Enter() on the next frame, and Enter() with nothing
//  opened falls back to a blank document. So every route below sets up the
//  document first and transitions second.
// ============================================================================
static void ForgeOpen(int index, bool remix)
{
    StrategyForgeSetReturn(&app_state_strategy_showcase);

    if (index < 0)
    {
        StrategyForgeOpenNew();
    }
    else
    {
        const SgaAsset *a = EntryAsset(index);
        if (a)
        {
            // An authored asset: REMIX copies it under a free name, EDIT opens
            // the file itself.
            StrategyForgeOpenAsset(a, remix);
        }
        else
        {
            // A built-in. Always a remix - strategy_models.c is compiled in and
            // cannot be written to - so the category is carried across and the
            // subtype seeded from the tile's own name.
            const CatalogEntry *e = &sc.catalog[index];
            int cat = (e->cat == CAT_BUILDING) ? SGA_BUILDING
                    : (e->cat == CAT_NODE)     ? SGA_RESOURCE
                                               : SGA_UNIT;
            StrategyForgeOpenBuiltin(EntryModel(index), EntryName(index),
                                     cat, EntryName(index));
        }
    }
    AppStateTransition(&app_state_strategy_forge);
}

// One dim line under the tile name: the stat that best identifies the asset.
static const char *EntrySubtitle(int index)
{
    const CatalogEntry *e = &sc.catalog[index];

    // An authored asset has no balance table to quote, so it shows its own
    // taxonomy instead - which is the thing you would filter it by anyway.
    if (e->cat == CAT_CUSTOM)
    {
        const SgaAsset *a = e->asset;
        if (a == NULL) return "";
        return TextFormat("%s / %s   %d parts",
                          StrategyAssetCategoryName(a->category),
                          (a->subtype[0] ? a->subtype : "-"), a->partCount);
    }

    if (e->cat == CAT_UNIT)
    {
        const UnitDef *d = StrategyUnitDef((UnitKind)e->kind);
        if (d->damage > 0.0f) return TextFormat("%.0f HP   %.0f DMG", d->maxHp, d->damage);
        return TextFormat("%.0f HP", d->maxHp);
    }
    if (e->cat == CAT_BUILDING)
    {
        const BuildingDef *d = StrategyBuildingDef((BuildingKind)e->kind);
        return TextFormat("%.0f HP   %d wood  %d stone",
                          d->maxHp, d->cost[RES_WOOD], d->cost[RES_STONE]);
    }
    switch ((NodeKind)e->kind)
    {
        case NODE_TREE:   return "yields WOOD";
        case NODE_ROCK:   return "yields STONE";
        case NODE_WHEAT:  return "yields FOOD";
        case NODE_CORPSE: return "yields FOOD";
        default:          return "";
    }
}

// ============================================================================
//  Filtering
//
//  A filter changes WHAT IS ON SCREEN, never what exists. sc.shown is rebuilt
//  from the catalog whenever the query changes; every other index in this file
//  keeps meaning the same thing it always did.
// ============================================================================

// Case-insensitive substring, the same helper shape as ZenStrContainsCI. An
// empty needle matches everything, so an empty search box is not a filter.
static bool ShowcaseContainsCI(const char *hay, const char *needle)
{
    if ((needle == NULL) || (needle[0] == '\0')) return true;
    if ((hay == NULL) || (hay[0] == '\0')) return false;

    for (int i = 0; hay[i] != '\0'; i++)
    {
        int j = 0;
        while (needle[j] != '\0')
        {
            char a = hay[i + j], b = needle[j];
            if ((a >= 'A') && (a <= 'Z')) a = (char)(a - 'A' + 'a');
            if ((b >= 'A') && (b <= 'Z')) b = (char)(b - 'A' + 'a');
            if (a != b) break;
            j++;
        }
        if (needle[j] == '\0') return true;
    }
    return false;
}

// True when no category chip is lit - which means "no category filter", not
// "hide everything". Turning every chip off by clicking them one by one has to
// land back on showing all, or the grid empties for no stated reason.
static bool FilterCatAll(void)
{
    for (int c = 0; c < CAT_COUNT; c++) if (sc.catFilter[c]) return false;
    return true;
}

static bool FilterAny(void)
{
    return !FilterCatAll() || (sc.search[0] != '\0');
}

// Does this entry survive the current query? Text matches on name, category
// label and - for an authored asset - its subtype, so "unit", "quarry" and
// "oak" all find something without the user knowing which field holds what.
static bool FilterAccepts(int index)
{
    const CatalogEntry *e = &sc.catalog[index];

    if (!FilterCatAll() && !sc.catFilter[e->cat]) return false;
    if (sc.search[0] == '\0') return true;

    if (ShowcaseContainsCI(EntryName(index), sc.search)) return true;
    if (ShowcaseContainsCI(categoryName[e->cat], sc.search)) return true;

    const SgaAsset *a = EntryAsset(index);
    if (a != NULL)
    {
        if (ShowcaseContainsCI(a->subtype, sc.search)) return true;
        if (ShowcaseContainsCI(StrategyAssetCategoryName(a->category), sc.search))
            return true;
    }
    return false;
}

// A text box has the keyboard. Every hotkey in this state has to stand down
// while it does, or typing "map" in the search box jumps to the battlefield on
// the M - the same Typing() gate the forge and the zen editor use.
static bool ShowcaseTyping(void)
{
    return sc.searchEdit;
}

static void FilterApply(void)
{
    sc.shownCount = 0;

    for (int c = 0; c < CAT_COUNT; c++)
    {
        sc.shownStart[c] = sc.shownCount;
        for (int i = sc.catStart[c]; i < sc.catStart[c] + sc.catCount[c]; i++)
            if (FilterAccepts(i)) sc.shown[sc.shownCount++] = i;
        sc.shownCat[c] = sc.shownCount - sc.shownStart[c];
    }
}

// ============================================================================
//  Rotation
//
//  autoYaw always advances, even while a tile is held. That is deliberate: on
//  release the model rejoins the free spin where it WOULD have been, so a tile
//  you fiddled with ends up in step with its neighbours instead of permanently
//  offset from them.
// ============================================================================
static float WrapAngle(float deg)
{
    while (deg >= 360.0f) deg -= 360.0f;
    while (deg <    0.0f) deg += 360.0f;
    return deg;
}

// Shortest-arc lerp: without this, easing from 350 deg to 10 deg unwinds the
// long way round and the hand-off reads as a glitch.
static float LerpAngle(float a, float b, float t)
{
    float d = WrapAngle(b - a);
    if (d > 180.0f) d -= 360.0f;
    return WrapAngle(a + d*t);
}

// Advance the free spin and, if this tile is the one being held, follow the
// mouse. `held` is passed in rather than read from the tile: exactly one tile
// can be held at a time and only ShowcaseDragUpdate decides which, so a tile
// can never keep consuming mouse motion after the grab has ended.
static void SpinUpdate(SpinState *s, float dt, bool hovered, bool held)
{
    float speed = SPIN_SPEED*(hovered ? SPIN_HOVER_BOOST : 1.0f);
    s->autoYaw = WrapAngle(s->autoYaw + speed*dt);

    if (held)
    {
        float dx = GetMouseDelta().x;
        s->dragDist += fabsf(dx);
        s->userYaw = WrapAngle(s->userYaw + dx*DRAG_SENS);
        s->blend = 1.0f;
    }
    else if (s->blend > 0.0f)
    {
        s->blend -= dt/RELEASE_TIME;
        if (s->blend < 0.0f) s->blend = 0.0f;
    }
}

// The angle actually drawn. blend is eased so the hand-off decelerates instead
// of snapping the instant the button comes up.
static float SpinYaw(const SpinState *s)
{
    if (s->blend <= 0.0f) return s->autoYaw;
    float t = cubicEaseOutf(s->blend);
    return LerpAngle(s->autoYaw, s->userYaw, t);
}

// ----------------------------------------------------------------------------
//  Drag ownership
//
//  sc.dragTile is the ONE tile the mouse is currently turning, and these three
//  functions are the only things that touch it. That matters: the earlier
//  version released the grab only when the released tile still happened to be
//  the one on screen, so stepping to the next asset mid-drag stranded a tile in
//  a permanently-held state - after which it kept eating mouse motion from
//  every other view, and the gallery appeared to spin whenever the mouse moved.
//
//  The rules now:
//    - a grab is only ever started by ShowcaseDragBegin,
//    - the button coming up ALWAYS ends it, wherever the cursor is,
//    - and leaving a view (or changing what it shows) ends it too.
// ----------------------------------------------------------------------------
static void ShowcaseDragBegin(int index)
{
    if ((index < 0) || (index >= sc.catalogCount)) return;
    SpinState *s = &sc.spin[index];
    sc.dragTile = index;
    s->userYaw = SpinYaw(s);    // take over from exactly where it looks now
    s->blend = 1.0f;
    s->dragDist = 0.0f;
}

// Ends the current grab, if any. Safe to call when nothing is held.
// Returns the tile that was released, or -1.
static int ShowcaseDragEnd(void)
{
    int released = sc.dragTile;
    sc.dragTile = -1;
    return released;
}

// Called once per frame before any view runs, to clear a STALE grab: one whose
// button is up and whose release frame has already gone by.
//
// The IsMouseButtonReleased() guard is the important part. That is true only on
// the single frame the button comes up, and the gallery needs that exact frame
// to tell a click (open the inspector) from a turn. Clearing the grab here
// would consume it first and the click would never register - so on the release
// frame the poll steps aside and lets the view decide. Every frame after, the
// button is up and NOT released, and any grab still standing is one nobody
// handled: a press that ended over a button, over the header, or off-window.
static void ShowcaseDragPoll(void)
{
    if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) return;
    if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) return;   // the view's frame

    if (sc.dragTile >= 0) ShowcaseDragEnd();
    sc.mapDragging = false;         // the map orbit obeys the same rule
}

// ============================================================================
//  3D preview rendering
//
//  Every preview goes through one shared render texture sized to the window.
//  A tile renders into the region it will occupy, then blits that region out.
//  One target for all tiles keeps GPU memory flat no matter how many assets
//  the catalog grows to.
// ============================================================================
static void PreviewEnsure(int w, int h)
{
    if ((w < 1) || (h < 1)) return;
    if (sc.preview.id && (sc.previewW == w) && (sc.previewH == h)) return;

    if (sc.preview.id) UnloadRenderTexture(sc.preview);
    sc.preview = LoadRenderTexture(w, h);
    sc.previewW = w;
    sc.previewH = h;
}

// Frame a model so it fills its tile regardless of whether it is a 0.24-tall
// corpse or a 2.6-tall chantry. The camera looks slightly down at the model's
// mid-height from a distance derived from its own extents.
static Camera3D ModelCamera(float height, float radius, float aspect)
{
    float reach = radius;
    if (height*0.5f > reach) reach = height*0.5f;
    if (reach < 0.4f) reach = 0.4f;

    // Widen the pull-back on narrow tiles so nothing clips at the sides.
    float dist = reach*4.2f;
    if (aspect < 1.0f) dist /= aspect;

    Camera3D cam = { 0 };
    cam.position   = (Vector3){ dist*0.62f, height*0.85f + reach*1.5f, dist*0.62f };
    cam.target     = (Vector3){ 0.0f, height*0.42f, 0.0f };
    cam.up         = (Vector3){ 0.0f, 1.0f, 0.0f };
    cam.fovy       = 45.0f;
    cam.projection = CAMERA_PERSPECTIVE;
    return cam;
}

// Render one model into the shared target at `region`, spinning at `yaw`.
// Caller must already be inside BeginTextureMode(sc.preview).
//
// BeginMode3D builds its projection from the CURRENT framebuffer's size, not
// from the tile - so a tile that is not the same aspect as the whole target
// would come out stretched. rlViewport is set first so raylib's own matrix
// math sees the tile's dimensions, and BeginMode3D then does the rest
// correctly. Scissor keeps the clear and the geometry inside the tile.
//
// Takes a CATALOG INDEX rather than a model pointer, because an entry may be
// either a built-in part table or an authored asset and only the index knows
// which. Everything else - framing, viewport, shadow - is identical for both.
static void PreviewDrawEntry(Rectangle region, int index, int faction,
                             float yaw, bool groundShadow, int state, float time)
{
    if ((index < 0) || (index >= sc.catalogCount)) return;
    if ((region.width < 1.0f) || (region.height < 1.0f)) return;

    const StrategyModel *m = EntryModel(index);
    const SgaAsset *a = EntryAsset(index);
    if ((m == NULL) && (a == NULL)) return;

    float height, radius;
    EntryExtents(index, &height, &radius);

    int rx = (int)region.x;
    int ry = (int)region.y;
    int rw = (int)region.width;
    int rh = (int)region.height;

    float aspect = region.width/region.height;
    Camera3D cam = ModelCamera(height, radius, aspect);

    BeginScissorMode(rx, ry, rw, rh);
    // Framebuffer Y is bottom-up; the region rect is top-down.
    rlViewport(rx, sc.previewH - ry - rh, rw, rh);

    BeginMode3D(cam);
        if (groundShadow)
        {
            // A soft disc under the model so it reads as standing on something
            // rather than floating in the void.
            float r = (radius > 0.0f) ? radius*1.6f : 0.6f;
            DrawCylinder((Vector3){ 0.0f, -0.005f, 0.0f }, r, r, 0.01f, 24,
                         (Color){ 0, 0, 0, 70 });
        }
        if (a) StrategyAssetDraw(a, faction, (Vector3){ 0.0f, 0.0f, 0.0f },
                                 yaw, 1.0f, state, time);
        else   StrategyModelDraw(m, faction, (Vector3){ 0.0f, 0.0f, 0.0f }, yaw, 1.0f);
    EndMode3D();

    rlViewport(0, 0, sc.previewW, sc.previewH);
    EndScissorMode();
}

// Draw the battlefield diorama: the REAL spawn layout, from the live world.
// Called inside BeginTextureMode(sc.preview) like PreviewDrawModel.
//
// It walks the world's arrays and draws each entry through the model tables
// rather than calling StrategyWorldDraw3D() - that function owns its own
// BeginMode3D and its own camera, so it cannot be aimed at a preview tile.
static void PreviewDrawMap(Rectangle region, float yaw, float pitch, float zoom)
{
    if ((region.width < 1.0f) || (region.height < 1.0f)) return;

    StrategyWorld *w = StrategyWorldGet();
    int rx = (int)region.x, ry = (int)region.y;
    int rw = (int)region.width, rh = (int)region.height;
    float aspect = region.width/region.height;

    float dist = STRAT_GROUND_HALF*2.1f*zoom;
    float pr = pitch*DEG2RAD;
    float yr = yaw*DEG2RAD;

    Camera3D cam = { 0 };
    cam.position = (Vector3){ dist*cosf(pr)*sinf(yr),
                              dist*sinf(pr),
                              dist*cosf(pr)*cosf(yr) };
    cam.target     = (Vector3){ 0.0f, 0.0f, 0.0f };
    cam.up         = (Vector3){ 0.0f, 1.0f, 0.0f };
    cam.fovy       = 45.0f;
    cam.projection = CAMERA_PERSPECTIVE;
    (void)aspect;

    BeginScissorMode(rx, ry, rw, rh);
    rlViewport(rx, sc.previewH - ry - rh, rw, rh);

    BeginMode3D(cam);
        DrawPlane((Vector3){ 0.0f, -0.01f, 0.0f },
                  (Vector2){ 2.0f*STRAT_GROUND_HALF, 2.0f*STRAT_GROUND_HALF },
                  (Color){ 74, 92, 66, 255 });
        // DrawGrid's first argument is a CELL COUNT, not a width: at spacing 5
        // it would otherwise draw a grid five times wider than the ground.
        DrawGrid((int)(2.0f*STRAT_GROUND_HALF/5.0f), 5.0f);

        for (int i = 0; i < STRAT_MAX_NODES; i++)
        {
            if (!w->nodes[i].active) continue;
            StrategyModelDraw(StrategyNodeModel(w->nodes[i].kind),
                              FACTION_NEUTRAL, w->nodes[i].pos, 0.0f, 1.0f);
        }
        for (int i = 0; i < STRAT_MAX_BUILDINGS; i++)
        {
            if (!w->buildings[i].active) continue;
            StrategyModelDraw(StrategyBuildingModel(w->buildings[i].kind),
                              w->buildings[i].faction, w->buildings[i].pos,
                              0.0f, 1.0f);
        }
        for (int i = 0; i < STRAT_MAX_UNITS; i++)
        {
            if (!w->units[i].active) continue;
            StrategyModelDraw(StrategyUnitModel(w->units[i].kind),
                              w->units[i].faction, w->units[i].pos, 0.0f, 1.0f);
        }
    EndMode3D();

    rlViewport(0, 0, sc.previewW, sc.previewH);
    EndScissorMode();
}

// Blit a region of the shared target into place on screen. The target's Y is
// flipped (OpenGL origin), hence the negative source height.
static void PreviewBlit(Rectangle region, Rectangle dest)
{
    Rectangle src = { region.x, (float)sc.previewH - region.y - region.height,
                      region.width, -region.height };
    DrawTexturePro(sc.preview.texture, src, dest, (Vector2){ 0.0f, 0.0f },
                   0.0f, WHITE);
}

// ============================================================================
//  Chrome helpers
// ============================================================================
static void PanelBG(Rectangle r, bool hot)
{
    DrawRectangleRec(r, hot ? COL_PANEL_HI : COL_PANEL);
    DrawRectangleLinesEx(r, 1.0f, hot ? COL_LINE_HI : COL_LINE);
}

// A label drawn right-aligned inside r.
static void TextRight(const char *t, Rectangle r, int fs, Color c)
{
    DrawText(t, (int)(r.x + r.width - (float)MeasureText(t, fs)), (int)r.y, fs, c);
}

// The faction switcher: a segmented control whose segments are painted in the
// colors they select, so the control previews its own effect.
static void FactionSwitcher(Rectangle r, int fs)
{
    const int   ids[3]    = { 0, 1, FACTION_NEUTRAL };
    const char *labels[3] = { "OWN", "ENEMY", "NEUTRAL" };

    float segW = r.width/3.0f;
    for (int i = 0; i < 3; i++)
    {
        Rectangle seg = { r.x + (float)i*segW, r.y, segW, r.height };
        bool active = (sc.faction == ids[i]);
        bool hot = CheckCollisionPointRec(GetMousePosition(), seg);

        Color tint = StrategyFactionTint(ids[i]);
        DrawRectangleRec(seg, active ? Fade(tint, 0.30f)
                                     : (hot ? COL_PANEL_HI : COL_PANEL));
        DrawRectangleLinesEx(seg, active ? 2.0f : 1.0f,
                             active ? tint : COL_LINE);

        // Swatch + label, so the segment reads even before it is selected.
        float sw = r.height*0.32f;
        Rectangle chip = { seg.x + 10.0f, seg.y + (seg.height - sw)*0.5f, sw, sw };
        DrawRectangleRec(chip, tint);
        DrawRectangleLinesEx(chip, 1.0f, Fade(BLACK, 0.35f));

        DrawText(labels[i], (int)(chip.x + sw + 8.0f),
                 (int)(seg.y + (seg.height - (float)fs)*0.5f), fs,
                 active ? COL_TEXT : COL_TEXT_DIM);

        if (hot && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
        {
            AudioPlayButton();
            ShowcaseDragEnd();   // a press up here is not a grab on a model
            sc.faction = ids[i];
        }
    }
}

// ============================================================================
//  Layout metrics, derived from the GUI scale so the grid reflows rather than
//  clipping when the user picks a bigger UI.
// ============================================================================
typedef struct {
    float s;                    // effective gui scale (1..3)
    int   fsSmall, fsBody, fsHead;
    float pad, gap;
    float tileW, tileH, thumbH;
    float headerH, footerH;
    int   cols;
    Rectangle content;          // the scrolling region
    Rectangle filter;           // the filter strip, ABOVE content (gallery only)
    float mapW, mapH;           // map panel size
    bool  mapRail;              // is there room for the panel beside the grid?
} Layout;

static Layout LayoutCompute(void)
{
    Layout L = { 0 };
    Vector2 screen = ScreenStateSize();
    Settings *settings = SettingsGet();

    const float scales[3] = { 1.0f, 2.0f, 3.0f };
    int wish = settings->gui_scale_wish;
    if (wish < 0) wish = 0;
    if (wish > 2) wish = 2;
    L.s = scales[wish];

    L.fsSmall = (int)(10.0f*L.s);
    L.fsBody  = (int)(14.0f*L.s);
    L.fsHead  = (int)(22.0f*L.s);

    L.pad = 18.0f*L.s;
    L.gap = 12.0f*L.s;

    L.headerH = 64.0f*L.s;
    L.footerH = 54.0f*L.s;

    L.content = (Rectangle){ L.pad, L.headerH,
                             screen.x - 2.0f*L.pad,
                             screen.y - L.headerH - L.footerH };

    // The filter strip is carved off the TOP of the content area rather than
    // added to the header, so it scrolls away with nothing and stays pinned
    // above the grid. Only the gallery has it - filtering means nothing while
    // inspecting one asset or looking at the battlefield.
    L.filter = (Rectangle){ L.content.x, L.content.y, L.content.width, 0.0f };
    if (sc.view == VIEW_GALLERY)
    {
        L.filter.height = 34.0f*L.s;
        L.content.y      += L.filter.height + L.gap*0.5f;
        L.content.height -= L.filter.height + L.gap*0.5f;
    }

    if (L.content.height < 80.0f) L.content.height = 80.0f;

    // The map panel gets its own rail down the right so it never sits on top
    // of a tile. Narrow windows drop the rail; M still opens the full map.
    L.mapW = 210.0f*L.s;
    L.mapH = 140.0f*L.s;
    L.mapRail = (L.content.width - L.mapW - L.gap) > 360.0f*L.s;
    if (L.mapRail) L.content.width -= L.mapW + L.gap*1.5f;

    // Matched to the grid AFTER the rail has taken its cut, so the strip ends
    // where the tiles do instead of running under the map panel.
    L.filter.width = L.content.width;

    // Tiles want to be about 190 logical px wide; fit as many as the row holds
    // and let them share out the remainder, so the grid never leaves a ragged
    // gutter and never clips a column off the right edge.
    float want = 190.0f*L.s;
    int fit = (int)((L.content.width + L.gap)/(want + L.gap));
    if (fit < 1) fit = 1;
    if (fit > 7) fit = 7;

    // Prefer a column count that does not strand a nearly-empty last row. With
    // 7 units in 6 columns the second row holds ONE tile beside a wall of
    // nothing; 7 or 4 columns both read far better. Score each candidate by how
    // badly its widest category's final row is left unfilled, and keep the
    // widest tiles among the best - never dropping more than one column below
    // what fits, so tiles stay large.
    int best = fit;
    int bestWaste = 1 << 30;
    for (int c = fit; (c >= 1) && (c >= fit - 2); c--)
    {
        int waste = 0;
        for (int k = 0; k < CAT_COUNT; k++)
        {
            int n = sc.shownCat[k];
            if (n <= 0) continue;
            int rem = n%c;
            if (rem != 0) waste += (c - rem);
        }
        if (waste < bestWaste) { bestWaste = waste; best = c; }
    }
    L.cols = best;

    L.tileW  = (L.content.width - (float)(L.cols - 1)*L.gap)/(float)L.cols;
    L.thumbH = L.tileW*0.78f;

    // Cap the thumbnail so a single row cannot eat the whole viewport - on a
    // wide window the tiles would otherwise grow until only one category was
    // ever on screen, which defeats the point of a gallery.
    float maxThumb = L.content.height*0.40f;
    if (L.thumbH > maxThumb) L.thumbH = maxThumb;

    L.tileH  = L.thumbH + 42.0f*L.s;
    return L;
}

// ============================================================================
//  Gallery
// ============================================================================
// ============================================================================
//  Filter bar
//
//  Category chips plus a search box, pinned above the grid. The chips are a
//  MULTI-select: clicking three of them shows three categories, and clicking
//  the last lit one back off returns to showing everything rather than showing
//  nothing. "None selected" and "all selected" are the same state on purpose -
//  it is the only reading that makes an empty bar mean an unfiltered gallery.
// ============================================================================
static void DrawFilterBar(const Layout *L)
{
    Rectangle bar = L->filter;
    if (bar.height <= 0.0f) return;

    Vector2 mp = GetMousePosition();
    bool before[CAT_COUNT];
    for (int c = 0; c < CAT_COUNT; c++) before[c] = sc.catFilter[c];
    char searchBefore[sizeof(sc.search)];
    TextCopy(searchBefore, sc.search);

    // -- category chips -------------------------------------------------------
    float x = bar.x;
    float chipH = bar.height;
    bool all = FilterCatAll();

    for (int c = 0; c < CAT_COUNT; c++)
    {
        const char *label = categoryName[c];
        float w = (float)MeasureText(label, L->fsSmall) + 26.0f*L->s;
        Rectangle chip = { x, bar.y, w, chipH };
        bool hot = CheckCollisionPointRec(mp, chip) && !sc_inModal;
        bool on  = sc.catFilter[c];

        // An unfiltered bar shows every chip in its accent at low strength:
        // "all of these are showing", not "none of these are selected".
        Color accent = categoryAccent[c];
        DrawRectangleRec(chip, on ? Fade(accent, 0.22f)
                                  : (hot ? COL_PANEL_HI : COL_PANEL));
        DrawRectangleLinesEx(chip, on ? 2.0f : 1.0f,
                             on ? accent : (hot ? COL_LINE_HI : COL_LINE));
        DrawText(label, (int)(chip.x + 13.0f*L->s),
                 (int)(bar.y + (chipH - (float)L->fsSmall)*0.5f), L->fsSmall,
                 on ? COL_TEXT : (all ? Fade(accent, 0.75f) : COL_TEXT_DIM));

        ShowcaseTip(chip, on ? "Showing this category. Click to stop filtering by it."
                             : "Show only this category. Chips add up, and turning "
                               "them all off shows everything again.");

        if (hot && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
        {
            AudioPlayButton();
            ShowcaseDragEnd();      // a press up here is not a grab on a model
            sc.catFilter[c] = !sc.catFilter[c];
        }
        x += w + 6.0f*L->s;
    }

    // -- search ---------------------------------------------------------------
    x += 6.0f*L->s;
    float clearW = (sc.search[0] != '\0') ? 24.0f*L->s : 0.0f;
    float searchW = bar.x + bar.width - x - clearW;
    if (searchW > 320.0f*L->s) searchW = 320.0f*L->s;

    if (searchW > 60.0f*L->s)
    {
        Rectangle box = { x, bar.y, searchW, chipH };

        GuiSetStyle(DEFAULT, TEXT_SIZE, L->fsSmall);
        if (GuiTextBox(box, sc.search, (int)sizeof(sc.search), sc.searchEdit))
            sc.searchEdit = !sc.searchEdit;

        // The placeholder says WHICH fields are searched. Otherwise a miss on
        // "quarry" reads as a broken search rather than as a subtype nobody
        // typed on that asset.
        if ((sc.search[0] == '\0') && !sc.searchEdit)
        {
            DrawText("search name, category or subtype",
                     (int)(box.x + 8.0f*L->s),
                     (int)(box.y + (chipH - (float)L->fsSmall)*0.5f),
                     L->fsSmall, Fade(COL_TEXT_DIM, 0.7f));
        }
        ShowcaseTip(box, "Matches an asset's name, its category, or the subtype "
                         "an authored asset was labelled with.");

        if (clearW > 0.0f)
        {
            Rectangle clr = { box.x + searchW + 2.0f*L->s, bar.y, clearW - 2.0f*L->s, chipH };
            bool hot = CheckCollisionPointRec(mp, clr) && !sc_inModal;
            DrawRectangleRec(clr, hot ? COL_PANEL_HI : COL_PANEL);
            DrawRectangleLinesEx(clr, 1.0f, hot ? COL_LINE_HI : COL_LINE);
            DrawText("x", (int)(clr.x + (clr.width - (float)MeasureText("x", L->fsSmall))*0.5f),
                     (int)(bar.y + (chipH - (float)L->fsSmall)*0.5f), L->fsSmall,
                     hot ? COL_TEXT : COL_TEXT_DIM);
            ShowcaseTip(clr, "Clear the search.");
            if (hot && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
            {
                AudioPlayButton();
                sc.search[0] = '\0';
                sc.searchEdit = false;
            }
        }
    }

    // -- result count, right-aligned -----------------------------------------
    if (FilterAny())
    {
        const char *msg = (sc.shownCount == 0)
                        ? "nothing matches"
                        : TextFormat("%d of %d", sc.shownCount, sc.catalogCount);
        int w = MeasureText(msg, L->fsSmall);
        DrawText(msg, (int)(bar.x + bar.width - (float)w),
                 (int)(bar.y + (chipH - (float)L->fsSmall)*0.5f), L->fsSmall,
                 (sc.shownCount == 0) ? COL_LINE_HI : COL_TEXT_DIM);
    }

    // Rebuild only on a real change. FilterApply walks the whole catalog and
    // there is no reason to pay for it on a frame where nothing was typed.
    bool changed = !TextIsEqual(searchBefore, sc.search);
    for (int c = 0; (c < CAT_COUNT) && !changed; c++)
        changed = (before[c] != sc.catFilter[c]);

    if (changed)
    {
        FilterApply();
        sc.scroll.y = 0.0f;     // the old offset means nothing over a new list
    }
}

static void DrawGallery(const Layout *L)
{
    Vector2 mp = GetMousePosition();
    float dt = GetFrameTime();

    sc.hoverTile = -1;

    // -- pass 1: geometry + hover + input ------------------------------------
    // Tile rects are computed once here and reused by both draw passes, so the
    // 3D pass and the 2D pass can never disagree about where a tile is.
    Rectangle tileRect[CATALOG_MAX];
    Rectangle thumbRect[CATALOG_MAX];

    // Cleared, not just filled: pass 1 now only writes the entries the FILTER
    // let through, and passes 2 and 3 walk the whole catalog. A filtered-out
    // entry has to read as "not visible" rather than as whatever was on the
    // stack, which would blit a tile at a garbage rectangle.
    bool      visible[CATALOG_MAX] = { 0 };

    float y = L->content.y - sc.scroll.y;

    bool first = true;
    for (int c = 0; c < CAT_COUNT; c++)
    {
        // A category the filter emptied contributes NOTHING - not a header, not
        // a gap. Four empty bands stacked above the one section that matched
        // would read as "the gallery broke", not as "nothing else matched".
        if (sc.shownCat[c] <= 0) continue;

        y += first ? 0.0f : L->gap*1.6f;
        first = false;
        y += 26.0f*L->s;        // section header band

        int rows = (sc.shownCat[c] + L->cols - 1)/L->cols;
        for (int r = 0; r < rows; r++)
        {
            for (int col = 0; col < L->cols; col++)
            {
                int within = r*L->cols + col;
                if (within >= sc.shownCat[c]) break;
                int i = sc.shown[sc.shownStart[c] + within];

                Rectangle t = { L->content.x + (float)col*(L->tileW + L->gap),
                                y, L->tileW, L->tileH };
                tileRect[i] = t;
                thumbRect[i] = (Rectangle){ t.x + 1.0f, t.y + 1.0f,
                                            t.width - 2.0f, L->thumbH };
                visible[i] = CheckCollisionRecs(t, L->content);

                bool hot = visible[i] && CheckCollisionPointRec(mp, t) &&
                           CheckCollisionPointRec(mp, L->content);
                if (hot) sc.hoverTile = i;
            }
            y += L->tileH + L->gap;
        }
    }

    sc.contentH = y + sc.scroll.y - L->content.y;

    // -- input: grab / release ------------------------------------------------
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && (sc.hoverTile >= 0))
    {
        ShowcaseDragBegin(sc.hoverTile);
    }
    if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT) && (sc.dragTile >= 0))
    {
        // A press that never really moved is a click: open the inspector. A
        // press that turned the model was a rotate and must NOT also open it.
        // The few-pixel threshold forgives the wobble in a normal click.
        int released = sc.dragTile;
        bool turned = (sc.spin[released].dragDist > CLICK_SLOP);
        ShowcaseDragEnd();
        if (!turned && (sc.hoverTile == released))
        {
            AudioPlayButton();
            ShowcaseInspect(released);
        }
    }

    // Only the grabbed tile follows the mouse; everything else free-spins.
    for (int i = 0; i < sc.catalogCount; i++)
    {
        SpinUpdate(&sc.spin[i], dt, (i == sc.hoverTile) || (i == sc.dragTile),
                   (i == sc.dragTile));
    }

    // -- pass 2: all 3D into the shared target -------------------------------
    BeginTextureMode(sc.preview);
        ClearBackground(BLANK);
        for (int i = 0; i < sc.catalogCount; i++)
        {
            if (!visible[i]) continue;
            Rectangle clipped = GetCollisionRec(thumbRect[i], L->content);
            if ((clipped.width < 1.0f) || (clipped.height < 1.0f)) continue;
            // Gallery tiles always show the IDLE rest pose: a wall of tiles
            // each playing its own loop is noise, and idle is what a static
            // asset already is.
            PreviewDrawEntry(thumbRect[i], i, EntryFaction(i),
                             SpinYaw(&sc.spin[i]), true, SGA_STATE_IDLE, 0.0f);
        }
    EndTextureMode();

    // -- pass 3: 2D chrome over the top --------------------------------------
    BeginScissorMode((int)L->content.x, (int)L->content.y,
                     (int)L->content.width, (int)L->content.height);

    y = L->content.y - sc.scroll.y;
    first = true;
    for (int c = 0; c < CAT_COUNT; c++)
    {
        // Skipped on exactly the same test as pass 1, or the two passes would
        // disagree about where every tile below this point sits.
        if (sc.shownCat[c] <= 0) continue;

        y += first ? 0.0f : L->gap*1.6f;
        first = false;

        // Section header: accent rule + label + count. This is the "colored
        // separation for types" - a band of color owned by the category,
        // never a fill behind the models.
        float bandH = 26.0f*L->s;
        Rectangle rule = { L->content.x, y + bandH*0.5f - 1.0f*L->s,
                           L->content.width, 2.0f*L->s };
        DrawRectangleRec(rule, Fade(categoryAccent[c], 0.25f));

        const char *label = categoryName[c];
        int lw = MeasureText(label, L->fsBody);
        Rectangle chip = { L->content.x, y, (float)lw + 26.0f*L->s, bandH };
        DrawRectangleRec(chip, COL_BG);
        DrawRectangleRec((Rectangle){ chip.x, chip.y + bandH*0.2f,
                                      4.0f*L->s, bandH*0.6f },
                         categoryAccent[c]);
        DrawText(label, (int)(chip.x + 12.0f*L->s),
                 (int)(y + (bandH - (float)L->fsBody)*0.5f),
                 L->fsBody, categoryAccent[c]);

        const char *count = TextFormat("%d", sc.shownCat[c]);
        int cw = MeasureText(count, L->fsSmall);
        DrawRectangleRec((Rectangle){ L->content.x + L->content.width - (float)cw - 16.0f*L->s,
                                      y, (float)cw + 16.0f*L->s, bandH }, COL_BG);
        DrawText(count,
                 (int)(L->content.x + L->content.width - (float)cw - 8.0f*L->s),
                 (int)(y + (bandH - (float)L->fsSmall)*0.5f),
                 L->fsSmall, COL_TEXT_DIM);
        y += bandH;

        int rows = (sc.shownCat[c] + L->cols - 1)/L->cols;
        for (int r = 0; r < rows; r++)
        {
            for (int col = 0; col < L->cols; col++)
            {
                int within = r*L->cols + col;
                if (within >= sc.shownCat[c]) break;
                int i = sc.shown[sc.shownStart[c] + within];
                if (!visible[i]) continue;

                Rectangle t = tileRect[i];
                bool hot = (i == sc.hoverTile) || (i == sc.dragTile);

                PanelBG(t, hot);
                // Accent edge along the top: the tile's category, at a glance.
                DrawRectangleRec((Rectangle){ t.x, t.y, t.width, 2.0f*L->s },
                                 Fade(categoryAccent[c], hot ? 1.0f : 0.55f));

                PreviewBlit(thumbRect[i], thumbRect[i]);

                float ty = t.y + L->thumbH + 6.0f*L->s;
                DrawText(EntryName(i), (int)(t.x + 10.0f*L->s), (int)ty,
                         L->fsBody, COL_TEXT);
                DrawText(EntrySubtitle(i), (int)(t.x + 10.0f*L->s),
                         (int)(ty + (float)L->fsBody + 3.0f*L->s),
                         L->fsSmall, COL_TEXT_DIM);
            }
            y += L->tileH + L->gap;
        }
    }
    EndScissorMode();

    // Scroll: wheel over the content area.
    if (CheckCollisionPointRec(mp, L->content))
    {
        sc.scroll.y -= GetMouseWheelMove()*48.0f*L->s;
    }
    float maxScroll = sc.contentH - L->content.height;
    if (maxScroll < 0.0f) maxScroll = 0.0f;
    if (sc.scroll.y > maxScroll) sc.scroll.y = maxScroll;
    if (sc.scroll.y < 0.0f) sc.scroll.y = 0.0f;

    // A slim scrollbar, drawn only when there is somewhere to scroll to.
    if (maxScroll > 0.0f)
    {
        float trackH = L->content.height;
        float thumbH = trackH*(L->content.height/sc.contentH);
        if (thumbH < 24.0f) thumbH = 24.0f;
        float t = sc.scroll.y/maxScroll;
        Rectangle bar = { L->content.x + L->content.width + 4.0f*L->s,
                          L->content.y + t*(trackH - thumbH),
                          4.0f*L->s, thumbH };
        DrawRectangleRec(bar, Fade(COL_LINE_HI, 0.7f));
    }

    // An empty grid must say WHY it is empty and how to leave that state -
    // a blank slab teaches nothing, and the filter that caused it is the one
    // thing the user cannot see from here.
    if (sc.shownCount == 0)
    {
        const char *msg = "No asset matches this filter.";
        const char *hint = "Clear the search, or turn a category chip back off.";
        int w1 = MeasureText(msg, L->fsBody);
        int w2 = MeasureText(hint, L->fsSmall);
        float cy = L->content.y + L->content.height*0.38f;
        DrawText(msg, (int)(L->content.x + (L->content.width - (float)w1)*0.5f),
                 (int)cy, L->fsBody, COL_TEXT_DIM);
        DrawText(hint, (int)(L->content.x + (L->content.width - (float)w2)*0.5f),
                 (int)(cy + (float)L->fsBody + 8.0f*L->s), L->fsSmall,
                 Fade(COL_TEXT_DIM, 0.75f));
    }

    // LAST, and outside the scissor: the strip is pinned above the grid, and
    // its raygui text box would be clipped by the content region it sits over.
    DrawFilterBar(L);
}

// ============================================================================
//  Inspector: one asset, large, with its real stats
// ============================================================================
// Show `index` in the inspector. Ending the drag here is the point: the grab
// belonged to the asset being navigated away from, and carrying it onto the
// next one made the new asset spin under a mouse button nobody was holding.
// Change view. Every transition ends any in-flight grab: a gesture belongs to
// the screen it started on, and must not follow the user to the next one.
static void ShowcaseSetView(ShowcaseView v)
{
    ShowcaseDragEnd();
    sc.mapDragging = false;
    sc.view = v;
}

static void ShowcaseInspect(int index)
{
    if ((index < 0) || (index >= sc.catalogCount)) return;
    sc.inspectIndex = index;

    // Every asset opens on IDLE. Carrying the previous asset's state over would
    // land on a tab this one may not animate at all, which reads as broken.
    sc.inspectState = SGA_STATE_IDLE;
    sc.stateClock = 0.0f;

    ShowcaseSetView(VIEW_INSPECT);
}

// True when this entry has keys on `state` - what greys out an empty tab.
static bool EntryHasState(int index, int state)
{
    const SgaAsset *a = EntryAsset(index);
    if (a == NULL) return (state == SGA_STATE_IDLE);    // built-ins are idle-only

    if (state == SGA_STATE_IDLE) return true;           // the rest pose always exists
    for (int k = 0; k < a->partCount; k++)
        if (a->parts[k].anim[state].keyCount > 0) return true;
    return false;
}

// Advances the inspector's animation clock, looping over the state's authored
// duration. A state with no duration holds at 0 - a still pose, not a freeze
// at some arbitrary time.
static void StateClockUpdate(int index, float dt)
{
    const SgaAsset *a = EntryAsset(index);
    if (a == NULL) { sc.stateClock = 0.0f; return; }

    float dur = a->duration[sc.inspectState];
    if (dur <= 0.0f) { sc.stateClock = 0.0f; return; }

    sc.stateClock += dt;
    while (sc.stateClock > dur) sc.stateClock -= dur;
}

static void InspectStep(int delta)
{
    if (sc.catalogCount <= 0) return;
    ShowcaseInspect((sc.inspectIndex + delta + sc.catalogCount)%sc.catalogCount);
    AudioPlayButton();
}

// The stat block, read straight from the def tables so it can never disagree
// with what the game actually uses.
static void DrawInspectStats(Rectangle r, int index, const Layout *L)
{
    const CatalogEntry *e = &sc.catalog[index];
    float y = r.y;
    float lh = (float)L->fsBody + 9.0f*L->s;

    #define STAT_ROW(label, value)                                            \
        do {                                                                  \
            DrawText((label), (int)r.x, (int)y, L->fsSmall, COL_TEXT_DIM);     \
            TextRight((value), (Rectangle){ r.x, y, r.width, lh },            \
                      L->fsBody, COL_TEXT);                                    \
            y += lh;                                                          \
            DrawRectangleRec((Rectangle){ r.x, y - lh*0.22f, r.width, 1.0f }, \
                             Fade(COL_LINE, 0.6f));                            \
        } while (0)

    // An authored asset has no balance table behind it. What it does have is
    // the shape of the thing itself, which is what an author needs to see.
    if (e->cat == CAT_CUSTOM)
    {
        const SgaAsset *a = e->asset;
        if (a == NULL) return;

        STAT_ROW("CATEGORY", StrategyAssetCategoryName(a->category));
        STAT_ROW("SUBTYPE", a->subtype[0] ? a->subtype : "-");
        STAT_ROW("PARTS", TextFormat("%d", a->partCount));

        int paths = 0;
        for (int k = 0; k < a->partCount; k++)
            if (a->parts[k].kind == SGA_PATH) paths++;
        if (paths > 0) STAT_ROW("MOTION PATHS", TextFormat("%d", paths));

        STAT_ROW("HEIGHT", TextFormat("%.2f", a->height));
        STAT_ROW("WIDTH", TextFormat("%.2f", a->radius*2.0f));

        // Which states carry animation - the thing the state selector steps
        // through, listed so an empty tab is expected rather than a surprise.
        char states[128] = { 0 };
        int pos = 0;
        for (int st = 0; st < SGA_STATE_COUNT; st++)
        {
            bool any = false;
            for (int k = 0; (k < a->partCount) && !any; k++)
                if (a->parts[k].anim[st].keyCount > 0) any = true;
            if (!any) continue;
            if (pos > 0) TextAppend(states, " ", &pos);
            TextAppend(states, StrategyAssetStateName(st), &pos);
        }
        STAT_ROW("ANIMATED", (pos > 0) ? states : "none - static");

        if (a->easeCount > 0)
            STAT_ROW("BAKED CURVES", TextFormat("%d", a->easeCount));

        return;     // STAT_ROW is #undef'd once, at the end of the function
    }

    if (e->cat == CAT_UNIT)
    {
        const UnitDef *d = StrategyUnitDef((UnitKind)e->kind);
        STAT_ROW("HEALTH", TextFormat("%.0f", d->maxHp));
        if (d->damage > 0.0f)
        {
            STAT_ROW("DAMAGE", TextFormat("%.0f", d->damage));
            STAT_ROW("ATTACK RANGE", TextFormat("%.1f", d->attackRange));
            STAT_ROW("ATTACK SPEED", TextFormat("%.1fs", d->attackPeriod));
        }
        STAT_ROW("MOVE SPEED", TextFormat("%.1f", d->moveSpeed));
        if (d->sightRange > 0.0f) STAT_ROW("SIGHT", TextFormat("%.1f", d->sightRange));
        if (d->preferredRange > 0.0f)
            STAT_ROW("KITES AT", TextFormat("%.1f", d->preferredRange));
        if (d->canGather) STAT_ROW("GATHERS", TextFormat("%.1fs / unit", d->gatherTime));
        if (d->corpseFood > 0) STAT_ROW("CORPSE FOOD", TextFormat("%d", d->corpseFood));
        if (d->trainTime > 0.0f)
        {
            STAT_ROW("TRAIN TIME", TextFormat("%.0fs", d->trainTime));
            STAT_ROW("COST", TextFormat("%d wood  %d stone  %d food  %d prov",
                                        d->cost[RES_WOOD], d->cost[RES_STONE],
                                        d->cost[RES_FOOD], d->cost[RES_PROVIDENCE]));
        }
        else STAT_ROW("ORIGIN", "wild - cannot be trained");
    }
    else if (e->cat == CAT_BUILDING)
    {
        const BuildingDef *d = StrategyBuildingDef((BuildingKind)e->kind);
        STAT_ROW("HEALTH", TextFormat("%.0f", d->maxHp));
        STAT_ROW("COST", TextFormat("%d wood  %d stone", d->cost[RES_WOOD],
                                    d->cost[RES_STONE]));
        STAT_ROW("BUILD TIME", TextFormat("%.0fs", d->buildTime));
        if (d->popCap > 0) STAT_ROW("POP CAP", TextFormat("+%d", d->popCap));
        if (d->critical)   STAT_ROW("CRITICAL", "losing all of these loses the game");

        if (d->trainableCount > 0)
        {
            // TextAppend dereferences its position argument unconditionally,
            // so it needs a real cursor - NULL would crash.
            char list[128] = { 0 };
            int pos = 0;
            for (int i = 0; i < d->trainableCount; i++)
            {
                if (i) TextAppend(list, "  ", &pos);
                TextAppend(list, StrategyUnitDef(d->trainable[i])->name, &pos);
            }
            STAT_ROW("TRAINS", list);
        }

        char acc[96] = { 0 };
        int accPos = 0;
        const char *resNames[RES_COUNT] = { "wood", "stone", "food", "prov" };
        for (int i = 0; i < RES_COUNT; i++)
        {
            if (!d->accepts[i]) continue;
            if (acc[0]) TextAppend(acc, "  ", &accPos);
            TextAppend(acc, resNames[i], &accPos);
        }
        if (acc[0]) STAT_ROW("ACCEPTS", acc);
        if (d->tendNode >= 0)
            STAT_ROW("PLANTS", TextFormat("%s  (%d units)",
                     StrategyNodeModel((NodeKind)d->tendNode)->name, d->tendAmount));
        STAT_ROW("SELL REFUND", TextFormat("%.0f%%", d->refundRate*100.0f));
    }
    else
    {
        const char *yields = "";
        switch ((NodeKind)e->kind)
        {
            case NODE_TREE:   yields = "WOOD";  break;
            case NODE_ROCK:   yields = "STONE"; break;
            case NODE_WHEAT:  yields = "FOOD";  break;
            case NODE_CORPSE: yields = "FOOD";  break;
            default: break;
        }
        STAT_ROW("YIELDS", yields);
        STAT_ROW("OWNER", "neutral - part of the terrain");
        if ((NodeKind)e->kind == NODE_CORPSE)
            STAT_ROW("SOURCE", "left behind by a hunted animal");
        if ((NodeKind)e->kind == NODE_WHEAT)
            STAT_ROW("SOURCE", "grows wild, or planted by a FARM");
        if ((NodeKind)e->kind == NODE_TREE)
            STAT_ROW("SOURCE", "grows wild, or planted by a FORESTRY");
    }

    #undef STAT_ROW
}

static void DrawInspect(const Layout *L)
{
    Vector2 screen = ScreenStateSize();
    Vector2 mp = GetMousePosition();
    float dt = GetFrameTime();

    int i = sc.inspectIndex;
    const CatalogEntry *e = &sc.catalog[i];

    // Stage on the left, stat sheet on the right. On a narrow window the sheet
    // gets a floor so it never collapses into an unreadable strip.
    float sheetW = screen.x*0.32f;
    if (sheetW < 240.0f*L->s) sheetW = 240.0f*L->s;
    if (sheetW > screen.x - 260.0f) sheetW = screen.x - 260.0f;

    Rectangle stage = { L->pad, L->headerH,
                        screen.x - sheetW - L->pad*3.0f,
                        screen.y - L->headerH - L->footerH };
    Rectangle sheet = { stage.x + stage.width + L->pad, L->headerH,
                        sheetW, stage.height };

    // -- rotation: the same grab-and-release rule as the gallery -------------
    // This view has no click action, so it just ends the grab on release.
    // ShowcaseDragPoll is still the backstop for a release this never sees
    // (button up over a chevron, the faction switcher, or off-window).
    bool overStage = CheckCollisionPointRec(mp, stage);
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && overStage) ShowcaseDragBegin(i);
    if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT) && (sc.dragTile >= 0)) ShowcaseDragEnd();
    SpinUpdate(&sc.spin[i], dt, overStage, (sc.dragTile == i));

    StateClockUpdate(i, dt);

    // -- 3D ------------------------------------------------------------------
    BeginTextureMode(sc.preview);
        ClearBackground(BLANK);
        PreviewDrawEntry(stage, i, EntryFaction(i), SpinYaw(&sc.spin[i]), true,
                         sc.inspectState, sc.stateClock);
    EndTextureMode();

    PanelBG(stage, false);
    DrawRectangleRec((Rectangle){ stage.x, stage.y, stage.width, 2.0f*L->s },
                     categoryAccent[e->cat]);
    PreviewBlit(stage, stage);

    // -- name plate over the stage -------------------------------------------
    DrawText(EntryName(i), (int)(stage.x + 20.0f*L->s),
             (int)(stage.y + stage.height - (float)L->fsHead - 30.0f*L->s),
             L->fsHead, COL_TEXT);
    DrawText(categoryName[e->cat], (int)(stage.x + 20.0f*L->s),
             (int)(stage.y + stage.height - 22.0f*L->s),
             L->fsSmall, categoryAccent[e->cat]);

    // Say who is being shown - and why some assets ignore the switcher.
    int shownFaction = EntryFaction(i);
    const char *whose = (shownFaction == FACTION_NEUTRAL) ? "NEUTRAL"
                      : (shownFaction == 0) ? "OWN" : "ENEMY";
    const char *note = (shownFaction != sc.faction)
                     ? TextFormat("%s  (always neutral)", whose) : whose;
    TextRight(note, (Rectangle){ stage.x, stage.y + stage.height - 22.0f*L->s,
                                 stage.width - 20.0f*L->s, 0.0f },
              L->fsSmall, COL_TEXT_DIM);

    DrawText("drag to turn  -  release to resume spin",
             (int)(stage.x + 20.0f*L->s), (int)(stage.y + 14.0f*L->s),
             L->fsSmall, Fade(COL_TEXT_DIM, 0.75f));

    // -- state tabs -----------------------------------------------------------
    // Every asset shows all six, not just the ones it animates: the empty tabs
    // are how an author sees what is still unauthored. A state with no keys is
    // dimmed and says so rather than silently showing the same still pose.
    {
        float tabH = 22.0f*L->s;
        float tabGap = 4.0f*L->s;
        float tx = stage.x + 20.0f*L->s;
        float ty = stage.y + 14.0f*L->s + (float)L->fsSmall + 10.0f*L->s;

        for (int st = 0; st < SGA_STATE_COUNT; st++)
        {
            const char *label = StrategyAssetStateName(st);
            float tw = (float)MeasureText(label, L->fsSmall) + 16.0f*L->s;
            Rectangle tab = { tx, ty, tw, tabH };

            bool has = EntryHasState(i, st);
            bool on = (sc.inspectState == st);
            bool hot = CheckCollisionPointRec(mp, tab);

            DrawRectangleRec(tab, on ? Fade(categoryAccent[e->cat], 0.22f)
                                     : Fade(COL_PANEL, hot ? 0.95f : 0.55f));
            DrawRectangleLinesEx(tab, 1.0f,
                                 on ? categoryAccent[e->cat]
                                    : (hot ? COL_LINE_HI : COL_LINE));

            Color tc = on ? COL_TEXT : (has ? COL_TEXT_DIM : Fade(COL_TEXT_DIM, 0.45f));
            DrawText(label, (int)(tab.x + 8.0f*L->s),
                     (int)(tab.y + (tabH - (float)L->fsSmall)*0.5f), L->fsSmall, tc);

            if (hot)
            {
                ShowcaseTip(tab, has ? TextFormat("Show the %s animation.", label)
                                     : TextFormat("%s is not animated on this "
                                                  "asset - it shows the idle pose.",
                                                  label));
                if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
                {
                    AudioPlayButton();
                    ShowcaseDragEnd();      // a press up here is not a grab
                    sc.inspectState = st;
                    sc.stateClock = 0.0f;   // restart, so the change is visible
                }
            }

            tx += tw + tabGap;
        }
    }

    // -- prev / next ----------------------------------------------------------
    float navW = 44.0f*L->s;
    Rectangle prev = { stage.x + 10.0f*L->s,
                       stage.y + (stage.height - navW)*0.5f, navW, navW };
    Rectangle next = { stage.x + stage.width - navW - 10.0f*L->s,
                       stage.y + (stage.height - navW)*0.5f, navW, navW };
    for (int b = 0; b < 2; b++)
    {
        Rectangle r = b ? next : prev;
        bool hot = CheckCollisionPointRec(mp, r);
        DrawRectangleRec(r, Fade(COL_PANEL, hot ? 0.95f : 0.6f));
        DrawRectangleLinesEx(r, 1.0f, hot ? COL_LINE_HI : COL_LINE);
        const char *gl = b ? "#119#" : "#118#";
        GuiSetStyle(DEFAULT, TEXT_COLOR_NORMAL, ColorToInt(hot ? COL_TEXT : COL_TEXT_DIM));
        GuiLabel((Rectangle){ r.x + navW*0.30f, r.y, r.width, r.height }, gl);
        if (hot && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) InspectStep(b ? 1 : -1);
    }

    // -- stat sheet -----------------------------------------------------------
    PanelBG(sheet, false);
    Rectangle inner = { sheet.x + 18.0f*L->s, sheet.y + 18.0f*L->s,
                        sheet.width - 36.0f*L->s, sheet.height - 36.0f*L->s };

    DrawText(EntryName(i), (int)inner.x, (int)inner.y, L->fsHead, COL_TEXT);
    float sy = inner.y + (float)L->fsHead + 6.0f*L->s;
    DrawText(TextFormat("%d of %d", i + 1, sc.catalogCount),
             (int)inner.x, (int)sy, L->fsSmall, COL_TEXT_DIM);
    sy += (float)L->fsSmall + 16.0f*L->s;
    DrawRectangleRec((Rectangle){ inner.x, sy, inner.width, 1.0f }, COL_LINE);
    sy += 14.0f*L->s;

    DrawInspectStats((Rectangle){ inner.x, sy, inner.width, 0.0f }, i, L);

    // Asset actions live at the bottom of the sheet. What they mean depends on
    // where the asset came from: an authored one can be edited in place and
    // deleted, a built-in can only be remixed into a new file.
    bool authored = (EntryAsset(i) != NULL);
    float bh = 30.0f*L->s;
    float by = sheet.y + sheet.height - 18.0f*L->s - bh;
    float bw = (inner.width - 8.0f*L->s)*0.5f;

    if (SheetButton((Rectangle){ inner.x, by, bw, bh },
                    authored ? "EDIT" : "REMIX", true,
                    authored ? "Open this asset in the forge."
                             : "Copy this built-in into a new asset you can edit. "
                               "The built-in itself is never changed.", L->fsSmall))
    {
        ForgeOpen(i, !authored);
        return;                 // the state is gone; touch nothing else
    }

    if (SheetButton((Rectangle){ inner.x + bw + 8.0f*L->s, by, bw, bh }, "DELETE",
                    authored, authored ? "Delete this asset's file."
                                       : "Built-in assets are part of the game and "
                                         "cannot be deleted.", L->fsSmall))
    {
        sc.confirmDelete = i;
    }

    DrawText("ASSET ACTIONS", (int)inner.x, (int)(by - (float)L->fsSmall - 6.0f*L->s),
             L->fsSmall, COL_TEXT_DIM);

    // Keyboard: arrows step, ESC backs out. A/D are letters, so they stand
    // down while a text box has the keyboard.
    if (!ShowcaseTyping())
    {
        if (IsKeyPressed(KEY_RIGHT) || IsKeyPressed(KEY_D)) InspectStep(1);
        if (IsKeyPressed(KEY_LEFT)  || IsKeyPressed(KEY_A)) InspectStep(-1);
    }
}

// ============================================================================
//  Map
//
//  The layout shown is the REAL one: Enter() runs StrategyWorldInit() and then
//  never ticks the simulation, so what you see is exactly the battlefield the
//  game spawns - two mirrored bases, the resource clusters, the wildlife -
//  frozen at t=0. Drawing it from the world's own arrays rather than
//  hand-placing markers means it cannot drift from the game.
// ============================================================================
static void DrawMapFull(const Layout *L)
{
    Vector2 screen = ScreenStateSize();
    Vector2 mp = GetMousePosition();

    Rectangle stage = { L->pad, L->headerH,
                        screen.x - 2.0f*L->pad,
                        screen.y - L->headerH - L->footerH };

    bool over = CheckCollisionPointRec(mp, stage);
    if (over && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) sc.mapDragging = true;
    if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) sc.mapDragging = false;

    if (sc.mapDragging)
    {
        Vector2 d = GetMouseDelta();
        sc.mapYaw = WrapAngle(sc.mapYaw - d.x*0.35f);
        sc.mapPitch -= d.y*0.25f;
        // Stay above the ground and below straight-down: past either the view
        // flips inside out.
        if (sc.mapPitch < 12.0f) sc.mapPitch = 12.0f;
        if (sc.mapPitch > 86.0f) sc.mapPitch = 86.0f;
    }
    if (over)
    {
        sc.mapZoom -= GetMouseWheelMove()*0.08f;
        if (sc.mapZoom < 0.35f) sc.mapZoom = 0.35f;
        if (sc.mapZoom > 1.60f) sc.mapZoom = 1.60f;
    }

    BeginTextureMode(sc.preview);
        ClearBackground(BLANK);
        PreviewDrawMap(stage, sc.mapYaw, sc.mapPitch, sc.mapZoom);
    EndTextureMode();

    PanelBG(stage, false);
    PreviewBlit(stage, stage);

    DrawText("BATTLEFIELD", (int)(stage.x + 20.0f*L->s),
             (int)(stage.y + 16.0f*L->s), L->fsHead, COL_TEXT);
    DrawText(TextFormat("%.0f x %.0f  -  drag to orbit, wheel to zoom",
                        2.0f*STRAT_GROUND_HALF, 2.0f*STRAT_GROUND_HALF),
             (int)(stage.x + 20.0f*L->s),
             (int)(stage.y + 20.0f*L->s + (float)L->fsHead),
             L->fsSmall, COL_TEXT_DIM);

    // Faction legend, so the two bases are readable as sides.
    float ly = stage.y + stage.height - 26.0f*L->s;
    float lx = stage.x + 20.0f*L->s;
    const char *legend[2] = { "OWN BASE", "ENEMY BASE" };
    for (int f = 0; f < STRAT_FACTIONS; f++)
    {
        float sw = 10.0f*L->s;
        DrawRectangleRec((Rectangle){ lx, ly + 2.0f*L->s, sw, sw },
                         StrategyFactionTint(f));
        DrawText(legend[f], (int)(lx + sw + 6.0f*L->s), (int)ly,
                 L->fsSmall, COL_TEXT_DIM);
        lx += (float)MeasureText(legend[f], L->fsSmall) + sw + 24.0f*L->s;
    }
}

// The small always-visible map panel in the gallery footer area.
// ============================================================================
//  Binding view: which asset stands in for which game role
//
//  Every role in the game listed against the asset currently standing in for
//  it, and any asset assignable to any role. That freedom is the point, not an
//  oversight - a warrior may look like a worker, a town hall like a tree - so
//  the picker offers the WHOLE catalog with no category filtering, and the
//  header says as much rather than leaving it looking like a missing check.
//
//  This drives the SHOWCASE PREVIEW only. strategy_world.c still draws the game
//  from its own tables; wiring bindings into live gameplay is deliberately out
//  of scope, which keeps a bad binding from being able to break the game.
// ============================================================================

// The built-in model for a role, which is both the fallback look and where the
// role's display name comes from - so a role is never a bare enum number.
static const StrategyModel *BindRoleModel(int family, int role)
{
    switch (family)
    {
        case SGB_ROLE_UNIT:     return StrategyUnitModel((UnitKind)role);
        case SGB_ROLE_BUILDING: return StrategyBuildingModel((BuildingKind)role);
        case SGB_ROLE_NODE:     return StrategyNodeModel((NodeKind)role);
        default:                return NULL;
    }
}

static const char *BindRoleName(int family, int role)
{
    const StrategyModel *m = BindRoleModel(family, role);
    return m ? m->name : "?";
}

// The accent a family borrows from the gallery, so the two screens agree about
// what colour a unit is.
static Color BindFamilyAccent(int family)
{
    switch (family)
    {
        case SGB_ROLE_BUILDING: return categoryAccent[CAT_BUILDING];
        case SGB_ROLE_NODE:     return categoryAccent[CAT_NODE];
        default:                return categoryAccent[CAT_UNIT];
    }
}

static void BindPickerOpen(int family, int role)
{
    sc.bindFamily = family;
    sc.bindRole = role;
    sc.bindPickScroll = 0.0f;
}

static void BindPickerClose(void)
{
    sc.bindFamily = -1;
    sc.bindRole = -1;
}

static bool BindPickerOpenNow(void)
{
    return (sc.bindFamily >= 0) && (sc.bindRole >= 0);
}

// ---------------------------------------------------------------------------
//  The picker overlay: every authored asset, plus "use the built-in".
// ---------------------------------------------------------------------------
static void BindPickerGui(const Layout *L)
{
    if (!BindPickerOpenNow()) return;

    sc_inModal = true;

    Vector2 screen = ScreenStateSize();
    Vector2 mp = GetMousePosition();

    float mw = 420.0f*L->s, mh = 380.0f*L->s;
    if (mw > screen.x - 40.0f) mw = screen.x - 40.0f;
    if (mh > screen.y - 40.0f) mh = screen.y - 40.0f;
    Rectangle m = { (screen.x - mw)*0.5f, (screen.y - mh)*0.5f, mw, mh };

    DrawRectangle(0, 0, (int)screen.x, (int)screen.y, (Color){ 0, 0, 0, 165 });
    DrawRectangleRec(m, COL_PANEL);
    DrawRectangleLinesEx(m, 1.0f, COL_LINE_HI);

    float pad = 14.0f*L->s;
    DrawText(TextFormat("ASSET FOR %s", BindRoleName(sc.bindFamily, sc.bindRole)),
             (int)(m.x + pad), (int)(m.y + pad), L->fsBody, COL_TEXT);
    DrawText("Any asset fits any role - the category is only a label.",
             (int)(m.x + pad), (int)(m.y + pad + (float)L->fsBody + 5.0f*L->s),
             L->fsSmall, COL_TEXT_DIM);

    float rowH = 30.0f*L->s;
    float listY = m.y + pad + (float)L->fsBody + (float)L->fsSmall + 16.0f*L->s;
    float listH = m.y + mh - listY - pad - rowH - 10.0f*L->s;
    Rectangle list = { m.x + pad, listY, mw - pad*2.0f, listH };

    // Row 0 is always "built-in", then one row per authored asset. Built-ins
    // are not offered as a SOURCE here: they are what a role already falls back
    // to, and remixing one into a file is how you bind its look.
    int rows = StrategyCatalogCount() + 1;
    float contentH = (float)rows*rowH;

    if (CheckCollisionPointRec(mp, list))
        sc.bindPickScroll -= GetMouseWheelMove()*40.0f*L->s;
    float maxScroll = contentH - listH;
    if (maxScroll < 0.0f) maxScroll = 0.0f;
    if (sc.bindPickScroll > maxScroll) sc.bindPickScroll = maxScroll;
    if (sc.bindPickScroll < 0.0f) sc.bindPickScroll = 0.0f;

    const char *bound = StrategyBindingGet(sc.bindFamily, sc.bindRole);

    BeginScissorMode((int)list.x, (int)list.y, (int)list.width, (int)list.height);
    for (int r = 0; r < rows; r++)
    {
        Rectangle row = { list.x, list.y + (float)r*rowH - sc.bindPickScroll,
                          list.width, rowH - 2.0f*L->s };

        // A scissor clips PIXELS but not hit-testing, so a row scrolled out of
        // the list would still take a click from behind the panel edge.
        if ((row.y + row.height < list.y) || (row.y > list.y + list.height)) continue;

        bool isBuiltin = (r == 0);
        const char *name = isBuiltin ? "" : StrategyCatalogAssets()[r - 1].name;
        bool active = isBuiltin ? (bound[0] == '\0') : TextIsEqual(bound, name);

        bool hot = CheckCollisionPointRec(mp, row) &&
                   CheckCollisionPointRec(mp, list);

        DrawRectangleRec(row, active ? Fade(COL_LINE_HI, 0.28f)
                                     : (hot ? COL_PANEL_HI : COL_PANEL));
        if (active) DrawRectangleLinesEx(row, 1.0f, COL_LINE_HI);

        if (isBuiltin)
        {
            DrawText("- built-in model -", (int)(row.x + 10.0f*L->s),
                     (int)(row.y + (row.height - (float)L->fsSmall)*0.5f),
                     L->fsSmall, active ? COL_TEXT : COL_TEXT_DIM);
        }
        else
        {
            const SgaAsset *a = &StrategyCatalogAssets()[r - 1];
            DrawText(a->name, (int)(row.x + 10.0f*L->s),
                     (int)(row.y + (row.height - (float)L->fsSmall)*0.5f),
                     L->fsSmall, active ? COL_TEXT : COL_TEXT_DIM);

            const char *tag = TextFormat("%s / %s",
                                         StrategyAssetCategoryName(a->category),
                                         (a->subtype[0] ? a->subtype : "-"));
            int tw = MeasureText(tag, L->fsSmall);
            DrawText(tag, (int)(row.x + row.width - (float)tw - 10.0f*L->s),
                     (int)(row.y + (row.height - (float)L->fsSmall)*0.5f),
                     L->fsSmall, Fade(categoryAccent[CAT_CUSTOM], 0.85f));
        }

        if (hot && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
        {
            AudioPlayButton();
            StrategyBindingSet(sc.bindFamily, sc.bindRole, isBuiltin ? NULL : name);
            StrategyBindingsSave();
            BindPickerClose();
            EndScissorMode();
            sc_inModal = false;
            return;
        }
    }
    EndScissorMode();

    if (StrategyCatalogCount() == 0)
    {
        const char *msg = "No authored assets yet - CREATE one first.";
        int w = MeasureText(msg, L->fsSmall);
        DrawText(msg, (int)(list.x + (list.width - (float)w)*0.5f),
                 (int)(list.y + listH*0.45f), L->fsSmall, COL_TEXT_DIM);
    }

    float by = m.y + mh - rowH - pad*0.5f;
    if (SheetButton((Rectangle){ m.x + mw - 96.0f*L->s - pad, by, 96.0f*L->s, rowH },
                    "CLOSE", true, NULL, L->fsSmall))
    { BindPickerClose(); sc_inModal = false; return; }

    // ESC is NOT handled here. Update() runs before Gui() and owns the whole
    // chain, so a key handled in both places would close the picker AND the
    // view behind it on one press.

    sc_inModal = false;
}

// ---------------------------------------------------------------------------
//  The table
// ---------------------------------------------------------------------------
static void DrawBindings(const Layout *L)
{
    Vector2 mp = GetMousePosition();
    Rectangle area = L->content;

    int missing = StrategyBindingsMissingCount(StrategyCatalogAssets(), StrategyCatalogCount());

    // -- explanation band -----------------------------------------------------
    float bandH = 40.0f*L->s;
    Rectangle band = { area.x, area.y, area.width, bandH };
    DrawRectangleRec(band, COL_PANEL);
    DrawRectangleLinesEx(band, 1.0f, COL_LINE);
    DrawText("Any asset can stand in for any role. A category is a label for "
             "finding assets, never a restriction.",
             (int)(band.x + 12.0f*L->s),
             (int)(band.y + (bandH - (float)L->fsSmall)*0.5f - (missing ? 6.0f*L->s : 0.0f)),
             L->fsSmall, COL_TEXT_DIM);

    if (missing > 0)
    {
        // A binding whose asset is not on this machine is KEPT, not erased -
        // so it has to be visible, or the role silently shows its built-in and
        // the user thinks the binding never saved.
        DrawText(TextFormat("%d role%s point at an asset that is not here - the "
                            "built-in is drawn instead.",
                            missing, (missing == 1) ? "" : "s"),
                 (int)(band.x + 12.0f*L->s),
                 (int)(band.y + bandH*0.5f + 2.0f*L->s),
                 L->fsSmall, categoryAccent[CAT_BUILDING]);
    }

    Rectangle list = { area.x, area.y + bandH + L->gap*0.5f, area.width,
                       area.height - bandH - L->gap*0.5f };
    if (list.height < 40.0f) return;

    // -- rows -----------------------------------------------------------------
    float rowH = 30.0f*L->s;
    float headH = 22.0f*L->s;

    // Measure first so the scroll clamp is right on the frame the view opens.
    float contentH = 0.0f;
    for (int f = 0; f < SGB_ROLE_FAMILY_COUNT; f++)
        contentH += headH + (float)StrategyBindingsRoleCount(f)*rowH + L->gap;

    if (CheckCollisionPointRec(mp, list) && !sc_inModal)
        sc.bindScroll -= GetMouseWheelMove()*48.0f*L->s;
    float maxScroll = contentH - list.height;
    if (maxScroll < 0.0f) maxScroll = 0.0f;
    if (sc.bindScroll > maxScroll) sc.bindScroll = maxScroll;
    if (sc.bindScroll < 0.0f) sc.bindScroll = 0.0f;

    BeginScissorMode((int)list.x, (int)list.y, (int)list.width, (int)list.height);

    float y = list.y - sc.bindScroll;
    for (int f = 0; f < SGB_ROLE_FAMILY_COUNT; f++)
    {
        Color accent = BindFamilyAccent(f);

        DrawRectangleRec((Rectangle){ list.x, y + headH*0.5f - 1.0f*L->s,
                                      list.width, 2.0f*L->s },
                         Fade(accent, 0.25f));
        const char *fam = StrategyBindingsFamilyName(f);
        int fw = MeasureText(fam, L->fsSmall);
        DrawRectangleRec((Rectangle){ list.x, y, (float)fw + 22.0f*L->s, headH }, COL_BG);
        DrawText(fam, (int)(list.x + 10.0f*L->s),
                 (int)(y + (headH - (float)L->fsSmall)*0.5f), L->fsSmall, accent);
        y += headH;

        int n = StrategyBindingsRoleCount(f);
        for (int r = 0; r < n; r++)
        {
            Rectangle row = { list.x, y, list.width, rowH - 2.0f*L->s };
            y += rowH;

            // Same reason as the picker: a scissor does not clip hit-testing.
            if ((row.y + row.height < list.y) || (row.y > list.y + list.height)) continue;

            bool isBound = StrategyBindingIsBound(f, r);
            const SgaAsset *res = StrategyBindingResolve(f, r, StrategyCatalogAssets(), StrategyCatalogCount());
            bool broken = isBound && (res == NULL);

            bool hot = CheckCollisionPointRec(mp, row) &&
                       CheckCollisionPointRec(mp, list) && !sc_inModal;

            DrawRectangleRec(row, hot ? COL_PANEL_HI : COL_PANEL);
            DrawRectangleRec((Rectangle){ row.x, row.y, 3.0f*L->s, row.height },
                             Fade(accent, isBound ? 1.0f : 0.35f));

            DrawText(BindRoleName(f, r), (int)(row.x + 14.0f*L->s),
                     (int)(row.y + (row.height - (float)L->fsSmall)*0.5f),
                     L->fsSmall, COL_TEXT);

            // The bound asset, mid-row, so the eye can run down one column.
            float valX = row.x + row.width*0.42f;
            const char *val = isBound ? StrategyBindingGet(f, r) : "- built-in model -";
            DrawText(val, (int)valX,
                     (int)(row.y + (row.height - (float)L->fsSmall)*0.5f),
                     L->fsSmall,
                     broken ? categoryAccent[CAT_BUILDING]
                            : (isBound ? categoryAccent[CAT_CUSTOM] : COL_TEXT_DIM));

            if (broken)
            {
                const char *warn = "missing";
                DrawText(warn, (int)(valX + (float)MeasureText(val, L->fsSmall) + 10.0f*L->s),
                         (int)(row.y + (row.height - (float)L->fsSmall)*0.5f),
                         L->fsSmall, categoryAccent[CAT_BUILDING]);
            }

            // -- actions, right-aligned ---------------------------------------
            float bw = 76.0f*L->s;
            Rectangle change = { row.x + row.width - bw - 6.0f*L->s, row.y + 2.0f*L->s,
                                 bw, row.height - 4.0f*L->s };
            if (SheetButton(change, isBound ? "CHANGE" : "ASSIGN", true,
                            "Pick which asset is drawn for this role.", L->fsSmall))
            { BindPickerOpen(f, r); EndScissorMode(); return; }

            if (isBound)
            {
                Rectangle clr = { change.x - 70.0f*L->s - 6.0f*L->s, change.y,
                                  70.0f*L->s, change.height };
                if (SheetButton(clr, "CLEAR", true,
                                "Go back to this role's built-in model.", L->fsSmall))
                {
                    StrategyBindingSet(f, r, NULL);
                    StrategyBindingsSave();
                    EndScissorMode();
                    return;
                }
            }
        }
        y += L->gap;
    }

    EndScissorMode();

    if (maxScroll > 0.0f)
    {
        float thumbH = list.height*(list.height/contentH);
        if (thumbH < 24.0f) thumbH = 24.0f;
        float t = sc.bindScroll/maxScroll;
        DrawRectangleRec((Rectangle){ list.x + list.width + 4.0f*L->s,
                                      list.y + t*(list.height - thumbH),
                                      4.0f*L->s, thumbH },
                         Fade(COL_LINE_HI, 0.7f));
    }
}

static void DrawMapPanel(Rectangle r, const Layout *L)
{
    Vector2 mp = GetMousePosition();
    bool hot = CheckCollisionPointRec(mp, r);

    sc.mapPanelSpin.autoYaw = WrapAngle(sc.mapPanelSpin.autoYaw +
                                        SPIN_SPEED*0.35f*GetFrameTime());

    BeginTextureMode(sc.preview);
        ClearBackground(BLANK);
        PreviewDrawMap(r, sc.mapPanelSpin.autoYaw, 62.0f, 1.0f);
    EndTextureMode();

    PanelBG(r, hot);
    PreviewBlit(r, r);
    DrawRectangleLinesEx(r, hot ? 2.0f : 1.0f, hot ? COL_LINE_HI : COL_LINE);

    DrawText("MAP", (int)(r.x + 10.0f*L->s), (int)(r.y + 8.0f*L->s),
             L->fsSmall, COL_TEXT);
    if (hot)
    {
        DrawText("click to open", (int)(r.x + 10.0f*L->s),
                 (int)(r.y + r.height - (float)L->fsSmall - 8.0f*L->s),
                 L->fsSmall, COL_TEXT);
    }

    if (hot && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
    {
        AudioPlayButton();
        ShowcaseSetView(VIEW_MAP);
    }
}

// ============================================================================
//  Header / footer chrome, shared by every view
// ============================================================================
static void DrawHeader(const Layout *L)
{
    Vector2 screen = ScreenStateSize();

    DrawRectangleRec((Rectangle){ 0.0f, 0.0f, screen.x, L->headerH }, COL_PANEL);
    DrawRectangleRec((Rectangle){ 0.0f, L->headerH - 1.0f, screen.x, 1.0f }, COL_LINE);

    float cy = (L->headerH - (float)L->fsHead)*0.5f;
    DrawText("STRATEGY ASSETS", (int)L->pad, (int)cy, L->fsHead, COL_TEXT);

    const char *sub = (sc.view == VIEW_MAP) ? "battlefield"
                    : (sc.view == VIEW_INSPECT) ? "inspecting"
                    : (sc.view == VIEW_BIND) ? "role bindings"
                    : TextFormat("%d assets", sc.catalogCount);
    DrawText(sub, (int)(L->pad + (float)MeasureText("STRATEGY ASSETS", L->fsHead) + 14.0f*L->s),
             (int)(cy + (float)L->fsHead - (float)L->fsSmall - 1.0f),
             L->fsSmall, COL_TEXT_DIM);

    // The faction switcher rides in the header, right-aligned, on every view -
    // so switching sides never means going back to the gallery first.
    float swW = 300.0f*L->s;
    if (swW > screen.x*0.45f) swW = screen.x*0.45f;
    float swH = 34.0f*L->s;
    Rectangle sw = { screen.x - L->pad - swW, (L->headerH - swH)*0.5f, swW, swH };
    FactionSwitcher(sw, L->fsSmall);
}

// ============================================================================
//  Delete confirmation
//
//  Deleting is the only irreversible thing the gallery can do - the file goes
//  from disk and no undo ring in this state holds it - so it always asks, and
//  names the asset it is about to remove rather than saying "this item".
// ============================================================================
static void ConfirmDeleteGui(const Layout *L)
{
    int i = sc.confirmDelete;
    if (i < 0) return;

    sc_inModal = true;

    // The catalog can be rebuilt between the click and this draw; a stale index
    // would confirm a delete on whatever slid into that slot.
    if ((i >= sc.catalogCount) || (EntryAsset(i) == NULL))
    { sc.confirmDelete = -1; sc_inModal = false; return; }

    Vector2 screen = ScreenStateSize();
    float mw = 420.0f*L->s, mh = 150.0f*L->s;
    if (mw > screen.x - 40.0f) mw = screen.x - 40.0f;
    Rectangle m = { (screen.x - mw)*0.5f, (screen.y - mh)*0.5f, mw, mh };

    DrawRectangle(0, 0, (int)screen.x, (int)screen.y, (Color){ 0, 0, 0, 150 });
    DrawRectangleRec(m, COL_PANEL);
    DrawRectangleLinesEx(m, 1.0f, COL_LINE_HI);
    DrawText("DELETE ASSET", (int)(m.x + 16.0f*L->s), (int)(m.y + 14.0f*L->s),
             L->fsBody, COL_TEXT);
    DrawText(TextFormat("Delete \"%s\" from disk? This cannot be undone.", EntryName(i)),
             (int)(m.x + 16.0f*L->s), (int)(m.y + 14.0f*L->s + (float)L->fsBody + 8.0f*L->s),
             L->fsSmall, COL_TEXT_DIM);

    float bh = 26.0f*L->s;
    float by = m.y + mh - bh - 14.0f*L->s;
    if (SheetButton((Rectangle){ m.x + mw - 100.0f*L->s, by, 88.0f*L->s, bh },
                    "DELETE", true, NULL, L->fsSmall))
    {
        // The name is about to stop existing, and the binding file is a
        // separate document - so clear the roles that pointed at it and write
        // that out, or the next run resolves them to nothing with no
        // explanation of what happened.
        const char *name = EntryName(i);
        bool ok = StrategyAssetDelete(name);
        if (ok && (StrategyBindingsRename(name, NULL) > 0)) StrategyBindingsSave();
        sc.confirmDelete = -1;

        // The catalog points INTO the shared asset array, so it is rebuilt whole
        // - every entry after the deleted one has moved.
        if (ok)
        {
            CatalogReload();
            if (sc.inspectIndex >= sc.catalogCount) sc.inspectIndex = sc.catalogCount - 1;
            if (sc.inspectIndex < 0) sc.inspectIndex = 0;
            if (sc.catalogCount == 0) ShowcaseSetView(VIEW_GALLERY);
        }
        sc_inModal = false;
        return;
    }
    if (SheetButton((Rectangle){ m.x + mw - 196.0f*L->s, by, 88.0f*L->s, bh },
                    "CANCEL", true, NULL, L->fsSmall))
        sc.confirmDelete = -1;

    if (IsKeyPressed(KEY_ESCAPE)) sc.confirmDelete = -1;

    sc_inModal = false;
}

static void DrawFooter(const Layout *L)
{
    Vector2 screen = ScreenStateSize();
    Vector2 mp = GetMousePosition();

    Rectangle bar = { 0.0f, screen.y - L->footerH, screen.x, L->footerH };
    DrawRectangleRec(bar, COL_PANEL);
    DrawRectangleRec((Rectangle){ 0.0f, bar.y, screen.x, 1.0f }, COL_LINE);

    float bh = 32.0f*L->s;
    float by = bar.y + (L->footerH - bh)*0.5f;
    float x = L->pad;

    // BACK / gallery
    float backW = 90.0f*L->s;
    Rectangle back = { x, by, backW, bh };
    bool backHot = CheckCollisionPointRec(mp, back);
    DrawRectangleRec(back, backHot ? COL_PANEL_HI : COL_PANEL);
    DrawRectangleLinesEx(back, 1.0f, backHot ? COL_LINE_HI : COL_LINE);
    const char *backLabel = (sc.view == VIEW_GALLERY) ? "MENU" : "GALLERY";
    DrawText(backLabel,
             (int)(back.x + (backW - (float)MeasureText(backLabel, L->fsSmall))*0.5f),
             (int)(by + (bh - (float)L->fsSmall)*0.5f), L->fsSmall,
             backHot ? COL_TEXT : COL_TEXT_DIM);
    if (backHot && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
    {
        AudioPlayButton();
        if (sc.view == VIEW_GALLERY) AppStateTransition(&app_state_main_menu);
        else ShowcaseSetView(VIEW_GALLERY);
        return;     // the state may be gone; touch nothing else this frame
    }
    x += backW + L->gap;

    // Authoring actions. REMIX works on whatever the gallery is pointing at,
    // which in the gallery view is the hovered tile and in the inspector is the
    // asset on screen - so the button means the same thing in both.
    float wipW = 96.0f*L->s;
    if (SheetButton((Rectangle){ x, by, wipW, bh }, "CREATE", true,
                    "Build a new asset from scratch.", L->fsSmall))
    {
        ForgeOpen(-1, false);
        return;                 // the state is gone; touch nothing else
    }
    x += wipW + 8.0f*L->s;

    int target = (sc.view == VIEW_INSPECT) ? sc.inspectIndex
               : ((sc.hoverTile >= 0) ? sc.hoverTile : -1);
    bool canRemix = (target >= 0) && (target < sc.catalogCount);
    if (SheetButton((Rectangle){ x, by, wipW, bh }, "REMIX", canRemix,
                    canRemix ? "Copy the selected asset into a new one you can edit."
                             : "Hover an asset first, or open one to inspect it.",
                    L->fsSmall))
    {
        ForgeOpen(target, EntryAsset(target) != NULL);
        return;
    }
    x += wipW + 8.0f*L->s;

    // Role binding. Named for what it does rather than "assign to faction":
    // a binding is per-ROLE and both factions share it, so the old label
    // promised something the file does not model.
    {
        float bindW = wipW + 30.0f*L->s;
        bool inBind = (sc.view == VIEW_BIND);
        if (SheetButton((Rectangle){ x, by, bindW, bh },
                        inBind ? "BACK TO GALLERY" : "ROLE BINDINGS", true,
                        inBind ? "Return to the asset gallery."
                               : "Choose which asset is drawn for each game role.",
                        L->fsSmall))
        {
            ShowcaseSetView(inBind ? VIEW_GALLERY : VIEW_BIND);
            return;
        }
    }

    // PLAY, right-aligned: the way on into the game.
    float playW = 190.0f*L->s;
    Rectangle play = { screen.x - L->pad - playW, by, playW, bh };
    bool playHot = CheckCollisionPointRec(mp, play);
    Color accent = StrategyFactionTint(0);
    DrawRectangleRec(play, playHot ? Fade(accent, 0.85f) : Fade(accent, 0.55f));
    DrawRectangleLinesEx(play, 1.0f, accent);
    const char *pl = "PLAY STRATEGY";
    DrawText(pl, (int)(play.x + (playW - (float)MeasureText(pl, L->fsBody))*0.5f),
             (int)(by + (bh - (float)L->fsBody)*0.5f), L->fsBody, COL_TEXT);
    if (playHot && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
    {
        AudioPlayButton();
        AppStateTransition(&app_state_strategy);
        return;
    }
}

// ============================================================================
//  AppState hooks
// ============================================================================
static void Enter()
{
    ScreenState *ss = ScreenStateGet();
    ss->clear_color = COL_BG;

    memset(&sc, 0, sizeof(sc));

    // strategy_asset.c deliberately does not link the game's world (it has to
    // stay headless-testable), so it learns the faction palette through this
    // hook. Installed before anything draws.
    StrategyAssetSetFactionTint(StrategyFactionTint);

    // The bindings module stays headless, so it learns the game's enum sizes
    // here rather than including strategy_types.h itself. Installed BEFORE the
    // load, so a file written by a longer-enum build is walked correctly.
    StrategyBindingsSetRoleCounts(UNIT_KIND_COUNT, BLD_COUNT, NODE_KIND_COUNT);
    StrategyBindingsLoad();

    CatalogReload();        // scans SGA_DIR, then builds the catalog around it

    sc.view = VIEW_GALLERY;
    sc.faction = 0;
    sc.dragTile = -1;
    sc.hoverTile = -1;
    sc.inspectIndex = 0;

    // Framed so BOTH bases are on screen at rest - the diagonal of the board
    // is what has to fit, not its edge.
    sc.mapYaw = 35.0f;
    sc.mapPitch = 55.0f;
    sc.mapZoom = 1.25f;
    sc.confirmDelete = -1;      // memset would leave this pointing at entry 0
    sc.bindFamily = -1;         // same trap: 0/0 would read as "picker is open"
    sc.bindRole = -1;

    // Stagger the starting angles so the grid does not read as one rigid
    // block of identically-posed models.
    for (int i = 0; i < sc.catalogCount; i++)
        sc.spin[i].autoYaw = WrapAngle((float)i*37.0f);

    // The battlefield, spawned once and then left frozen: this state never
    // calls StrategyWorldUpdate, so nothing moves and nothing fights. The
    // real strategy state re-inits the world in its own Enter(), so a world
    // left dirty here cannot leak into a game.
    StrategyWorldInit();
}

static void Exit()
{
    if (sc.preview.id)
    {
        UnloadRenderTexture(sc.preview);
        sc.preview = (RenderTexture2D){ 0 };
        sc.previewW = 0;
        sc.previewH = 0;
    }
}

static void Update()
{
    if (ShowcaseTyping())
    {
        // ESC still works, but it means "leave the box", not "leave the view".
        // Innermost first, the way the zen editor's ESC chain does it.
        if (IsKeyPressed(KEY_ESCAPE)) sc.searchEdit = false;
        return;
    }

    if (IsKeyPressed(KEY_ESCAPE))
    {
        // Innermost first, the way the zen editor's chain does it: the picker
        // is inside the binding view, so escape closes it without also
        // throwing away the view behind it. (BindPickerGui also handles this,
        // but only on frames it actually draws.)
        if (BindPickerOpenNow()) { BindPickerClose(); return; }

        if (sc.view == VIEW_GALLERY) AppStateTransition(&app_state_main_menu);
        else ShowcaseSetView(VIEW_GALLERY);
        return;
    }

    if ((sc.view == VIEW_GALLERY) && IsKeyPressed(KEY_M)) ShowcaseSetView(VIEW_MAP);
}

// Everything is drawn in Gui() at window resolution - see the file header.
static void Draw()
{
}

static void Gui()
{
    Vector2 screen = ScreenStateSize();
    PreviewEnsure((int)screen.x, (int)screen.y);
    if (!sc.preview.id) return;

    Layout L = LayoutCompute();

    // Before any view runs: if the button is no longer down, the grab is over.
    // This is what guarantees a release ANYWHERE ends it - over a button, over
    // the header, or outside the window entirely.
    ShowcaseDragPoll();

    // raygui only styles the WIP buttons and the nav glyphs here; everything
    // else is drawn directly so the screen is not bound to the default theme.
    int baseSize = GuiGetFont().baseSize;
    GuiSetStyle(DEFAULT, TEXT_SIZE, baseSize*(int)L.s);
    GuiSetIconScale((int)L.s);

    DrawRectangleRec((Rectangle){ 0.0f, 0.0f, screen.x, screen.y }, COL_BG);

    switch (sc.view)
    {
        case VIEW_GALLERY:
        {
            DrawGallery(&L);

            // Map panel sits in its own rail beside the grid (see
            // LayoutCompute), so it never covers a tile.
            if (L.mapRail)
            {
                Rectangle panel = { L.content.x + L.content.width + L.gap*1.5f,
                                    L.content.y, L.mapW, L.mapH };
                DrawMapPanel(panel, &L);

                DrawText("press M for fullscreen",
                         (int)panel.x, (int)(panel.y + panel.height + 6.0f*L.s),
                         L.fsSmall, COL_TEXT_DIM);
            }
        } break;

        case VIEW_INSPECT: DrawInspect(&L); break;
        case VIEW_BIND: DrawBindings(&L); break;

        case VIEW_MAP:     DrawMapFull(&L); break;
        default: break;
    }

    // A view above may have opened the forge. AppStateTransition switches
    // states synchronously, so the forge has ALREADY painted its own full frame
    // by the time we get here - drawing our header, footer and tooltip over the
    // top of it would flash this screen's furniture across the forge for one
    // frame. The views return early for the same reason; this catches the case
    // where the transition happened several draw calls deep.
    if (!AppStateIsCurrent(&app_state_strategy_showcase)) return;

    DrawHeader(&L);
    DrawFooter(&L);

    // The footer's CREATE / REMIX / PLAY / BACK all transition, and a tip
    // recorded by the button that fired would otherwise paint onto the screen
    // we just left.
    if (!AppStateIsCurrent(&app_state_strategy_showcase)) return;

    // Last, so a tip paints over every panel it might overlap.
    ConfirmDeleteGui(&L);
    BindPickerGui(&L);
    ShowcaseTipDraw(L.fsSmall);
}
