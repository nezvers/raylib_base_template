#ifndef ZEN_INTERNAL_H
#define ZEN_INTERNAL_H

// ============================================================================
//  zen_internal.h  -  shared internals of the Zen animation editor module
//
//  The zen editor is split across several .c files (state glue, viewport,
//  menu bar, panels, modals). The house pattern of file-scope statics can't
//  span files, so the module's whole mutable state lives in ONE plain value
//  struct (ZenCtx) defined in anim_editor_zen.c and shared through this
//  header. Still no heap, still POD, still a singleton - just one that more
//  than one translation unit can see.
//
//  Everything here is module-PRIVATE: only src/anim_editor_zen/*.c may
//  include this header. The public face is anim_editor_zen.h.
// ============================================================================

#include "raylib.h"
#include "../anim/anim.h"
#include "../anim/anim_io.h"    // property groups (ANIM_GROUP_PROPS, AnimGroupAt)
#include "../anim/anim_ease_custom.h"   // custom easings + hide flags
#include "../anim/anim_library.h"
#include "../anim/anim_stage.h"  // AnimHandle - the Help guided tour plays here

// Animations live as one .cfg each under ZEN_ANIM_DIR (CWD-relative, writable;
// same directory the classic editor and AnimStage use).
#define ZEN_ANIM_DIR      "anims"
#define ZEN_ANIM_EXT      ".cfg"
#define ZEN_ANIM_LIST_MAX 64
// element library: '_'-prefixed so ZenRescanAnims() skips it (not an anim).
#define ZEN_LIB_PATH      ZEN_ANIM_DIR "/_library" ZEN_ANIM_EXT
// Help > guided tour: an ordinary authored animation, played by AnimStagePlay
// (which appends ZEN_ANIM_DIR and the extension itself). Spaces are literal.
#define ZEN_GUIDE_ANIM    "ZEN EDITOR - GUIDE"
#define ZEN_UNDO_MAX      16
#define ZEN_AUTOKEY_EPS   0.02f   // playhead-to-key snap when auto-keying (s)
#define ZEN_PAUSE_EPS     0.02f   // marker-to-time snap: how close counts as
                                  // "there is already a pause here" (s)

// Viewport: while EDITING the doc is shown zoomed out so elements can fly in
// from off-screen and still be seen/grabbed; the real screen is the dotted
// rect. Play zooms to 1:1 first (sineIn), THEN starts the clock.
#define ZEN_ZOOM_EDIT     0.5f    // editing zoom (0.5 = 2x the screen visible)
#define ZEN_ZOOM_TIME     0.2f    // zoom in/out duration in seconds

#define ZEN_MENU_BAR_H    26.0f   // top menu bar height (panels sit below it)

// Modal prompts (small centered boxes, drawn topmost, block other input).
typedef enum {
    ZEN_PROMPT_NONE = 0,
    ZEN_PROMPT_NEW_NAME,            // File > New: name the fresh animation
    ZEN_PROMPT_SAVE_THEN_SWITCH,    // dirty doc guarded before an Open
    ZEN_PROMPT_SAVE_THEN_EXIT,      // dirty doc guarded before leaving
    ZEN_PROMPT_CONFIRM_DELETE,      // File > Delete of the current animation
    ZEN_PROMPT_RENAME_ANIM,         // File > Rename (moves the .cfg)
    ZEN_PROMPT_COPY_ANIM,           // File > Save As (copy under a new name)
    ZEN_PROMPT_DURATION,            // Editor > Duration: type an exact length
    ZEN_PROMPT_LIB_SAVE_NAME,       // Element > Save to library: entry name
    ZEN_PROMPT_LIB_RENAME,          // library modal: rename an entry
} ZenPromptKind;

// A scrolling panel view (content taller than the panel scrolls under the
// wheel, clipped by a scissor).
typedef struct {
    float scroll;                   // <= 0; how far the content is pulled up
    float contentH;                 // measured by the draw callback last frame
} ZenPanelView;

