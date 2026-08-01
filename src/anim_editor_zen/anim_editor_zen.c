// ============================================================================
//  anim_editor_zen.c  -  state glue + document lifecycle of the Zen editor
//
//  Owns the ZenCtx singleton, the undo ring, the .cfg CRUD and the AppState
//  callbacks. The viewport lives in zen_view.c; menu bar / panels / modals
//  arrive in their own files as the rework progresses.
//
//  Invariants inherited from the classic editor (keep them):
//    - ZenUndoPush() is the ONLY writer of zen.docDirty.
//    - AnimSignalUnregister/Register wrap EVERY replacement of zen.doc
//      (load, undo, redo): bindings capture by value.
//  raygui's implementation is compiled once in main_menu.c; we only include it.
// ============================================================================

#include "raylib.h"
#include "raygui.h"
#include "anim_editor_zen.h"
#include "zen_internal.h"
#include "../app_state/app_state.h"
#include "../screen_state/screen_state.h"
#include "../anim/anim.h"
#include "../anim/anim_io.h"
#include "../anim/anim_library.h"
#include "../anim/signal.h"
#include "../anim/anim_signal.h"
#include <math.h>
#include <stdio.h>          // remove() for deleting anim files off disk

// Forward declares (house pattern).
static void Enter();
static void Exit();
static void Update();
static void Draw();
static void Gui();

                            /* Enter, Exit, Update, Draw, Gui, "Name" */
AppState app_state_anim_editor_zen = {Enter, Exit, Update, Draw, Gui, "AnimEditorZen"};

// The one module-wide context (see zen_internal.h for the rationale).
ZenCtx zen;

// ---------------------------------------------------------------------------
//  Session memory: survives leaving/re-entering the editor, not a restart.
//  Deliberately statics-not-files, same rationale as the classic editor.
// ---------------------------------------------------------------------------
static char lastOpened[ANIM_NAME_MAX] = {0};

// Per-animation element selection, keyed by anim NAME (rescan shifts indices)
// -> element INDEX (element names are not unique).
static char lastSelAnim[ZEN_ANIM_LIST_MAX][ANIM_NAME_MAX];
static int  lastSelElem[ZEN_ANIM_LIST_MAX];
static int  lastSelCount = 0;

static void RememberSelElem(const char *anim, int elem)
{
    if (!anim || !anim[0]) return;
    for (int i = 0; i < lastSelCount; i++)
        if (TextIsEqual(lastSelAnim[i], anim)) { lastSelElem[i] = elem; return; }
    if (lastSelCount >= ZEN_ANIM_LIST_MAX) return;
    TextCopy(lastSelAnim[lastSelCount], anim);
    lastSelElem[lastSelCount++] = elem;
}

static int RecallSelElem(const char *anim)
{
    if (!anim || !anim[0]) return -1;
    for (int i = 0; i < lastSelCount; i++)
        if (TextIsEqual(lastSelAnim[i], anim)) return lastSelElem[i];
    return -1;
}

// Drop the remembered selection for `anim` (its file is gone), so a later
// animation reusing the name doesn't inherit a stale index.
static void ForgetSelElem(const char *anim)
{
    for (int i = 0; i < lastSelCount; i++)
        if (TextIsEqual(lastSelAnim[i], anim))
        {
            lastSelCount--;
            TextCopy(lastSelAnim[i], lastSelAnim[lastSelCount]);
            lastSelElem[i] = lastSelElem[lastSelCount];
            return;
        }
}

// ---------------------------------------------------------------------------
//  Undo: fixed ring of whole-document snapshots.
// ---------------------------------------------------------------------------
void ZenUndoPush(void)
{
    zen.undoBuf[zen.undoHead] = zen.doc;        // value copy of the whole doc
    zen.undoHead = (zen.undoHead + 1) % ZEN_UNDO_MAX;
    if (zen.undoCount < ZEN_UNDO_MAX - 1) zen.undoCount++;
    zen.redoCount = 0;                          // a new edit invalidates redo
    zen.docDirty = true;                        // every mutating edit routes here
}

