// ============================================================================
//  strategy_forge.c  -  the asset forge (see strategy_forge.h)
//
//  EVERYTHING HERE IS SCREEN SPACE. main.c runs Draw() inside the 320x180
//  letterboxed render target but runs Gui() AFTER EndTextureMode, against the
//  real window. A three-pane part editor cannot live in 320 pixels, so the
//  whole tool is drawn in Gui() and Draw() is empty. Mouse coordinates are raw
//  GetMousePosition() with NO Screen2Target - that call converts INTO target
//  space, which is exactly the space this state does not use. The shape editor
//  and the showcase both make this same call, for this same reason.
//
//  MEMORY. An SgaAsset is ~68 KB on the Web tier and ~450 KB on the desktop one
//  (see the warning at the top of strategy_asset.h). That single fact shapes
//  this file:
//    - s_doc, s_ref and the undo ring are all FILE-STATIC. Not one SgaAsset is
//      ever a local, because two or three on a 1 MB Web stack is a segfault at
//      the declaration rather than anywhere near the code that looks wrong.
//    - the undo ring is DELIBERATELY SHORT (SF_UNDO_MAX). At desktop capacity
//      it is already ~3.6 MB; a 32-deep ring like the shape editor's would be
//      14 MB of mostly-empty part arrays. Depth is bought back by pushing ONE
//      STEP PER GESTURE, not per frame - a slider drag is one Ctrl+Z, which is
//      what a person means by "undo that".
//
//  UNDO GRANULARITY is therefore the same rule the shape editor uses for
//  strokes: a gesture opens on mouse-press, and the push happens once at the
//  moment the first real change lands. s_gestureOpen is that latch.
// ============================================================================

#include "raylib.h"
#include "raygui.h"
#include "raymath.h"
#include "rlgl.h"
#include "strategy_forge.h"
#include "../strategy_asset/strategy_asset_io.h"
#include "../strategy_asset/strategy_bindings.h"
#include "../strategy_asset/strategy_asset_ease.h"
#include "../anim/anim.h"
#include "../anim/anim_ease_custom.h"
#include "../strategy_ui/strategy_ui.h"
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
AppState app_state_strategy_forge = { Enter, Exit, Update, Draw, Gui, "StrategyForge" };

// ---------------------------------------------------------------------------
//  Metrics. Screen-space pixels, scaled by SF_S() against a 1280-wide design.
// ---------------------------------------------------------------------------
#define SF_LEFT_W       230.0f      // part list
#define SF_RIGHT_W      290.0f      // part inspector
#define SF_HEADER_H      52.0f      // a label line + a short control row
#define SF_FOOTER_H      64.0f      // a label line + a control row
#define SF_RH            22.0f      // one control row
#define SF_GAP            6.0f
#define SF_PAD           14.0f
#define SF_STATUS_SECS    3.0f
#define SF_BROWSE_MAX    128

#define SF_TIMELINE_H   150.0f      // lane strip + ruler + transport
#define SF_LANE_H        18.0f      // one part's key lane
#define SF_KEY_R          5.0f      // key diamond half-width, unscaled

// Short on purpose - see the MEMORY note above.
#define SF_UNDO_MAX       8

// The palette now has ONE owner (strategy_ui.h); these are the local spellings
// the ~200 call sites below already use.
#define COL_BG        UI_COL_BG
#define COL_PANEL     UI_COL_PANEL
#define COL_PANEL_HI  UI_COL_PANEL_HI
#define COL_LINE      UI_COL_LINE
#define COL_LINE_HI   UI_COL_LINE_HI
#define COL_TEXT      UI_COL_TEXT
#define COL_TEXT_DIM  UI_COL_TEXT_DIM
#define COL_ACCENT    UI_COL_ACCENT
#define COL_WARN      UI_COL_WARN
#define COL_TIP_BG    UI_COL_TIP_BG

// ---------------------------------------------------------------------------
//  State
// ---------------------------------------------------------------------------
static SgaAsset s_doc;                  // the asset being edited
static SgaAsset s_ref;                  // the ghosted reference model
static bool     s_refLoaded = false;
static const StrategyModel *s_refBuiltin = NULL;    // ghost may be a built-in
static float    s_refAlpha = 0.25f;
static Vector3  s_refOff = { 0 };       // so the ghost can sit BESIDE the model
static bool     s_refOn = true;

static int      s_sel = 0;              // selected part index
static int      s_state = SGA_STATE_IDLE;
static int      s_faction = 0;          // live tint preview
static bool     s_opened = false;       // an Open* ran before this Enter

// The file this doc saves over. Empty means "not saved yet" - a remix always
// clears it so REMIX cannot overwrite what it was remixed FROM.
static char     s_file[SGA_NAME_MAX];

static bool     s_dirty = false;

// Viewport orbit. Same gesture rules as the showcase's map view.
static float    s_yaw = 35.0f, s_pitch = 22.0f, s_zoom = 1.0f;
static bool     s_orbit = false;

// -- playback + timeline ----------------------------------------------------
// s_clock is the transport head. It is what the viewport poses against, so
// scrubbing and playing are the same thing to everything downstream.
static float    s_clock = 0.0f;         // playback time / play head
static bool     s_playing = false;
static bool     s_loop = true;
static float    s_tlScroll = 0.0f;      // first visible lane
static int      s_keySel = -1;          // selected key WITHIN the selected part
static bool     s_keyDrag = false;
static bool     s_keyPicked = false;    // a click chose part+key together
static bool     s_scrubbing = false;
static bool     s_easeOpen = false;     // easing picker modal
static int      s_easeScroll = 0;

// Part inspector scroll. The panel's content height depends on the part KIND
// and on whether a key is selected, so it is not known until the panel has been
// laid out - it is measured at the end of a frame and used by the next one.
// A one-frame lag is invisible here: the clamp only matters once the content
// actually overflows, and it settles before the wheel can move that far.
static float    s_insScroll = 0.0f;
static float    s_insContentH = 0.0f;

static char     s_status[128];
static float    s_statusT = 0.0f;
static Color    s_statusCol;


// raygui text boxes are stateful: each needs its own "is being edited" flag.
static bool     s_edName = false, s_edSub = false, s_edPart = false;
static bool     s_edSaveName = false;

static AppState *s_return = NULL;

// Modals. Only one is ever open at a time; ESC closes the innermost first.
static bool     s_saveOpen = false;     // save-as prompt
static char     s_saveBuf[SGA_NAME_MAX];
static bool     s_confirmDelete = false;
static bool     s_confirmExit = false;  // unsaved-changes guard

// Reference picker. Lists BOTH built-in models and authored .sga files - the
// most useful ghost is very often a shipped model you are matching the scale
// of, so limiting this to authored assets made the feature nearly useless for
// the first asset anyone builds.
//
// A row is either a built-in (model set, index into that set) or a file
// (model NULL, name is the filename stem).
typedef struct {
    char  name[SGA_NAME_MAX];
    char  category[16];             // for the search filter and the row's label
    char  subtype[SGA_SUBTYPE_MAX];
    const StrategyModel *model;     // NULL = an authored file
} PickRow;

static bool     s_pickOpen = false;
static PickRow  s_pickList[SF_BROWSE_MAX];
static int      s_pickCount = 0;
static float    s_pickScroll = 0.0f;
static char     s_pickSearch[32];
static bool     s_edSearch = false;

// Undo ring. See the MEMORY note: short, and one push per gesture.
static SgaAsset s_undo[SF_UNDO_MAX];
static int      s_undoHead = 0, s_undoCount = 0;
static bool     s_gestureOpen = false;

// REDO is a separate, shallower stack rather than a cursor into the undo ring.
// Kept to 3 because each entry is a whole SgaAsset (~450 KB on desktop) and the
// case it serves is narrow: "I pressed Ctrl+Z one or two times too many".
// Any new edit clears it - the classic branch rule, so a redo can never
// resurrect a document that never followed from what is on screen now.
#define SF_REDO_MAX 3
static SgaAsset s_redo[SF_REDO_MAX];
static int      s_redoCount = 0;

// ---------------------------------------------------------------------------
//  Small helpers
// ---------------------------------------------------------------------------
// One scale factor against a 1280-wide design, clamped so the tool stays usable
// on a small window without the panels eating the viewport.
static float SF_S(void) { return UiScale(); }

static void Status(const char *msg, Color c)
{
    TextCopy(s_status, msg);
    s_statusT = SF_STATUS_SECS;
    s_statusCol = c;
}

static void StatusOK(const char *m)   { Status(m, COL_TEXT); }
static void StatusWarn(const char *m) { Status(m, COL_WARN); }

// True while any text field owns the keyboard, so hotkeys stay out of the way.
static bool Typing(void)
{
    return s_edName || s_edSub || s_edPart || s_edSaveName || s_edSearch;
}

static bool ModalOpen(void)
{
    return s_saveOpen || s_confirmDelete || s_confirmExit || s_pickOpen ||
           s_easeOpen;
}

// The modal gate and the scroll clip now live in strategy_ui.c, because the
// widgets that consult them do. These keep the local spelling used below;
// UiFrameBegin() publishes ModalOpen() to the shared module once per frame.
static void SetInModal(bool v) { UiSetInModal(v); }
static bool ModalBlocks(void)  { return UiModalBlocks(); }
static bool ClipAllows(Vector2 mouse) { return UiClipAllows(mouse); }

static SgaPart *SelPart(void)
{
    if ((s_sel < 0) || (s_sel >= s_doc.partCount)) return NULL;
    return &s_doc.parts[s_sel];
}

// Tooltip: same hand-rolled scheme as the showcase, and for the same reason -
// raygui's own tooltips only fire on FOCUSED controls, so a disabled button
// (exactly where the explanation matters most) never shows one.
// The key the timeline has selected, or NULL. Guarded on the selection still
// being in range: deleting a key, switching state or picking another part can
// all leave s_keySel pointing past the end.
static SgaKey *SelKey(void)
{
    SgaPart *p = SelPart();
    if (p == NULL) return NULL;
    if ((s_state < 0) || (s_state >= SGA_STATE_COUNT)) return NULL;
    SgaPartAnim *an = &p->anim[s_state];
    if ((s_keySel < 0) || (s_keySel >= an->keyCount)) return NULL;
    return &an->keys[s_keySel];
}

static void Tip(Rectangle r, const char *text) { UiTip(r, text); }
static void TipDraw(int fontSize) { UiTipDraw(fontSize); }

// ---------------------------------------------------------------------------
//  Undo
//
//  Whole-document snapshots. That is heavy (see the MEMORY note) but it is the
//  only scheme that survives a part being removed or reordered, which repoints
//  every path reference in the asset - a field-level undo would have to replay
//  RepointPaths backwards, and get it exactly right, forever.
// ---------------------------------------------------------------------------
static void UndoPush(void)
{
    s_undo[s_undoHead] = s_doc;
    s_undoHead = (s_undoHead + 1) % SF_UNDO_MAX;
    if (s_undoCount < SF_UNDO_MAX) s_undoCount++;
}

// Clamp the selection and refresh derived state after the document is swapped
// wholesale, which both undo and redo do.
static void AfterDocSwap(void)
{
    if (s_sel >= s_doc.partCount) s_sel = s_doc.partCount - 1;
    if (s_sel < 0) s_sel = 0;
    StrategyAssetMeasure(&s_doc);
    s_dirty = true;
}

static void UndoPop(void)
{
    if (s_undoCount == 0) { StatusWarn("nothing to undo"); return; }

    // The document being left behind becomes the redo target, so the pair is
    // symmetric: undo then redo returns exactly what was on screen.
    if (s_redoCount == SF_REDO_MAX)
    {
        for (int i = 0; i < SF_REDO_MAX - 1; i++) s_redo[i] = s_redo[i + 1];
        s_redoCount--;
    }
    s_redo[s_redoCount++] = s_doc;

    s_undoHead = (s_undoHead - 1 + SF_UNDO_MAX) % SF_UNDO_MAX;
    s_undoCount--;
    s_doc = s_undo[s_undoHead];
    AfterDocSwap();
    StatusOK("undo");
}

static void RedoPop(void)
{
    if (s_redoCount == 0) { StatusWarn("nothing to redo"); return; }

    // Push what we are leaving back onto the undo ring directly - going through
    // UndoPush would be right, but it must NOT clear the redo stack the way an
    // ordinary edit does.
    s_undo[s_undoHead] = s_doc;
    s_undoHead = (s_undoHead + 1) % SF_UNDO_MAX;
    if (s_undoCount < SF_UNDO_MAX) s_undoCount++;

    s_doc = s_redo[--s_redoCount];
    AfterDocSwap();
    StatusOK("redo");
}

static void UndoClear(void)
{
    s_undoHead = 0; s_undoCount = 0; s_gestureOpen = false;
    s_redoCount = 0;
}

// Mark the document changed. `push` snapshots first - callers pass false for
// the continuing frames of a drag, so one gesture costs one ring slot.
static void Touch(bool push)
{
    if (push) UndoPush();
    s_redoCount = 0;        // a new edit branches: what was redone is unreachable
    s_dirty = true;
}

// A gesture that spans frames (a slider drag): push on the first changed frame
// only, and re-arm when the mouse comes up.
static void GestureTouch(void)
{
    if (!s_gestureOpen) { UndoPush(); s_gestureOpen = true; s_redoCount = 0; }
    s_dirty = true;
}

static void GestureEnd(void)
{
    if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT))
    {
        s_gestureOpen = false;
        // A new drag starts from the cursor, not the last bias.
        UiCtxGet()->fineActive = false;
        UiCtxGet()->fineBias   = 0.0f;
    }
}

// ---------------------------------------------------------------------------
//  Opening
// ---------------------------------------------------------------------------
// Everything an Open* has in common: clear the view state so a new document
// never inherits the last one's selection, ghost or undo history.
static void ResetView(void)
{
    s_sel = 0;
    s_state = SGA_STATE_IDLE;
    s_yaw = 35.0f; s_pitch = 22.0f; s_zoom = 1.0f;
    s_orbit = false;
    s_refLoaded = false;
    s_refBuiltin = NULL;
    s_refOff = (Vector3){ 0 };
    s_refOn = true;
    s_edName = s_edSub = s_edPart = s_edSaveName = s_edSearch = false;
    s_saveOpen = s_confirmDelete = s_confirmExit = s_pickOpen = false;
    UiCtxGet()->tip[0] = '\0';
    UndoClear();
    s_opened = true;
}