// The ONE track/key modal: draggable, shared by the inspector, the timeline
// and the signal modal. Its content follows the current track/key selection.
// Editing semantics: exactly one key selected -> edits apply LIVE (like a
// normal inspector); several keys (or none = whole track) -> edits are STAGED
// per field and an Apply button writes only the touched fields to every key.
typedef struct {
    bool open;
    int  sig;                       // -1 = doc-track mode; else the signal
                                    // index whose (zen.sigSel*) key is edited
    Vector2 pos;                    // top-left, user-draggable
    Rectangle rect;                 // as drawn last frame (input claiming)
    bool dragging; Vector2 dragOff;

    // staged values + per-field dirty flags for the bulk modes.
    float vals[8];                  // per member prop (ANIM_GROUP_PROPS <= 8)
    bool  dVals[8];
    Color cval;  bool dCval;
    int   ease;  bool dEase;
    char  timeBuf[16]; bool edTime; // key time textbox (single-key mode only)
} ZenTrackModal;

// Which layer a mouse press landed in. Latched on press and held until
// release, so the locks below follow the OWNER of a drag rather than whatever
// the cursor happens to be over: raygui fires GuiButton on RELEASE inside its
// bounds without caring where the press began, and GuiSlider grabs the knob on
// IsMouseButtonDown, so a foreign drag passing over either would trigger it.
// Ordered back-to-front; ZEN_LAYER_NONE = the press owned nothing.
typedef enum {
    ZEN_LAYER_NONE = 0,
    ZEN_LAYER_VIEWPORT,
    ZEN_LAYER_BASE,                 // menu bar + panels
    ZEN_LAYER_FLOAT_SIG,            // signal modal
    ZEN_LAYER_FLOAT_TRACK,          // track modal (drawn over the signal one)
    ZEN_LAYER_OVERLAY,              // dropdowns, context menus, full modals
} ZenLayer;