// Selections must survive undo/redo/loads: revalidate against the new doc.
static void ClampSelection(void)
{
    if (zen.selElem >= zen.doc.elemCount) zen.selElem = zen.doc.elemCount - 1;
    for (int i = 0; i < ANIM_ELEMS_MAX; i++)
        if (i >= zen.doc.elemCount) zen.multiSel[i] = false;

    ZenSelValidate();                       // track/key selection + modal

    // an undo/redo or doc switch can shrink signalCount under the open modal
    if (zen.sigModalIdx >= zen.doc.signalCount)
    {
        zen.sigModalIdx = -1; zen.edSigIdx = -1;
        ZenSigCloseDrops(); ZenSigClearKeySel();
    }
    if (zen.sigSelElem >= zen.doc.elemCount) ZenSigClearKeySel();
}

void ZenUndoApply(int delta)   // delta -1 = undo, +1 = redo
{
    if (delta < 0 ? zen.undoCount == 0 : zen.redoCount == 0) return;
    AnimSignalUnregister(&zen.doc, &zen.preview);   // bindings match the OLD doc
    if (delta < 0)
    {
        zen.undoHead = (zen.undoHead - 1 + ZEN_UNDO_MAX) % ZEN_UNDO_MAX;
        AnimDoc prev = zen.undoBuf[zen.undoHead];
        zen.undoBuf[zen.undoHead] = zen.doc;        // store current for redo
        zen.doc = prev;
        zen.undoCount--; zen.redoCount++;
    }
    else
    {
        AnimDoc next = zen.undoBuf[zen.undoHead];
        zen.undoBuf[zen.undoHead] = zen.doc;
        zen.doc = next;
        zen.undoHead = (zen.undoHead + 1) % ZEN_UNDO_MAX;
        zen.undoCount++; zen.redoCount--;
    }
    AnimSignalRegister(&zen.doc, &zen.preview, &zen.playhead);
    ClampSelection();
}

// ---------------------------------------------------------------------------
//  A tiny starter document so the editor opens on something visible.
// ---------------------------------------------------------------------------
static void MakeStarterDoc(void)
{
    AnimDocInit(&zen.doc);
    TextCopy(zen.doc.name, "new_anim");
    zen.doc.duration = 2.0f;

    AnimElem *t = AnimDocAddElem(&zen.doc, AE_TEXT);
    TextCopy(t->name, "title");
    TextCopy(t->text, "HELLO");
    t->posFrac  = (Vector2){ 0.5f, 0.10f };
    t->sizeFrac = (Vector2){ 0.12f, 0.12f };
    AnimTrack *a = AnimElemAddTrack(t, AP_T_ALPHA);
    AnimTrackAddKey(a, 0.0f, 0.0f, ANIM_EASE_LINEAR);
    AnimTrackAddKey(a, 0.6f, 1.0f, ANIM_EASE_SINE_OUT);

    TextCopy(zen.doc.signals[0].name, "enter");
    zen.doc.signals[0].length      = 1.0f;
    zen.doc.signals[0].targetCount = 0;
    zen.doc.signalCount = 1;

    zen.selElem = 0;
}

// ---------------------------------------------------------------------------
//  Animation files: one .cfg per animation under ZEN_ANIM_DIR.
// ---------------------------------------------------------------------------
const char *ZenAnimPath(const char *name)
{
    return TextFormat("%s/%s%s", ZEN_ANIM_DIR, name, ZEN_ANIM_EXT);
}

int ZenAnimFind(const char *name)
{
    for (int i = 0; i < zen.animCount; i++)
        if (TextIsEqual(zen.animList[i], name)) return i;
    return -1;
}

