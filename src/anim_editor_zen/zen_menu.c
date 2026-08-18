// ============================================================================
//  zen_menu.c  -  the Zen editor's top menu bar
//
//  One data-driven table (label / hotkey letter / action / enabled / checked /
//  description) drives everything: the bar, the dropdowns, the Alt hotkey
//  navigation AND the future tooltips - so a menu item can never disagree
//  with its shortcut.
//
//  Alt navigation: pressing Alt arms hotkey mode (letters get highlighted);
//  a section letter opens that menu, an item letter fires the action, any
//  other key or a mouse press leaves the mode. Alt+F+N = File > New, etc.
//
//  Tables are filled at runtime in MenusInit() - file-scope address-of
//  initializers are banned for MSVC (same rationale as anim.c's ease table).
//
//  Also home to the modal prompts (New/Rename/Delete/Duration/...), the
//  element library shelf and the Help modal, since the menu opens them.
// ============================================================================

#include "raylib.h"
#include "raygui.h"
#include "zen_internal.h"
#include "../screen_state/screen_state.h"
#include "../audio_state/audio_state.h"
#include "../anim/anim_io.h"    // AnimElemKindName for the library rows
#include <stdlib.h>             // strtof for the duration prompt
#include <string.h>

// pixel width of a label at the current gui font/size (raygui has no query).
static float TextWpx(const char *text)
{
    Font f = GuiGetFont();
    float fs = (float)GuiGetStyle(DEFAULT, TEXT_SIZE);
    float sp = (float)GuiGetStyle(DEFAULT, TEXT_SPACING);
    return MeasureTextEx(f, text, fs, sp).x;
}

#define ZEN_MENU_ITEM_H   24.0f
#define ZEN_MENUS_MAX     6
#define ZEN_MENU_ITEMS_MAX 16

typedef struct {
    const char *label;
    char key;                   // Alt-mode letter, unique within its menu
    void (*action)(void);       // NULL = separator row
    bool (*enabled)(void);      // NULL = always enabled
    bool (*checked)(void);      // NULL = plain item, else drawn as a toggle
    const char *desc;           // full description (tooltips / help)
} ZenMenuItem;

typedef struct {
    const char *label;
    char key;                   // Alt-mode letter, unique across menus
    ZenMenuItem items[ZEN_MENU_ITEMS_MAX];
    int count;
} ZenMenu;

static ZenMenu s_menus[ZEN_MENUS_MAX];
static int  s_menuCount = 0;
static bool s_menusInit = false;

// header rects of the current frame, for hover-switching and click-away.
static Rectangle s_hdrRect[ZEN_MENUS_MAX];

// ---------------------------------------------------------------------------
//  Actions. Small static fns over the shared ctx; grouped per menu.
// ---------------------------------------------------------------------------
static void CloseMenus(void) { zen.menuOpen = -1; zen.hotkeyNav = false; }

// -- File -------------------------------------------------------------------
static void ActFileNew(void)
{
    zen.nameBuf[0] = '\0'; zen.edNameBuf = true;
    zen.prompt = ZEN_PROMPT_NEW_NAME;
}
static float s_openScroll     = 0.0f;                // File>Open list scroll
static char  s_openSearch[ANIM_NAME_MAX];            // File>Open filter text
static bool  s_openSearchEdit = false;

static void ActFileOpen(void)
{
    zen.openListOpen = true;
    s_openScroll = 0.0f;
    s_openSearch[0] = '\0';
    s_openSearchEdit = true;                         // type-to-filter right away
}
static void ActFileSave(void)    { ZenSaveCurrent(); }
static void ActFileSaveAs(void)
{
    TextCopy(zen.nameBuf, zen.doc.name); zen.edNameBuf = true;
    zen.prompt = ZEN_PROMPT_COPY_ANIM;
}
static void ActFileRename(void)
{
    TextCopy(zen.nameBuf, zen.doc.name); zen.edNameBuf = true;
    zen.prompt = ZEN_PROMPT_RENAME_ANIM;
}
static void ActFileDelete(void)
{
    zen.prompt = ZEN_PROMPT_CONFIRM_DELETE;
    zen.promptTargetIdx = zen.animCurrent;
}
static void ActFileExit(void)    { ZenExitEditor(); }
static void ActFileExport(void)  { ZenExportShow(); }
static bool EnSavedAnim(void)    { return zen.animCurrent >= 0; }
// nothing to render out of an empty document.
static bool EnHasElems(void)     { return zen.doc.elemCount > 0; }

// -- Editor -----------------------------------------------------------------
static void ActEdLoop(void)      { zen.loopPlay = !zen.loopPlay; }
static bool ChkEdLoop(void)      { return zen.loopPlay; }
static void ActEdAutoKey(void)   { zen.autoKey = !zen.autoKey; }
static bool ChkEdAutoKey(void)   { return zen.autoKey; }
static void ActEdSmooth(void)    { ZenUndoPush(); zen.doc.loopSmooth = !zen.doc.loopSmooth; }
static bool ChkEdSmooth(void)    { return zen.doc.loopSmooth; }
static void ActEdStopSig(void)   { zen.stopOnSignal = !zen.stopOnSignal; }
static bool ChkEdStopSig(void)   { return zen.stopOnSignal; }
static void ActEdDuration(void)
{
    TextCopy(zen.nameBuf, TextFormat("%.2f", zen.doc.duration));
    zen.edNameBuf = true;
    zen.prompt = ZEN_PROMPT_DURATION;
}
static void ActEdAlpha(void)     { zen.panelAlphaMode = (zen.panelAlphaMode + 1) % 3; }
static void ActEdStrings(void)   { ZenStringPoolShow(); }
static void ActEdPause(void)
{
    ZenUndoPush();
    if (AnimDocAddPause(&zen.doc, zen.playhead, ZEN_PAUSE_EPS))
    {
        zen.selPause = AnimDocPauseAt(&zen.doc, zen.playhead, ZEN_PAUSE_EPS);
        zen.docDirty = true;
    }
}
// Greyed out when the playhead already carries a marker (one hold per instant)
// or the document is at its marker limit.
static bool EnNoPauseHere(void)
{
    return (zen.doc.pauseCount < ANIM_PAUSES_MAX)
        && (AnimDocPauseAt(&zen.doc, zen.playhead, ZEN_PAUSE_EPS) < 0);
}

