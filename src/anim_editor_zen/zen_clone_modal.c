// ============================================================================
//  zen_clone_modal.c  -  Element > Clone from...
//
//  WHY THIS EXISTS. An animation gets ANIM_ELEMS_MAX (12) elements, and that is
//  the budget shown in the ELEMENTS panel title. A text or shape block that has
//  finished its part is dead weight: the natural move is to reuse it later in
//  the timeline rather than spend another slot on a near-copy. Doing that by
//  hand means re-typing every property that differs. This modal does it in one
//  step - pick the element whose LOOK you want, pick the time, and the delta
//  becomes keys on the element you already have selected.
//
//  DIRECTION. The SELECTED element RECEIVES; the element picked in the list is
//  the source, read at the playhead. Only properties that actually differ get a
//  key (see AnimDocCloneElemState), so the key budget is spent on real changes.
//
//  SELF IS A VALID SOURCE. The selected element appears in the list too: picking
//  it copies its own look at the playhead onto a later time, which is how a pose
//  is brought back without spending an element or retyping it.
//
//  THE OLD POSE IS PRESERVED. A property that had no track gets a key at 0
//  holding what it already showed, so the clone changes the element at the chosen
//  time and not across the whole timeline. The "hold" checkbox goes further and
//  pins that old pose just before the clone, making the change a step rather than
//  a slide from whatever key came before.
//
//  TEXT IS A KEY, NOT A REPLACEMENT. The words are an AP_T_STRING track keying
//  a string-pool index, so cloning text writes a KEY at the chosen time like
//  every other property - the element says the old thing before it and the new
//  thing after. It never overwrites the element's text for the whole timeline.
//
//  BASE-ONLY STATE. A shape's shapeKind still has no track - it is a whole-
//  element field. Its checkbox copies it, and because it is untracked that
//  change applies to the ENTIRE timeline. The modal says so; it is the one
//  non-obvious part left.
//
//  File-static state owned by this one caller (the zen_easing.c pattern), so it
//  costs nothing in ZenCtx and nothing in the undo snapshots.
// ============================================================================

#include "raylib.h"
#include "raygui.h"
#include "zen_internal.h"
#include "../anim/anim_io.h"
#include "../screen_state/screen_state.h"
#include "../audio_state/audio_state.h"
#include <stdlib.h>
#include <math.h>

#define CLONE_ROW_H  26.0f

static bool  s_open = false;
static int   s_srcIdx = -1;             // picked source element, -1 = none
static float s_scroll = 0.0f;
static char  s_timeBuf[16] = "0.00";    // destination time textbox
static bool  s_edTime = false;
static bool  s_copyShapeKind = true;
static bool  s_holdBefore = true;
static char  s_status[96] = "";

// ---------------------------------------------------------------------------
//  Accessors for the Gui()/ESC plumbing in anim_editor_zen.c
// ---------------------------------------------------------------------------
void ZenCloneShow(void)
{
    s_open = true;
    s_srcIdx = -1;
    s_scroll = 0.0f;
    s_edTime = false;
    s_status[0] = '\0';
    // default the destination to the playhead: cloning "here" is the common
    // case, and the user retypes only when placing it elsewhere.
    TextCopy(s_timeBuf, TextFormat("%.2f", zen.playhead));
}

bool ZenCloneOpen(void)   { return s_open; }
bool ZenCloneTyping(void) { return s_open && s_edTime; }

bool ZenCloneEscClose(void)
{
    if (!s_open) return false;
    s_open = false; s_edTime = false;
    return true;
}