void ZenRescanAnims(void)
{
    if (!DirectoryExists(ZEN_ANIM_DIR)) MakeDirectory(ZEN_ANIM_DIR);

    zen.animCount = 0;
    FilePathList fl = LoadDirectoryFilesEx(ZEN_ANIM_DIR, ZEN_ANIM_EXT, false);
    for (unsigned int i = 0; i < fl.count && zen.animCount < ZEN_ANIM_LIST_MAX; i++)
    {
        // '_'-prefixed files are editor data (library, easings), not anims.
        const char *base = GetFileNameWithoutExt(fl.paths[i]);
        if (base[0] == '_') continue;
        TextCopy(zen.animList[zen.animCount++], base);
    }
    UnloadDirectoryFiles(fl);
}

void ZenLoadAnimByIndex(int idx)
{
    if (idx < 0 || idx >= zen.animCount) return;
    ZenUndoPush();                              // switching is undoable
    if (zen.animCurrent >= 0)
        RememberSelElem(zen.animList[zen.animCurrent], zen.selElem);
    AnimSignalUnregister(&zen.doc, &zen.preview);
    if (!AnimDocLoad(&zen.doc, ZenAnimPath(zen.animList[idx]))) MakeStarterDoc();
    AnimSignalRegister(&zen.doc, &zen.preview, &zen.playhead);
    zen.animCurrent = idx;
    zen.docDirty = false;                       // freshly loaded == clean
    int wantSel = RecallSelElem(zen.animList[idx]);
    zen.selElem = (wantSel >= 0 && wantSel < zen.doc.elemCount) ? wantSel
                : (zen.doc.elemCount > 0 ? 0 : -1);
    zen.preview = (AnimSignalPlayer){0};        // bindings pointed at the OLD doc
    // the new doc's tracks/signals are unrelated to the old one's: close the
    // modals and reset the panel scrolls rather than pointing at stale data.
    ZenSelClear();
    zen.sigModalIdx = -1; zen.edSigIdx = -1;
    ZenSigCloseDrops(); ZenSigClearKeySel();
    ZenViewCtxClose();
    zen.elemView.scroll = zen.sigView.scroll = zen.inspView.scroll = 0.0f;
    ClampSelection();
    zen.playhead = 0.0f;
    zen.playing = false; zen.playPending = false;
}

void ZenSaveCurrent(void)
{
    const char *nm = (zen.animCurrent >= 0) ? zen.animList[zen.animCurrent]
                   : (zen.doc.name[0] ? zen.doc.name : "untitled");
    AnimDocSave(&zen.doc, ZenAnimPath(nm));
    ZenRescanAnims();
    zen.animCurrent = ZenAnimFind(nm);
    zen.docDirty = false;
}

// Open guarded by the dirty prompt so edits can't be dropped by accident.
void ZenRequestSwitch(int idx)
{
    zen.openListOpen = false;
    if (idx == zen.animCurrent) return;
    if (zen.docDirty)
    {
        zen.prompt = ZEN_PROMPT_SAVE_THEN_SWITCH;
        zen.promptTargetIdx = idx;
    }
    else ZenLoadAnimByIndex(idx);
}

// Create a fresh named animation, save it, make it current.
void ZenCreateAnim(const char *name)
{
    ZenUndoPush();
    AnimSignalUnregister(&zen.doc, &zen.preview);
    MakeStarterDoc();
    TextCopy(zen.doc.name, name);
    AnimSignalRegister(&zen.doc, &zen.preview, &zen.playhead);
    AnimDocSave(&zen.doc, ZenAnimPath(name));
    ZenRescanAnims();
    zen.animCurrent = ZenAnimFind(name);
    zen.docDirty = false;
    ClampSelection();
    zen.playhead = 0.0f;
    zen.playing = false; zen.playPending = false;
}