// ---------------------------------------------------------------------------
//  The one shared context. Plain value struct: undo snapshots and doc are
//  memcpy-able exactly like in the classic editor.
// ---------------------------------------------------------------------------
typedef struct {
    // -- document -----------------------------------------------------------
    AnimDoc doc;                    // the working document
    bool docDirty;                  // edits not written to its file
    AnimSignalPlayer preview;       // fired-signal override on top of timeline
    int previewSeq;                 // sequence number the preview pretends to be

    // -- animation files (one .cfg per anim under ZEN_ANIM_DIR) -------------
    char animList[ZEN_ANIM_LIST_MAX][ANIM_NAME_MAX];  // basenames, no ext
    int  animCount;
    int  animCurrent;               // index into animList (-1 = unsaved)

    // -- element library (shared shelf, eager-saved on every mutation) ------
    AnimLibrary library;

    // -- selection ----------------------------------------------------------
    int  selElem;                   // primary selected element (-1 = none)
    bool multiSel[ANIM_ELEMS_MAX];  // additional selected elements

    // -- playback -----------------------------------------------------------
    float playhead;                 // scrubber time, 0..doc.duration
    bool  playing;                  // timeline clock running (post-zoom)
    bool  playPending;              // play requested, waiting on the zoom-in
    bool  loopPlay;
    bool  autoKey;
    bool  stopOnSignal;             // a fired signal ends playback (vs. resume)

    // -- viewport zoom ------------------------------------------------------
    float zoomT;                    // 0 = editing (ZEN_ZOOM_EDIT) .. 1 = 1:1
    bool  zoomFull;                 // View toggle: edit at 1:1 instead

    // -- menu bar / hotkey navigation ---------------------------------------
    int  menuOpen;                  // open menu index (-1 = none)
    bool hotkeyNav;                 // Alt navigation mode active
    bool helpOpen;                  // Help modal
    bool openListOpen;              // File > Open anim list overlay
    AnimHandle guideAnim;           // Help > guided tour instance on the stage
                                    // (ANIM_HANDLE_NONE when not playing)

    // -- prompts (ZenPromptKind) --------------------------------------------
    int  prompt;
    int  promptTargetIdx;           // anim index a prompt acts on
    char nameBuf[ANIM_NAME_MAX];    // prompt textbox buffer
    bool edNameBuf;                 // its raygui edit-mode flag

    // -- element library modal ----------------------------------------------
    bool  libOpen;
    float libScroll;
    int   libTargetIdx;             // entry being renamed

    // -- panel chrome -------------------------------------------------------
    int  panelAlphaMode;            // 0 opaque, 1 semi, 2 faint (all panels)
    bool showElems, showSignals, showInspector, showTimeline;
    ZenPanelView elemView, sigView, inspView;
    float panelAnim;                // 0 editing .. 1 playing (panels slid away)
    bool  prevPlaybackUi;
    Rectangle inspPanelRect;        // this frame's inspector panel (key reveal)
    bool  scrollToSelKey;           // scroll the selected track row into view
    bool  guiLocked;                // mirrors GuiLock() for hand-drawn UI
    bool  uiHover;                  // last frame's mouse was over gui chrome,
                                    // so the viewport must not react to it
    ZenLayer mouseOwner;            // layer that owns the held gesture
    bool  mouseHeld;                // a press is in flight (no release yet)
    Vector2 mousePress;             // where that press landed
    // Set when a press reflows the UI under itself (picking a key in the track
    // modal switches it to single-key mode, inserting a row). The press point
    // and the cursor are the same point on the press frame, so a widget that
    // slid under the cursor passes a "did the press land on me" test. Nothing
    // positional can tell the two apart - the gesture has to be poisoned.
    bool  mouseReflow;              // this gesture moved widgets; no new grabs

    // -- textbox edit flags -------------------------------------------------
    bool edName, edText;            // inspector name/text boxes
    int  edSigIdx;                  // signal whose name textbox is in edit mode

    // -- track / key selection (GROUP level, like the classic timeline) -----
    // The selected track is (selElem, selGroup); the selected KEYS are group
    // key times within it. 0 selected = the whole track is the target.
    int   selGroup;                 // group index on selElem, -1 = none
    float selKeys[ANIM_KEYS_MAX * ANIM_GROUP_PROPS];
    int   selKeyCount;

    // per-element bitmask: bit gi set = that group's key list is EXPANDED in
    // the inspector (collapsed by default keeps the overview readable).
    unsigned trackExpand[ANIM_ELEMS_MAX];

    ZenTrackModal trackModal;

    // ease dropdown of the track modal (list drawn topmost as an overlay)
    bool easeDropOpen; Rectangle easeDropRect;
    // string-pick dropdown of the track modal (same overlay treatment)
    bool strDropOpen; Rectangle strDropRect;
    // add-track dropdown of the inspector
    bool addTrackOpen; int addTrackSel; Rectangle addTrackRect;

    // -- timeline drag state ------------------------------------------------
    bool  dragPlayhead;
    int   dragKeyGroup;             // group being dragged (-1 = none)
    float dragKeyTime;
    bool  dragIntro, dragOutro;
    int   dragPause;                // pause marker being dragged (-1 = none)

    // -- pause markers ------------------------------------------------------
    int   selPause;                 // marker the timeline highlights (-1 = none)
    bool  pausedOnMarker;           // preview playback is HELD on selPause's
                                    // marker, waiting for a key (mirrors the
                                    // runtime hold in anim_stage.c)
    int   heldPause;                // which marker the preview is held on

    // -- signal modal -------------------------------------------------------
    int   sigModalIdx;              // open signal (-1 = none); survives playback
    Vector2 sigModalPos;            // draggable position
    Rectangle sigModalRect;         // as drawn last frame (input claiming)
    bool  sigDragging; Vector2 sigDragOff;
    float sigScroll;
    float sigLastU;                 // last u typed on ANY signal key (seed)
    // selected signal group key: (element, group, u); <0 = none
    int   sigSelElem, sigSelGroup;
    float sigSelU;
    int   sigPosSelElem, sigPosSelSlot;   // "--params--" key selection
    float sigPosSelU;
    float sigSeqSelU;                     // "--sequence--" key selection
    int   sigDropMode, sigDropElem;       // the ONE open modal dropdown
    Rectangle sigDropHdr;

    // -- tooltip of this frame (drawn topmost by ZenTipDraw) ----------------
    char tipText[320];              // wrapped by ZenTipDraw, so it can be long
    Rectangle tipRect;              // widget the tip belongs to

    // -- undo ring (whole-doc value snapshots) ------------------------------
    AnimDoc undoBuf[ZEN_UNDO_MAX];
    int undoHead;                   // next write slot
    int undoCount;                  // valid snapshots behind head
    int redoCount;                  // snapshots ahead (after undo)
} ZenCtx;

extern ZenCtx zen;

// ---------------------------------------------------------------------------
//  anim_editor_zen.c - doc lifecycle, undo, file CRUD
// ---------------------------------------------------------------------------
void ZenUndoPush(void);             // snapshot BEFORE a mutating edit; the
                                    // ONLY writer of zen.docDirty