// ---------------------------------------------------------------------------
//  Draw (unlocked overlay pass - see the note in anim_editor_zen.c)
// ---------------------------------------------------------------------------
void ZenCloneGui(void)
{
    if (!s_open) return;

    // the selection can vanish under us (element deleted from the menu while
    // the modal is up); close rather than clone onto a stale index.
    if (zen.selElem < 0 || zen.selElem >= zen.doc.elemCount) { s_open = false; return; }

    AnimElem *dst = &zen.doc.elems[zen.selElem];

    ScreenState *ss = ScreenStateGet();
    float W = (float)ss->width, H = (float)ss->height;
    float mw = 460, mh = 404;
    Rectangle m = { (W-mw)/2, (H-mh)/2, mw, mh };
    DrawRectangle(0, 0, (int)W, (int)H, (Color){ 0, 0, 0, 120 });
    DrawRectangleRec(m, (Color){ 40, 42, 48, 255 });
    DrawRectangleLinesEx(m, 1.0f, (Color){ 90, 94, 104, 255 });

    GuiLabel((Rectangle){ m.x+16, m.y+10, mw-32, 20 }, "CLONE ELEMENT STATE");

    // -- header: what receives, and when ------------------------------------
    ZenLabelTip((Rectangle){ m.x+16, m.y+32, mw-150, 20 },
                TextFormat("into  %s  [%s]", dst->name, AnimElemKindName(dst->kind)),
                "The SELECTED element receives the keys. Pick a different element "
                "in the Element panel to clone into that one instead.");

    GuiLabel((Rectangle){ m.x+mw-134, m.y+32, 40, 20 }, "at");
    if (GuiTextBox((Rectangle){ m.x+mw-108, m.y+32, 92, 22 }, s_timeBuf,
                   (int)sizeof(s_timeBuf), s_edTime))
        s_edTime = !s_edTime;
    ZenTip((Rectangle){ m.x+mw-108, m.y+32, 92, 22 },
           "Timeline position (seconds) the cloned keys are written at");

    float dstT = strtof(s_timeBuf, NULL);
    bool timeOk = (dstT >= 0.0f) && (dstT <= zen.doc.duration);

    ZenLabelTip((Rectangle){ m.x+16, m.y+56, mw-32, 18 },
                TextFormat("from an element as it looks at the playhead (%.2fs):",
                           zen.playhead),
                "The source is sampled at the CURRENT playhead - scrub first if "
                "you want a different moment of it.");

    // -- element list --------------------------------------------------------
    Rectangle list = { m.x+12, m.y+78, mw-24, mh-78-128 };
    if (CheckCollisionPointRec(GetMousePosition(), list))
        s_scroll += GetMouseWheelMove() * 24.0f;

    int rows = zen.doc.elemCount;                        // self included
    if (rows < 0) rows = 0;
    float contentH = rows * (CLONE_ROW_H + 4.0f);
    float maxScroll = contentH - list.height;
    if (maxScroll < 0) maxScroll = 0;
    if (s_scroll < -maxScroll) s_scroll = -maxScroll;
    if (s_scroll > 0) s_scroll = 0;

    if (rows == 0)
        GuiLabel((Rectangle){ list.x+4, list.y, list.width-8, 20 },
                 "(no element to clone from)");

    BeginScissorMode((int)list.x, (int)list.y, (int)list.width, (int)list.height);
    float ly = list.y + s_scroll;
    for (int i = 0; i < zen.doc.elemCount; i++)
    {
        AnimElem *e = &zen.doc.elems[i];
        Rectangle r = { list.x, ly, list.width, CLONE_ROW_H };
        bool self = (i == zen.selElem);

        // Cross-kind clones have nothing to copy (the AP_* ranges are disjoint
        // per kind), so they are shown but not selectable - clearer than hiding
        // them, which would look like the element had gone missing.
        bool sameKind = (e->kind == dst->kind);
        if (!sameKind) GuiDisable();
        if (GuiButton(r, ""))
        {
            AudioPlayButton();
            s_srcIdx = i;
            s_status[0] = '\0';
        }
        if (!sameKind) GuiEnable();

        GuiLabel((Rectangle){ r.x+8, r.y+4, r.width-16, r.height-8 },
                 self     ? TextFormat("%s   [itself - its pose at the playhead]",
                                       e->name)
                 : sameKind ? TextFormat("%s   [%s]", e->name, AnimElemKindName(e->kind))
                            : TextFormat("%s   [%s - different kind]",
                                         e->name, AnimElemKindName(e->kind)));
        if (i == s_srcIdx) DrawRectangleRec(r, (Color){ 90, 140, 220, 90 });
        ly += CLONE_ROW_H + 4.0f;
    }
    EndScissorMode();

    // -- base-only extras ----------------------------------------------------
    float cy = m.y + mh - 120;
    bool haveSrc = (s_srcIdx >= 0 && s_srcIdx < zen.doc.elemCount);
    const AnimElem *src = haveSrc ? &zen.doc.elems[s_srcIdx] : NULL;

    bool canText  = haveSrc && dst->kind == AE_TEXT  && src->kind == AE_TEXT;
    bool canShape = haveSrc && dst->kind == AE_SHAPE && src->kind == AE_SHAPE;

    GuiCheckBox((Rectangle){ m.x+16, cy, 18, 18 },
                "hold the old pose until this time", &s_holdBefore);
    ZenTip((Rectangle){ m.x+16, cy, 260, 18 },
           "Writes the element's current pose just BEFORE the clone time, so it "
           "snaps to the new look instead of sliding into it from the previous "
           "key. Leave it off for a smooth transition.");
    cy += 24;

    if (canText)
        ZenLabelTip((Rectangle){ m.x+16, cy, mw-32, 18 },
                    "the text becomes a string key at that time",
                    "The words are a tracked property: cloning writes a key, so "
                    "the element keeps its old text before that time and shows "
                    "the cloned text from it onwards.");

    if (canShape)
    {
        GuiCheckBox((Rectangle){ m.x+16, cy, 18, 18 }, "copy the shape kind",
                    &s_copyShapeKind);
        ZenTip((Rectangle){ m.x+16, cy, 200, 18 },
               "The shape kind has no track - copying it changes the element for "
               "the WHOLE timeline, not just at the chosen time.");
    }

    // -- status + footer -----------------------------------------------------
    if (s_status[0])
        GuiLabel((Rectangle){ m.x+16, cy+22, mw-32, 18 }, s_status);

    float bh = 28, by = m.y + mh - bh - 12;
    // Cloning an element from itself onto the same instant is a no-op by
    // definition, so it is refused rather than reported as "identical".
    bool selfSameT = haveSrc && (s_srcIdx == zen.selElem) &&
                     (fabsf(dstT - zen.playhead) <= ZEN_AUTOKEY_EPS);
    bool canClone = haveSrc && timeOk && !selfSameT && (src->kind == dst->kind);

    if (selfSameT)
        GuiLabel((Rectangle){ m.x+16, by-18, mw-200, 16 },
                 "pick a different time to clone onto");

    if (!canClone) GuiDisable();
    if (GuiButton((Rectangle){ m.x+mw-2*86-24, by, 86, bh }, "Clone"))
    {
        AudioPlayButton();
        ZenUndoPush();

        int n = AnimDocCloneElemState(&zen.doc, zen.selElem, dstT,
                                      s_srcIdx, zen.playhead, ZEN_AUTOKEY_EPS,
                                      s_holdBefore);
        int extras = 0;
        if (n >= 0)
        {
            if (canShape && s_copyShapeKind && dst->shapeKind != src->shapeKind)
            { dst->shapeKind = src->shapeKind; extras++; }
        }

        if (n < 0)
            TextCopy(s_status, "out of tracks or keys - clone incomplete (Ctrl+Z)");
        else if (n == 0 && extras == 0)
            TextCopy(s_status, "already identical at that time - nothing written");
        else
            TextCopy(s_status, TextFormat("wrote %d key%s%s at %.2fs", n,
                                          n == 1 ? "" : "s",
                                          extras ? " + base fields" : "", dstT));

        // duration must cover the new keys, or they sit past the end unseen.
        float maxT = AnimDocMaxKeyTime(&zen.doc);
        if (maxT > zen.doc.duration) zen.doc.duration = maxT;

        zen.docDirty = true;
    }
    if (!canClone) GuiEnable();

    if (GuiButton((Rectangle){ m.x+mw-86-12, by, 86, bh }, "Close"))
    { AudioPlayButton(); s_open = false; s_edTime = false; }
}