// Delete animList[idx] from disk; if it was current fall back to another
// animation (or a fresh starter doc).
void ZenDeleteAnim(int idx)
{
    if (idx < 0 || idx >= zen.animCount) return;
    bool wasCurrent = (idx == zen.animCurrent);
    char curName[ANIM_NAME_MAX] = {0};
    if (zen.animCurrent >= 0) TextCopy(curName, zen.animList[zen.animCurrent]);

    ForgetSelElem(zen.animList[idx]);   // while animList[idx] still names it
    remove(ZenAnimPath(zen.animList[idx]));
    ZenRescanAnims();

    if (wasCurrent)
    {
        zen.animCurrent = -1;
        if (zen.animCount > 0) ZenLoadAnimByIndex(0);
        else
        {
            ZenUndoPush();
            AnimSignalUnregister(&zen.doc, &zen.preview);
            MakeStarterDoc();
            AnimSignalRegister(&zen.doc, &zen.preview, &zen.playhead);
            zen.docDirty = false;
            ClampSelection();
        }
    }
    else zen.animCurrent = ZenAnimFind(curName);   // follow current by name
}

// Rename the CURRENT animation: save under the new name, drop the old file.
void ZenRenameAnim(const char *newName)
{
    if (!newName || !newName[0]) return;
    char oldName[ANIM_NAME_MAX] = {0};
    if (zen.animCurrent >= 0) TextCopy(oldName, zen.animList[zen.animCurrent]);

    TextCopy(zen.doc.name, newName);
    AnimDocSave(&zen.doc, ZenAnimPath(newName));
    if (oldName[0] && !TextIsEqual(oldName, newName))
    {
        ForgetSelElem(oldName);
        remove(ZenAnimPath(oldName));
    }
    ZenRescanAnims();
    zen.animCurrent = ZenAnimFind(newName);
    zen.docDirty = false;
}

// Save As: write a copy under the new name and continue editing the COPY.
void ZenCopyAnim(const char *newName)
{
    if (!newName || !newName[0]) return;
    TextCopy(zen.doc.name, newName);
    AnimDocSave(&zen.doc, ZenAnimPath(newName));
    ZenRescanAnims();
    zen.animCurrent = ZenAnimFind(newName);
    zen.docDirty = false;
}

// Leave to the main menu, guarded by the dirty prompt.
void ZenExitEditor(void)
{
    if (zen.docDirty) zen.prompt = ZEN_PROMPT_SAVE_THEN_EXIT;
    else AppStateTransition(&app_state_main_menu);
}

bool ZenTyping(void)
{
    return zen.edNameBuf || zen.edName || zen.edText || zen.edSigIdx >= 0
        || zen.trackModal.edTime || ZenSliderTyping() || ZenEasingTyping()
        || ZenMenuTyping();
}

// ---------------------------------------------------------------------------
//  Element operations on the current selection. The menu bar and (later) the
//  ELEMENTS panel both route here, so behavior can't drift between the two.
// ---------------------------------------------------------------------------
bool ZenIsSelected(int i)
{
    return i == zen.selElem || (i >= 0 && i < ANIM_ELEMS_MAX && zen.multiSel[i]);
}

void ZenElemAdd(int kind)
{
    if (zen.doc.elemCount >= ANIM_ELEMS_MAX) return;
    ZenUndoPush();
    AnimElem *e = AnimDocAddElem(&zen.doc, kind);
    if (e) zen.selElem = zen.doc.elemCount - 1;
}

void ZenElemDuplicate(void)
{
    if (zen.selElem < 0 || zen.doc.elemCount >= ANIM_ELEMS_MAX) return;
    ZenUndoPush();
    if (AnimDocDuplicateElem(&zen.doc, zen.selElem))
    {
        // the copy lands at selElem+1: trailing multi-select rows shift down.
        for (int i = ANIM_ELEMS_MAX - 1; i > zen.selElem + 1; i--)
            zen.multiSel[i] = zen.multiSel[i-1];
        zen.multiSel[zen.selElem + 1] = false;
        zen.selElem = zen.selElem + 1;              // select the new copy
        ClampSelection();
    }
}