// -- Element ----------------------------------------------------------------
static void ActElNewText(void)   { ZenElemAdd(AE_TEXT); }
static void ActElNewShape(void)  { ZenElemAdd(AE_SHAPE); }
static void ActElNewGlobal(void) { ZenElemAdd(AE_GLOBAL); }
static void ActElDup(void)       { ZenElemDuplicate(); }
static void ActElDelete(void)    { ZenElemDelete(); }
static void ActElUp(void)        { ZenElemMove(-1); }
static void ActElDown(void)      { ZenElemMove(+1); }
static void ActElToLib(void)
{
    TextCopy(zen.nameBuf, zen.doc.elems[zen.selElem].name);
    zen.edNameBuf = true; zen.libTargetIdx = -1;
    zen.prompt = ZEN_PROMPT_LIB_SAVE_NAME;
}
static void ActElClone(void)     { ZenCloneShow(); }
static void ActElLibrary(void)   { zen.libOpen = true; zen.libScroll = 0.0f; }
static void ActElUnits(void)
{
    ZenUndoPush();
    AnimElem *e = &zen.doc.elems[zen.selElem];
    e->sizeAbsolute = !e->sizeAbsolute;
}
static void ActElAnchor(void)
{
    ZenUndoPush();
    AnimElem *e = &zen.doc.elems[zen.selElem];
    e->cornerMode = !e->cornerMode;
}
static bool EnSel(void)      { return zen.selElem >= 0 && zen.selElem < zen.doc.elemCount; }
static bool EnSelUp(void)    { return EnSel() && zen.selElem > 0; }
static bool EnSelDown(void)  { return EnSel() && zen.selElem < zen.doc.elemCount - 1; }
static bool EnSelShape(void) { return EnSel() && zen.doc.elems[zen.selElem].kind == AE_SHAPE; }
static bool EnRoom(void)     { return zen.doc.elemCount < ANIM_ELEMS_MAX; }
// Clone needs something to clone FROM: a second element of the same kind.
static bool EnCloneSrc(void)
{
    if (!EnSel()) return false;
    int kind = zen.doc.elems[zen.selElem].kind;
    for (int i = 0; i < zen.doc.elemCount; i++)
        if ((i != zen.selElem) && (zen.doc.elems[i].kind == kind)) return true;
    return false;
}
static bool EnSelRoom(void)  { return EnSel() && EnRoom(); }
static bool ChkElUnits(void) { return EnSel() && zen.doc.elems[zen.selElem].sizeAbsolute; }
static bool ChkElAnchor(void){ return EnSel() && zen.doc.elems[zen.selElem].cornerMode; }

// -- View -------------------------------------------------------------------
static void ActVwElems(void)     { zen.showElems = !zen.showElems; }
static bool ChkVwElems(void)     { return zen.showElems; }
static void ActVwSignals(void)   { zen.showSignals = !zen.showSignals; }
static bool ChkVwSignals(void)   { return zen.showSignals; }
static void ActVwInspector(void) { zen.showInspector = !zen.showInspector; }
static bool ChkVwInspector(void) { return zen.showInspector; }
static void ActVwTimeline(void)  { zen.showTimeline = !zen.showTimeline; }
static bool ChkVwTimeline(void)  { return zen.showTimeline; }
static void ActVwZoom(void)      { zen.zoomFull = !zen.zoomFull; }
static bool ChkVwZoom(void)      { return zen.zoomFull; }
static void ActVwFollow(void)    { zen.tlFollow = !zen.tlFollow; }
static bool ChkVwFollow(void)    { return zen.tlFollow; }

// -- Easing -----------------------------------------------------------------
static void ActEaBrowse(void)    { ZenEasingBrowserOpen(); }

// -- Help -------------------------------------------------------------------
static void ActHelp(void)        { zen.helpOpen = true; }
static void ActGuide(void)       { ZenGuidePlay(); }

// The guided tour is just an authored animation on the runtime stage, so it
// brings its own pause-marker holds and any-key advance for free. loop=false
// is what makes it a SINGLE playback: the .cfg's loopSmooth only shapes the
// seam, it never decides whether the doc repeats (that is the call argument).
void ZenGuidePlay(void)
{
    ZenGuideStop();                 // restart from the top, never two at once

    // The editor's own preview clock is stopped for the duration: it holds on
    // pause markers by the same any-key rule, and two clocks reading the same
    // keypress would advance together.
    zen.playing = false; zen.playPending = false;
    zen.pausedOnMarker = false;

    zen.guideAnim = AnimStagePlay(ZEN_GUIDE_ANIM, false, 0);
}

void ZenGuideStop(void)
{
    AnimStageStop(zen.guideAnim);   // a no-op on a stale or NONE handle
    zen.guideAnim = ANIM_HANDLE_NONE;
}

bool ZenGuideActive(void) { return AnimStageAlive(zen.guideAnim); }

// ---------------------------------------------------------------------------
//  Table construction (runtime, once).
// ---------------------------------------------------------------------------
static void AddItem(ZenMenu *m, const char *label, char key, void (*action)(void),
                    bool (*enabled)(void), bool (*checked)(void), const char *desc)
{
    if (m->count >= ZEN_MENU_ITEMS_MAX) return;
    m->items[m->count++] = (ZenMenuItem){ label, key, action, enabled, checked, desc };
}