void ZenUndoApply(int delta);       // -1 undo, +1 redo
void ZenRescanAnims(void);          // rebuild zen.animList from disk
int  ZenAnimFind(const char *name); // index in animList, or -1
const char *ZenAnimPath(const char *name);  // rotating TextFormat buf - use now
void ZenLoadAnimByIndex(int idx);
void ZenSaveCurrent(void);
void ZenRequestSwitch(int idx);     // Open guarded by the dirty prompt
void ZenCreateAnim(const char *name);
void ZenDeleteAnim(int idx);
void ZenRenameAnim(const char *newName);   // moves the .cfg on disk
void ZenCopyAnim(const char *newName);     // Save As: copy becomes current
void ZenExitEditor(void);           // to main menu, guarded by dirty prompt
bool ZenTyping(void);               // a textbox is capturing the keyboard

// element operations on the current selection (menu + panels both call these)
bool ZenIsSelected(int i);          // primary or ctrl-multi selected
void ZenElemAdd(int kind);          // AE_TEXT / AE_SHAPE / AE_GLOBAL
void ZenElemDuplicate(void);
void ZenElemDelete(void);           // deletes ALL selected
void ZenElemMove(int delta);        // -1 up / +1 down, selection follows
void ZenLibraryInsert(int entry);   // library entry -> new doc element

// ---------------------------------------------------------------------------
//  zen_widgets.c - shared gui building blocks
// ---------------------------------------------------------------------------
float ZenTextW(const char *text);   // label width at the current gui font
float ZenClampF(float v, float lo, float hi);
bool  ZenEditSlider(Rectangle r, const char *label, float *v, float lo, float hi);
bool  ZenSliderTyping(void);        // a slider's value textbox is open

// Multi-line text field (raygui's GuiTextBox is single-line only). Same
// contract as GuiTextBox: returns true when the caller should flip `active`.
bool  ZenEditTextArea(Rectangle r, char *text, int cap, bool active);
bool  ZenTextAreaTyping(void);      // a text area owns the keyboard
void  ZenTextAreaClose(void);       // commit and release it (ESC / selection change)
int   ZenTextAreaRows(const char *s);
float ZenTextAreaHeight(const char *s, int maxRows);
void  ZenWidgetsFrameEnd(void);     // close drag gestures on mouse release

// Mouse gesture ownership. ZenMouseOwnerUpdate runs once at the top of Gui();
// ZenLayerActive(l) is false while another layer owns the gesture, and the
// caller keeps that layer GuiLock()ed for its whole duration.
void  ZenMouseOwnerUpdate(void);
bool  ZenLayerActive(ZenLayer l);
// Call from any handler that changes the layout in response to a press, before
// the moved widgets are drawn. Suppresses value grabs for the rest of the
// gesture, so a slider that lands under the cursor stays inert until release.
void  ZenMouseReflow(void);
// GuiButton that also requires the press to have landed on it, for buttons
// that can reflow under a held cursor.
bool  ZenButton(Rectangle r, const char *text);
// Title-bar drag shared by the floating modals. `own` is the layer the caller
// draws in, so two overlapping title bars can't both grab one press.
void  ZenModalDrag(Rectangle title, Vector2 *pos, Vector2 origin,
                   bool *dragging, Vector2 *dragOff, ZenLayer own);
void  ZenDrawSwatch(Rectangle r, Color c);
void  ZenDrawPanelBG(Rectangle r, int mode);
int   ZenColorPropFor(int kind);    // the kind's one colour property
void  ZenPropRange(const AnimElem *e, int prop, float *lo, float *hi);
void  ZenConvertSizeUnits(AnimElem *e, bool toAbsolute);
void  ZenEnsureZeroKey(AnimElem *e, AnimTrack *tr);
void  ZenEnsureZeroColorKey(AnimElem *e, AnimTrack *tr);
void  ZenPropSlider(Rectangle r, AnimElem *e, int prop, float *baseField);
float ZenColorRGBRows(float x, float y, float w, AnimElem *e, int prop,
                      Color *base, const char *label);
// hover tooltip: widgets call ZenTip after drawing; ZenTipDraw paints the one
// recorded tip topmost at the end of Gui (labels aren't focusable in raygui,
// so this is hand-rolled rather than GuiSetTooltip).
void  ZenTip(Rectangle r, const char *text);
void  ZenTipDraw(void);
// GuiLabel that ellipsizes to fit and tips the FULL text when clipped.
void  ZenLabelTip(Rectangle r, const char *text, const char *desc);

