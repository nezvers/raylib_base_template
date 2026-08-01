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
#define ZEN_MENU_ITEMS_MAX 14

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
static void ActFileOpen(void)    { zen.openListOpen = true; }
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
static bool EnSavedAnim(void)    { return zen.animCurrent >= 0; }

// -- Editor -----------------------------------------------------------------
static void ActEdLoop(void)      { zen.loopPlay = !zen.loopPlay; }
static bool ChkEdLoop(void)      { return zen.loopPlay; }
static void ActEdAutoKey(void)   { zen.autoKey = !zen.autoKey; }
static bool ChkEdAutoKey(void)   { return zen.autoKey; }
static void ActEdSmooth(void)    { ZenUndoPush(); zen.doc.loopSmooth = !zen.doc.loopSmooth; }
static bool ChkEdSmooth(void)    { return zen.doc.loopSmooth; }
static void ActEdDuration(void)
{
    TextCopy(zen.nameBuf, TextFormat("%.2f", zen.doc.duration));
    zen.edNameBuf = true;
    zen.prompt = ZEN_PROMPT_DURATION;
}
static void ActEdAlpha(void)     { zen.panelAlphaMode = (zen.panelAlphaMode + 1) % 3; }

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

// -- Easing -----------------------------------------------------------------
static void ActEaBrowse(void)    { ZenEasingBrowserOpen(); }

// -- Help -------------------------------------------------------------------
static void ActHelp(void)        { zen.helpOpen = true; }

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
    AddItem(m, "Exit editor",'X', ActFileExit,   NULL, NULL, "Back to the main menu (asks when unsaved)");

    m = &s_menus[s_menuCount++]; *m = (ZenMenu){ "Editor", 'E' };
    AddItem(m, "Loop playback",     'L', ActEdLoop,    NULL, ChkEdLoop,   "Restart playback at the loop point when the end is reached");
    AddItem(m, "Auto-key",          'A', ActEdAutoKey, NULL, ChkEdAutoKey,"Editing a tracked property writes a key at the playhead");
    AddItem(m, "Smooth loop",       'S', ActEdSmooth,  NULL, ChkEdSmooth, "Blend the loop seam so the restart doesn't pop");
    AddItem(m, "Duration...",       'D', ActEdDuration,NULL, NULL,        "Type an exact animation length in seconds");
    AddItem(m, "Panel transparency",'T', ActEdAlpha,   NULL, NULL,        "Cycle panel background opacity: opaque / semi / faint");

    m = &s_menus[s_menuCount++]; *m = (ZenMenu){ "Element", 'L' };
    AddItem(m, "New text",       'T', ActElNewText,   EnRoom,    NULL, "Add a text element");
    AddItem(m, "New shape",      'S', ActElNewShape,  EnRoom,    NULL, "Add a shape element (rect/circle/line/...)");
    AddItem(m, "New global",     'G', ActElNewGlobal, EnRoom,    NULL, "Add a global element (screen fade / background)");
    AddItem(m, "-",              0,   NULL,           NULL,      NULL, NULL);
    AddItem(m, "Duplicate",      'C', ActElDup,       EnSelRoom, NULL, "Clone the selected element with all its tracks");
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

    m = &s_menus[s_menuCount++]; *m = (ZenMenu){ "Easing", 'G' };
    AddItem(m, "Browse easings...", 'B', ActEaBrowse, NULL, NULL, "View, hide and create easing curves");

    m = &s_menus[s_menuCount++]; *m = (ZenMenu){ "Help", 'H' };
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
        y += ZEN_MENU_ITEM_H;
    }

    if (fired) FireItem(fired);
    else if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
             !CheckCollisionPointRec(GetMousePosition(), bg) &&
             !CheckCollisionPointRec(GetMousePosition(), hdr))
        CloseMenus();                                // click-away closes
}