static void MenusInit(void)
{
    if (s_menusInit) return;
    s_menusInit = true;
    s_menuCount = 0;

    ZenMenu *m;

    m = &s_menus[s_menuCount++]; *m = (ZenMenu){ "File", 'F' };
    AddItem(m, "New...",     'N', ActFileNew,    NULL, NULL, "Create a new animation .cfg and open it");
    AddItem(m, "Open...",    'O', ActFileOpen,   NULL, NULL, "Switch to another saved animation");
    AddItem(m, "Save",       'S', ActFileSave,   NULL, NULL, "Write the animation to its .cfg (Ctrl+S)");
    AddItem(m, "Save As...", 'A', ActFileSaveAs, NULL, NULL, "Save a copy under a new name and edit the copy");
    AddItem(m, "Rename...",  'R', ActFileRename, EnSavedAnim, NULL, "Rename the animation file on disk");
    AddItem(m, "Delete",     'D', ActFileDelete, EnSavedAnim, NULL, "Delete the animation file (asks first)");
    AddItem(m, "-",          0,   NULL,          NULL, NULL, NULL);
    AddItem(m, "Export...",  'E', ActFileExport, EnHasElems, NULL, "Render the animation to a GIF, a video, or a PNG frame sequence");
    AddItem(m, "-",          0,   NULL,          NULL, NULL, NULL);
    AddItem(m, "Exit editor",'X', ActFileExit,   NULL, NULL, "Back to the main menu (asks when unsaved)");

    m = &s_menus[s_menuCount++]; *m = (ZenMenu){ "Editor", 'E' };
    AddItem(m, "Loop playback",     'L', ActEdLoop,    NULL, ChkEdLoop,   "Restart playback at the loop point when the end is reached");
    AddItem(m, "Auto-key",          'A', ActEdAutoKey, NULL, ChkEdAutoKey,"Editing a tracked property writes a key at the playhead");
    AddItem(m, "Smooth loop",       'S', ActEdSmooth,  NULL, ChkEdSmooth, "Blend the loop seam so the restart doesn't pop");
    AddItem(m, "Stop on signal",    'G', ActEdStopSig, NULL, ChkEdStopSig,"A fired signal ends playback instead of resuming it when the signal finishes. Either way the timeline freezes and shows the signal while it plays.");
    AddItem(m, "String pool...",    'R', ActEdStrings, NULL, NULL,        "The document's shared text strings - assign, edit and reuse them, and animate which one a text element shows");
    AddItem(m, "Add pause marker",  'P', ActEdPause,   EnNoPauseHere, NULL, "Hold playback at the playhead until the viewer presses a key. Greyed out when this position already has one.");
    AddItem(m, "Duration...",       'D', ActEdDuration,NULL, NULL,        "Type an exact animation length in seconds");
    AddItem(m, "Panel transparency",'T', ActEdAlpha,   NULL, NULL,        "Cycle panel background opacity: opaque / semi / faint");

    m = &s_menus[s_menuCount++]; *m = (ZenMenu){ "Element", 'L' };
    AddItem(m, "New text",       'T', ActElNewText,   EnRoom,    NULL, "Add a text element");
    AddItem(m, "New shape",      'S', ActElNewShape,  EnRoom,    NULL, "Add a shape element (rect/circle/line/...)");
    AddItem(m, "New global",     'G', ActElNewGlobal, EnRoom,    NULL, "Add a global element (screen fade / background)");
    AddItem(m, "-",              0,   NULL,           NULL,      NULL, NULL);
    AddItem(m, "Duplicate",      'C', ActElDup,       EnSelRoom, NULL, "Copy the selected element into a new slot with all its tracks");
    AddItem(m, "Clone from...",  'O', ActElClone,     EnCloneSrc,NULL, "Write another element's look at the playhead onto THIS element as keys at a time you pick - reuse an expired block instead of spending a new element slot");
    AddItem(m, "Delete",         'X', ActElDelete,    EnSel,     NULL, "Delete every selected element");
    AddItem(m, "Move up",        'U', ActElUp,        EnSelUp,   NULL, "Draw the selected element earlier (further back)");
    AddItem(m, "Move down",      'D', ActElDown,      EnSelDown, NULL, "Draw the selected element later (further front)");
    AddItem(m, "-",              0,   NULL,           NULL,      NULL, NULL);
    AddItem(m, "Save to library...", 'B', ActElToLib, EnSel,     NULL, "Shelve the selected element for reuse in any animation");
    AddItem(m, "Library...",     'L', ActElLibrary,   NULL,      NULL, "Browse the element library and insert entries");
    AddItem(m, "-",              0,   NULL,           NULL,      NULL, NULL);

    AddItem(m, "Absolute px sizes", 'P', ActElUnits,  EnSel,      ChkElUnits,  "Sizes in pixels instead of canvas fractions (%)");
    AddItem(m, "Anchor by corners", 'A', ActElAnchor, EnSelShape, ChkElAnchor, "Author the selected shape by two opposite corners");

    m = &s_menus[s_menuCount++]; *m = (ZenMenu){ "View", 'V' };
    AddItem(m, "Elements panel",  'E', ActVwElems,     NULL, ChkVwElems,     "Show / hide the ELEMENTS panel");
    AddItem(m, "Signals panel",   'S', ActVwSignals,   NULL, ChkVwSignals,   "Show / hide the SIGNALS panel");
    AddItem(m, "Inspector panel", 'I', ActVwInspector, NULL, ChkVwInspector, "Show / hide the INSPECTOR panel");
    AddItem(m, "Timeline panel",  'T', ActVwTimeline,  NULL, ChkVwTimeline,  "Show / hide the TIMELINE panel");
    AddItem(m, "-",               0,   NULL,           NULL, NULL,           NULL);
    AddItem(m, "Edit at 1:1 zoom",'Z', ActVwZoom,      NULL, ChkVwZoom,      "Edit at actual screen size instead of zoomed out");
    AddItem(m, "Follow playhead", 'F', ActVwFollow,    NULL, ChkVwFollow,    "Keep the playhead in frame while the timeline is zoomed in");

    m = &s_menus[s_menuCount++]; *m = (ZenMenu){ "Easing", 'G' };
    AddItem(m, "Browse easings...", 'B', ActEaBrowse, NULL, NULL, "View, hide and create easing curves");

    m = &s_menus[s_menuCount++]; *m = (ZenMenu){ "Help", 'H' };
    AddItem(m, "Play the guided tour", 'G', ActGuide, NULL, NULL, "An animated tour of every panel, played over the editor - press any key to advance it");
    AddItem(m, "How the Zen editor works...", 'H', ActHelp, NULL, NULL, "All interactions explained in one page");
}

// Fire one item: sound, close the menus, run the action.
static void FireItem(const ZenMenuItem *it)
{
    if (!it->action) return;
    if (it->enabled && !it->enabled()) return;
    AudioPlayButton();
    CloseMenus();
    it->action();
}

// ---------------------------------------------------------------------------
//  Keyboard side (called from Update).
// ---------------------------------------------------------------------------
bool ZenMenuModalOpen(void)
{
    return zen.prompt != ZEN_PROMPT_NONE || zen.libOpen || zen.helpOpen
        || zen.openListOpen;
}

void ZenMenuUpdate(void)
{
    MenusInit();

    if (ZenTyping() || ZenMenuModalOpen()) return;

    // Alt arms (or disarms) hotkey navigation.
    if (IsKeyPressed(KEY_LEFT_ALT) || IsKeyPressed(KEY_RIGHT_ALT))
    {
        zen.hotkeyNav = !zen.hotkeyNav;
        if (!zen.hotkeyNav) zen.menuOpen = -1;
        return;
    }
    if (!zen.hotkeyNav) return;

    // Any mouse press leaves the mode (the click itself still lands on
    // whatever it hit - the bar keeps working by mouse in parallel).
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) ||
        IsMouseButtonPressed(MOUSE_BUTTON_RIGHT))
    { zen.hotkeyNav = false; return; }

    int k = GetKeyPressed();
    if (k == 0 || k == KEY_LEFT_ALT || k == KEY_RIGHT_ALT) return;

    if (zen.menuOpen < 0)
    {
        for (int i = 0; i < s_menuCount; i++)
            if (k == (int)s_menus[i].key) { zen.menuOpen = i; return; }
    }
    else
    {
        const ZenMenu *m = &s_menus[zen.menuOpen];
        for (int i = 0; i < m->count; i++)
            if (m->items[i].key && k == (int)m->items[i].key)
            { FireItem(&m->items[i]); return; }
    }

    // Unrecognized key: leave the mode (per spec, any other key exits).
    CloseMenus();
}

// ---------------------------------------------------------------------------
//  The bar (widgets pass; runs under the modal GuiLock like everything else).
// ---------------------------------------------------------------------------
void ZenMenuBarGui(void)
{
    MenusInit();

    Vector2 screen = ScreenStateSize();
    GuiStatusBar((Rectangle){ 0, 0, screen.x, ZEN_MENU_BAR_H }, "");

    float x = 4.0f;
    for (int i = 0; i < s_menuCount; i++)
    {
        const char *label = zen.hotkeyNav
                          ? TextFormat("%s [%c]", s_menus[i].label, s_menus[i].key)
                          : s_menus[i].label;
        float w = TextWpx(label) + 20.0f;
        s_hdrRect[i] = (Rectangle){ x, 0, w, ZEN_MENU_BAR_H };

        if (GuiButton(s_hdrRect[i], label))
        {
            AudioPlayButton();
            zen.menuOpen = (zen.menuOpen == i) ? -1 : i;
        }
        // classic menu behavior: with one open, hovering another switches.
        if (zen.menuOpen >= 0 && zen.menuOpen != i &&
            CheckCollisionPointRec(GetMousePosition(), s_hdrRect[i]))
            zen.menuOpen = i;
        // open menu's header stays visually pressed.
        if (zen.menuOpen == i)
            DrawRectangleRec(s_hdrRect[i], (Color){ 90, 140, 220, 70 });

        x += w + 2.0f;
    }

    // right side of the bar: the loaded animation + dirty marker.
    const char *nm = (zen.animCurrent >= 0) ? zen.animList[zen.animCurrent]
                                            : zen.doc.name;
    const char *info = TextFormat("%s%s", nm, zen.docDirty ? " *" : "");
    float iw = TextWpx(info) + 16.0f;
    GuiLabel((Rectangle){ screen.x - iw, 0, iw, ZEN_MENU_BAR_H }, info);
}