// ---------------------------------------------------------------------------
//  zen_groups.c - coordinated group editing over per-prop tracks
// ---------------------------------------------------------------------------
#define ZEN_GROUP_TIMES_MAX (ANIM_KEYS_MAX * ANIM_GROUP_PROPS)
int   ZenGroupKeyTimes(AnimElem *e, int gi, float *out);  // union, sorted
bool  ZenGroupHasTrack(AnimElem *e, int gi);
void  ZenGroupWriteKey(AnimElem *e, int gi, float t);     // key every member
void  ZenGroupDeleteTracks(AnimElem *e, int gi);
void  ZenGroupDeleteKeyAt(AnimElem *e, int gi, float t);
void  ZenGroupMoveKeyTo(AnimElem *e, int gi, float oldT, float newT);
// Restate the group key at srcT verbatim (value, colour and ease) at dstT,
// rather than sampling the element there the way ZenGroupWriteKey does.
bool  ZenGroupCloneKeyTo(AnimElem *e, int gi, float srcT, float dstT);
// Time of the nearest group key strictly before t, or -1 when there is none.
float ZenGroupKeyTimeLeftOf(AnimElem *e, int gi, float t);
void  ZenGroupSetEaseAt(AnimElem *e, int gi, float t, int ease);
int   ZenGroupEase(AnimElem *e, int gi, float t);         // representative ease
int   ZenGroupColorProp(int kind, int gi);                // colour member or -1
const char *ZenGroupKeyLabel(AnimElem *e, int gi, float t);

// True when every member property snaps instead of blending (the "string"
// group). Such a group has no meaningful easing, so the UI must not offer one.
bool ZenGroupIsStepped(int kind, int gi);

// -- signal-target groups (same coordinated editing, keys in normalized u) --
#define ZEN_SIG_U_EPS     0.001f
#define ZEN_SIG_TIMES_MAX (ANIM_SIG_KEYS_MAX * ANIM_GROUP_PROPS)
AnimSigTarget *ZenSigFindTarget(AnimSignal *sg, int elemIdx, int prop);
int   ZenSigTargetKeyNear(const AnimSigTarget *tg, float u);
void  ZenSigTargetSortKeys(AnimSigTarget *tg);
bool  ZenSigGroupHasTarget(AnimSignal *sg, int elemIdx, int gi);
bool  ZenSigGroupFits(AnimSignal *sg, int elemIdx, int gi);
int   ZenSigGroupKeyTimes(AnimSignal *sg, int elemIdx, int gi, float *out);
float ZenSigFreeU(const float *times, int nt, float pref);
float ZenSigGroupFreeU(AnimSignal *sg, int elemIdx, int gi, float pref);
void  ZenSigGroupWriteKey(AnimSignal *sg, int elemIdx, int gi, float u);
void  ZenSigGroupAddTargets(AnimSignal *sg, int elemIdx, int gi);
void  ZenSigGroupDeleteTargets(AnimSignal *sg, int elemIdx, int gi);
void  ZenSigGroupDeleteKeyAt(AnimSignal *sg, int elemIdx, int gi, float u);
void  ZenSigGroupMoveKeyTo(AnimSignal *sg, int elemIdx, int gi, float oldU, float newU);
void  ZenSigGroupSetEaseAt(AnimSignal *sg, int elemIdx, int gi, float u, int ease);
int   ZenSigGroupEase(AnimSignal *sg, int elemIdx, int gi, float u);
const char *ZenSigGroupKeyLabel(AnimSignal *sg, int elemIdx, int gi, float u);

// -- track/key selection helpers (ctx-level, shared by panels + modals) -----
void  ZenSelClear(void);                        // drop track + key selection
void  ZenSelTrack(int elem, int gi);            // select a whole track
void  ZenSelKey(int elem, int gi, float t, bool additive);  // click / shift-click
bool  ZenKeyIsSelected(int elem, int gi, float t);
void  ZenSelValidate(void);                     // after undo/load/deletes
void  ZenAutoKeyFocus(int elem, int prop, float t);  // show what auto-key wrote
const char *ZenPropDesc(int prop);              // plain-language property help

// ---------------------------------------------------------------------------
//  zen_panels.c - the four panels
// ---------------------------------------------------------------------------
void ZenPanelsGui(void);            // layout + ELEMENTS/SIGNALS/INSPECTOR/TIMELINE
void ZenPanelsOverlaysGui(void);    // the add-track list, topmost (must run unlocked)
void ZenPanelsUpdate(float dt);     // playback slide bookkeeping
void ZenFireSignal(const AnimSignal *sg);

