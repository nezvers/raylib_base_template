// ============================================================================
//  zen_string_pool.c  -  the document's shared TEXT STRING pool
//
//  WHY A POOL. A text element's words used to be a base field with no track, so
//  text could never change during an animation. Putting a string on the KEY is
//  unaffordable - AnimKey is 16 B at the deepest nesting, and a 512 B buffer
//  there measures at 28 MB across the build. So the strings live once, here on
//  the document, and an AP_T_STRING track keys their INDEX (which SNAPS, never
//  blends - you cannot be half-way between two strings).
//
//  SHARING IS DELIBERATE. Two elements can point at one entry and both follow an
//  edit to it. That is a feature, not an accident, so every row shows its usage
//  count and an entry still in use cannot be deleted. Empty entries nobody
//  points at are reclaimed automatically (AnimDocGCStrings) so the list stays
//  free of noise.
//
//  Two panes: STRINGS and ELEMENTS. The Strings tab lists the pool AND the
//  private text of every element that has not been pooled, under a second
//  heading - otherwise those words are invisible here, which is exactly when a
//  user goes looking for them.
//
//  ASSIGNING IS A TWO-STEP GESTURE, deliberately. Clicking a row only selects
//  it; "assign" arms that string and jumps to the Elements tab, where each row
//  has its own "set". Nothing is written until that second click, so a stray
//  click in a list of elements cannot silently repoint one.
//
//  File-static state owned by this one caller, like zen_easing.c.
// ============================================================================

#include "raylib.h"
#include "raygui.h"
#include "zen_internal.h"
#include "../screen_state/screen_state.h"
#include "../audio_state/audio_state.h"
#include <math.h>

#define SP_ROW_H     34.0f
#define SP_PAD       14.0f
// Status band + button band. The list is clipped above this, so the footer can
// never sit on top of a row - the failure this constant exists to prevent.
#define SP_FOOTER_H  74.0f

static bool  s_open = false;
static int   s_sel = -1;            // selected pool entry (-1 = none)
static float s_scroll = 0.0f;
static float s_elemScroll = 0.0f;
static bool  s_editing = false;     // the edit area owns the keyboard
static char  s_buf[ANIM_TEXT_LEN_MAX];
static int   s_editIdx = -1;        // pool entry being edited; -1 = a NEW string
static int   s_editElem = -1;       // OR the element whose own text is edited
static char  s_status[96] = "";
static int   s_tab = 0;             // 0 = strings, 1 = elements
static int   s_armed = -1;          // pool entry armed for assignment, -1 = none
static double s_lastClick = 0.0;    // double-click detection on string rows
static int   s_lastClickRow = -1;

// --- palette (a step away from the flat grey boxes elsewhere) ---------------
static const Color SP_BG      = { 28, 30, 36, 255 };
static const Color SP_PANEL   = { 36, 39, 46, 255 };
static const Color SP_ROW     = { 44, 48, 57, 255 };
static const Color SP_ROW_HOT = { 56, 61, 72, 255 };
static const Color SP_ACCENT  = { 92, 158, 232, 255 };
static const Color SP_EDGE    = { 62, 67, 79, 255 };
static const Color SP_TEXT    = { 226, 230, 238, 255 };
static const Color SP_DIM     = { 138, 145, 160, 255 };

// ---------------------------------------------------------------------------
//  Accessors for the Gui()/ESC plumbing in anim_editor_zen.c
// ---------------------------------------------------------------------------
void ZenStringPoolShow(void)
{
    s_open = true;
    s_scroll = 0.0f; s_elemScroll = 0.0f;
    s_editing = false; s_editIdx = -1; s_editElem = -1;
    s_status[0] = '\0';
    s_tab = 0;
    s_armed = -1;
    s_lastClickRow = -1;
    // housekeeping on entry: blank entries nobody points at are noise.
    AnimDocGCStrings(&zen.doc);
    if (s_sel >= zen.doc.stringCount) s_sel = -1;
}