// ---------------------------------------------------------------------------
//  Overlays: the open dropdown, the File>Open list, prompts, library, help.
// ---------------------------------------------------------------------------
static void DrawMenuDropdown(void)
{
    if (zen.menuOpen < 0) return;
    const ZenMenu *m = &s_menus[zen.menuOpen];
    Rectangle hdr = s_hdrRect[zen.menuOpen];

    // width: widest label + room for the check mark and the hotkey letter.
    float w = 180.0f;
    for (int i = 0; i < m->count; i++)
    {
        float lw = TextWpx(m->items[i].label) + 76.0f;
        if (lw > w) w = lw;
    }
    Rectangle bg = { hdr.x, hdr.y + hdr.height, w, m->count * ZEN_MENU_ITEM_H + 8 };
    DrawRectangleRec(bg, (Color){ 32, 34, 40, 250 });
    DrawRectangleLinesEx(bg, 1.0f, (Color){ 70, 74, 84, 255 });

    const ZenMenuItem *fired = NULL;
    float y = bg.y + 4;
    for (int i = 0; i < m->count; i++)
    {
        const ZenMenuItem *it = &m->items[i];
        Rectangle r = { bg.x + 4, y, w - 8, ZEN_MENU_ITEM_H };
        if (!it->action)                             // separator
        {
            GuiLine((Rectangle){ r.x, y + ZEN_MENU_ITEM_H*0.5f - 1, r.width, 2 }, NULL);
            y += ZEN_MENU_ITEM_H;
            continue;
        }

        bool off = it->enabled && !it->enabled();
        if (off) GuiDisable();
        const char *mark = it->checked ? (it->checked() ? "[x] " : "[  ] ") : "";
        const char *label = zen.hotkeyNav && it->key
                          ? TextFormat("%s%s  [%c]", mark, it->label, it->key)
                          : TextFormat("%s%s", mark, it->label);
        if (GuiButton(r, label)) fired = it;
        if (off) GuiEnable();
        ZenTip(r, it->desc);                        // the table already has it
        y += ZEN_MENU_ITEM_H;
    }

    if (fired) FireItem(fired);
    else if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
             !CheckCollisionPointRec(GetMousePosition(), bg) &&
             !CheckCollisionPointRec(GetMousePosition(), hdr))
        CloseMenus();                                // click-away closes
}

// File > Open: centered searchable, scrollable list of every saved animation.
static void DrawOpenList(void)
{
    if (!zen.openListOpen) return;

    ScreenState *ss = ScreenStateGet();
    float W = (float)ss->width, H = (float)ss->height;
    float mw = 320, mh = 104.0f + zen.animCount * ZEN_MENU_ITEM_H;
    if (mh > H - 80) mh = H - 80;
    Rectangle m = { (W-mw)/2, (H-mh)/2, mw, mh };
    DrawRectangle(0, 0, (int)W, (int)H, (Color){ 0, 0, 0, 120 });
    DrawRectangleRec(m, (Color){ 40, 42, 48, 255 });
    DrawRectangleLinesEx(m, 1.0f, (Color){ 90, 94, 104, 255 });
    GuiLabel((Rectangle){ m.x+16, m.y+10, mw-32, 20 }, "OPEN ANIMATION");

    Rectangle sb = { m.x+12, m.y+34, mw-24, 24 };
    if (GuiTextBox(sb, s_openSearch, ANIM_NAME_MAX, s_openSearchEdit))
        s_openSearchEdit = !s_openSearchEdit;
    if (!s_openSearch[0] && !s_openSearchEdit)
        GuiLabel((Rectangle){ sb.x+8, sb.y, sb.width-16, sb.height }, "search...");

    // filter first so scroll bounds match what's shown
    int vis[ZEN_ANIM_LIST_MAX], nvis = 0;
    for (int i = 0; i < zen.animCount; i++)
        if (ZenStrContainsCI(zen.animList[i], s_openSearch)) vis[nvis++] = i;

    Rectangle list = { m.x+12, m.y+64, mw-24, mh-64-46 };
    if (CheckCollisionPointRec(GetMousePosition(), list))
        s_openScroll += GetMouseWheelMove() * 24.0f;
    float maxScroll = nvis * ZEN_MENU_ITEM_H - list.height;
    if (maxScroll < 0) maxScroll = 0;
    if (s_openScroll < -maxScroll) s_openScroll = -maxScroll;
    if (s_openScroll > 0) s_openScroll = 0;

    int req = -1;
    BeginScissorMode((int)list.x, (int)list.y, (int)list.width, (int)list.height);
    float y = list.y + s_openScroll;
    for (int v = 0; v < nvis; v++)
    {
        int i = vis[v];
        Rectangle r = { list.x, y, list.width, ZEN_MENU_ITEM_H };
        if (GuiButton(r, zen.animList[i])) req = i;
        if (i == zen.animCurrent) DrawRectangleRec(r, (Color){ 90, 140, 220, 60 });
        y += ZEN_MENU_ITEM_H;
    }
    EndScissorMode();
    if (nvis == 0)
        GuiLabel((Rectangle){ list.x+4, list.y+4, list.width-8, 20 }, "(no match)");

    if (GuiButton((Rectangle){ m.x + mw - 90, m.y + mh - 34, 78, 26 }, "Close"))
    { AudioPlayButton(); zen.openListOpen = false; }

    // Enter opens the single/first match while typing in the search box
    if (req < 0 && s_openSearchEdit && nvis > 0 &&
        (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)))
        req = vis[0];

    if (req >= 0) { AudioPlayButton(); ZenRequestSwitch(req); }
}

bool ZenMenuTyping(void) { return zen.openListOpen && s_openSearchEdit; }