// File > Open: a centered list of every saved animation.
static void DrawOpenList(void)
{
    if (!zen.openListOpen) return;

    ScreenState *ss = ScreenStateGet();
    float W = (float)ss->width, H = (float)ss->height;
    float mw = 320, mh = 72.0f + zen.animCount * ZEN_MENU_ITEM_H;
    if (mh > H - 80) mh = H - 80;
    Rectangle m = { (W-mw)/2, (H-mh)/2, mw, mh };
    DrawRectangle(0, 0, (int)W, (int)H, (Color){ 0, 0, 0, 120 });
    DrawRectangleRec(m, (Color){ 40, 42, 48, 255 });
    DrawRectangleLinesEx(m, 1.0f, (Color){ 90, 94, 104, 255 });
    GuiLabel((Rectangle){ m.x+16, m.y+10, mw-32, 20 }, "OPEN ANIMATION");

    int req = -1;
    float y = m.y + 36;
    for (int i = 0; i < zen.animCount && y + ZEN_MENU_ITEM_H < m.y + mh - 36; i++)
    {
        Rectangle r = { m.x + 12, y, mw - 24, ZEN_MENU_ITEM_H };
        if (GuiButton(r, zen.animList[i])) req = i;
        if (i == zen.animCurrent) DrawRectangleRec(r, (Color){ 90, 140, 220, 60 });
        y += ZEN_MENU_ITEM_H;
    }
    if (GuiButton((Rectangle){ m.x + mw - 90, m.y + mh - 34, 78, 26 }, "Close"))
    { AudioPlayButton(); zen.openListOpen = false; }

    if (req >= 0) { AudioPlayButton(); ZenRequestSwitch(req); }
}

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
    {
        bool exiting = zen.prompt == ZEN_PROMPT_SAVE_THEN_EXIT;
        GuiLabel(msg, TextFormat("Save changes to \"%s\"?",
                                 zen.animCurrent >= 0 ? zen.animList[zen.animCurrent]
                                                      : "*unsaved"));
        if (GuiButton((Rectangle){ m.x+16, by, bw, bh }, "Save"))
        {   // saving rescans (list may reorder): re-find the target by name.
            AudioPlayButton();
            char target[ANIM_NAME_MAX] = {0};
            if (!exiting) TextCopy(target, zen.animList[zen.promptTargetIdx]);
            ZenSaveCurrent();
            zen.prompt = ZEN_PROMPT_NONE;
            if (exiting) ZenExitEditor();
            else ZenLoadAnimByIndex(ZenAnimFind(target));
        }
        if (GuiButton((Rectangle){ m.x+(mw-bw)/2, by, bw, bh }, "Discard"))
        {
            AudioPlayButton();
            zen.docDirty = false;
            int t = zen.promptTargetIdx;
            zen.prompt = ZEN_PROMPT_NONE;
            if (exiting) ZenExitEditor();
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
    "",
    "VIEWPORT",
    "  While editing, the view is zoomed out and the dotted rectangle marks",
    "  the actual screen. Pressing play first zooms to full size (0.2s),",
    "  then the animation starts. Stopping zooms back out.",
    "",
    "PLAYBACK",
    "  Space          play / pause",
    "  Ctrl+Space     play from start / stop",
    "  Editor > Loop  restart at the loop point when the end is reached",
    "",
    "FILES",
    "  Animations are .cfg files in the anims/ folder. File > Open switches,",
    "  File > Save As copies, File > Rename moves the file on disk.",
    "  Unsaved changes always prompt before they could be lost.",
    "",
    "EDITING",
    "  Ctrl+S save    Ctrl+Z undo    Ctrl+Y redo",
    "  Elements are added from the Element menu or re-used from the library.",
    "  With auto-key on, editing a tracked property writes a key at the",
    "  playhead.",
    "",
    "  (Panels, the track modal, viewport picking and custom easings arrive",
    "   in the next phases of the rework.)",
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