bool ZenStringPoolOpen(void)   { return s_open; }
bool ZenStringPoolTyping(void) { return s_open && s_editing; }

bool ZenStringPoolEscClose(void)
{
    if (!s_open) return false;
    if (s_editing) { s_editing = false; s_editElem = -1; ZenTextAreaClose(); return true; }
    s_open = false; s_armed = -1;
    AnimDocGCStrings(&zen.doc);      // leave the pool tidy
    return true;
}

// ---------------------------------------------------------------------------
//  Helpers
// ---------------------------------------------------------------------------
// One-line preview of a possibly multi-line string, with an ellipsis. Shared:
// the track modal shows string keys the same way this modal lists them.
const char *ZenTextPreview(const char *s, int maxChars)
{
    static char out[96];
    int o = 0;
    for (int i = 0; s[i] && o < maxChars && o < (int)sizeof(out) - 4; i++)
    {
        char c = s[i];
        if (c == '\n') { out[o++] = ' '; out[o++] = '/'; out[o++] = ' '; }
        else           out[o++] = c;
    }
    if (s[0] == '\0') { TextCopy(out, "(empty)"); return out; }
    if (o >= maxChars) { out[o++] = '.'; out[o++] = '.'; out[o++] = '.'; }
    out[o] = '\0';
    return out;
}

// The string index element `e` shows AT THE PLAYHEAD, or -1 for its own text.
// Resolved rather than read from keys[0]: once the text changes over time, the
// first key is not what is on screen.
static int ElemStringIdx(const AnimElem *e)
{
    const AnimTrack *tr = NULL;
    for (int i = 0; i < e->trackCount; i++)
        if (e->tracks[i].prop == AP_T_STRING) { tr = &e->tracks[i]; break; }
    if (!tr || tr->keyCount == 0) return -1;
    return (int)(AnimTrackEval(tr, zen.playhead, -1.0f) + 0.5f);
}

// Point element `ei` at pool entry `idx` AT THE PLAYHEAD. With no string track
// this creates one; with the playhead past 0 it also anchors the words the
// element showed before, so the assignment changes it from here on rather than
// across the whole timeline (a lone key overrides the base value everywhere -
// the same rule AnimDocCloneElemState documents).
static void AssignString(int ei, int idx)
{
    if (ei < 0 || ei >= zen.doc.elemCount) return;
    AnimElem *e = &zen.doc.elems[ei];
    if (e->kind != AE_TEXT) return;

    ZenUndoPush();

    // Read what it showed at 0 BEFORE creating anything: making a track changes
    // what is sampled.
    float t = zen.playhead;
    int   before = (t > ZEN_AUTOKEY_EPS) ? AnimDocStringIdxAt(&zen.doc, e, 0.0f) : -1;

    AnimTrack *tr = AnimElemFindTrack(e, AP_T_STRING);
    if (!tr) tr = AnimElemAddTrack(e, AP_T_STRING);
    if (!tr) { TextCopy(s_status, "no track slots left on that element"); return; }

    if (tr->keyCount == 0 && before >= 0)
        AnimTrackAddKey(tr, 0.0f, (float)before, ANIM_EASE_LINEAR);
    AnimTrackWriteKeyAt(tr, t, (float)idx, ZEN_AUTOKEY_EPS);

    // Keep the element's own text in step ONLY when this is its first key: it
    // is the fallback if the track is removed, and past the first key there is
    // no single set of words to stand for the element.
    if (tr->keyCount > 0 && fabsf(tr->keys[0].t - t) <= ZEN_AUTOKEY_EPS)
    {
        const char *s = AnimDocStringAt(&zen.doc, idx);
        if (s) TextCopy(e->text, s);
    }

    zen.docDirty = true;
    TextCopy(s_status, TextFormat("string %02d set on %s at %.2fs", idx, e->name, t));
    s_armed = -1;                   // the gesture is complete
}

// True when the element shows words of its own that are NOT a pool entry - the
// case the Strings tab would otherwise hide.
static bool HasPrivateText(const AnimElem *e)
{
    return e->kind == AE_TEXT && ElemStringIdx(e) < 0 && e->text[0] != '\0';
}