// Propose a name no .sga on disk already holds, so a fresh asset can be saved
// without first inventing one. StrategyAssetNameFree does the disk check.
static void ProposeName(char *out, int cap, const char *stem)
{
    if (StrategyAssetNameFree(stem)) { TextCopy(out, stem); return; }
    for (int n = 2; n < 100; n++)
    {
        const char *t = TextFormat("%s %d", stem, n);
        if (StrategyAssetNameFree(t))
        {
            int i = 0;
            for (; (i < cap - 1) && t[i]; i++) out[i] = t[i];
            out[i] = '\0';
            return;
        }
    }
    TextCopy(out, stem);
}

void StrategyForgeOpenNew(void)
{
    ResetView();
    StrategyAssetInit(&s_doc, "");      // one default part, sane defaults
    ProposeName(s_doc.name, SGA_NAME_MAX, "new asset");
    s_file[0] = '\0';                   // never saved: SAVE will prompt
    s_dirty = false;
    StrategyAssetMeasure(&s_doc);
}

void StrategyForgeOpenAsset(const SgaAsset *a, bool remix)
{
    if (a == NULL) { StrategyForgeOpenNew(); return; }
    ResetView();
    s_doc = *a;                         // by value: the catalog is rebuilt on save

    if (remix)
    {
        // A remix must not be able to overwrite its source, so it gets a free
        // name and NO file binding. Saving it is therefore a create.
        char stem[SGA_NAME_MAX];
        TextCopy(stem, TextFormat("%s copy", a->name));
        ProposeName(s_doc.name, SGA_NAME_MAX, stem);
        s_file[0] = '\0';
        s_dirty = true;                 // there IS unsaved work: the copy itself
    }
    else
    {
        TextCopy(s_file, a->name);
        s_dirty = false;
    }
    StrategyAssetMeasure(&s_doc);
}

void StrategyForgeOpenBuiltin(const StrategyModel *m, const char *name,
                              int category, const char *subtype)
{
    ResetView();
    StrategyAssetInit(&s_doc, "");
    s_doc.partCount = 0;                // Init's default part is not wanted here

    if (m != NULL)
    {
        int n = m->partCount;
        if (n > SGA_PARTS_MAX) n = SGA_PARTS_MAX;   // capacity, not a crash
        for (int i = 0; i < n; i++)
        {
            const ModelPart *src = &m->parts[i];
            SgaPart *dst = &s_doc.parts[i];
            memset(dst, 0, sizeof(*dst));

            // SgaPart is a superset of ModelPart by design, so the geometry is
            // a straight field copy and only the colour policy is translated.
            TextCopy(dst->name, TextFormat("part %d", i + 1));
            dst->kind    = (int32_t)src->kind;
            dst->visible = 1;
            dst->offset  = src->offset;
            dst->size    = src->size;
            dst->r0      = src->r0;
            dst->r1      = src->r1;
            dst->h       = src->h;
            dst->sides   = src->sides;
            dst->color   = src->color;
            StrategyAssetTintFromRole((int)src->role, &dst->tintMode,
                                      &dst->tintAmount, &dst->brightness);

            // A faction-driven built-in part has NO authored .color: the game's
            // tables use designated initialisers and simply omit it for those
            // roles, because PartColor never reads it there. It arrives here as
            // {0,0,0,0} - fully transparent - and the moment the author pulls
            // FACTION below 1 the part blends toward that and vanishes.
            //
            // Seed it with the faction colour the part is showing right now, so
            // dropping the slider fades toward the colour it already has rather
            // than toward nothing, and the RGB rows start on something real.
            if (dst->color.a == 0)
            {
                dst->color = StrategyAssetPartColor(dst, s_faction, 1.0f);
                dst->color.a = 255;
            }
            for (int st = 0; st < SGA_STATE_COUNT; st++) dst->anim[st].pathPart = -1;
        }
        s_doc.partCount = n;
        if (n < m->partCount)
            StatusWarn(TextFormat("kept %d of %d parts - asset capacity is %d",
                                  n, m->partCount, SGA_PARTS_MAX));
    }

    if (s_doc.partCount == 0) StrategyAssetAddPart(&s_doc, SGA_CUBE);

    char stem[SGA_NAME_MAX];
    TextCopy(stem, TextFormat("%s copy", (name && name[0]) ? name : "asset"));
    ProposeName(s_doc.name, SGA_NAME_MAX, stem);
    if (subtype && subtype[0]) TextCopy(s_doc.subtype, subtype);
    s_doc.category = category;
    s_file[0] = '\0';                   // a built-in is never writable
    s_dirty = true;
    StrategyAssetMeasure(&s_doc);
}

void StrategyForgeSetReturn(AppState *state) { s_return = state; }

// ---------------------------------------------------------------------------
//  Saving
// ---------------------------------------------------------------------------
// The asset's NAME is its filename (strategy_asset_io treats the file as the
// identity), so a rename is a save under a new name plus a delete of the old.
static bool SaveAs(const char *name)
{
    const char *why = NULL;
    if (!StrategyAssetValid(&s_doc, &why))
    {
        StatusWarn(why ? why : "asset is not valid");
        return false;
    }
    if ((name == NULL) || (name[0] == '\0'))
    {
        StatusWarn("name is required");
        return false;
    }

    TextCopy(s_doc.name, name);
    StrategyAssetMeasure(&s_doc);

    // Drop easing slots no key points at any more. A long session picking
    // curves and moving off them fills SGA_EASES_MAX with shapes nothing uses,
    // and then a real curve has nowhere to go. Compacting at save is the moment
    // where the set of curves in use is exactly known.
    StrategyAssetCompactEases(&s_doc);

    if (!StrategyAssetSaveNamed(&s_doc, name))
    {
        StatusWarn(TextFormat("could not write %s%s", name, SGA_EXT));
        return false;
    }

    // Renaming: drop the file the doc used to live in, so the asset does not
    // quietly exist twice under two names.
    if (s_file[0] && !TextIsEqual(s_file, name))
    {
        StrategyAssetDelete(s_file);

        // Bindings name their asset, so a rename orphans every role pointing
        // at the old one. Repoint them here - the alternative is a set of
        // bindings that silently falls back to built-ins after a rename, with
        // nothing on screen explaining why.
        StrategyBindingsLoad();
        if (StrategyBindingsRename(s_file, name) > 0) StrategyBindingsSave();
    }

    TextCopy(s_file, name);
    s_dirty = false;
    StatusOK(TextFormat("saved %s%s", name, SGA_EXT));
    return true;
}

// SAVE proper: straight to the bound file, or the prompt when there isn't one.
static void SaveOpen(void)
{
    TextCopy(s_saveBuf, s_doc.name);
    s_saveOpen = true;
    s_edSaveName = false;
}

static void DoSave(void)
{
    if (s_file[0]) SaveAs(s_file);
    else           SaveOpen();
}

// ---------------------------------------------------------------------------
//  Viewport
//
//  Drawn directly into the window inside Gui(), NOT through a render target:
//  unlike the showcase - which renders a dozen small previews and needs a
//  scratch target to do it - the forge has exactly one 3D view, and a scissor
//  plus rlViewport is all it takes to keep it inside its pane.
// ---------------------------------------------------------------------------
static Camera3D ForgeCamera(Rectangle vp)
{
    // Frame the MODEL, so the camera stays sane as parts are added and moved.
    float reach = s_doc.radius;
    if (s_doc.height*0.5f > reach) reach = s_doc.height*0.5f;
    if (reach < 0.4f) reach = 0.4f;

    float aspect = (vp.height > 0.0f) ? (vp.width / vp.height) : 1.0f;
    float dist = reach*4.2f*s_zoom;
    if (aspect < 1.0f) dist /= aspect;

    float py = s_pitch*DEG2RAD;
    float yw = s_yaw*DEG2RAD;
    float horiz = cosf(py)*dist;

    Camera3D cam = { 0 };
    cam.target     = (Vector3){ 0.0f, s_doc.height*0.42f, 0.0f };
    cam.position   = (Vector3){ cam.target.x + sinf(yw)*horiz,
                                cam.target.y + sinf(py)*dist,
                                cam.target.z + cosf(yw)*horiz };
    cam.up         = (Vector3){ 0.0f, 1.0f, 0.0f };
    cam.fovy       = 45.0f;
    cam.projection = CAMERA_PERSPECTIVE;
    return cam;
}

// The ground plane and origin axes. Without them "at the model's feet" is a
// number in a box rather than a place you can see.
static void DrawGrid3D(void)
{
    rlDisableBackfaceCulling();

    // Hand-rolled rather than raylib's DrawGrid, which hard-codes 75% grey with
    // no colour parameter - that made the floor the brightest thing in the
    // viewport and the model the dimmest, which is exactly backwards for a tool
    // whose whole job is judging a model's colour.
    const int   half = 10;
    const float step = 0.5f;
    const float ext  = (float)half*step;
    for (int i = -half; i <= half; i++)
    {
        // The two axis lines read slightly stronger, so the origin is findable
        // without competing with the model.
        Color c = (i == 0) ? (Color){ 78, 86, 104, 255 } : (Color){ 46, 51, 64, 255 };
        DrawLine3D((Vector3){ (float)i*step, 0.0f, -ext },
                   (Vector3){ (float)i*step, 0.0f,  ext }, c);
        DrawLine3D((Vector3){ -ext, 0.0f, (float)i*step },
                   (Vector3){  ext, 0.0f, (float)i*step }, c);
    }

    // Origin axes, short and dim: orientation without competing with the model.
    DrawLine3D((Vector3){ 0, 0.001f, 0 }, (Vector3){ 1.2f, 0.001f, 0 },
               (Color){ 220, 90, 90, 190 });      // +X
    DrawLine3D((Vector3){ 0, 0.001f, 0 }, (Vector3){ 0, 1.2f, 0 },
               (Color){ 120, 220, 120, 190 });    // +Y
    DrawLine3D((Vector3){ 0, 0.001f, 0 }, (Vector3){ 0, 0.001f, 1.2f },
               (Color){ 110, 150, 240, 190 });    // +Z
    rlEnableBackfaceCulling();
}

// A wire box around the selected part, so selection reads in the 3D view and
// not only in the list.
static void DrawSelectionCage(const SgaPart *p)
{
    if (p == NULL) return;

    Vector3 c = p->offset;
    Vector3 e = { 0.3f, 0.3f, 0.3f };

    switch (p->kind)
    {
        case SGA_CUBE:
        case SGA_CUBE_WIRES:
            e = (Vector3){ p->size.x*0.5f, p->size.y*0.5f, p->size.z*0.5f };
            break;
        case SGA_SPHERE:
            e = (Vector3){ p->r0, p->r0, p->r0 };
            break;
        case SGA_CYLINDER:
        {
            float r = (p->r0 > p->r1) ? p->r0 : p->r1;
            e = (Vector3){ r, p->h*0.5f, r };
            c.y += p->h*0.5f;               // cylinders grow UP from offset
            break;
        }
        case SGA_CYLINDER_EX:
        case SGA_LINE:
            // These run from offset to offset+size, so the cage spans both ends.
            c = (Vector3){ p->offset.x + p->size.x*0.5f,
                           p->offset.y + p->size.y*0.5f,
                           p->offset.z + p->size.z*0.5f };
            e = (Vector3){ fabsf(p->size.x)*0.5f + 0.06f,
                           fabsf(p->size.y)*0.5f + 0.06f,
                           fabsf(p->size.z)*0.5f + 0.06f };
            break;
        case SGA_PATH:
            return;                         // the path's own loop IS its handle
        default: break;
    }

    DrawCubeWiresV(c, (Vector3){ e.x*2.0f + 0.04f, e.y*2.0f + 0.04f, e.z*2.0f + 0.04f },
                   COL_ACCENT);
}

static void ViewportDraw(Rectangle vp, float time)
{
    BeginScissorMode((int)vp.x, (int)vp.y, (int)vp.width, (int)vp.height);
    DrawRectangleRec(vp, (Color){ 22, 24, 31, 255 });

    // FLUSH before touching the viewport. Draw calls only queue into raylib's
    // batch; they are rasterised when the batch is drawn. Changing the viewport
    // with the background rect still queued would rasterise it under the NEW
    // transform, painting it stretched across the pane - which is exactly the
    // stray rectangle this looked like at first.
    rlDrawRenderBatchActive();

    // rlViewport so BeginMode3D's projection is built from the PANE's aspect
    // rather than the whole window's - otherwise the model comes out stretched.
    rlViewport((int)vp.x, (int)(GetScreenHeight() - vp.y - vp.height),
               (int)vp.width, (int)vp.height);

    Camera3D cam = ForgeCamera(vp);
    BeginMode3D(cam);
        DrawGrid3D();

        // Ghost FIRST and without depth writes, so it never occludes the real
        // model - a reference you have to look around is not a reference.
        if (s_refOn && (s_refLoaded || s_refBuiltin))
        {
            // Depth TEST off, not just depth writes. With the test still on,
            // each of the ghost's own parts blends separately against the ones
            // behind it, so overlapping parts stack alpha and the ghost reads
            // solid where it is thickest - which is where a silhouette matters
            // most. Testing off draws it as one flat translucent layer at the
            // alpha the author asked for, and since it is drawn BEFORE the
            // model the real geometry still paints over it normally.
            rlDisableDepthTest();
            rlDisableDepthMask();
            if (s_refBuiltin)
                StrategyModelDraw(s_refBuiltin, s_faction, s_refOff, 0.0f, s_refAlpha);
            else
                StrategyAssetDraw(&s_ref, s_faction, s_refOff, 0.0f, s_refAlpha,
                                  SGA_STATE_IDLE, 0.0f);
            // Flush while the ghost's state is still in force - the batch is
            // rasterised later otherwise, under whatever state follows.
            rlDrawRenderBatchActive();
            rlEnableDepthMask();
            rlEnableDepthTest();
        }

        StrategyAssetDraw(&s_doc, s_faction, (Vector3){ 0 }, 0.0f, 1.0f,
                          s_state, time);

        // Motion paths: every path is faint, the selected one is bright. Drawn
        // after the model so the loop is never buried inside geometry.
        for (int i = 0; i < s_doc.partCount; i++)
        {
            if (s_doc.parts[i].kind != SGA_PATH) continue;
            bool on = (i == s_sel);
            StrategyAssetDrawPath(&s_doc.parts[i].path, (Vector3){ 0 }, 0.0f,
                                  on ? COL_ACCENT : Fade(COL_LINE_HI, 0.45f));
        }

        DrawSelectionCage(SelPart());
    EndMode3D();

    // Restore the full-window viewport, or every later Gui() draw lands inside
    // the pane's little rectangle. Flush first, for the same reason as above.
    rlDrawRenderBatchActive();
    rlViewport(0, 0, GetScreenWidth(), GetScreenHeight());
    EndScissorMode();

    DrawRectangleLinesEx(vp, 1.0f, COL_LINE);
}