// The small centered prompt box (name inputs / confirmations / duration).
static void DrawPromptModal(void)
{
    if (zen.prompt == ZEN_PROMPT_NONE) return;

    ScreenState *ss = ScreenStateGet();
    float W = (float)ss->width, H = (float)ss->height;
    float mw = 320, mh = 120;
    Rectangle m = { (W-mw)/2, (H-mh)/2, mw, mh };
    DrawRectangle(0, 0, (int)W, (int)H, (Color){ 0, 0, 0, 120 });
    DrawRectangleRec(m, (Color){ 40, 42, 48, 255 });
    DrawRectangleLinesEx(m, 1.0f, (Color){ 90, 94, 104, 255 });

    float bw = 90, bh = 28, by = m.y + mh - bh - 12;
    Rectangle msg = { m.x+16, m.y+14, mw-32, 40 };
    Rectangle tb  = { m.x+16, m.y+44, mw-32, 26 };
    bool enter = IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER);

    switch ((ZenPromptKind)zen.prompt)
    {
    case ZEN_PROMPT_SAVE_THEN_SWITCH:
    case ZEN_PROMPT_SAVE_THEN_EXIT:
    case ZEN_PROMPT_SAVE_THEN_EXPORT:
    {
        bool exiting   = zen.prompt == ZEN_PROMPT_SAVE_THEN_EXIT;
        // The export reads every animation from its .cfg, so an unsaved edit
        // would silently not appear in the output. Same three choices, but
        // neither branch leaves the current document - it just has to be on
        // disk (or knowingly not) before the job may read it.
        bool exporting = zen.prompt == ZEN_PROMPT_SAVE_THEN_EXPORT;
        const char *nm = zen.animCurrent >= 0 ? zen.animList[zen.animCurrent]
                                              : "*unsaved";
        GuiLabel(msg, exporting
            ? TextFormat("Export reads \"%s\" from disk. Save changes first?", nm)
            : TextFormat("Save changes to \"%s\"?", nm));
        if (GuiButton((Rectangle){ m.x+16, by, bw, bh }, "Save"))
        {   // saving rescans (list may reorder): re-find the target by name.
            AudioPlayButton();
            char target[ANIM_NAME_MAX] = {0};
            if (!exiting && !exporting)
                TextCopy(target, zen.animList[zen.promptTargetIdx]);
            ZenSaveCurrent();
            zen.prompt = ZEN_PROMPT_NONE;
            if (exporting)   ZenExportStartAfterSave();
            else if (exiting) ZenExitEditor();
            else ZenLoadAnimByIndex(ZenAnimFind(target));
        }
        // "Discard" is wrong for the export: nothing is thrown away, the job
        // just reads what is already on disk.
        if (GuiButton((Rectangle){ m.x+(mw-bw)/2, by, bw, bh },
                      exporting ? "Use saved" : "Discard"))
        {
            AudioPlayButton();
            // Discard for switch/exit means throwing the edits away, so the
            // dirty flag goes with them. For an export it means only "do not
            // write them to the file first" - the document is not being closed
            // and the edits are still in it, so it stays DIRTY or the next Open
            // would drop them without asking.
            if (!exporting) zen.docDirty = false;
            int t = zen.promptTargetIdx;
            zen.prompt = ZEN_PROMPT_NONE;
            if (exporting)   ZenExportStartAfterSave();
            else if (exiting) ZenExitEditor();
            else ZenLoadAnimByIndex(t);
        }
        if (GuiButton((Rectangle){ m.x+mw-bw-16, by, bw, bh }, "Cancel"))
        { AudioPlayButton(); zen.prompt = ZEN_PROMPT_NONE; }
        break;
    }
    case ZEN_PROMPT_CONFIRM_DELETE:
    {
        const char *nm = (zen.promptTargetIdx >= 0 && zen.promptTargetIdx < zen.animCount)
                       ? zen.animList[zen.promptTargetIdx] : "?";
        GuiLabel(msg, TextFormat("Delete animation \"%s\"? This cannot be undone.", nm));
        if (GuiButton((Rectangle){ m.x+mw-2*bw-24, by, bw, bh }, "Delete"))
        { AudioPlayButton(); ZenDeleteAnim(zen.promptTargetIdx); zen.prompt = ZEN_PROMPT_NONE; }
        if (GuiButton((Rectangle){ m.x+mw-bw-16, by, bw, bh }, "Cancel"))
        { AudioPlayButton(); zen.prompt = ZEN_PROMPT_NONE; }
        break;
    }
    case ZEN_PROMPT_NEW_NAME:
    case ZEN_PROMPT_RENAME_ANIM:
    case ZEN_PROMPT_COPY_ANIM:
    {
        ZenPromptKind kind = (ZenPromptKind)zen.prompt;
        GuiLabel(msg, kind == ZEN_PROMPT_NEW_NAME    ? "New animation name:"
                    : kind == ZEN_PROMPT_RENAME_ANIM ? "Rename animation to:"
                                                     : "Save a copy as:");
        if (GuiTextBox(tb, zen.nameBuf, ANIM_NAME_MAX, zen.edNameBuf))
            zen.edNameBuf = !zen.edNameBuf;
        // a fresh name is required - silently overwriting a sibling file
        // through New/Rename/Copy would be data loss.
        bool valid = zen.nameBuf[0] && ZenAnimFind(zen.nameBuf) < 0;
        if (!valid) GuiDisable();
        bool go = GuiButton((Rectangle){ m.x+mw-2*bw-24, by, bw, bh },
                            kind == ZEN_PROMPT_NEW_NAME ? "Create"
                          : kind == ZEN_PROMPT_RENAME_ANIM ? "Rename" : "Save");
        if (!valid) GuiEnable();
        if (go || (valid && enter))
        {
            AudioPlayButton();
            if      (kind == ZEN_PROMPT_NEW_NAME)    ZenCreateAnim(zen.nameBuf);
            else if (kind == ZEN_PROMPT_RENAME_ANIM) ZenRenameAnim(zen.nameBuf);
            else                                     ZenCopyAnim(zen.nameBuf);
            zen.prompt = ZEN_PROMPT_NONE; zen.edNameBuf = false;
        }
        if (GuiButton((Rectangle){ m.x+mw-bw-16, by, bw, bh }, "Cancel"))
        { AudioPlayButton(); zen.prompt = ZEN_PROMPT_NONE; zen.edNameBuf = false; }
        break;
    }
    case ZEN_PROMPT_DURATION:
    {
        GuiLabel(msg, "Animation duration (seconds):");
        if (GuiTextBox(tb, zen.nameBuf, ANIM_NAME_MAX, zen.edNameBuf))
            zen.edNameBuf = !zen.edNameBuf;
        float v = strtof(zen.nameBuf, NULL);
        bool valid = v >= 0.1f;
        if (!valid) GuiDisable();
        bool go = GuiButton((Rectangle){ m.x+mw-2*bw-24, by, bw, bh }, "Apply");
        if (!valid) GuiEnable();
        if (go || (valid && enter))
        {
            AudioPlayButton();
            ZenUndoPush();
            zen.doc.duration = v;
            if (zen.playhead > v) zen.playhead = v;
            zen.prompt = ZEN_PROMPT_NONE; zen.edNameBuf = false;
        }
        if (GuiButton((Rectangle){ m.x+mw-bw-16, by, bw, bh }, "Cancel"))
        { AudioPlayButton(); zen.prompt = ZEN_PROMPT_NONE; zen.edNameBuf = false; }
        break;
    }
    case ZEN_PROMPT_LIB_SAVE_NAME:
    case ZEN_PROMPT_LIB_RENAME:
    {
        bool renaming = zen.prompt == ZEN_PROMPT_LIB_RENAME;
        GuiLabel(msg, renaming ? "Rename library entry:"
                               : "Save element to library as:");
        if (GuiTextBox(tb, zen.nameBuf, ANIM_NAME_MAX, zen.edNameBuf))
            zen.edNameBuf = !zen.edNameBuf;
        // saving may reuse a name (overwrite); a rename onto another entry's
        // name is rejected by AnimLibraryRename.
        int clash = AnimLibraryFind(&zen.library, zen.nameBuf);
        bool valid = zen.nameBuf[0]
                  && !(renaming && clash >= 0 && clash != zen.libTargetIdx);
        if (!valid) GuiDisable();
        bool go = GuiButton((Rectangle){ m.x+mw-2*bw-24, by, bw, bh },
                            renaming ? "Rename" : (clash >= 0 ? "Overwrite" : "Save"));
        if (!valid) GuiEnable();
        if (go || (valid && enter))
        {
            AudioPlayButton();
            if (renaming) AnimLibraryRename(&zen.library, zen.libTargetIdx, zen.nameBuf);
            else if (EnSel())
                AnimLibraryAdd(&zen.library, zen.nameBuf, &zen.doc.elems[zen.selElem]);
            AnimLibrarySave(&zen.library, ZEN_LIB_PATH);
            zen.edNameBuf = false; zen.libTargetIdx = -1;
            zen.prompt = ZEN_PROMPT_NONE;
            zen.libOpen = true;      // back to the shelf: result visible at once
        }
        if (GuiButton((Rectangle){ m.x+mw-bw-16, by, bw, bh }, "Cancel"))
        {
            AudioPlayButton();
            zen.edNameBuf = false; zen.libTargetIdx = -1;
            zen.prompt = ZEN_PROMPT_NONE;
            zen.libOpen = renaming;
        }
        break;
    }
    default: break;
    }
}