void ZenElemDelete(void)
{
    if (zen.selElem < 0) return;
    ZenUndoPush();
    // remove ALL selected, high to low so indices stay valid while shifting.
    for (int i = zen.doc.elemCount - 1; i >= 0; i--)
        if (ZenIsSelected(i)) AnimDocRemoveElem(&zen.doc, i);
    zen.selElem = zen.doc.elemCount > 0 ? 0 : -1;
    for (int i = 0; i < ANIM_ELEMS_MAX; i++) zen.multiSel[i] = false;
    ClampSelection();
}

void ZenElemMove(int delta)
{
    int from = zen.selElem, to = from + delta;
    if (from < 0 || to < 0 || to >= zen.doc.elemCount) return;
    ZenUndoPush();
    AnimDocMoveElem(&zen.doc, from, delta);
    // follow the moved element with the selection; swap companion rows too.
    bool a = zen.multiSel[from], b = zen.multiSel[to];
    zen.multiSel[from] = b; zen.multiSel[to] = a;
    zen.selElem = to;
    ClampSelection();
}

void ZenLibraryInsert(int entry)
{
    if (entry < 0 || entry >= zen.library.count) return;
    if (zen.doc.elemCount >= ANIM_ELEMS_MAX) return;
    ZenUndoPush();
    // add a slot of the right kind, then overwrite it with the shelved
    // element (a plain value, so tracks and keys come along).
    AnimElem *dst = AnimDocAddElem(&zen.doc, zen.library.entries[entry].elem.kind);
    if (dst)
    {
        *dst = zen.library.entries[entry].elem;
        AnimDocUniquifyElemName(&zen.doc, zen.doc.elemCount - 1);
        zen.selElem = zen.doc.elemCount - 1;
        for (int k = 0; k < ANIM_ELEMS_MAX; k++) zen.multiSel[k] = false;
        ClampSelection();
    }
}

// ===========================================================================
//  State lifecycle
// ===========================================================================
static void Enter()
{
    ZenRescanAnims();
    AnimLibraryLoad(&zen.library, ZEN_LIB_PATH);
    AnimCustomEasesLoad(ZenAnimPath("_easings"));   // before any doc load

    // Reopen the animation last left (this run only), else the first saved
    // one, else start on a demo. A remembered anim deleted since just misses.
    zen.animCurrent = -1;
    int openIdx = lastOpened[0] ? ZenAnimFind(lastOpened) : -1;
    if (openIdx < 0) openIdx = 0;
    if (zen.animCount > 0 &&
        AnimDocLoad(&zen.doc, ZenAnimPath(zen.animList[openIdx])) &&
        zen.doc.elemCount > 0)
        zen.animCurrent = openIdx;
    else
        MakeStarterDoc();
    zen.docDirty = false;

    int wantSel = (zen.animCurrent >= 0)
                ? RecallSelElem(zen.animList[zen.animCurrent]) : -1;
    zen.selElem = (wantSel >= 0 && wantSel < zen.doc.elemCount) ? wantSel
                : (zen.doc.elemCount > 0 ? 0 : -1);
    for (int i = 0; i < ANIM_ELEMS_MAX; i++) zen.multiSel[i] = false;

    zen.playhead = 0.0f;
    zen.playing = false; zen.playPending = false;
    zen.loopPlay = true;
    zen.stopOnSignal = true;
    zen.zoomT = 0.0f; zen.zoomFull = false;
    zen.undoCount = zen.redoCount = 0; zen.undoHead = 0;

    zen.menuOpen = -1; zen.hotkeyNav = false;
    zen.helpOpen = false; zen.openListOpen = false;
    zen.prompt = ZEN_PROMPT_NONE; zen.promptTargetIdx = -1;
    zen.nameBuf[0] = '\0'; zen.edNameBuf = false;
    zen.libOpen = false; zen.libScroll = 0.0f; zen.libTargetIdx = -1;
    zen.panelAlphaMode = 0;
    zen.showElems = zen.showSignals = zen.showInspector = zen.showTimeline = true;

    zen.elemView = zen.sigView = zen.inspView = (ZenPanelView){0};
    zen.panelAnim = 0.0f; zen.prevPlaybackUi = false;
    zen.guiLocked = false;
    zen.edName = zen.edText = false; zen.edSigIdx = -1;
    zen.selGroup = -1; zen.selKeyCount = 0;
    for (int i = 0; i < ANIM_ELEMS_MAX; i++) zen.trackExpand[i] = 0;
    zen.trackModal = (ZenTrackModal){0};
    zen.trackModal.sig = -1;
    zen.easeDropOpen = false; zen.addTrackOpen = false; zen.addTrackSel = 0;
    zen.dragPlayhead = false; zen.dragKeyGroup = -1;
    zen.dragIntro = zen.dragOutro = false;
    zen.sigModalIdx = -1; zen.sigModalPos = (Vector2){0};
    zen.sigDragging = false; zen.sigScroll = 0.0f; zen.sigLastU = 1.0f;
    ZenSigClearKeySel(); ZenSigCloseDrops();
    zen.tipText[0] = '\0';

    zen.preview = (AnimSignalPlayer){0};
    AnimSignalRegister(&zen.doc, &zen.preview, &zen.playhead);
}