// Orbit + zoom. Same gesture rules as the showcase's map view, so the two 3D
// views in this app do not disagree about what dragging means.
static void ViewportInput(Rectangle vp)
{
    if (ModalOpen()) return;
    Vector2 mp = GetMousePosition();
    bool over = CheckCollisionPointRec(mp, vp);

    if (over && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) s_orbit = true;
    if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) s_orbit = false;

    if (s_orbit)
    {
        Vector2 d = GetMouseDelta();
        s_yaw -= d.x*0.35f;
        if (s_yaw < 0.0f) s_yaw += 360.0f;
        if (s_yaw >= 360.0f) s_yaw -= 360.0f;
        s_pitch -= d.y*0.25f;
        // Clamped away from straight down/level: the poles gimbal and a
        // ground-level camera sees nothing but the grid edge-on.
        if (s_pitch < -10.0f) s_pitch = -10.0f;
        if (s_pitch > 86.0f) s_pitch = 86.0f;
    }

    if (over)
    {
        s_zoom -= GetMouseWheelMove()*0.08f;
        if (s_zoom < 0.35f) s_zoom = 0.35f;
        if (s_zoom > 2.50f) s_zoom = 2.50f;
    }
}

// ---------------------------------------------------------------------------
//  Widgets
//
//  raygui for anything stateful (text boxes, sliders), hand-drawn for anything
//  that needs to paint itself in the palette. The showcase draws its own chrome
//  for the same reason: raygui's style is global, and restyling it per-control
//  is more code than drawing the rectangle.
// ---------------------------------------------------------------------------
// `primary` gives the accent to the one action a screen is really offering, so
// SAVE does not look like the CANCEL beside it.
//
// Enabled and disabled MUST differ at rest, not only on hover: an enabled
// button drawn in the dim text colour is pixel-identical to a disabled one
// until the cursor happens to cross it, which reads as "this tool is broken"
// rather than "this action is unavailable".
static bool ForgeButtonEx(Rectangle r, const char *label, bool enabled,
                          const char *tip, int fs, bool primary)
{
    return UiButtonEx(r, label, enabled, tip, fs, primary);
}

static bool ForgeButton(Rectangle r, const char *label, bool enabled,
                        const char *tip, int fs)
{
    return UiButton(r, label, enabled, tip, fs);
}

static bool ForgeSlider(Rectangle r, const char *label, float *v,
                        float lo, float hi, int fs)
{
    return UiSlider(r, label, v, lo, hi, fs);
}

// A row of mutually exclusive chips. Returns the newly picked index, or -1.
static int ForgeChips(Rectangle r, const char **labels, int count, int active,
                      int fs, Color accent)
{
    return UiChips(r, labels, count, active, fs, accent);
}