// Element library shelf: insert / rename / delete entries.
static void DrawLibraryModal(void)
{
    // hidden while a prompt is up (the lib prompts return here on close).
    if (!zen.libOpen || zen.prompt != ZEN_PROMPT_NONE) return;

    ScreenState *ss = ScreenStateGet();
    float W = (float)ss->width, H = (float)ss->height;
    float mw = 420, mh = 340;
    Rectangle m = { (W-mw)/2, (H-mh)/2, mw, mh };
    DrawRectangle(0, 0, (int)W, (int)H, (Color){ 0, 0, 0, 120 });
    DrawRectangleRec(m, (Color){ 40, 42, 48, 255 });
    DrawRectangleLinesEx(m, 1.0f, (Color){ 90, 94, 104, 255 });

    GuiLabel((Rectangle){ m.x+16, m.y+10, mw-32, 20 }, "ELEMENT LIBRARY");
    if (zen.library.count == 0)
        GuiLabel((Rectangle){ m.x+16, m.y+34, mw-32, 20 },
                 "(empty - use Element > Save to library)");

    Rectangle list = { m.x+12, m.y+34, mw-24, mh-34-48 };
    if (CheckCollisionPointRec(GetMousePosition(), list))
        zen.libScroll += GetMouseWheelMove() * 24.0f;

    float rh = 26.0f, gap = 4.0f;
    float contentH = zen.library.count * (rh + gap);
    float maxScroll = contentH - list.height;
    if (maxScroll < 0) maxScroll = 0;
    if (zen.libScroll < -maxScroll) zen.libScroll = -maxScroll;
    if (zen.libScroll > 0) zen.libScroll = 0;

    int reqInsert = -1, reqRename = -1, reqDelete = -1;
    BeginScissorMode((int)list.x, (int)list.y, (int)list.width, (int)list.height);
    float ly = list.y + zen.libScroll;
    for (int i = 0; i < zen.library.count; i++)
    {
        const AnimLibEntry *en = &zen.library.entries[i];
        float bw2 = 60.0f;
        Rectangle nameR = { list.x, ly, list.width - 2*bw2 - 8, rh };
        if (GuiButton(nameR, TextFormat("%s   [%s]", en->name,
                                        AnimElemKindName(en->elem.kind))))
            reqInsert = i;
        if (GuiButton((Rectangle){ list.x + list.width - 2*bw2 - 4, ly, bw2, rh }, "rename"))
            reqRename = i;
        if (GuiButton((Rectangle){ list.x + list.width - bw2, ly, bw2, rh }, "delete"))
            reqDelete = i;
        ly += rh + gap;
    }
    EndScissorMode();

    float bh = 28, by = m.y + mh - bh - 12;
    GuiLabel((Rectangle){ m.x+16, by, mw-120, bh }, "click an entry to add it here");
    if (GuiButton((Rectangle){ m.x+mw-90, by, 78, bh }, "Close"))
    { AudioPlayButton(); zen.libOpen = false; }

    if (reqInsert >= 0)
    {
        AudioPlayButton();
        ZenLibraryInsert(reqInsert);
    }
    else if (reqRename >= 0)
    {
        AudioPlayButton();
        zen.libTargetIdx = reqRename;
        TextCopy(zen.nameBuf, zen.library.entries[reqRename].name);
        zen.edNameBuf = true; zen.prompt = ZEN_PROMPT_LIB_RENAME;
    }
    else if (reqDelete >= 0)
    {
        AudioPlayButton();
        AnimLibraryRemove(&zen.library, reqDelete);
        AnimLibrarySave(&zen.library, ZEN_LIB_PATH);
    }
}