static void Exit()
{
    if (zen.animCurrent >= 0)
    {
        TextCopy(lastOpened, zen.animList[zen.animCurrent]);
        RememberSelElem(zen.animList[zen.animCurrent], zen.selElem);
    }
    else lastOpened[0] = '\0';

    AnimSignalUnregister(&zen.doc, &zen.preview);
}

static void Update()
{
    float dt = GetFrameTime();

    // A live signal preview freezes the doc clock: the timeline shows the
    // signal's own track instead, and the frozen pose is what it eases off.
    bool sigLive = !AnimSignalPlayerDone(&zen.preview);
    if (sigLive && zen.playing && zen.stopOnSignal)
        zen.playing = false;                // stops for good; Space resumes

    // Timeline playback over the TRIMMED section [0..outroStart]; looping
    // restarts at introEnd (the intro is a one-shot lead-in).
    if (zen.playing && !sigLive)
    {
        float inEnd = AnimDocIntroEnd(&zen.doc);
        float outStart = AnimDocOutroStart(&zen.doc);
        zen.playhead += dt;
        if (zen.playhead >= outStart)
        {
            if (zen.loopPlay)
            {
                float cycle = outStart - inEnd;
                zen.playhead = (cycle > 0.0f)
                             ? inEnd + fmodf(zen.playhead - outStart, cycle)
                             : inEnd;
            }
            else zen.playhead = outStart;
        }
    }

    // Fired signals run on their own clock as an override (playhead untouched).
    if (!AnimSignalPlayerDone(&zen.preview)) AnimSignalPlayerUpdate(&zen.preview, dt);

    // ESC closes whatever is open, innermost first; leaves the editor once
    // nothing is (so it can't discard work behind a modal by accident).
    // (a slider's precise-entry textbox consumes its own ESC in the widget)
    if (IsKeyPressed(KEY_ESCAPE) && !ZenSliderTyping())
    {
        if (zen.helpOpen)                   zen.helpOpen = false;
        else if (zen.prompt != ZEN_PROMPT_NONE)
        { zen.prompt = ZEN_PROMPT_NONE; zen.edNameBuf = false; }
        else if (ZenEasingEscClose()) { }
        else if (zen.libOpen)               zen.libOpen = false;
        else if (zen.openListOpen)          zen.openListOpen = false;
        else if (zen.easeDropOpen || zen.addTrackOpen || zen.sigDropMode != 0)
        { zen.easeDropOpen = false; zen.addTrackOpen = false; ZenSigCloseDrops(); }
        else if (ZenViewCtxOpen())          ZenViewCtxClose();
        else if (zen.trackModal.open)       zen.trackModal.open = false;
        else if (zen.sigModalIdx >= 0)
        { zen.sigModalIdx = -1; zen.edSigIdx = -1; ZenSigClearKeySel(); }
        else if (zen.menuOpen >= 0 || zen.hotkeyNav)
        { zen.menuOpen = -1; zen.hotkeyNav = false; }
        else ZenExitEditor();
    }

    // Alt hotkey navigation + menu shortcut letters.
    ZenMenuUpdate();

    bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    bool typing = ZenTyping();
    if (!typing && ctrl && IsKeyPressed(KEY_Z)) ZenUndoApply(-1);
    if (!typing && ctrl && IsKeyPressed(KEY_Y)) ZenUndoApply(+1);
    if (!typing && ctrl && IsKeyPressed(KEY_S)) ZenSaveCurrent();

    // Space = play/pause, Ctrl+Space = play-from-start/stop. Play does NOT
    // start the clock directly: it arms playPending and ZenViewUpdate()
    // flips playing on once the zoom-in lands at 1:1.
    if (!typing && IsKeyPressed(KEY_SPACE))
    {
        if (zen.playing || zen.playPending)
        {
            zen.playing = false; zen.playPending = false;
            if (ctrl) zen.playhead = 0.0f;
        }
        else
        {
            if (ctrl) zen.playhead = 0.0f;
            zen.playPending = true;
        }
        zen.preview.playing = false;
    }

    ZenViewUpdate(dt);
    ZenPanelsUpdate(dt);
}