// ---------------------------------------------------------------------------
//  Draw (unlocked overlay pass)
// ---------------------------------------------------------------------------
void ZenStringPoolGui(void)
{
    if (!s_open) return;

    ScreenState *ss = ScreenStateGet();
    float W = (float)ss->width, H = (float)ss->height;
    float mw = 660, mh = 500;
    Rectangle m = { (W-mw)/2, (H-mh)/2, mw, mh };

    DrawRectangle(0, 0, (int)W, (int)H, (Color){ 0, 0, 0, 150 });
    DrawRectangleRec(m, SP_BG);
    DrawRectangleLinesEx(m, 1.0f, SP_EDGE);
    // accent rule under the header: the one bit of colour that says "modern"
    DrawRectangleRec((Rectangle){ m.x, m.y + 44, mw, 2 }, SP_ACCENT);

    Vector2 mouse = GetMousePosition();
    int used = 0;
    for (int i = 0; i < zen.doc.stringCount; i++) if (zen.doc.strings[i].used) used++;

    // -- header -------------------------------------------------------------
    DrawText("STRING POOL", (int)(m.x + SP_PAD), (int)(m.y + 15), 18, SP_TEXT);
    const char *cap = TextFormat("%d/%d", used, ANIM_STRINGS_MAX);
    DrawText(cap, (int)(m.x + mw - SP_PAD - MeasureText(cap, 12)), (int)(m.y + 20), 12,
             used >= ANIM_STRINGS_MAX ? (Color){ 232, 140, 110, 255 } : SP_DIM);

    // -- tabs ---------------------------------------------------------------
    const char *tabs[2] = { "Strings", "Elements" };
    float tw = 96.0f, tx = m.x + 150;
    for (int i = 0; i < 2; i++)
    {
        Rectangle tr = { tx + i*(tw+4), m.y + 14, tw, 26 };
        bool hot = CheckCollisionPointRec(mouse, tr);
        DrawRectangleRec(tr, (s_tab == i) ? SP_ROW_HOT : (hot ? SP_ROW : SP_PANEL));
        if (s_tab == i)
            DrawRectangleRec((Rectangle){ tr.x, tr.y + tr.height - 2, tr.width, 2 }, SP_ACCENT);
        int lw = MeasureText(tabs[i], 11);
        DrawText(tabs[i], (int)(tr.x + (tw - lw)/2), (int)(tr.y + 8), 11,
                 (s_tab == i) ? SP_TEXT : SP_DIM);
        if (hot && ZenLayerActive(ZEN_LAYER_OVERLAY) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !s_editing)
        { AudioPlayButton(); s_tab = i; }
    }

    // The body stops well clear of the footer: the status line and the buttons
    // each own a band of their own, so neither can be drawn over the list.
    float bodyY = m.y + 74;                    // room for a per-tab hint line
    float bodyH = mh - 74 - SP_FOOTER_H;
    Rectangle body = { m.x + SP_PAD, bodyY, mw - 2*SP_PAD, bodyH };

    // -- editing pane takes over the body -----------------------------------
    if (s_editing)
    {
        DrawText(s_editElem >= 0 ? "EDIT ELEMENT TEXT"
                                 : (s_editIdx < 0 ? "NEW STRING" : "EDIT STRING"),
                 (int)body.x, (int)body.y, 12, SP_DIM);

        float th = ZenTextAreaHeight(s_buf, 10);
        if (th < 60.0f) th = 60.0f;
        if (th > bodyH - 40.0f) th = bodyH - 40.0f;
        Rectangle ta = { body.x, body.y + 20, body.width, th };
        DrawRectangleRec((Rectangle){ ta.x-2, ta.y-2, ta.width+4, ta.height+4 }, SP_ACCENT);
        ZenEditTextArea(ta, s_buf, ANIM_TEXT_LEN_MAX, true);

        DrawText("Shift+Enter adds a line - Enter or Save commits - Esc cancels",
                 (int)body.x, (int)(ta.y + ta.height + 8), 10, SP_DIM);

        // editing an entry others share changes it for all of them: say so.
        if (s_editIdx >= 0 && s_editElem < 0)
        {
            int users = AnimDocStringUsers(&zen.doc, s_editIdx);
            if (users > 1)
                DrawText(TextFormat("shared by %d keys - editing changes all of them",
                                    users),
                         (int)body.x, (int)(ta.y + ta.height + 24), 10,
                         (Color){ 232, 190, 110, 255 });
        }

        float bh = 30, by = m.y + mh - bh - SP_PAD;
        if (GuiButton((Rectangle){ m.x + mw - 2*96 - SP_PAD - 6, by, 96, bh }, "Save"))
        {
            AudioPlayButton();
            ZenUndoPush();
            if (s_editElem >= 0)
            {
                // An element's PRIVATE words: they belong to the element, not
                // the pool, so this edits e->text and leaves the pool alone.
                TextCopy(zen.doc.elems[s_editElem].text, s_buf);
            }
            else if (s_editIdx < 0)
            {
                int at = AnimDocAddString(&zen.doc, s_buf);
                if (at < 0) TextCopy(s_status, "pool is full");
                else { s_sel = at; TextCopy(s_status, "string added"); }
            }
            else
            {
                TextCopy(zen.doc.strings[s_editIdx].text, s_buf);
                // every element pointing here shows the new words immediately
                for (int i = 0; i < zen.doc.elemCount; i++)
                    if (zen.doc.elems[i].kind == AE_TEXT &&
                        ElemStringIdx(&zen.doc.elems[i]) == s_editIdx)
                        TextCopy(zen.doc.elems[i].text, s_buf);
                TextCopy(s_status, "string updated");
            }
            zen.docDirty = true;
            s_editing = false; s_editElem = -1; ZenTextAreaClose();
        }
        if (GuiButton((Rectangle){ m.x + mw - 96 - SP_PAD, by, 96, bh }, "Cancel"))
        { AudioPlayButton(); s_editing = false; s_editElem = -1; ZenTextAreaClose(); }
        return;
    }

    float hintY = body.y - 16;      // the per-tab hint sits ABOVE the list

    // -- STRINGS tab --------------------------------------------------------
    if (s_tab == 0)
    {
        // Elements holding words of their own appear under a second heading:
        // they are strings in this document too, just not in the pool yet.
        int privates = 0;
        for (int i = 0; i < zen.doc.elemCount; i++)
            if (HasPrivateText(&zen.doc.elems[i])) privates++;

        DrawText(s_armed >= 0
                 ? TextFormat("string %02d armed - go to Elements and press Set on a row",
                              s_armed)
                 : "Click selects - double-click edits - Assign points it at an element.",
                 (int)body.x, (int)hintY, 10, s_armed >= 0 ? SP_ACCENT : SP_DIM);

        if (CheckCollisionPointRec(mouse, body)) s_scroll += GetMouseWheelMove() * 28.0f;
        // pool rows + (heading + private rows) when there are any
        float contentH = used * (SP_ROW_H + 6.0f)
                       + (privates ? 24.0f + privates * (SP_ROW_H + 6.0f) : 0.0f);
        float maxS = contentH - body.height; if (maxS < 0) maxS = 0;
        if (s_scroll < -maxS) s_scroll = -maxS;
        if (s_scroll > 0) s_scroll = 0;

        if (used == 0 && privates == 0)
            DrawText("No strings yet - New string, or type into a text element.",
                     (int)body.x, (int)(body.y + 10), 11, SP_DIM);

        BeginScissorMode((int)body.x, (int)body.y, (int)body.width, (int)body.height);
        float ry = body.y + s_scroll;

        for (int i = 0; i < zen.doc.stringCount; i++)
        {
            if (!zen.doc.strings[i].used) continue;      // a hole: index kept, not shown
            Rectangle r = { body.x, ry, body.width, SP_ROW_H };
            bool hot = CheckCollisionPointRec(mouse, r) &&
                       CheckCollisionPointRec(mouse, body);
            bool sel = (s_sel == i);

            DrawRectangleRec(r, sel ? SP_ROW_HOT : (hot ? SP_ROW : SP_PANEL));
            if (sel) DrawRectangleRec((Rectangle){ r.x, r.y, 3, r.height }, SP_ACCENT);
            if (s_armed == i)
                DrawRectangleLinesEx(r, 1.0f, SP_ACCENT);

            DrawText(TextFormat("%02d", i), (int)(r.x + 10), (int)(r.y + 11), 11, SP_DIM);
            DrawText(ZenTextPreview(zen.doc.strings[i].text, 30),
                     (int)(r.x + 40), (int)(r.y + 10), 12, SP_TEXT);

            int users = AnimDocStringUsers(&zen.doc, i);
            const char *u = users ? TextFormat("used by %d", users) : "unused";
            DrawText(u, (int)(r.x + r.width - 148), (int)(r.y + 12), 10,
                     users ? SP_ACCENT : SP_DIM);

            // per-row Assign: arms this string and switches to the Elements tab,
            // where the second click does the writing.
            Rectangle ab = { r.x + r.width - 68, r.y + 6, 60, SP_ROW_H - 12 };
            bool aHot = CheckCollisionPointRec(mouse, ab) &&
                        CheckCollisionPointRec(mouse, body);
            DrawRectangleRec(ab, aHot ? SP_ACCENT : SP_ROW_HOT);
            DrawRectangleLinesEx(ab, 1.0f, SP_EDGE);
            DrawText("assign", (int)(ab.x + 12), (int)(ab.y + 6), 10,
                     aHot ? SP_BG : SP_TEXT);

            if (hot && ZenLayerActive(ZEN_LAYER_OVERLAY) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
            {
                AudioPlayButton();
                if (aHot)
                {
                    s_sel = i; s_armed = i; s_tab = 1; s_elemScroll = 0.0f;
                    TextCopy(s_status,
                             TextFormat("string %02d armed - press Set on an element", i));
                }
                else
                {
                    double now = GetTime();
                    bool dbl = (s_lastClickRow == i) && (now - s_lastClick < 0.35);
                    s_sel = i; s_lastClickRow = i; s_lastClick = now;
                    if (dbl)
                    {
                        // double-click opens the entry for editing, the gesture
                        // the user expects from a list of editable text.
                        TextCopy(s_buf, zen.doc.strings[i].text);
                        s_editIdx = i; s_editElem = -1; s_editing = true;
                    }
                    else s_status[0] = '\0';
                }
            }
            ry += SP_ROW_H + 6.0f;
        }

        if (privates)
        {
            DrawText("IN ELEMENTS ONLY  (not pooled)",
                     (int)(body.x + 4), (int)(ry + 6), 10, SP_DIM);
            ry += 24.0f;

            for (int i = 0; i < zen.doc.elemCount; i++)
            {
                AnimElem *e = &zen.doc.elems[i];
                if (!HasPrivateText(e)) continue;

                Rectangle r = { body.x, ry, body.width, SP_ROW_H };
                bool hot = CheckCollisionPointRec(mouse, r) &&
                           CheckCollisionPointRec(mouse, body);
                DrawRectangleRec(r, hot ? SP_ROW : SP_PANEL);
                DrawRectangleRec((Rectangle){ r.x, r.y, 3, r.height }, SP_DIM);

                DrawText(e->name, (int)(r.x + 10), (int)(r.y + 4), 10, SP_DIM);
                DrawText(ZenTextPreview(e->text, 34), (int)(r.x + 10), (int)(r.y + 18), 11,
                         SP_TEXT);

                // "pool it" makes those private words a shared entry, which is
                // the only way another element can ever key them.
                Rectangle pb = { r.x + r.width - 68, r.y + 6, 60, SP_ROW_H - 12 };
                bool pHot = CheckCollisionPointRec(mouse, pb) &&
                            CheckCollisionPointRec(mouse, body);
                DrawRectangleRec(pb, pHot ? SP_ACCENT : SP_ROW_HOT);
                DrawRectangleLinesEx(pb, 1.0f, SP_EDGE);
                DrawText("pool it", (int)(pb.x + 12), (int)(pb.y + 6), 10,
                         pHot ? SP_BG : SP_TEXT);

                if (hot && ZenLayerActive(ZEN_LAYER_OVERLAY) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
                {
                    AudioPlayButton();
                    if (pHot)
                    {
                        // AssignString pushes its own undo step; a second push
                        // here would cost two Ctrl+Z to walk back one gesture.
                        int at = AnimDocAddString(&zen.doc, e->text);
                        if (at < 0) TextCopy(s_status, "pool is full");
                        else { AssignString(i, at); s_sel = at; }
                    }
                    else
                    {
                        double now = GetTime();
                        // negative row ids keep element rows from colliding with
                        // pool indices in the double-click tracker
                        int rowId = -(i + 1);
                        bool dbl = (s_lastClickRow == rowId) && (now - s_lastClick < 0.35);
                        s_lastClickRow = rowId; s_lastClick = now;
                        if (dbl)
                        {
                            TextCopy(s_buf, e->text);
                            s_editElem = i; s_editIdx = -1; s_editing = true;
                        }
                    }
                }
                ry += SP_ROW_H + 6.0f;
            }
        }
        EndScissorMode();
    }
    // -- ELEMENTS tab: who uses what, and where an armed string lands --------
    else
    {
        int textElems = 0;
        for (int i = 0; i < zen.doc.elemCount; i++)
            if (zen.doc.elems[i].kind == AE_TEXT) textElems++;

        if (textElems == 0)
            DrawText("No text elements in this animation.",
                     (int)body.x, (int)(body.y + 10), 11, SP_DIM);
        else if (s_armed >= 0)
            DrawText(TextFormat("SETTING string %02d  \"%s\"  - press Set on a row",
                                s_armed, ZenTextPreview(AnimDocStringAt(&zen.doc, s_armed), 30)),
                     (int)body.x, (int)hintY, 10, SP_ACCENT);
        else
            DrawText("Press Assign on a string in the Strings tab to set it here.",
                     (int)body.x, (int)hintY, 10, SP_DIM);

        if (CheckCollisionPointRec(mouse, body)) s_elemScroll += GetMouseWheelMove() * 28.0f;
        float contentH = textElems * (SP_ROW_H + 6.0f);
        float maxS = contentH - body.height; if (maxS < 0) maxS = 0;
        if (s_elemScroll < -maxS) s_elemScroll = -maxS;
        if (s_elemScroll > 0) s_elemScroll = 0;

        BeginScissorMode((int)body.x, (int)body.y, (int)body.width, (int)body.height);
        float ry = body.y + s_elemScroll;
        for (int i = 0; i < zen.doc.elemCount; i++)
        {
            AnimElem *e = &zen.doc.elems[i];
            if (e->kind != AE_TEXT) continue;

            Rectangle r = { body.x, ry, body.width, SP_ROW_H };
            bool hot = CheckCollisionPointRec(mouse, r) &&
                       CheckCollisionPointRec(mouse, body);
            bool isSelElem = (i == zen.selElem);
            DrawRectangleRec(r, hot ? SP_ROW : SP_PANEL);
            if (isSelElem) DrawRectangleRec((Rectangle){ r.x, r.y, 3, r.height }, SP_ACCENT);

            DrawText(e->name, (int)(r.x + 12), (int)(r.y + 5), 12, SP_TEXT);

            int idx = ElemStringIdx(e);
            const char *s = AnimDocStringAt(&zen.doc, idx);
            DrawText(s ? TextFormat("-> %02d  %s", idx, ZenTextPreview(s, 24))
                       : TextFormat("own text: %s", ZenTextPreview(e->text, 24)),
                     (int)(r.x + 12), (int)(r.y + 19), 10, s ? SP_ACCENT : SP_DIM);

            // Set is live only while a string is armed - nothing is written by
            // simply browsing this list.
            Rectangle sb = { r.x + r.width - 68, r.y + 6, 60, SP_ROW_H - 12 };
            bool sHot = (s_armed >= 0) && CheckCollisionPointRec(mouse, sb) &&
                        CheckCollisionPointRec(mouse, body);
            DrawRectangleRec(sb, sHot ? SP_ACCENT
                                      : (s_armed >= 0 ? SP_ROW_HOT : SP_PANEL));
            DrawRectangleLinesEx(sb, 1.0f, s_armed >= 0 ? SP_EDGE : SP_PANEL);
            DrawText("set", (int)(sb.x + 22), (int)(sb.y + 6), 10,
                     sHot ? SP_BG : (s_armed >= 0 ? SP_TEXT : SP_DIM));

            if (sHot && ZenLayerActive(ZEN_LAYER_OVERLAY) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
            { AudioPlayButton(); AssignString(i, s_armed); }

            ry += SP_ROW_H + 6.0f;
        }
        EndScissorMode();
    }

    // -- status band, then the button band ----------------------------------
    float bh = 30, by = m.y + mh - bh - SP_PAD;
    if (s_status[0])
        DrawText(s_status, (int)(m.x + SP_PAD), (int)(by - 16), 10, SP_DIM);

    float bw = 84, gap = 6, bx = m.x + SP_PAD;

    bool full = (used >= ANIM_STRINGS_MAX);
    if (full) GuiDisable();
    if (GuiButton((Rectangle){ bx, by, bw, bh }, "New"))
    {
        AudioPlayButton();
        s_buf[0] = '\0'; s_editIdx = -1; s_editElem = -1; s_editing = true;
    }
    if (full) GuiEnable();
    bx += bw + gap;

    bool haveSel = (s_sel >= 0 && AnimDocStringAt(&zen.doc, s_sel) != NULL);
    if (!haveSel) GuiDisable();
    if (GuiButton((Rectangle){ bx, by, bw, bh }, "Edit"))
    {
        AudioPlayButton();
        TextCopy(s_buf, AnimDocStringAt(&zen.doc, s_sel));
        s_editIdx = s_sel; s_editElem = -1; s_editing = true;
    }
    if (haveSel)
        ZenTip((Rectangle){ bx, by, bw, bh },
               "Edit the selected string - double-clicking its row does the same");
    if (!haveSel) GuiEnable();
    bx += bw + gap;

    // an entry still referenced cannot be deleted - the tracks pointing at it
    // would be left dangling, so the button says why instead of failing.
    int selUsers = haveSel ? AnimDocStringUsers(&zen.doc, s_sel) : 0;
    bool canDel = haveSel && selUsers == 0;
    if (!canDel) GuiDisable();
    if (GuiButton((Rectangle){ bx, by, bw, bh }, "Delete"))
    {
        AudioPlayButton();
        ZenUndoPush();
        if (AnimDocRemoveString(&zen.doc, s_sel))
        {
            TextCopy(s_status, "string deleted");
            if (s_armed == s_sel) s_armed = -1;
            s_sel = -1; zen.docDirty = true;
        }
    }
    if (!canDel) GuiEnable();
    if (haveSel && selUsers > 0)
        ZenTip((Rectangle){ bx, by, bw, bh },
               TextFormat("In use by %d key(s) - repoint them first", selUsers));
    bx += bw + gap;

    // an armed assignment is abandonable without leaving the modal
    if (s_armed >= 0 && GuiButton((Rectangle){ bx, by, bw, bh }, "Cancel set"))
    { AudioPlayButton(); s_armed = -1; TextCopy(s_status, "assignment cancelled"); }

    if (GuiButton((Rectangle){ m.x + mw - 92 - SP_PAD, by, 92, bh }, "Close"))
    { AudioPlayButton(); s_open = false; s_armed = -1; AnimDocGCStrings(&zen.doc); }
}