// ---------------------------------------------------------------------------
//  zen_track_modal.c - the draggable track/key modal
// ---------------------------------------------------------------------------
void ZenTrackModalOpen(int elem, int gi);       // doc-track mode
void ZenTrackModalOpenSig(int sigIdx);          // signal mode: edits the key
                                                // selected in zen.sigSel*
void ZenTrackModalSync(void);                   // reload staged values
void ZenTrackModalGui(void);                    // drawn above the panels
void ZenEaseDropOverlayGui(void);               // its ease list, topmost
void ZenStringDropOverlayGui(void);             // its string-pool list, topmost

// ---------------------------------------------------------------------------
//  zen_signal_modal.c - the draggable signal modal
// ---------------------------------------------------------------------------
void ZenSignalModalGui(void);
void ZenSignalOverlaysGui(void);                // its dropdown lists, topmost
void ZenSigClearKeySel(void);
void ZenSigCloseDrops(void);
void ZenReRegisterSignals(void);

// ---------------------------------------------------------------------------
//  zen_menu.c - menu bar, Alt hotkey navigation, prompts, library, help
// ---------------------------------------------------------------------------
void ZenMenuUpdate(void);           // Alt mode + menu hotkeys (keyboard side)
void ZenMenuBarGui(void);           // the bar itself (widgets)
void ZenMenuOverlaysGui(void);      // open dropdown + modals, drawn topmost
bool ZenMenuModalOpen(void);        // a prompt/library/help modal is up
bool ZenMenuTyping(void);           // File>Open search box capturing keys

// Help > guided tour: the authored ZEN_GUIDE_ANIM played over the live editor
// by anim_stage (single playback, its pause markers held by any keypress).
void ZenGuidePlay(void);            // start, or restart if already running
void ZenGuideStop(void);            // cancel; safe when nothing is playing
bool ZenGuideActive(void);          // a tour instance is alive on the stage

// ---------------------------------------------------------------------------
//  zen_view.c - main viewport (zoom, dotted screen rect, later: picking)
// ---------------------------------------------------------------------------
void ZenViewUpdate(float dt);       // zoom + picking/dragging input
void ZenViewDraw(void);             // game-space draw of doc + overlays
void ZenViewContextGui(void);       // right-click track menu (screen space)
bool ZenViewCtxOpen(void);          // is the context menu showing
void ZenViewCtxClose(void);

// -- zen_easing.c -----------------------------------------------------------
void ZenEasingBrowserOpen(void);    // Easing > Browse
bool ZenEasingModalOpen(void);      // browser or graph editor showing
bool ZenEasingEscClose(void);       // close topmost easing modal; false if none
bool ZenEasingTyping(void);         // its name textbox captures the keyboard
void ZenEasingGui(void);

// -- zen_clone_modal.c ------------------------------------------------------
// Element > Clone from...: write another element's look at the playhead onto
// the SELECTED element as keys at a chosen time. Saves element slots by reusing
// an expired block instead of adding a new one.
void ZenCloneShow(void);            // Element > Clone from...
bool ZenCloneOpen(void);            // the modal is showing
bool ZenCloneEscClose(void);        // close it; false if it was not open
bool ZenCloneTyping(void);          // its time textbox captures the keyboard
void ZenCloneGui(void);

// -- zen_string_pool.c ------------------------------------------------------
// The document's shared text strings. Text elements point at entries here, and
// an AP_T_STRING track keys the index, which is what lets text CHANGE mid-
// animation (see AnimString in anim.h for why it cannot live on the key).
// One-line preview of a possibly multi-line string, ellipsised at maxChars.
// Returns a shared static buffer - use it before the next call.
const char *ZenTextPreview(const char *s, int maxChars);

void ZenStringPoolShow(void);
bool ZenStringPoolOpen(void);
bool ZenStringPoolEscClose(void);
bool ZenStringPoolTyping(void);
void ZenStringPoolGui(void);

// timeline right-click menu (insert / delete a pause marker)
void ZenTimelineCtxGui(void);
bool ZenTimelineCtxOpen(void);
void ZenTimelineCtxClose(void);

Vector2 ZenScreenToDoc(Vector2 screenPos);  // raw mouse -> doc/game space
void ZenDrawDottedRect(Rectangle r, Color c);
void ZenDrawDottedLine(Vector2 a, Vector2 b, Color c);

#endif // ZEN_INTERNAL_H