// Help modal: one scrollable page of bulk text. A dedicated guided animation
// will replace most of this later; the text is the stopgap.
static const char *k_helpLines[] = {
    "ZEN ANIMATION EDITOR",
    "",
    "MENU BAR",
    "  Every operation lives in the top menus. Press Alt to enter hotkey",
    "  navigation: each menu and item shows its letter. Alt+F+N = File > New,",
    "  Alt+E+L toggles looping. Any other key or a click leaves the mode.",
    "  Hovering an item shows what it does.",
    "",
    "VIEWPORT",
    "  While editing, the view is zoomed out and the dotted rectangle marks",
    "  the actual screen. Pressing play first zooms to full size (0.2s),",
    "  then the animation starts. Stopping zooms back out.",
    "  Click an element to select it; clicking the same spot again cycles",
    "  through whatever overlaps there. Ctrl+drag moves the selection, and",
    "  Ctrl+Shift+drag locks the move to the nearest of four axes (dotted",
    "  guides show which). Right-click opens that element's track menu.",
    "",
    "PLAYBACK",
    "  Space          play / pause",
    "  Ctrl+Space     play from start / stop",
    "  Editor > Loop  restart at the loop point when the end is reached",
    "",
    "FILES",
    "  Animations are .cfg files in the anims/ folder. File > Open switches",
    "  (it filters as you type; Enter opens the first match), File > Save As",
    "  copies, File > Rename moves the file on disk.",
    "  Unsaved changes always prompt before they could be lost.",
    "",
    "EDITING",
    "  Ctrl+S save    Ctrl+Z undo    Ctrl+Y redo",
    "  Elements are added from the Element menu or re-used from the library.",
    "  Every property row in the Inspector explains itself on hover - what it",
    "  means, what its units are and how it relates to the tracks.",
    "",
    "SLIDERS & VALUES",
    "  Drag a slider to set it. The modifiers change how far a drag moves:",
    "  Shift+drag  fine - the smallest useful change, for nudging a value",
    "  Ctrl+drag   snaps to the increment (0.01 on small ranges, else 0.1)",
    "  Shift can go on and off mid-drag: the value carries over instead of",
    "  jumping to the mouse, so you can drag roughly then hold Shift to",
    "  settle on the exact number without letting go.",
    "  Over a slider, Ctrl+wheel steps by the increment and Shift+wheel",
    "  steps by a tenth of it.",
    "  Double-click the value on the right to type an exact number - Enter",
    "  commits, Esc cancels. Each drag is one undo step, however long it is.",
    "",
    "TEXT ELEMENTS",
    "  The Inspector's text box holds multiple lines. It grows as you type,",
    "  up to 8 rows, then scrolls - you are not meant to read it all there,",
    "  the canvas shows the text.",
    "  Shift+Enter  new line          Enter  commit and close",
    "  Ctrl+A/C/X/V select all, copy, cut, paste (the system clipboard, so",
    "  you can paste a block in from anywhere or take the text out to read)",
    "  Ctrl+Left/Right jump a word; hold Shift with any arrow to select.",
    "  Esc commits too, and closes only the text box - not the editor.",
    "",
    "AUTO-KEY",
    "  With auto-key ON, changing any property in the Inspector writes a key",
    "  at the playhead and opens it in the track modal, so you always see what",
    "  was written. If the property had no track yet, editing it starts one:",
    "  a key holds the original pose at 0s and your edit lands at the",
    "  playhead, so the change animates instead of just moving the element.",
    "  With auto-key OFF, tracked properties are locked and edits only touch",
    "  the untracked base pose.",
    "",
    "TEXT AND THE STRING POOL",
    "  A text element can show either its OWN words, typed straight into the",
    "  Inspector, or a string from the document's shared STRING POOL. Anything",
    "  you type is added to the pool automatically (identical text is reused,",
    "  never duplicated), so strings are always available to share.",
    "  Editor > String pool... opens it, and so does DOUBLE-CLICKING the small",
    "  'text' label to the left of the Inspector's text box. When an element",
    "  is showing a pooled string the label turns blue and shows the entry",
    "  number; editing the box then edits THAT POOL ENTRY.",
    "  The pool modal has two tabs. STRINGS lists every pool entry with how",
    "  many keys use it, and below them, under IN ELEMENTS ONLY, the words of",
    "  any text element that has not been pooled yet - press 'pool it' on such",
    "  a row to make them a shared entry. Clicking a row only SELECTS it;",
    "  DOUBLE-CLICKING opens it for editing (this works on element rows too,",
    "  where it edits that element's own words rather than the pool).",
    "  ASSIGNING takes two clicks on purpose. Press 'assign' on a string: it",
    "  is armed, the modal jumps to the ELEMENTS tab and a banner says which",
    "  string is about to be set. Then press 'set' on the element you want it",
    "  on. Nothing is written until that second press, so browsing the element",
    "  list is safe; 'Cancel set' disarms it. The words are set AT THE PLAYHEAD,",
    "  so park it where you want them to change (at 0 for the whole animation).",
    "  New, Edit and Delete sit at the bottom. An entry still in use cannot be",
    "  deleted - repoint the keys first.",
    "  Sharing is the point: two elements on the same entry both follow an",
    "  edit to it. Empty strings nobody uses are cleaned up on their own.",
    "",
    "  ANIMATING TEXT. Give a text element a 'string' track and its keys hold",
    "  POOL ENTRY NUMBERS, so the words CHANGE during the animation.",
    "  The whole workflow lives in the track modal. Add the 'string' track and",
    "  its first key carries the words the element ALREADY shows - a key never",
    "  starts out pointing at nothing. Scrub to where the text should change,",
    "  press +key, then CLICK THE WORDS on the modal's 'str' row: a list of",
    "  every pool entry drops down and picking one changes THAT KEY ONLY, at",
    "  that time. '+ new string...' at the bottom opens the pool to write words",
    "  that do not exist yet, then come back and pick them.",
    "  A string track SNAPS: the entry at a key holds until the next key, with",
    "  no in-between (there is no half-way point between two strings), so the",
    "  'ease' row reads 'steps - no easing' and cannot be opened. Do any fading",
    "  or movement with the other properties.",
    "  Text is pooled rather than stored per key because a key is 16 bytes and",
    "  there are thousands of them - a string on every key would cost the",
    "  build about 28 MB instead of a fraction of one.",
    "",
    "CLONING ELEMENTS",
    "  An animation has a FIXED budget: 12 elements and 4 signals. Both are",
    "  shown live in the ELEMENTS and SIGNALS panel titles (5/12, and FULL when",
    "  there is no room left), so you can see what is left before you run out.",
    "  When a text or shape block has finished its part, do NOT spend another",
    "  element on something similar - bring the expired one back instead.",
    "  Element > Clone from... does exactly that. The SELECTED element is the",
    "  one that receives; you pick another element from the list, and it is",
    "  read as it looks AT THE PLAYHEAD (so scrub to the moment you want",
    "  first). Type the time the result should appear at and press Clone.",
    "  Only properties that actually DIFFER become keys, so a clone costs the",
    "  minimum of the 16-keys-per-track budget - re-cloning the same look",
    "  writes nothing at all.",
    "  The WORDS are cloned as a key too, not as a replacement: the element",
    "  keeps saying its old text up to the clone time and the new text from",
    "  there on.",
    "  THE OLD PROPERTY IS ALWAYS KEPT. When a cloned property had no track",
    "  yet, a key holding what the element already showed is written at 0s.",
    "  Without it that single new key would override the element's value",
    "  across the WHOLE timeline instead of changing it at the chosen time.",
    "  The \"hold the old pose\" checkbox goes one step further: it also writes",
    "  that old pose just BEFORE the clone, so the element snaps to the new",
    "  look rather than sliding into it from whatever key came before. Turn",
    "  it off when you want a smooth transition into the cloned look.",
    "  THE ELEMENT ITSELF is in the list. Cloning from itself copies its own",
    "  look at the playhead onto another time, which is how a pose is brought",
    "  back later without spending an element or retyping anything. The two",
    "  times must differ.",
    "  One checkbox copies a thing that has no track: a shape's kind. Because",
    "  it is untracked it changes the element for the WHOLE timeline, not",
    "  just at the chosen time.",
    "  Only elements of the same kind can be cloned; the rest are greyed out.",
    "  Ctrl+Z undoes the whole clone as one step.",
    "",
    "PAUSE MARKERS",
    "  A green dashed line on the timeline is a PAUSE MARKER: playback holds",
    "  there and the pose freezes until the viewer presses any key. It is",
    "  shaped like the playhead on purpose - it is the second cursor, the one",
    "  playback parks on.",
    "  Add one by right-clicking the timeline and choosing Insert pause marker,",
    "  or with Editor > Add pause marker, which drops one at the playhead and",
    "  greys out when this position already has a marker. The count sits in the",
    "  bottom-left of the timeline (12 markers max).",
    "  Drag a marker by the tab at its top to retime it; hold Ctrl to snap to",
    "  the 0.1s grid. Right-click a marker to delete it.",
    "  A hold is LOCAL to its own animation: anything else playing on screen",
    "  keeps running, and a signal already in flight on the same animation",
    "  keeps running too and can still end it. A held animation is still",
    "  alive - it is waiting, not finished.",
    "",
    "TIMELINE",
    "  Click a key to select it and open it in the track modal.",
    "  Shift+click ONLY adds or removes a key from the selection - it never",
    "  moves the key. Dragging a key (no Shift) moves it in time; hold Ctrl",
    "  while dragging to snap to the 0.1s grid.",
    "  Dragging anywhere else on the bar scrubs the playhead, and scrubbing",
    "  follows the row it passes through: the track modal switches to that",
    "  property's track, showing the key under the playhead when there is one",
    "  (or all of them when several sit together) and the whole track when",
    "  there is not.",
    "  Hold Shift while scrubbing to sweep-select: every key the playhead",
    "  crosses joins the selection, which beats Shift+clicking them one by",
    "  one. Release and the track modal has the whole run ready to bulk edit.",
    "  The triangles at the top-left and bottom-right trim the intro and",
    "  outro sections.",
    "  Right-click anywhere on the bar to add or remove a pause marker, and",
    "  see the marker count in the bottom-left (see PAUSE MARKERS).",
    "  ZOOM: on a long animation a second is only a few pixels wide, so keys",
    "  are hard to place. Ctrl+Wheel over the bar zooms about the cursor, and",
    "  Ctrl+Up / Ctrl+Down zoom about the playhead. Gridlines subdivide as you",
    "  go in, down to 0.1s. Plain wheel over the bar pans, and dragging a key",
    "  or marker to either edge pans the view after it. The zoom factor shows",
    "  in the bottom-right whenever you are zoomed in.",
    "  Zoom survives playback and switching elements; leaving the editor",
    "  resets it. View > Follow playhead decides whether the window chases the",
    "  playhead when it runs off the edge.",
    "",
    "TRACK MODAL",
    "  Drag its title bar to move it; the editor stays usable underneath.",
    "  The tree at the top says what an edit will hit: a badge reads KEY,",
    "  BULK or TRACK, and under it sits one row per key in scope. Click a row",
    "  to edit that key alone, Ctrl+click a row to add/remove it from the",
    "  selection, and click the top row to go back to the whole selection.",
    "  One key = edits land immediately. Several keys or a whole track =",
    "  edits are staged (an amber dot marks each changed field) and Apply",
    "  writes only those fields to every key in scope, as one undo step.",
    "  The reset button next to it drops every staged edit at once.",
    "  +key makes a NEW key at the playhead - Apply only ever rewrites keys",
    "  that already exist, so this is how you extend a track. It greys out",
    "  when a key is already sitting at the playhead. With one key selected",
    "  the new key copies its values; otherwise it takes the pose the",
    "  element actually has at that moment.",
    "  With exactly ONE key selected, Apply's slot becomes \"duplicate key\":",
    "  it copies that key's values to a new key at the playhead. Apply is not",
    "  offered there because a single key is edited directly anyway.",
    "  A string row shows the WORDS the key switches to rather than a slider -",
    "  a string key holds a pool index, which is not a quantity you can drag",
    "  through. Click it to pick another entry for that one key; it is greyed",
    "  out when there is no key at this time (see TEXT AND THE STRING POOL).",
    "",
    "SIGNALS",
    "  The play glyph on a signal row fires it. A fired signal plays on its",
    "  own clock, so the timeline freezes and shows the signal's track -",
    "  its keys sit at u 0..1 over the signal's length, with a marker riding",
    "  the clock. Editor > Stop on signal decides what happens afterwards:",
    "  on, playback stays stopped; off, the animation resumes where it froze.",
    "  The panel title counts the signals in use (4 max). A signal is a",
    "  reaction to an event; to hold playback at a fixed MOMENT use a pause",
    "  marker instead - it costs no signal slot (see PAUSE MARKERS).",
    "",
    "EASINGS",
    "  Easing > Browse lists every curve with a thumbnail. Hide keeps a curve",
    "  working but drops it from the pickers; Delete frees the name, and any",
    "  key still referencing it falls back to linear - .cfg files store",
    "  easings by NAME, so prefer Hide.",
    "  New... opens the curve editor: drag the knots and their handles,",
    "  double-click the curve to add a knot, right-click one to remove it.",
    "  Alt+drag breaks a knot's tangent, Ctrl snaps to a 0.05 grid. More",
    "  knots is how bounce, elastic and staircase shapes are built; two knots",
    "  at the same x hold the value, which makes a step.",
    "  Save as new keeps the old curve intact; Overwrite changes the curve",
    "  everywhere its name is already used.",
};