// ===========================================================================
//  Draw / Gui
// ===========================================================================
static void Draw()
{
    ZenViewDraw();
}

static void Gui()
{
    ZenWidgetsFrameEnd();

    // The base layer (bar + panels) locks whenever something floats above it:
    // a menu modal, an open dropdown list, or the cursor being over one of
    // the floating modals (which stay usable while the background does too).
    Vector2 mouse = GetMousePosition();
    bool overFloat =
        (zen.trackModal.open &&
         CheckCollisionPointRec(mouse, zen.trackModal.rect)) ||
        (zen.sigModalIdx >= 0 &&
         CheckCollisionPointRec(mouse, zen.sigModalRect));
    bool dropOpen = zen.menuOpen >= 0 || zen.easeDropOpen || zen.addTrackOpen
                 || zen.sigDropMode != 0 || ZenViewCtxOpen();
    zen.guiLocked = ZenMenuModalOpen() || ZenEasingModalOpen() || dropOpen || overFloat;
    if (zen.guiLocked) GuiLock();

    // uiHover keeps the viewport's hands off the mouse while it's on chrome;
    // ZenPanelsGui ORs in each panel rect it actually draws.
    zen.uiHover = overFloat || mouse.y <= ZEN_MENU_BAR_H;

    ZenMenuBarGui();
    ZenPanelsGui();

    GuiUnlock();

    // Floating modals: usable while the panels stay clickable outside them.
    // Locked only under their own dropdown lists or a menu modal.
    bool floatLock = ZenMenuModalOpen() || ZenEasingModalOpen()
                  || zen.easeDropOpen || zen.sigDropMode != 0;
    if (floatLock) GuiLock();
    ZenSignalModalGui();
    ZenTrackModalGui();
    if (floatLock) GuiUnlock();

    // Overlays topmost: dropdown lists, then the menu's modals, then the tip.
    ZenViewContextGui();
    ZenSignalOverlaysGui();
    ZenEaseDropOverlayGui();
    ZenMenuOverlaysGui();
    ZenEasingGui();
    ZenTipDraw();
}