// ---------------------------------------------------------------------------
//  Part list (left pane)
// ---------------------------------------------------------------------------
static void PartListGui(Rectangle pane, float s, int fs)
{
    DrawRectangleRec(pane, COL_PANEL);
    DrawRectangleLinesEx(pane, 1.0f, COL_LINE);

    float x = pane.x + 10.0f*s;
    float w = pane.width - 20.0f*s;
    float y = pane.y + 10.0f*s;

    DrawText("PARTS", (int)x, (int)y, fs, COL_TEXT_DIM);
    DrawText(TextFormat("%d / %d", s_doc.partCount, SGA_PARTS_MAX),
             (int)(x + w - (float)MeasureText(TextFormat("%d / %d", s_doc.partCount,
                                                         SGA_PARTS_MAX), fs)),
             (int)y, fs, Fade(COL_TEXT_DIM, 0.7f));
    y += (float)fs + 8.0f*s;

    float rowH = 26.0f*s;
    float listH = pane.height - (y - pane.y) - (34.0f*s + 10.0f*s) - 8.0f*s;
    Rectangle list = { x, y, w, listH };

    BeginScissorMode((int)list.x, (int)list.y, (int)list.width, (int)list.height);
    Vector2 mp = GetMousePosition();
    for (int i = 0; i < s_doc.partCount; i++)
    {
        Rectangle r = { list.x, list.y + (float)i*rowH, list.width, rowH - 2.0f };
        if (r.y + r.height < list.y) continue;
        if (r.y > list.y + list.height) break;

        bool on = (i == s_sel);
        bool hot = !ModalBlocks() && CheckCollisionPointRec(mp, r);
        SgaPart *p = &s_doc.parts[i];

        DrawRectangleRec(r, on ? Fade(COL_ACCENT, 0.18f) : (hot ? COL_PANEL_HI : COL_PANEL));
        if (on) DrawRectangleRec((Rectangle){ r.x, r.y, 2.0f, r.height }, COL_ACCENT);

        // The eye toggle sits at the row's right edge, inside the row's hit
        // area - so it is tested BEFORE the row's own click below.
        Rectangle eye = { r.x + r.width - rowH, r.y, rowH - 2.0f, r.height };
        bool eyeHot = !ModalBlocks() && CheckCollisionPointRec(mp, eye);
        bool isPath = (p->kind == SGA_PATH);

        // A swatch showing the part's ACTUAL tinted colour, not its raw one:
        // the whole point of the tint controls is what lands on screen.
        Rectangle sw = { r.x + 8.0f*s, r.y + (r.height - 10.0f*s)*0.5f, 10.0f*s, 10.0f*s };
        if (isPath) DrawRectangleLinesEx(sw, 1.0f, COL_LINE_HI);
        else        DrawRectangleRec(sw, StrategyAssetPartColor(p, s_faction, 1.0f));

        const char *nm = p->name[0] ? p->name : StrategyAssetKindName(p->kind);
        Color tc = on ? COL_TEXT : COL_TEXT_DIM;
        if (!p->visible && !isPath) tc = Fade(COL_TEXT_DIM, 0.45f);
        BeginScissorMode((int)(r.x + 22.0f*s), (int)r.y,
                         (int)(r.width - 22.0f*s - rowH), (int)r.height);
        DrawText(nm, (int)(r.x + 22.0f*s), (int)(r.y + (r.height - (float)fs)*0.5f),
                 fs, tc);
        EndScissorMode();

        // Paths are structure, not geometry: they never render, so a
        // visibility toggle on one would be a control that does nothing.
        if (isPath)
        {
            DrawText("path", (int)(eye.x + 2.0f), (int)(eye.y + (eye.height - (float)fs)*0.5f),
                     fs, Fade(COL_ACCENT, 0.8f));
        }
        else
        {
            DrawText(p->visible ? "o" : "-",
                     (int)(eye.x + eye.width*0.4f),
                     (int)(eye.y + (eye.height - (float)fs)*0.5f), fs,
                     eyeHot ? COL_TEXT : COL_TEXT_DIM);
            if (eyeHot && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
            {
                AudioPlayButton();
                Touch(true);
                p->visible = !p->visible;
                s_sel = i;
                continue;               // consumed: not also a row select
            }
        }

        if (hot && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
        { AudioPlayButton(); s_sel = i; s_edPart = false; }
    }
    EndScissorMode();

    if (s_doc.partCount == 0)
        DrawText("no parts yet", (int)(list.x + 8.0f*s), (int)(list.y + 8.0f*s),
                 fs, Fade(COL_TEXT_DIM, 0.6f));

    // -- row of part actions --------------------------------------------------
    float by = pane.y + pane.height - 34.0f*s;
    float bh = 24.0f*s;
    float bw = (w - 3.0f*4.0f*s)/4.0f;
    bool room = (s_doc.partCount < SGA_PARTS_MAX);
    bool has = (s_doc.partCount > 0);

    if (ForgeButton((Rectangle){ x, by, bw, bh }, "+", room,
                    room ? "Add a part." : "Part capacity reached.", fs))
    {
        Touch(true);
        int n = StrategyAssetAddPart(&s_doc, SGA_CUBE);
        if (n >= 0) { s_sel = n; StrategyAssetMeasure(&s_doc); }
    }
    if (ForgeButton((Rectangle){ x + (bw + 4.0f*s), by, bw, bh }, "copy", has && room,
                    room ? "Duplicate the selected part."
                         : "Part capacity reached.", fs))
    {
        Touch(true);
        int n = StrategyAssetDuplicatePart(&s_doc, s_sel);
        if (n >= 0) { s_sel = n; StrategyAssetMeasure(&s_doc); }
    }
    if (ForgeButton((Rectangle){ x + 2.0f*(bw + 4.0f*s), by, bw, bh }, "up",
                    has && (s_sel > 0), "Draw earlier - parts paint in list order.", fs))
    {
        Touch(true);
        if (StrategyAssetMovePart(&s_doc, s_sel, -1)) s_sel--;
    }
    if (ForgeButton((Rectangle){ x + 3.0f*(bw + 4.0f*s), by, bw, bh }, "dn",
                    has && (s_sel < s_doc.partCount - 1),
                    "Draw later - parts paint in list order.", fs))
    {
        Touch(true);
        if (StrategyAssetMovePart(&s_doc, s_sel, 1)) s_sel++;
    }
}

// ---------------------------------------------------------------------------
//  Part inspector (right pane)
//
//  Which numeric fields appear depends on the part KIND, because the fields
//  genuinely mean different things per kind (a sphere has no height, a line has
//  no radius). Showing all of them always would put six dead boxes on screen.
// ---------------------------------------------------------------------------
// Three stacked ForgeSliders rather than three squeezed into one row: they
// inherit the Ctrl/Shift modifiers that way, and an X/Y/Z that cannot be nudged
// precisely is most of the reason to reach for the numbers at all.
// Returns the Y below the last row, since the rows are no longer a fixed height.
static float Vec3Row(float *x, float *y, float *z, const char *label,
                     float lo, float hi, Rectangle r, int fs, float s)
{
    DrawText(label, (int)r.x, (int)r.y, fs, COL_TEXT_DIM);
    float ry = r.y + (float)fs + 4.0f*s;

    static const char *axis[3] = { "X", "Y", "Z" };
    float *v[3] = { x, y, z };
    for (int i = 0; i < 3; i++)
    {
        if (ForgeSlider((Rectangle){ r.x, ry, r.width, SF_RH*s }, axis[i],
                        v[i], lo, hi, fs))
        {
            GestureTouch();
            StrategyAssetMeasure(&s_doc);
        }
        ry += SF_RH*s + 3.0f*s;
    }
    return ry;
}

// The panel's body. Draws from `y0` downward and returns the Y it finished at,
// so the caller can measure the content and decide whether it needs scrolling.
static float PartInspectorBody(Rectangle pane, float x, float w, float y,
                               float s, int fs, int fsSmall);

static void PartInspectorGui(Rectangle pane, float s, int fs, int fsSmall)
{
    DrawRectangleRec(pane, COL_PANEL);
    DrawRectangleLinesEx(pane, 1.0f, COL_LINE);

    float x = pane.x + 12.0f*s;
    float w = pane.width - 24.0f*s;

    // Scroll only when it is needed. A panel that scrolls when everything
    // already fits just loses its top row to a stray wheel click.
    float over = s_insContentH - pane.height;
    bool scrollable = (over > 1.0f);
    float barW = scrollable ? 5.0f*s : 0.0f;

    if (scrollable)
    {
        // The wheel belongs to whatever is under the cursor. Every ForgeSlider in
        // this panel puts its bar in the same x column (see ForgeSlider: the
        // bar starts 74px in and stops 46px short of the right edge), so the
        // panel can tell "over a slider bar" from "over the panel" without
        // waiting to find out which widget the cursor actually hit. Rows that
        // are not sliders simply have nothing there to claim it.
        Vector2 wm = GetMousePosition();
        Rectangle sliderCol = { x + 74.0f, pane.y,
                                (w - barW) - 74.0f - 46.0f, pane.height };
        bool onSlider = CheckCollisionPointRec(wm, sliderCol);

        if (!ModalBlocks() && !onSlider && CheckCollisionPointRec(wm, pane))
            s_insScroll += GetMouseWheelMove()*28.0f;
        if (s_insScroll < -over) s_insScroll = -over;
        if (s_insScroll > 0.0f) s_insScroll = 0.0f;
    }
    else s_insScroll = 0.0f;

    float y0 = pane.y + 10.0f*s + s_insScroll;

    // Scissor clips PIXELS but not hit testing, so a control scrolled out of
    // sight still answers the mouse. Every widget below is inside the pane's
    // x range, so gating on the pane rect is enough to stop a hidden slider
    // from catching a drag aimed at the viewport.
    BeginScissorMode((int)pane.x, (int)pane.y, (int)pane.width, (int)pane.height);
    UiSetClip(pane);
    float endY = PartInspectorBody(pane, x, w - barW, y0, s, fs, fsSmall);
    UiSetClip((Rectangle){ 0 });
    EndScissorMode();

    // Measure for the NEXT frame, in content coordinates.
    s_insContentH = (endY - y0) + 14.0f*s;

    if (scrollable)
    {
        // A plain track-and-thumb, drawn over the clipped content so it is
        // always visible - the only cue that there is more panel below.
        Rectangle track = { pane.x + pane.width - barW - 3.0f*s, pane.y + 4.0f*s,
                            barW, pane.height - 8.0f*s };
        DrawRectangleRec(track, Fade(COL_LINE, 0.5f));

        float frac = pane.height/s_insContentH;
        if (frac > 1.0f) frac = 1.0f;
        float thumbH = track.height*frac;
        if (thumbH < 18.0f*s) thumbH = 18.0f*s;
        float t = (over > 0.0f) ? (-s_insScroll/over) : 0.0f;
        DrawRectangleRec((Rectangle){ track.x, track.y + t*(track.height - thumbH),
                                      barW, thumbH },
                         Fade(COL_LINE_HI, 0.85f));
    }
}

static float PartInspectorBody(Rectangle pane, float x, float w, float y,
                               float s, int fs, int fsSmall)
{
    (void)pane;
    SgaPart *p = SelPart();
    if (p == NULL)
    {
        DrawText("PART", (int)x, (int)y, fsSmall, COL_TEXT_DIM);
        y += (float)fsSmall + 10.0f*s;
        DrawText("Add a part to begin.", (int)x, (int)y, fs, Fade(COL_TEXT_DIM, 0.7f));
        return y + (float)fs;
    }

    DrawText("PART", (int)x, (int)y, fsSmall, COL_TEXT_DIM);
    y += (float)fsSmall + 6.0f*s;

    // -- name -----------------------------------------------------------------
    // raygui hit-tests inside GuiTextBox and knows nothing about the panel's
    // scroll, so a box scrolled out of view would still take a click. Skip it
    // entirely when its row is outside the pane rather than trying to gate it.
    {
        Rectangle nb = { x, y, w, SF_RH*s };
        Rectangle clip = UiGetClip();
        if ((nb.y >= clip.y - 1.0f) &&
            (nb.y + nb.height <= clip.y + clip.height + 1.0f))
        {
            if (GuiTextBox(nb, p->name, SGA_NAME_MAX, s_edPart))
            { s_edPart = !s_edPart; Touch(true); }
        }
        else
        {
            // Still show WHAT the part is called, just not editably.
            DrawRectangleLinesEx(nb, 1.0f, Fade(COL_LINE, 0.6f));
            DrawText(p->name, (int)(nb.x + 5.0f*s),
                     (int)(nb.y + (nb.height - (float)fs)*0.5f), fs, COL_TEXT_DIM);
            s_edPart = false;
        }
    }
    y += SF_RH*s + 8.0f*s;

    // -- kind -----------------------------------------------------------------
    DrawText("SHAPE", (int)x, (int)y, fsSmall, COL_TEXT_DIM);
    y += (float)fsSmall + 4.0f*s;
    {
        // Two rows of chips: seven kinds do not fit one row at this width.
        static const char *k1[] = { "CUBE", "WIRES", "BALL", "CYL" };
        static const char *k2[] = { "BAR", "LINE", "PATH" };
        int a1 = (p->kind <= SGA_CYLINDER) ? p->kind : -1;
        int a2 = (p->kind >= SGA_CYLINDER_EX) ? p->kind - SGA_CYLINDER_EX : -1;

        int got = ForgeChips((Rectangle){ x, y, w, SF_RH*s }, k1, 4, a1, fsSmall, COL_ACCENT);
        y += SF_RH*s + 4.0f*s;
        int got2 = ForgeChips((Rectangle){ x, y, w*0.75f, SF_RH*s }, k2, 3, a2,
                              fsSmall, COL_ACCENT);
        y += SF_RH*s + 8.0f*s;

        int pick = (got >= 0) ? got : ((got2 >= 0) ? got2 + SGA_CYLINDER_EX : -1);
        if ((pick >= 0) && (pick != p->kind))
        {
            Touch(true);
            p->kind = pick;
            StrategyAssetMeasure(&s_doc);
        }
    }

    // -- geometry, per kind ---------------------------------------------------
    if (p->kind == SGA_PATH)
    {
        // A path is not geometry: it is a curve other parts ride. Its controls
        // are the shape of that curve, which is what Phase 3 animates against.
        DrawText("PATH SHAPE", (int)x, (int)y, fsSmall, COL_TEXT_DIM);
        y += (float)fsSmall + 6.0f*s;

        y = Vec3Row(&p->path.center.x, &p->path.center.y, &p->path.center.z,
                    "CENTER", -4.0f, 4.0f, (Rectangle){ x, y, w, 0 }, fsSmall, s) + 5.0f*s;

        if (ForgeSlider((Rectangle){ x, y, w, SF_RH*s }, "RADIUS X",
                        &p->path.radiusX, 0.0f, 4.0f, fsSmall)) GestureTouch();
        y += SF_RH*s + 4.0f*s;
        if (ForgeSlider((Rectangle){ x, y, w, SF_RH*s }, "RADIUS Z",
                        &p->path.radiusZ, 0.0f, 4.0f, fsSmall)) GestureTouch();
        y += SF_RH*s + 4.0f*s;
        if (ForgeSlider((Rectangle){ x, y, w, SF_RH*s }, "SQUARE",
                        &p->path.squareness, 0.0f, 1.0f, fsSmall)) GestureTouch();
        Tip((Rectangle){ x, y, w, SF_RH*s },
            "0 is an ellipse, 1 is a rectangle. Equal radii make a circle; "
            "one radius at 0 makes a line.");
        y += SF_RH*s + 8.0f*s;

        y = Vec3Row(&p->path.rotation.x, &p->path.rotation.y, &p->path.rotation.z,
                    "ROTATE", -180.0f, 180.0f, (Rectangle){ x, y, w, 0 }, fsSmall, s) + 8.0f*s;

        DrawText("Paths never render. They exist so a", (int)x, (int)y,
                 fsSmall, Fade(COL_TEXT_DIM, 0.75f));
        y += (float)fsSmall + 2.0f*s;
        DrawText("part can travel along them.", (int)x, (int)y,
                 fsSmall, Fade(COL_TEXT_DIM, 0.75f));
        return y + (float)fsSmall;      // no colour controls: a path has no colour
    }

    // -- the selected key -----------------------------------------------------
    //
    //  This band is the answer to "which thing am I editing?". Without it the
    //  panel always edited the part's REST POSE - one value shared by every key
    //  in every state - so moving a part while a key was selected appeared to
    //  change every key at once, because it genuinely did.
    //
    //  With a key selected, these controls edit THAT KEY. The rest-pose rows
    //  below stay visible and keep working, because both are real and an author
    //  needs to tell them apart: the rest pose is where the part sits with no
    //  animation, the key is what the animation does to it from there.
    {
        SgaKey *key = SelKey();
        if (key != NULL)
        {
            Rectangle band = { x - 4.0f*s, y - 4.0f*s, w + 8.0f*s, 0 };
            float bandTop = y;

            DrawText(TextFormat("KEY %d  @ %.2fs", s_keySel + 1, (double)key->t),
                     (int)x, (int)y, fsSmall, COL_ACCENT);
            y += (float)fsSmall + 6.0f*s;

            y = Vec3Row(&key->offset.x, &key->offset.y, &key->offset.z, "MOVE",
                        -4.0f, 4.0f, (Rectangle){ x, y, w, 0 }, fsSmall, s) + 4.0f*s;
            y = Vec3Row(&key->rot.x, &key->rot.y, &key->rot.z, "TURN",
                        -180.0f, 180.0f, (Rectangle){ x, y, w, 0 }, fsSmall, s) + 4.0f*s;
            y = Vec3Row(&key->scale.x, &key->scale.y, &key->scale.z, "SCALE",
                        0.0f, 3.0f, (Rectangle){ x, y, w, 0 }, fsSmall, s) + 4.0f*s;

            // The path binding lives here rather than on the part, because it
            // is per-state: a unit may orbit while MOVING and sit still on IDLE.
            {
                SgaPartAnim *an = &p->anim[s_state];
                const char *lbl = "PATH: none";
                if ((an->pathPart >= 0) && (an->pathPart < s_doc.partCount))
                    lbl = TextFormat("PATH: %s", s_doc.parts[an->pathPart].name);

                if (ForgeButton((Rectangle){ x, y, w, SF_RH*s }, lbl, true,
                                "Bind this part to a motion path for this state. "
                                "Cycles through the asset's PATH parts.", fsSmall))
                {
                    Touch(true);
                    // Cycle to the next PATH part, then back to none.
                    int start = an->pathPart;
                    int next = -1;
                    for (int i = start + 1; i < s_doc.partCount; i++)
                        if (s_doc.parts[i].kind == SGA_PATH) { next = i; break; }
                    an->pathPart = next;
                }
                y += SF_RH*s + 4.0f*s;

                if (an->pathPart >= 0)
                {
                    if (ForgeSlider((Rectangle){ x, y, w, SF_RH*s }, "ALONG",
                                    &key->u, 0.0f, 1.0f, fsSmall)) GestureTouch();
                    Tip((Rectangle){ x, y, w, SF_RH*s },
                        "How far around the path this key sits, 0 to 1.");
                    y += SF_RH*s + 4.0f*s;
                }
            }

            band.height = y - bandTop + 4.0f*s;
            DrawRectangleLinesEx(band, 1.0f, Fade(COL_ACCENT, 0.35f));
            y += 8.0f*s;

            DrawText("REST POSE", (int)x, (int)y, fsSmall, COL_TEXT_DIM);
            y += (float)fsSmall + 4.0f*s;
        }
    }

    y = Vec3Row(&p->offset.x, &p->offset.y, &p->offset.z, "OFFSET",
                -4.0f, 4.0f, (Rectangle){ x, y, w, 0 }, fsSmall, s) + 5.0f*s;

    if ((p->kind == SGA_CUBE) || (p->kind == SGA_CUBE_WIRES) ||
        (p->kind == SGA_CYLINDER_EX) || (p->kind == SGA_LINE))
    {
        const char *lbl = ((p->kind == SGA_CUBE) || (p->kind == SGA_CUBE_WIRES))
                        ? "SIZE" : "END OFFSET";
        y = Vec3Row(&p->size.x, &p->size.y, &p->size.z, lbl,
                    -4.0f, 4.0f, (Rectangle){ x, y, w, 0 }, fsSmall, s) + 5.0f*s;
    }

    if (p->kind == SGA_SPHERE)
    {
        if (ForgeSlider((Rectangle){ x, y, w, SF_RH*s }, "RADIUS", &p->r0,
                        0.0f, 3.0f, fsSmall)) { GestureTouch(); StrategyAssetMeasure(&s_doc); }
        y += SF_RH*s + 6.0f*s;
    }

    if (p->kind == SGA_CYLINDER)
    {
        if (ForgeSlider((Rectangle){ x, y, w, SF_RH*s }, "TOP R", &p->r0,
                        0.0f, 3.0f, fsSmall)) { GestureTouch(); StrategyAssetMeasure(&s_doc); }
        y += SF_RH*s + 4.0f*s;
        if (ForgeSlider((Rectangle){ x, y, w, SF_RH*s }, "BASE R", &p->r1,
                        0.0f, 3.0f, fsSmall)) { GestureTouch(); StrategyAssetMeasure(&s_doc); }
        y += SF_RH*s + 4.0f*s;
        if (ForgeSlider((Rectangle){ x, y, w, SF_RH*s }, "HEIGHT", &p->h,
                        0.0f, 4.0f, fsSmall)) { GestureTouch(); StrategyAssetMeasure(&s_doc); }
        y += SF_RH*s + 4.0f*s;

        float sides = (float)p->sides;
        if (ForgeSlider((Rectangle){ x, y, w, SF_RH*s }, "SIDES", &sides,
                        3.0f, 24.0f, fsSmall))
        { GestureTouch(); p->sides = (int32_t)(sides + 0.5f); }
        y += SF_RH*s + 6.0f*s;
    }

    if (p->kind == SGA_CYLINDER_EX)
    {
        if (ForgeSlider((Rectangle){ x, y, w, SF_RH*s }, "THICK", &p->r0,
                        0.0f, 1.0f, fsSmall)) GestureTouch();
        y += SF_RH*s + 6.0f*s;
    }

    // -- colour ---------------------------------------------------------------
    y += 4.0f*s;
    DrawRectangleRec((Rectangle){ x, y, w, 1.0f }, COL_LINE);
    y += 8.0f*s;
    DrawText("FACTION COLOUR", (int)x, (int)y, fsSmall, COL_TEXT_DIM);
    y += (float)fsSmall + 4.0f*s;

    // ONE slider, not a mode picker. The three stored modes are not three
    // behaviours - PARTIAL at 0 is exactly FIXED and PARTIAL at 1 is exactly
    // FULL (see StrategyAssetPartColor) - so a radio row was asking the author
    // to pick a mode that the number already says. The slider is the control;
    // the mode is derived from it, purely so the file keeps its v1 layout and
    // an asset saved before this change still reads back identically.
    {
        float blend = (p->tintMode == SGA_TINT_FULL) ? 1.0f
                    : (p->tintMode == SGA_TINT_NONE) ? 0.0f
                                                     : p->tintAmount;
        if (ForgeSlider((Rectangle){ x, y, w, SF_RH*s }, "FACTION", &blend,
                        0.0f, 1.0f, fsSmall))
        {
            GestureTouch();
            p->tintAmount = blend;
            // Snap to the exact endpoints so a part the author dragged all the
            // way over stores FULL rather than PARTIAL at 0.999 - which matters
            // because FULL follows a faction whose palette may change later,
            // while a blend of 1.0 only matches it today.
            if (blend <= 0.0001f)      p->tintMode = SGA_TINT_NONE;
            else if (blend >= 0.9999f) p->tintMode = SGA_TINT_FULL;
            else                       p->tintMode = SGA_TINT_PARTIAL;
        }
        Tip((Rectangle){ x, y, w, SF_RH*s },
            "How much of the faction's colour this part takes. 0 keeps its own "
            "colour, 1 is the faction colour, and anything between reads as the "
            "same army while staying darker or lighter than its neighbours.");
        y += SF_RH*s + 6.0f*s;
    }

    // RGB is the part's OWN colour, so it disappears only when the faction has
    // fully replaced it - at which point the swatch would be a lie.
    if (p->tintMode != SGA_TINT_FULL)
    {
        // Full-width rows rather than three squeezed side by side: these are
        // ForgeSliders now, so they carry the Ctrl/Shift modifiers and need
        // room for their readout.
        float rc = (float)p->color.r, gc = (float)p->color.g, bc = (float)p->color.b;
        bool moved = false;
        if (ForgeSlider((Rectangle){ x, y, w, SF_RH*s }, "RED", &rc, 0.0f, 255.0f, fsSmall))
            moved = true;
        y += SF_RH*s + 3.0f*s;
        if (ForgeSlider((Rectangle){ x, y, w, SF_RH*s }, "GREEN", &gc, 0.0f, 255.0f, fsSmall))
            moved = true;
        y += SF_RH*s + 3.0f*s;
        if (ForgeSlider((Rectangle){ x, y, w, SF_RH*s }, "BLUE", &bc, 0.0f, 255.0f, fsSmall))
            moved = true;
        y += SF_RH*s + 8.0f*s;
        if (moved)
        {
            GestureTouch();
            p->color.r = (unsigned char)(rc + 0.5f);
            p->color.g = (unsigned char)(gc + 0.5f);
            p->color.b = (unsigned char)(bc + 0.5f);
        }
    }
    {
        if (ForgeSlider((Rectangle){ x, y, w, SF_RH*s }, "BRIGHT", &p->brightness,
                        -1.0f, 1.0f, fsSmall)) GestureTouch();
        Tip((Rectangle){ x, y, w, SF_RH*s },
            "Lightens or darkens AFTER the faction mix - this is what makes one "
            "part read darker than another in the same army.");
        y += SF_RH*s + 8.0f*s;
    }

    // Live result, against the faction actually selected. A raw swatch would
    // hide the entire point of the tint controls.
    Rectangle swatch = { x, y, w, 22.0f*s };
    Color shown = StrategyAssetPartColor(p, s_faction, 1.0f);
    DrawRectangleRec(swatch, shown);
    DrawRectangleLinesEx(swatch, 1.0f, COL_LINE);

    // Pick the label's colour from the swatch's LUMINANCE, not a fixed value:
    // a light part rendered white-on-white, and the one control whose entire
    // job is showing a colour must never hide its own caption. The Rec.709
    // weights are the usual perceptual ones - green reads far brighter than
    // blue at the same numeric value.
    float lum = (0.2126f*(float)shown.r + 0.7152f*(float)shown.g +
                 0.0722f*(float)shown.b)/255.0f;
    Color labelCol = (lum > 0.55f) ? (Color){ 16, 18, 24, 255 } : COL_TEXT;
    DrawText("RESULT", (int)(x + 6.0f*s), (int)(y + (22.0f*s - (float)fsSmall)*0.5f),
             fsSmall, labelCol);
    Tip(swatch, "The colour this part will actually be, for the faction shown above.");
    y += 22.0f*s + 8.0f*s;

    // -- remove ---------------------------------------------------------------
    if (ForgeButton((Rectangle){ x, y, w, 24.0f*s }, "REMOVE PART",
                    s_doc.partCount > 1, s_doc.partCount > 1
                        ? "Delete this part. Motion paths bound to it are cleared."
                        : "An asset needs at least one part.", fsSmall))
    {
        Touch(true);
        if (StrategyAssetRemovePart(&s_doc, s_sel))
        {
            if (s_sel >= s_doc.partCount) s_sel = s_doc.partCount - 1;
            StrategyAssetMeasure(&s_doc);
        }
    }
    return y + 24.0f*s;
}

// ---------------------------------------------------------------------------
//  Ghost reference
// ---------------------------------------------------------------------------
static void PickAdd(const char *name, const char *cat, const char *sub,
                    const StrategyModel *m)
{
    if (s_pickCount >= SF_BROWSE_MAX) return;
    PickRow *r = &s_pickList[s_pickCount++];
    TextCopy(r->name, name);
    TextCopy(r->category, cat);
    TextCopy(r->subtype, sub ? sub : "");
    r->model = m;
}

static void PickScan(void)
{
    s_pickCount = 0;
    s_pickScroll = 0.0f;
    s_pickSearch[0] = '\0';    // a stale filter would silently hide everything
    s_edSearch = false;

    // Built-ins first: they are the shipped scale reference.
    for (int i = 0; i < UNIT_KIND_COUNT; i++)
    {
        const StrategyModel *m = StrategyUnitModel((UnitKind)i);
        if (m) PickAdd(m->name, "UNIT", m->name, m);
    }
    for (int i = 0; i < BLD_COUNT; i++)
    {
        const StrategyModel *m = StrategyBuildingModel((BuildingKind)i);
        if (m) PickAdd(m->name, "BUILDING", m->name, m);
    }
    for (int i = 0; i < NODE_KIND_COUNT; i++)
    {
        const StrategyModel *m = StrategyNodeModel((NodeKind)i);
        if (m) PickAdd(m->name, "RESOURCE", m->name, m);
    }

    if (!DirectoryExists(SGA_DIR)) return;

    // Then authored files. Their category/subtype come from the file, which
    // means a load per row - acceptable because this runs once when the picker
    // opens, not per frame, and an SgaAsset is far too big to cache one each.
    FilePathList fl = LoadDirectoryFilesEx(SGA_DIR, SGA_EXT, false);
    static SgaAsset probe;      // static: ~450 KB is not a stack local
    for (unsigned int i = 0; (i < fl.count) && (s_pickCount < SF_BROWSE_MAX); i++)
    {
        const char *base = GetFileNameWithoutExt(fl.paths[i]);
        if (base[0] == '_') continue;               // the pool's private files
        if (s_file[0] && TextIsEqual(base, s_file)) continue;   // not itself

        if (StrategyAssetLoad(&probe, fl.paths[i]))
            PickAdd(base, StrategyAssetCategoryName(probe.category), probe.subtype, NULL);
        else
            PickAdd(base, "AUTHORED", "", NULL);
    }
    UnloadDirectoryFiles(fl);
}

// Case-insensitive substring, for the picker's search box.
static bool PickContainsCI(const char *hay, const char *needle)
{
    if (!needle || !needle[0]) return true;
    if (!hay) return false;
    for (int i = 0; hay[i]; i++)
    {
        int j = 0;
        while (needle[j] && hay[i + j])
        {
            char a = hay[i + j], b = needle[j];
            if ((a >= 'A') && (a <= 'Z')) a = (char)(a - 'A' + 'a');
            if ((b >= 'A') && (b <= 'Z')) b = (char)(b - 'A' + 'a');
            if (a != b) break;
            j++;
        }
        if (!needle[j]) return true;
    }
    return false;
}

// Name, category OR subtype - so "unit", "quarry" and "oak" all find something.
static bool PickMatches(const PickRow *r)
{
    if (!s_pickSearch[0]) return true;
    return PickContainsCI(r->name, s_pickSearch) ||
           PickContainsCI(r->category, s_pickSearch) ||
           PickContainsCI(r->subtype, s_pickSearch);
}

static void PickGui(float s, int fs)
{
    if (!s_pickOpen) return;

    Vector2 sc = ScreenStateSize();
    float mw = 420.0f*s, mh = 380.0f*s;
    if (mw > sc.x - 40.0f) mw = sc.x - 40.0f;
    if (mh > sc.y - 40.0f) mh = sc.y - 40.0f;
    Rectangle m = { (sc.x - mw)*0.5f, (sc.y - mh)*0.5f, mw, mh };

    DrawRectangle(0, 0, (int)sc.x, (int)sc.y, (Color){ 0, 0, 0, 150 });
    DrawRectangleRec(m, COL_PANEL);
    DrawRectangleLinesEx(m, 1.0f, COL_LINE_HI);
    DrawText("REFERENCE GHOST", (int)(m.x + 14.0f*s), (int)(m.y + 12.0f*s), fs, COL_TEXT);
    DrawText("Drawn faintly behind your model, to trace size and silhouette.",
             (int)(m.x + 14.0f*s), (int)(m.y + 12.0f*s + (float)fs + 4.0f*s),
             fs, COL_TEXT_DIM);

    // Search: name, category or subtype. The list is short enough to scroll,
    // but scrolling to compare against a specific shipped model is exactly the
    // friction this removes.
    Rectangle sb = { m.x + 14.0f*s, m.y + 52.0f*s, mw - 28.0f*s, SF_RH*s };
    if (GuiTextBox(sb, s_pickSearch, sizeof(s_pickSearch), s_edSearch))
        s_edSearch = !s_edSearch;
    if (!s_pickSearch[0] && !s_edSearch)
        DrawText("search name, category or subtype",
                 (int)(sb.x + 6.0f*s), (int)(sb.y + (sb.height - (float)fs)*0.5f),
                 fs, Fade(COL_TEXT_DIM, 0.55f));

    Rectangle list = { m.x + 14.0f*s, sb.y + sb.height + 8.0f*s,
                       mw - 28.0f*s, mh - (sb.y + sb.height + 8.0f*s - m.y) - 50.0f*s };
    DrawRectangleRec(list, COL_BG);
    DrawRectangleLinesEx(list, 1.0f, COL_LINE);

    Vector2 mp = GetMousePosition();
    if (CheckCollisionPointRec(mp, list)) s_pickScroll += GetMouseWheelMove()*24.0f;
    float rowH = 26.0f*s;

    // Count matches first, so the scroll clamp is against what is actually
    // shown rather than the unfiltered list.
    int shown = 0;
    for (int i = 0; i < s_pickCount; i++) if (PickMatches(&s_pickList[i])) shown++;

    float maxScroll = (float)shown*rowH - list.height;
    if (maxScroll < 0.0f) maxScroll = 0.0f;
    if (s_pickScroll < -maxScroll) s_pickScroll = -maxScroll;
    if (s_pickScroll > 0.0f) s_pickScroll = 0.0f;

    int clicked = -1;
    BeginScissorMode((int)list.x, (int)list.y, (int)list.width, (int)list.height);
    int row = 0;
    for (int i = 0; i < s_pickCount; i++)
    {
        if (!PickMatches(&s_pickList[i])) continue;
        Rectangle r = { list.x + 2.0f, list.y + s_pickScroll + (float)row*rowH,
                        list.width - 4.0f, rowH - 2.0f };
        row++;
        // Skip rows scrolled out of the panel entirely: a scissor clips PIXELS
        // but not hit-testing, so an unclipped row would still take the click.
        if (r.y + r.height < list.y) continue;
        if (r.y > list.y + list.height) break;

        bool hot = CheckCollisionPointRec(mp, r);
        DrawRectangleRec(r, hot ? COL_PANEL_HI : COL_PANEL);
        DrawText(s_pickList[i].name, (int)(r.x + 8.0f*s),
                 (int)(r.y + (r.height - (float)fs)*0.5f), fs,
                 hot ? COL_TEXT : COL_TEXT_DIM);

        // The category, right-aligned, so the built-ins and authored assets are
        // distinguishable without opening them.
        const char *cat = s_pickList[i].category;
        Color cc = s_pickList[i].model ? Fade(COL_TEXT_DIM, 0.7f) : Fade(COL_ACCENT, 0.85f);
        DrawText(cat, (int)(r.x + r.width - (float)MeasureText(cat, fs) - 8.0f*s),
                 (int)(r.y + (r.height - (float)fs)*0.5f), fs, cc);

        if (hot && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) clicked = i;
    }
    EndScissorMode();

    if (shown == 0)
        DrawText(s_pickSearch[0] ? "Nothing matches that search."
                                 : "No assets available.",
                 (int)(list.x + 8.0f*s), (int)(list.y + 8.0f*s), fs,
                 Fade(COL_TEXT_DIM, 0.7f));

    if (ForgeButton((Rectangle){ m.x + mw - 90.0f*s, m.y + mh - 38.0f*s, 78.0f*s, 26.0f*s },
                    "CANCEL", true, NULL, fs))
    { s_pickOpen = false; s_edSearch = false; }

    if (clicked >= 0)
    {
        AudioPlayButton();
        const PickRow *pr = &s_pickList[clicked];
        if (pr->model)
        {
            s_refBuiltin = pr->model;
            s_refLoaded = false;
            s_refOn = true;
            StatusOK(TextFormat("ghost: %s", pr->name));
        }
        else if (StrategyAssetLoad(&s_ref, StrategyAssetPath(pr->name)))
        {
            s_refLoaded = true;
            s_refBuiltin = NULL;
            s_refOn = true;
            StatusOK(TextFormat("ghost: %s", pr->name));
        }
        else StatusWarn("could not load that asset");
        s_pickOpen = false;
        s_edSearch = false;
    }
}

// ---------------------------------------------------------------------------
//  Header: identity. Category and subtype are MANDATORY, so they live here in
//  the permanently visible chrome rather than behind a save dialog.
// ---------------------------------------------------------------------------
static void HeaderGui(Rectangle bar, float s, int fs, int fsSmall)
{
    DrawRectangleRec(bar, COL_PANEL);
    DrawRectangleRec((Rectangle){ bar.x, bar.y + bar.height - 1.0f, bar.width, 1.0f },
                     COL_LINE);

    // Controls are SHORTER than a standard row here and pinned to the bar's
    // bottom, so the label line above them is clear space rather than something
    // the inputs grow up into.
    float ctlH = SF_RH*s - 4.0f*s;
    float y = bar.y + bar.height - ctlH - 7.0f*s;
    float labelY = y - (float)fsSmall - 3.0f*s;
    float x = bar.x + SF_PAD*s;

    DrawText("FORGE", (int)x, (int)(bar.y + 7.0f*s), fsSmall, COL_ACCENT);
    DrawText(s_dirty ? "unsaved" : (s_file[0] ? "saved" : "new"),
             (int)x, (int)(y + (ctlH - (float)fsSmall)*0.5f), fsSmall,
             s_dirty ? COL_WARN : Fade(COL_TEXT_DIM, 0.8f));
    x += 70.0f*s;

    // -- name -----------------------------------------------------------------
    float nameW = 190.0f*s;
    DrawText("NAME", (int)x, (int)labelY, fsSmall, COL_TEXT_DIM);
    if (GuiTextBox((Rectangle){ x, y, nameW, ctlH }, s_doc.name, SGA_NAME_MAX, s_edName))
    { s_edName = !s_edName; Touch(true); }
    x += nameW + 10.0f*s;

    // -- category -------------------------------------------------------------
    float catW = 210.0f*s;
    DrawText("CATEGORY", (int)x, (int)labelY, fsSmall, COL_TEXT_DIM);
    {
        static const char *cats[] = { "BUILDING", "UNIT", "RESOURCE" };
        int pick = ForgeChips((Rectangle){ x, y, catW, ctlH }, cats, 3,
                              s_doc.category, fsSmall, COL_ACCENT);
        Tip((Rectangle){ x, y, catW, ctlH },
            "Used to FIND assets, never to restrict them - any asset can be "
            "assigned to any game role.");
        if ((pick >= 0) && (pick != s_doc.category)) { Touch(true); s_doc.category = pick; }
    }
    x += catW + 10.0f*s;

    // -- subtype --------------------------------------------------------------
    float subW = 150.0f*s;
    bool subMissing = (s_doc.subtype[0] == '\0');
    DrawText("SUBTYPE", (int)x, (int)labelY, fsSmall,
             subMissing ? COL_WARN : COL_TEXT_DIM);
    if (GuiTextBox((Rectangle){ x, y, subW, ctlH }, s_doc.subtype, SGA_SUBTYPE_MAX, s_edSub))
    { s_edSub = !s_edSub; Touch(true); }
    if (subMissing)
    {
        DrawRectangleLinesEx((Rectangle){ x, y, subW, ctlH }, 1.0f, COL_WARN);
        Tip((Rectangle){ x, y, subW, ctlH },
            "Required. A free label like \"soldier\", \"quarry\" or \"oak\".");
    }
    x += subW + 14.0f*s;

    // -- faction preview ------------------------------------------------------
    DrawText("PREVIEW AS", (int)x, (int)labelY, fsSmall, COL_TEXT_DIM);
    {
        static const char *facs[] = { "A", "B", "NEUTRAL" };
        int cur = (s_faction == FACTION_NEUTRAL) ? 2 : s_faction;
        int pick = ForgeChips((Rectangle){ x, y, 130.0f*s, ctlH }, facs, 3, cur,
                              fsSmall, StrategyFactionTint(s_faction));
        Tip((Rectangle){ x, y, 130.0f*s, ctlH },
            "Which faction's colours the preview uses. Does not change the asset.");
        if (pick >= 0) s_faction = (pick == 2) ? FACTION_NEUTRAL : pick;
    }
}

// ---------------------------------------------------------------------------
//  Timeline
//
//  The view model is SECONDS, not pixels: the strip always shows exactly
//  [0, duration] for the current state. The zen editor's timeline pans and
//  zooms because animations there run minutes; an asset state is a couple of
//  seconds of loop, so a fixed fit is less machinery AND less to explain.
//  Duration is a control, so "show me more time" is the same gesture as
//  "make it longer".
//
//  One lane per part, including paths - a path part can be keyed too, which is
//  how a whole orbit is made to drift. Lanes scroll when there are more parts
//  than fit; the selected part's lane is always dragged into view.
// ---------------------------------------------------------------------------
static float TlDuration(void)
{
    float d = s_doc.duration[s_state];
    return (d > 0.01f) ? d : 1.0f;      // never divide by a zero-length state
}

static float TlTimeToX(Rectangle track, float t)
{
    return track.x + (t/TlDuration())*track.width;
}

static float TlXToTime(Rectangle track, float x)
{
    if (track.width <= 1.0f) return 0.0f;
    float t = ((x - track.x)/track.width)*TlDuration();
    if (t < 0.0f) t = 0.0f;
    if (t > TlDuration()) t = TlDuration();
    return t;
}

// The lane strip's own scissor-and-scroll. A scissor clips PIXELS but not hit
// testing, so every interaction below is gated on the row rect being inside the
// visible band as well - otherwise a key scrolled out of sight still catches
// clicks aimed at the transport underneath it.
static void DrawKeyDiamond(float cx, float cy, float r, Color fill, Color edge)
{
    Vector2 a = { cx, cy - r }, b = { cx + r, cy }, c = { cx, cy + r }, d = { cx - r, cy };
    DrawTriangle(a, d, b, fill);
    DrawTriangle(b, d, c, fill);
    DrawLineV(a, b, edge); DrawLineV(b, c, edge);
    DrawLineV(c, d, edge); DrawLineV(d, a, edge);
}

static void TimelineGui(Rectangle bar, float s, int fs, int fsSmall)
{
    DrawRectangleRec(bar, COL_PANEL);
    DrawRectangleRec((Rectangle){ bar.x, bar.y, bar.width, 1.0f }, COL_LINE);

    Vector2 mp = GetMousePosition();
    bool block = ModalBlocks();

    float pad = 10.0f*s;
    float x = bar.x + pad;
    float w = bar.width - pad*2.0f;

    // -- transport row --------------------------------------------------------
    float rowH = 20.0f*s;
    float y = bar.y + 6.0f*s;
    float bw = 54.0f*s;

    if (ForgeButtonEx((Rectangle){ x, y, bw, rowH }, s_playing ? "PAUSE" : "PLAY",
                      true, "Play this state's animation in the viewport. SPACE.",
                      fsSmall, s_playing))
        s_playing = !s_playing;

    if (ForgeButton((Rectangle){ x + bw + 4.0f*s, y, bw, rowH }, "STOP", true,
                    "Stop and rewind to 0.", fsSmall))
    { s_playing = false; s_clock = 0.0f; }

    if (ForgeButtonEx((Rectangle){ x + (bw + 4.0f*s)*2.0f, y, bw, rowH }, "LOOP",
                      true, "Repeat when the head reaches the end.", fsSmall, s_loop))
        s_loop = !s_loop;

    // Duration is here rather than in a settings panel because it IS the
    // timeline's extent - changing it rescales what the strip shows.
    {
        Rectangle dr = { x + (bw + 4.0f*s)*3.0f + 10.0f*s, y + 2.0f*s,
                         190.0f*s, rowH };
        float d = s_doc.duration[s_state];
        if (ForgeSlider(dr, "LENGTH", &d, 0.0f, 8.0f, fsSmall))
        {
            GestureTouch();
            s_doc.duration[s_state] = d;
        }
        Tip(dr, "How long this state lasts, in seconds. 0 makes it a still pose.");
    }

    // Key actions operate on the SELECTED part, which is the same selection the
    // inspector and the viewport use - one selection for the whole tool.
    {
        SgaPart *sp = SelPart();
        float kx = bar.x + bar.width - pad - bw*3.0f - 8.0f*s;
        bool canAdd = (sp != NULL);
        bool canDel = (sp != NULL) && (s_keySel >= 0) &&
                      (s_keySel < sp->anim[s_state].keyCount);

        if (ForgeButtonEx((Rectangle){ kx, y, bw, rowH }, "+ KEY", canAdd,
                          canAdd ? "Add a key for this part at the play head. K."
                                 : "Select a part first.", fsSmall, true))
        {
            Touch(true);
            int k = StrategyAssetAddKey(&s_doc, s_sel, s_state, s_clock);
            if (k < 0) StatusWarn("This part is out of keys for this state.");
            else
            {
                s_keySel = k;
                // A key at 0 on a state with no length is invisible and cannot
                // be dragged anywhere. Give the state a default span the first
                // time it gets a key, so there is somewhere to put the second.
                if (s_doc.duration[s_state] <= 0.01f) s_doc.duration[s_state] = 1.0f;
            }
        }

        if (ForgeButton((Rectangle){ kx + bw + 4.0f*s, y, bw, rowH }, "DEL KEY",
                        canDel, canDel ? "Remove the selected key. DELETE."
                                       : "Select a key on the strip below.", fsSmall))
        {
            Touch(true);
            StrategyAssetRemoveKey(&s_doc, s_sel, s_state, s_keySel);
            s_keySel = -1;
        }

        // The ease belongs to the key, and shapes the segment ENDING at it -
        // the same rule the zen editor uses, so the two never disagree.
        const char *elbl = "EASE";
        if (canDel)
        {
            int e = sp->anim[s_state].keys[s_keySel].ease;
            elbl = (e < 0) ? "LINEAR" : StrategyAssetEaseName(&s_doc, e);
        }
        if (ForgeButton((Rectangle){ kx + (bw + 4.0f*s)*2.0f, y, bw, rowH }, elbl,
                        canDel, canDel
                            ? "The curve shaping the segment that ENDS at this key."
                            : "Select a key to give it a curve.", fsSmall))
        { s_easeOpen = true; s_easeScroll = 0; }
    }

    y += rowH + 8.0f*s;

    // -- ruler ----------------------------------------------------------------
    float nameW = 92.0f*s;
    Rectangle track = { x + nameW, y, w - nameW, 14.0f*s };
    DrawRectangleRec(track, (Color){ 21, 23, 30, 255 });
    DrawRectangleLinesEx(track, 1.0f, COL_LINE);

    {
        // A tick every quarter second up to a second per tick, so the ruler
        // stays readable whether the state is 0.5s or 8s long.
        float dur = TlDuration();
        float step = 0.25f;
        while ((dur/step) > 16.0f) step *= 2.0f;
        for (float t = 0.0f; t <= dur + 1e-4f; t += step)
        {
            float tx = TlTimeToX(track, t);
            DrawLine((int)tx, (int)track.y, (int)tx, (int)(track.y + track.height),
                     Fade(COL_LINE_HI, 0.55f));
            DrawText(TextFormat("%.2g", (double)t), (int)(tx + 2.0f*s),
                     (int)track.y, fsSmall, Fade(COL_TEXT_DIM, 0.75f));
        }
    }

    DrawText("PART", (int)x, (int)(track.y + 1.0f*s), fsSmall, COL_TEXT_DIM);

    // -- lanes ----------------------------------------------------------------
    float laneTop = track.y + track.height + 3.0f*s;
    float laneH = SF_LANE_H*s;
    float bandH = bar.y + bar.height - laneTop - 6.0f*s;
    int visible = (bandH > laneH) ? (int)(bandH/laneH) : 1;
    if (visible < 1) visible = 1;

    // Keep the selected part's lane on screen - the strip is useless if the
    // part you are editing has scrolled out of it.
    if (s_sel < s_tlScroll) s_tlScroll = (float)s_sel;
    if (s_sel >= s_tlScroll + (float)visible) s_tlScroll = (float)(s_sel - visible + 1);
    int maxScroll = s_doc.partCount - visible;
    if (maxScroll < 0) maxScroll = 0;
    if (s_tlScroll > (float)maxScroll) s_tlScroll = (float)maxScroll;
    if (s_tlScroll < 0.0f) s_tlScroll = 0.0f;

    Rectangle band = { x, laneTop, w, bandH };
    if (!block && CheckCollisionPointRec(mp, band) && (s_doc.partCount > visible))
    {
        float wheel = GetMouseWheelMove();
        if (wheel != 0.0f)
        {
            s_tlScroll -= wheel;
            if (s_tlScroll < 0.0f) s_tlScroll = 0.0f;
            if (s_tlScroll > (float)maxScroll) s_tlScroll = (float)maxScroll;
        }
    }

    // Did this frame's click land on a key? Anything else inside the strip is
    // empty space, and clicking empty space clears the selection - so there is
    // always a way OUT of editing a key, not just a way in.
    bool hitKey = false;

    int first = (int)s_tlScroll;
    for (int i = 0; i < visible; i++)
    {
        int pi = first + i;
        if (pi >= s_doc.partCount) break;

        SgaPart *p = &s_doc.parts[pi];
        SgaPartAnim *an = &p->anim[s_state];
        float ly = laneTop + (float)i*laneH;
        Rectangle lane = { track.x, ly, track.width, laneH - 2.0f*s };
        Rectangle nameR = { x, ly, nameW - 6.0f*s, laneH - 2.0f*s };
        bool selRow = (pi == s_sel);

        DrawRectangleRec(lane, selRow ? Fade(COL_ACCENT, 0.07f)
                                      : (Color){ 21, 23, 30, 255 });
        DrawRectangleLinesEx(lane, 1.0f, selRow ? Fade(COL_ACCENT, 0.5f) : COL_LINE);

        const char *nm = (p->name[0] != '\0') ? p->name : StrategyAssetKindName(p->kind);
        DrawText(nm, (int)nameR.x, (int)(ly + (laneH - (float)fsSmall)*0.5f - 1.0f*s),
                 fsSmall, selRow ? COL_TEXT : COL_TEXT_DIM);

        // Clicking a lane's name selects the part, so the strip doubles as a
        // second part list - the one you are already looking at while keying.
        if (!block && CheckCollisionPointRec(mp, nameR) &&
            IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
        { AudioPlayButton(); s_sel = pi; s_keySel = -1; }

        // The segments between keys, so a lane reads as a span of motion rather
        // than as loose dots.
        for (int k = 0; k + 1 < an->keyCount; k++)
        {
            float ax = TlTimeToX(lane, an->keys[k].t);
            float bx = TlTimeToX(lane, an->keys[k + 1].t);
            DrawRectangleRec((Rectangle){ ax, ly + laneH*0.5f - 1.5f*s,
                                          bx - ax, 3.0f*s },
                             selRow ? Fade(COL_ACCENT, 0.45f) : Fade(COL_LINE_HI, 0.35f));
        }

        for (int k = 0; k < an->keyCount; k++)
        {
            float kx = TlTimeToX(lane, an->keys[k].t);
            float ky = ly + (laneH - 2.0f*s)*0.5f;
            float r = SF_KEY_R*s;
            bool kSel = selRow && (k == s_keySel);

            Rectangle hit = { kx - r - 2.0f*s, ly, r*2.0f + 4.0f*s, laneH };
            bool hot = !block && CheckCollisionPointRec(mp, hit);

            DrawKeyDiamond(kx, ky, r,
                           kSel ? COL_ACCENT : (hot ? COL_TEXT : COL_TEXT_DIM),
                           kSel ? COL_TEXT : COL_LINE);

            if (hot && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
            {
                AudioPlayButton();
                hitKey = true;
                s_keyPicked = true;
                // Set the part FIRST: the stale-selection guard in Gui() keys
                // off s_sel changing, and would clear the key we just picked if
                // it saw the two move in the wrong order on the next frame.
                s_sel = pi;
                s_keySel = k;
                s_keyDrag = true;
                s_playing = false;      // dragging a key while it plays is a fight
            }
        }
    }

    if (s_doc.partCount > visible)
        DrawText(TextFormat("%d/%d", first + visible, s_doc.partCount),
                 (int)(x + w - 34.0f*s), (int)(bar.y + bar.height - (float)fsSmall - 3.0f*s),
                 fsSmall, Fade(COL_TEXT_DIM, 0.6f));

    // Empty space inside the lane band deselects. Deliberately NOT the whole
    // bar: the transport row and the ruler have their own jobs, and clearing
    // the selection when you reach for PLAY would be its own bug.
    if (!block && !hitKey && !s_keyDrag &&
        CheckCollisionPointRec(mp, band) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
        s_keySel = -1;

    // -- key drag -------------------------------------------------------------
    // Dragging is resolved AFTER the lanes are drawn so a key that jumps past a
    // neighbour (and so changes index) does not skip a frame of feedback.
    if (s_keyDrag)
    {
        if (IsMouseButtonDown(MOUSE_BUTTON_LEFT))
        {
            SgaPart *sp = SelPart();
            if ((sp != NULL) && (s_keySel >= 0) && (s_keySel < sp->anim[s_state].keyCount))
            {
                float t = TlXToTime(track, mp.x);
                bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
                if (ctrl) t = roundf(t/0.05f)*0.05f;    // same snap idiom as the sliders
                if (fabsf(t - sp->anim[s_state].keys[s_keySel].t) > 1e-5f)
                {
                    GestureTouch();
                    s_keySel = StrategyAssetMoveKey(&s_doc, s_sel, s_state, s_keySel, t);
                    s_clock = t;        // the head follows the key, so the pose previews
                }
            }
        }
        else s_keyDrag = false;
    }

    // -- play head ------------------------------------------------------------
    // Drawn over the lanes, and scrubbable anywhere on the ruler.
    if (!block && !s_keyDrag && CheckCollisionPointRec(mp, track) &&
        IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
    { s_scrubbing = true; s_playing = false; }
    if (s_scrubbing)
    {
        if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) s_clock = TlXToTime(track, mp.x);
        else s_scrubbing = false;
    }

    {
        float hx = TlTimeToX(track, s_clock);
        DrawLine((int)hx, (int)track.y, (int)hx, (int)(bar.y + bar.height - 4.0f*s),
                 COL_WARN);
        DrawKeyDiamond(hx, track.y + 3.0f*s, 4.0f*s, COL_WARN, COL_WARN);
    }

    Tip(track, "Click or drag to scrub. The viewport poses to the head.");

    // Empty states teach rather than sit blank.
    if (!StrategyAssetStateHasKeys(&s_doc, s_state))
    {
        const char *msg = TextFormat("No %s animation. Select a part, move the head, "
                                     "then + KEY.", StrategyAssetStateName(s_state));
        DrawText(msg, (int)(track.x + 10.0f*s),
                 (int)(laneTop + 4.0f*s), fsSmall, Fade(COL_TEXT_DIM, 0.55f));
    }
}

// ---------------------------------------------------------------------------
//  State tabs. Phase 3 fills these with a timeline; for now they select which
//  animation the viewport plays, which is enough to author against.
// ---------------------------------------------------------------------------
static void StateTabsGui(Rectangle r, float s, int fsSmall)
{
    Vector2 mp = GetMousePosition();
    float cw = (r.width - 5.0f*4.0f*s)/6.0f;

    for (int i = 0; i < SGA_STATE_COUNT; i++)
    {
        Rectangle c = { r.x + (float)i*(cw + 4.0f*s), r.y, cw, r.height };
        bool on = (i == s_state);
        bool hot = !ModalBlocks() && CheckCollisionPointRec(mp, c);

        // Does this state have any keys at all? An empty tab is dimmed, so the
        // row shows at a glance which states are authored.
        bool has = false;
        for (int k = 0; (k < s_doc.partCount) && !has; k++)
            if (s_doc.parts[k].anim[i].keyCount > 0) has = true;

        DrawRectangleRec(c, on ? Fade(COL_ACCENT, 0.20f) : (hot ? COL_PANEL_HI : COL_PANEL));
        DrawRectangleLinesEx(c, 1.0f, on ? COL_ACCENT : (hot ? COL_LINE_HI : COL_LINE));

        const char *nm = StrategyAssetStateName(i);
        Color tc = on ? COL_TEXT : (has ? COL_TEXT_DIM : Fade(COL_TEXT_DIM, 0.45f));
        DrawText(nm, (int)(c.x + (cw - (float)MeasureText(nm, fsSmall))*0.5f),
                 (int)(c.y + (c.height - (float)fsSmall)*0.5f), fsSmall, tc);

        Tip(c, has ? "Play this state in the viewport."
                   : "No animation on this state yet - it plays the rest pose.");
        if (hot && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
        { AudioPlayButton(); s_state = i; }
    }
}

// ---------------------------------------------------------------------------
//  Footer: the ghost controls and the file actions
// ---------------------------------------------------------------------------
static void CloseForge(void)
{
    AppStateTransition(s_return ? s_return : &app_state_strategy_showcase);
}

static void FooterGui(Rectangle bar, float s, int fs, int fsSmall)
{
    DrawRectangleRec(bar, COL_PANEL);
    DrawRectangleRec((Rectangle){ bar.x, bar.y, bar.width, 1.0f }, COL_LINE);

    // Controls are pushed to the BOTTOM of the bar so the label line above them
    // has its own space. Centring them left ~12px of headroom, which a label
    // drawn at a fixed offset from the bar's top overran.
    float bh = 28.0f*s;
    float by = bar.y + bar.height - bh - 8.0f*s;
    float labelY = by - (float)fsSmall - 3.0f*s;
    float x = bar.x + SF_PAD*s;

    // -- back -----------------------------------------------------------------
    if (ForgeButton((Rectangle){ x, by, 84.0f*s, bh }, "GALLERY", true,
                    s_dirty ? "Leave the forge. You will be asked about unsaved work."
                            : "Back to the gallery.", fsSmall))
    {
        if (s_dirty) s_confirmExit = true;
        else { CloseForge(); return; }
    }
    x += 84.0f*s + 10.0f*s;

    // -- ghost ----------------------------------------------------------------
    DrawText("GHOST", (int)x, (int)labelY, fsSmall, COL_TEXT_DIM);
    bool hasGhost = (s_refLoaded || s_refBuiltin);
    if (ForgeButton((Rectangle){ x, by, 74.0f*s, bh },
                    hasGhost ? (s_refOn ? "SHOWN" : "HIDDEN") : "PICK...", true,
                    hasGhost ? "Toggle the reference ghost. (G)"
                             : "Load another asset to trace against. (G)", fsSmall))
    {
        if (hasGhost) s_refOn = !s_refOn;
        else { PickScan(); s_pickOpen = true; }
    }
    x += 74.0f*s + 6.0f*s;

    if (hasGhost)
    {
        if (ForgeButton((Rectangle){ x, by, 60.0f*s, bh }, "SWAP", true,
                        "Pick a different reference asset.", fsSmall))
        { PickScan(); s_pickOpen = true; }
        x += 60.0f*s + 6.0f*s;

        if (ForgeButton((Rectangle){ x, by, 60.0f*s, bh }, "CLEAR", true,
                        "Remove the reference ghost.", fsSmall))
        { s_refLoaded = false; s_refBuiltin = NULL; }
        x += 60.0f*s + 10.0f*s;

        // Alpha and a sideways offset, so the ghost can sit BESIDE the model
        // rather than inside it - which is the only way to compare silhouettes.
        float sw = 92.0f*s;
        DrawText("FADE", (int)x, (int)labelY, fsSmall, COL_TEXT_DIM);
        GuiSliderBar((Rectangle){ x, by + 4.0f*s, sw, bh - 8.0f*s }, NULL, NULL,
                     &s_refAlpha, 0.05f, 0.8f);
        x += sw + 10.0f*s;

        DrawText("BESIDE", (int)x, (int)labelY, fsSmall, COL_TEXT_DIM);
        GuiSliderBar((Rectangle){ x, by + 4.0f*s, sw, bh - 8.0f*s }, NULL, NULL,
                     &s_refOff.x, -4.0f, 4.0f);
        Tip((Rectangle){ x, by, sw, bh },
            "Slide the ghost aside to compare silhouettes rather than overlay them.");
        x += sw + 10.0f*s;
    }

    // -- right-aligned file actions ------------------------------------------
    Vector2 screen = ScreenStateSize();
    float saveW = 110.0f*s, undoW = 74.0f*s;
    float rx = screen.x - SF_PAD*s - saveW;

    const char *why = NULL;
    bool ok = StrategyAssetValid(&s_doc, &why);

    if (ForgeButtonEx((Rectangle){ rx, by, saveW, bh }, "SAVE", ok,
                      ok ? "Write this asset to disk. (Ctrl+S)" : why, fsSmall, true))
        DoSave();
    // The reason a disabled SAVE is disabled has to be reachable, and a
    // disabled control never takes a hover in raygui - so the tip is recorded
    // here, over the same rectangle, unconditionally.
    if (!ok) Tip((Rectangle){ rx, by, saveW, bh }, why);
    rx -= undoW + 8.0f*s;

    if (ForgeButton((Rectangle){ rx, by, undoW, bh }, "UNDO", s_undoCount > 0,
                    s_undoCount > 0 ? "Undo the last change. (Ctrl+Z)"
                                    : "Nothing to undo.", fsSmall))
        UndoPop();
    rx -= undoW + 8.0f*s;

    if (ForgeButton((Rectangle){ rx, by, undoW, bh }, "REDO", s_redoCount > 0,
                    s_redoCount > 0 ? "Redo the last undo. (Ctrl+Y)"
                                    : "Nothing to redo.", fsSmall))
        RedoPop();
    rx -= 88.0f*s + 8.0f*s;

    // SAVE AS is gated on the SAME validity as SAVE: a name prompt that ends in
    // "Subtype is required" has wasted the author's time twice over.
    if (ForgeButton((Rectangle){ rx, by, 88.0f*s, bh }, "SAVE AS", ok,
                    ok ? "Write this asset under a different name." : why, fsSmall))
        SaveOpen();
    if (!ok) Tip((Rectangle){ rx, by, 88.0f*s, bh }, why);
    rx -= 88.0f*s + 8.0f*s;

    // DELETE only means anything once the doc actually has a file behind it.
    if (ForgeButton((Rectangle){ rx, by, 88.0f*s, bh }, "DELETE", s_file[0] != '\0',
                    s_file[0] ? "Delete this asset's file."
                              : "This asset has not been saved yet.", fsSmall))
        s_confirmDelete = true;

    // -- status ---------------------------------------------------------------
    if (s_statusT > 0.0f)
    {
        float a = (s_statusT > 1.0f) ? 1.0f : s_statusT;
        // On the label line, right of the controls' labels: the status is
        // transient and must not shove a button around when it appears.
        DrawText(s_status, (int)(bar.x + bar.width*0.42f), (int)labelY,
                 fsSmall, Fade(s_statusCol, a));
    }
}

// ---------------------------------------------------------------------------
//  Modals
// ---------------------------------------------------------------------------
// Shared frame: dimmer, panel, title, message. Returns the panel rect so the
// caller can lay its buttons out inside it.
static Rectangle ModalFrame(const char *title, const char *msg, float s, int fs,
                            float mw, float mh)
{
    return UiModalFrame(title, msg, s, fs, mw, mh);
}

static void ModalsGui(float s, int fs)
{
    // Everything drawn from here on is the modal itself, so its own controls
    // are exempt from the guard that blocks the editor behind it.
    SetInModal(true);

    PickGui(s, fs);

    if (s_saveOpen)
    {
        Rectangle m = ModalFrame("SAVE ASSET", "The name is also the filename.",
                                 s, fs, 400.0f*s, 170.0f*s);
        Rectangle tb = { m.x + 16.0f*s, m.y + 74.0f*s, m.width - 32.0f*s, SF_RH*s };
        if (GuiTextBox(tb, s_saveBuf, SGA_NAME_MAX, s_edSaveName))
            s_edSaveName = !s_edSaveName;

        bool taken = s_saveBuf[0] && !StrategyAssetNameFree(s_saveBuf) &&
                     !TextIsEqual(s_saveBuf, s_file);
        if (taken)
            DrawText("overwrites an existing asset", (int)tb.x,
                     (int)(tb.y + tb.height + 5.0f*s), fs, COL_WARN);

        float bh = 26.0f*s;
        float by = m.y + m.height - bh - 14.0f*s;
        if (ForgeButtonEx((Rectangle){ m.x + m.width - 100.0f*s, by, 88.0f*s, bh },
                          taken ? "OVERWRITE" : "SAVE", s_saveBuf[0] != '\0', NULL, fs, true) ||
            (s_edSaveName && (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER))))
        {
            s_edSaveName = false;
            if (SaveAs(s_saveBuf)) s_saveOpen = false;
        }
        if (ForgeButton((Rectangle){ m.x + m.width - 196.0f*s, by, 88.0f*s, bh },
                        "CANCEL", true, NULL, fs))
        { s_saveOpen = false; s_edSaveName = false; }
    }

    if (s_confirmDelete)
    {
        Rectangle m = ModalFrame("DELETE ASSET",
                                 TextFormat("Delete \"%s\" from disk? This cannot be undone.",
                                            s_file), s, fs, 440.0f*s, 150.0f*s);
        float bh = 26.0f*s;
        float by = m.y + m.height - bh - 14.0f*s;
        if (ForgeButtonEx((Rectangle){ m.x + m.width - 100.0f*s, by, 88.0f*s, bh },
                          "DELETE", true, NULL, fs, true))
        {
            if (StrategyAssetDelete(s_file))
            {
                StatusOK(TextFormat("deleted %s%s", s_file, SGA_EXT));
                s_file[0] = '\0';       // the doc lives on in memory, unbound
                s_dirty = true;
            }
            else StatusWarn("could not delete that file");
            s_confirmDelete = false;
        }
        if (ForgeButton((Rectangle){ m.x + m.width - 196.0f*s, by, 88.0f*s, bh },
                        "CANCEL", true, NULL, fs))
            s_confirmDelete = false;
    }

    if (s_confirmExit)
    {
        Rectangle m = ModalFrame("UNSAVED CHANGES",
                                 "Leave the forge without saving?", s, fs,
                                 420.0f*s, 150.0f*s);
        float bh = 26.0f*s;
        float by = m.y + m.height - bh - 14.0f*s;
        if (ForgeButton((Rectangle){ m.x + m.width - 100.0f*s, by, 88.0f*s, bh },
                        "DISCARD", true, NULL, fs))
        { s_confirmExit = false; SetInModal(false); CloseForge(); return; }
        if (ForgeButtonEx((Rectangle){ m.x + m.width - 196.0f*s, by, 88.0f*s, bh },
                          "SAVE", true, NULL, fs, true))
        {
            s_confirmExit = false;
            // Save straight through when the doc already has a file and is
            // valid; otherwise fall into the prompt rather than failing quietly.
            if (s_file[0] && StrategyAssetValid(&s_doc, NULL))
            { if (SaveAs(s_file)) { SetInModal(false); CloseForge(); return; } }
            else SaveOpen();
        }
        if (ForgeButton((Rectangle){ m.x + m.width - 292.0f*s, by, 88.0f*s, bh },
                        "CANCEL", true, NULL, fs))
            s_confirmExit = false;
    }

    // -- easing picker --------------------------------------------------------
    //  Lists the builtins AND the custom curves from _easings.cfg, honouring
    //  the hidden flag the way the zen editor's dropdown does. Whatever is
    //  chosen is BAKED into the asset on the spot (see strategy_asset_ease.c),
    //  so the .sga keeps playing that shape even if the .cfg later changes.
    if (s_easeOpen)
    {
        Rectangle m = ModalFrame("EASING", "Shapes the segment ending at this key.",
                                 s, fs, 460.0f*s, 380.0f*s);

        SgaPart *sp = SelPart();
        SgaKey *key = NULL;
        if ((sp != NULL) && (s_keySel >= 0) && (s_keySel < sp->anim[s_state].keyCount))
            key = &sp->anim[s_state].keys[s_keySel];

        float rowH = 20.0f*s;
        float lx = m.x + 16.0f*s;
        float lw = m.width*0.5f - 24.0f*s;
        float ly = m.y + 66.0f*s;
        float listH = m.height - 66.0f*s - 46.0f*s;
        int rows = (int)(listH/rowH);
        if (rows < 1) rows = 1;

        // Build the visible id list first: the runtime range is SPARSE (unused
        // custom slots are invalid ids), so it cannot be indexed directly.
        int ids[ANIM_EASE_COUNT + ANIM_CUSTOM_EASE_MAX + 1];
        int n = 0;
        ids[n++] = -1;                  // linear, which is "no curve" here
        int range = AnimEaseIdRange();
        for (int i = 1; i < range; i++)
        {
            if (!AnimEaseIdValid(i)) continue;
            if (AnimEaseIsHidden(i)) continue;
            if (n < (int)(sizeof(ids)/sizeof(ids[0]))) ids[n++] = i;
        }

        if (s_easeScroll > n - rows) s_easeScroll = n - rows;
        if (s_easeScroll < 0) s_easeScroll = 0;

        Rectangle listR = { lx, ly, lw, listH };
        if (CheckCollisionPointRec(GetMousePosition(), listR))
        {
            float wheel = GetMouseWheelMove();
            if (wheel != 0.0f)
            {
                s_easeScroll -= (int)wheel;
                if (s_easeScroll > n - rows) s_easeScroll = n - rows;
                if (s_easeScroll < 0) s_easeScroll = 0;
            }
        }

        int curRuntime = -1;
        if ((key != NULL) && (key->ease >= 0))
            curRuntime = StrategyAssetEaseRuntimeId(&s_doc, key->ease);

        for (int r = 0; r < rows; r++)
        {
            int i = s_easeScroll + r;
            if (i >= n) break;
            int id = ids[i];
            Rectangle rr = { lx, ly + (float)r*rowH, lw, rowH - 2.0f*s };

            bool on;
            if (key == NULL) on = false;
            else if (id < 0) on = (key->ease < 0);
            else on = (curRuntime == id);

            const char *nm = (id < 0) ? "linear" : AnimEaseName(id);
            bool custom = (id >= ANIM_EASE_COUNT);

            if (ForgeButtonEx(rr, nm, key != NULL,
                              custom ? "A custom curve from anims/_easings.cfg. "
                                       "Its shape is copied into this asset."
                                     : NULL, fs, on))
            {
                Touch(true);
                key->ease = (id < 0) ? -1 : StrategyAssetBakeEase(&s_doc, id);
                if ((id >= 0) && (key->ease < 0))
                    StatusWarn("this asset is out of easing slots");
            }
        }

        // -- curve preview ----------------------------------------------------
        // The shape as it will ACTUALLY play, read back out of the baked slot
        // rather than from the live easing function - so if baking ever loses
        // something, this is where it shows.
        Rectangle g = { m.x + m.width*0.5f + 8.0f*s, ly, m.width*0.5f - 24.0f*s,
                        lw*0.72f };
        if (g.height > listH) g.height = listH;
        DrawRectangleRec(g, (Color){ 21, 23, 30, 255 });
        DrawRectangleLinesEx(g, 1.0f, COL_LINE);
        DrawLine((int)g.x, (int)(g.y + g.height), (int)(g.x + g.width),
                 (int)(g.y + g.height), Fade(COL_LINE_HI, 0.4f));

        if ((key != NULL) && (key->ease >= 0))
        {
            Vector2 prev = { g.x, g.y + g.height };
            for (int i = 1; i <= 48; i++)
            {
                float px = (float)i/48.0f;
                float py = StrategyAssetEaseApplyBaked(&s_doc, key->ease, px);
                // Curves overshoot (backOut, elasticOut), so the plot is scaled
                // to a -0.25..1.25 window and clipped rather than clamped - a
                // flattened overshoot would misrepresent the curve.
                float ny = (py + 0.25f)/1.5f;
                Vector2 cur = { g.x + px*g.width, g.y + g.height - ny*g.height };
                if ((cur.y > g.y) && (cur.y < g.y + g.height) &&
                    (prev.y > g.y) && (prev.y < g.y + g.height))
                    DrawLineEx(prev, cur, 2.0f, COL_ACCENT);
                prev = cur;
            }
            DrawText(StrategyAssetEaseName(&s_doc, key->ease), (int)(g.x + 6.0f*s),
                     (int)(g.y + 4.0f*s), fs, COL_TEXT_DIM);
        }
        else
        {
            DrawLineEx((Vector2){ g.x, g.y + g.height }, (Vector2){ g.x + g.width, g.y },
                       2.0f, Fade(COL_ACCENT, 0.55f));
            DrawText("linear", (int)(g.x + 6.0f*s), (int)(g.y + 4.0f*s), fs, COL_TEXT_DIM);
        }

        float bh = 26.0f*s;
        if (ForgeButton((Rectangle){ m.x + m.width - 100.0f*s,
                                     m.y + m.height - bh - 14.0f*s, 88.0f*s, bh },
                        "DONE", true, NULL, fs))
            s_easeOpen = false;
    }

    SetInModal(false);
}

// ---------------------------------------------------------------------------
//  AppState hooks
// ---------------------------------------------------------------------------

static void Enter()
{
    ScreenState *ss = ScreenStateGet();
    ss->clear_color = COL_BG;

    // The forge is normally opened through StrategyForgeOpenNew/Asset/Builtin,
    // which run BEFORE the transition. Reaching Enter with nothing opened means
    // something jumped here directly - give it a blank document rather than
    // whatever the last session left in s_doc.
    if (!s_opened) StrategyForgeOpenNew();

    // The custom easing set is normally loaded by the zen editor, which the
    // user may never have opened this session - without this the picker would
    // offer builtins only. A missing file is not an error: baked assets never
    // needed it in the first place.
    //
    // Guarded on the set being EMPTY, because AnimCustomEasesLoad treats the
    // file as the whole truth and wipes what is loaded first. Reloading it
    // unconditionally would throw away curve edits the zen editor is holding
    // unsaved, from a tool that has no business touching them.
    {
        bool anyCustom = false;
        int range = AnimEaseIdRange();
        for (int i = ANIM_EASE_COUNT; (i < range) && !anyCustom; i++)
            if (AnimEaseIdValid(i)) anyCustom = true;
        if (!anyCustom) AnimCustomEasesLoad("anims/_easings.cfg");
    }

    s_playing = false;      // never arrive mid-playback
    s_keySel = -1;
    s_clock = 0.0f;
    s_statusT = 0.0f;
    UiCtxGet()->tip[0] = '\0';
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

    // The head only MOVES while playing - otherwise it is the author's cursor,
    // parked wherever they scrubbed it to. Advancing it always would make the
    // pose drift out from under a key they are trying to set.
    if (s_playing)
    {
        s_clock += GetFrameTime();

        float d = s_doc.duration[s_state];
        if (d > 0.0f)
        {
            if (s_clock > d)
            {
                if (s_loop) s_clock -= d*floorf(s_clock/d);
                else { s_clock = d; s_playing = false; }
            }
        }
        else { s_clock = 0.0f; s_playing = false; }  // a still state has nothing to play
    }
}

static void Draw()
{
    // Deliberately empty: everything is screen space, drawn in Gui().
    // See the file header.
}

static void Gui()
{
    Vector2 screen = ScreenStateSize();
    float s = SF_S();
    int fs      = (int)(14.0f*s);
    int fsSmall = (int)(11.0f*s);
    if (fs < 10) fs = 10;
    if (fsSmall < 8) fsSmall = 8;

    // Publish this frame's modal state to the shared widget module BEFORE any
    // widget runs - every one of them gates its hit testing on it. Also clears
    // the tip, which TipDraw() at the end of this function re-consumes.
    UiFrameBegin(ModalOpen());

    DrawRectangle(0, 0, (int)screen.x, (int)screen.y, COL_BG);

    float headerH = SF_HEADER_H*s;
    float footerH = SF_FOOTER_H*s;
    float leftW   = SF_LEFT_W*s;
    float rightW  = SF_RIGHT_W*s;
    float tabsH   = 26.0f*s;

    Rectangle header = { 0.0f, 0.0f, screen.x, headerH };
    Rectangle footer = { 0.0f, screen.y - footerH, screen.x, footerH };
    Rectangle left   = { 0.0f, headerH, leftW, screen.y - headerH - footerH };
    Rectangle right  = { screen.x - rightW, headerH, rightW,
                         screen.y - headerH - footerH };
    Rectangle mid    = { leftW, headerH, screen.x - leftW - rightW,
                         screen.y - headerH - footerH };
    Rectangle tabs   = { mid.x + 8.0f*s, mid.y + 6.0f*s, mid.width - 16.0f*s, tabsH };

    // The timeline spans the FULL width under the panes rather than sitting
    // inside the middle column: key lanes need the horizontal room far more
    // than the viewport does, and a strip that lines up with the window edge
    // reads as one instrument instead of a box inside a box.
    float tlH = SF_TIMELINE_H*s;
    if (tlH > screen.y*0.34f) tlH = screen.y*0.34f;      // never crowd the model out
    Rectangle timeline = { 0.0f, screen.y - footerH - tlH, screen.x, tlH };

    left.height  -= tlH;
    right.height -= tlH;
    mid.height   -= tlH;

    Rectangle vp     = { mid.x, tabs.y + tabsH + 6.0f*s, mid.width,
                         mid.height - (tabs.y + tabsH + 6.0f*s - mid.y) };

    // A key selection belongs to ONE part in ONE state. Rather than clear it at
    // every place s_sel or s_state can change - the part list, the lane names,
    // the tab row, TAB, the viewport - the invariant is enforced here, once:
    // if the selection owner moved, the key selection is stale and goes.
    {
        static int lastSel = -1, lastState = -1;
        if ((s_sel != lastSel) || (s_state != lastState))
        {
            // s_keyPicked is set by the timeline when a click selected a part
            // and a key TOGETHER. Without it this guard would see s_sel change
            // on the next frame and wipe the very key that click chose - the
            // selection would flicker on for one frame and vanish.
            if (((lastSel >= 0) || (lastState >= 0)) && !s_keyPicked) s_keySel = -1;
            lastSel = s_sel;
            lastState = s_state;
        }
        s_keyPicked = false;
        // Deleting a key or switching to a part with fewer keys can also leave
        // it dangling past the end.
        SgaPart *sp = SelPart();
        if (sp == NULL) s_keySel = -1;
        else if (s_keySel >= sp->anim[s_state].keyCount) s_keySel = -1;
    }

    // Input BEFORE drawing, so a drag started this frame is already reflected.
    ViewportInput(vp);

    ViewportDraw(vp, s_clock);
    StateTabsGui(tabs, s, fsSmall);
    PartListGui(left, s, fsSmall);
    PartInspectorGui(right, s, fs, fsSmall);
    HeaderGui(header, s, fs, fsSmall);
    TimelineGui(timeline, s, fs, fsSmall);
    FooterGui(footer, s, fs, fsSmall);

    // The footer's BACK / SAVE-and-close leave the forge, and AppStateTransition
    // switches synchronously - the showcase has already painted its own frame by
    // now. Everything below draws forge furniture and reads forge hotkeys, so
    // running it would both flash this tool over the screen we left and let a
    // keypress edit a document that is no longer open.
    if (!AppStateIsCurrent(&app_state_strategy_forge)) return;

    // Hints in the viewport's corner: an empty-ish tool should teach.
    if (s_doc.partCount <= 1)
        DrawText("Drag to orbit  -  wheel to zoom  -  + adds a part",
                 (int)(vp.x + 10.0f*s), (int)(vp.y + vp.height - (float)fsSmall - 8.0f*s),
                 fsSmall, Fade(COL_TEXT_DIM, 0.55f));

    ModalsGui(s, fs);

    // -- keyboard -------------------------------------------------------------
    // Every shortcut is gated on Typing(), or typing a name would fire them.
    if (!Typing())
    {
        bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);

        bool shiftK = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
        if (ctrl && IsKeyPressed(KEY_Z)) { if (shiftK) RedoPop(); else UndoPop(); }
        if (ctrl && IsKeyPressed(KEY_Y)) RedoPop();
        if (ctrl && IsKeyPressed(KEY_S)) { if (!ModalOpen()) DoSave(); }

        if (!ModalOpen())
        {
            if (IsKeyPressed(KEY_G))
            {
                if (s_refLoaded || s_refBuiltin) s_refOn = !s_refOn;
                else { PickScan(); s_pickOpen = true; }
            }
            if (IsKeyPressed(KEY_TAB) && (s_doc.partCount > 0))
                s_sel = (s_sel + 1) % s_doc.partCount;

            // -- transport ----------------------------------------------------
            if (IsKeyPressed(KEY_SPACE)) s_playing = !s_playing;

            if (IsKeyPressed(KEY_K) && (SelPart() != NULL))
            {
                Touch(true);
                int k = StrategyAssetAddKey(&s_doc, s_sel, s_state, s_clock);
                if (k < 0) StatusWarn("This part is out of keys for this state.");
                else
                {
                    s_keySel = k;
                    if (s_doc.duration[s_state] <= 0.01f) s_doc.duration[s_state] = 1.0f;
                }
            }

            if (IsKeyPressed(KEY_DELETE))
            {
                SgaPart *sp = SelPart();
                if ((sp != NULL) && (s_keySel >= 0) &&
                    (s_keySel < sp->anim[s_state].keyCount))
                {
                    Touch(true);
                    StrategyAssetRemoveKey(&s_doc, s_sel, s_state, s_keySel);
                    s_keySel = -1;
                }
            }

            // Step between the selected part's keys. Landing exactly ON a key is
            // what makes editing one possible - eyeballing a drag never does.
            if (IsKeyPressed(KEY_LEFT) || IsKeyPressed(KEY_RIGHT))
            {
                SgaPart *sp = SelPart();
                if (sp != NULL)
                {
                    const SgaPartAnim *an = &sp->anim[s_state];
                    bool fwd = IsKeyPressed(KEY_RIGHT);
                    int best = -1;
                    for (int i = 0; i < an->keyCount; i++)
                    {
                        float dt = an->keys[i].t - s_clock;
                        if (fwd ? (dt > 1e-4f) : (dt < -1e-4f))
                        { best = i; if (fwd) break; }
                    }
                    if (best >= 0) { s_keySel = best; s_clock = an->keys[best].t; }
                }
            }
        }
    }

    // ESC is decided ONCE, innermost first. Splitting it across two blocks -
    // one opening the unsaved-changes prompt, a later one closing whatever
    // modal is open - meant a plain ESC opened the prompt and then closed it in
    // the same frame, so ESC appeared to do nothing at all in the forge.
    if (IsKeyPressed(KEY_ESCAPE))
    {
        if (s_easeOpen)             s_easeOpen = false;
        else if (s_pickOpen)        s_pickOpen = false;
        else if (s_confirmDelete)   s_confirmDelete = false;
        else if (s_confirmExit)     s_confirmExit = false;
        else if (s_saveOpen)      { s_saveOpen = false; s_edSaveName = false; }
        else if (Typing())
        {
            // Leave the field first: ESC in a text box means "stop editing",
            // not "leave the tool".
            s_edName = s_edSub = s_edPart = s_edSaveName = s_edSearch = false;
        }
        else if (s_dirty)           s_confirmExit = true;
        else                      { CloseForge(); return; }
    }

    GestureEnd();       // a slider drag ends when the button comes up
    TipDraw(fsSmall);   // last, so it paints over every panel
}