static float s_helpScroll = 0.0f;

static void DrawHelpModal(void)
{
    if (!zen.helpOpen) return;

    ScreenState *ss = ScreenStateGet();
    float W = (float)ss->width, H = (float)ss->height;
    float mw = 560, mh = H * 0.8f;
    Rectangle m = { (W-mw)/2, (H-mh)/2, mw, mh };
    DrawRectangle(0, 0, (int)W, (int)H, (Color){ 0, 0, 0, 140 });
    DrawRectangleRec(m, (Color){ 40, 42, 48, 255 });
    DrawRectangleLinesEx(m, 1.0f, (Color){ 90, 94, 104, 255 });

    Rectangle list = { m.x+16, m.y+12, mw-32, mh-12-48 };
    int lines = (int)(sizeof(k_helpLines)/sizeof(k_helpLines[0]));
    float lh = 18.0f;
    float contentH = lines * lh;
    if (CheckCollisionPointRec(GetMousePosition(), list))
        s_helpScroll += GetMouseWheelMove() * 24.0f;
    float maxScroll = contentH - list.height;
    if (maxScroll < 0) maxScroll = 0;
    if (s_helpScroll < -maxScroll) s_helpScroll = -maxScroll;
    if (s_helpScroll > 0) s_helpScroll = 0;

    BeginScissorMode((int)list.x, (int)list.y, (int)list.width, (int)list.height);
    float y = list.y + s_helpScroll;
    for (int i = 0; i < lines; i++, y += lh)
        GuiLabel((Rectangle){ list.x, y, list.width, lh }, k_helpLines[i]);
    EndScissorMode();

    if (GuiButton((Rectangle){ m.x+mw-90, m.y+mh-40, 78, 28 }, "Close"))
    { AudioPlayButton(); zen.helpOpen = false; }
}

void ZenMenuOverlaysGui(void)
{
    DrawMenuDropdown();
    DrawOpenList();
    DrawLibraryModal();
    DrawHelpModal();
    DrawPromptModal();      // topmost: prompts stack over the library shelf
}
