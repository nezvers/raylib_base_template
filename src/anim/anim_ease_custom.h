#ifndef ANIM_EASE_CUSTOM_H
#define ANIM_EASE_CUSTOM_H

// ============================================================================
//  anim_ease_custom.h  -  user-authored multi-segment bezier easings + hides
//
//  A curve is a chain of knots from (0,0) to (1,1); every neighbouring pair
//  spans one cubic bezier segment whose inner controls are the left knot's
//  out-handle and the right knot's in-handle. Two knots = the classic CSS
//  timing function; more knots reproduce bounce / elastic / staircase shapes
//  a single cubic cannot. Two knots at (nearly) the same x make a hold step.
//
//  Custom easings live in fixed slots addressed by runtime ids
//  ANIM_EASE_COUNT + slot. .cfg files reference easings by NAME only, so the
//  ids never need to be stable across runs: AnimEaseByName resolves builtins
//  first, then used custom slots, and unknown names degrade to linear (which
//  is also what happens in builds/editors that never load the custom file).
//
//  The whole set persists in one shared file, anims/_easings.cfg (the leading
//  underscore keeps it out of animation listings). Grammar, one entry per
//  line, `#` comments:
//
//      ease <name> <n> then n * (x y ix iy ox oy)   # n knots, handles are
//                                                   # offsets from the knot
//      ease <name> <x1> <y1> <x2> <y2>   # legacy single cubic, still read
//      hide <name>                       # builtin or custom; dropdown filter
//                                        # only - hidden easings still evaluate
// ============================================================================

#include <stdbool.h>
#include "anim.h"   // ANIM_EASE_COUNT

#define ANIM_CUSTOM_EASE_MAX   32
#define ANIM_CUSTOM_NAME_MAX   24
#define ANIM_EASE_PTS_MAX       8   // knots per curve, both endpoints included

typedef struct
{
    float x, y;     // knot
    float ix, iy;   // in-handle  offset from the knot (unused on the first)
    float ox, oy;   // out-handle offset from the knot (unused on the last)
} AnimEasePt;

typedef struct
{
    char  name[ANIM_CUSTOM_NAME_MAX];
    AnimEasePt pts[ANIM_EASE_PTS_MAX];
    int   ptCount;          // >= 2; pts[0].x is 0 and pts[ptCount-1].x is 1
    bool  hidden;           // filtered from selection lists (never from eval)
    bool  used;             // slot occupied
} AnimCustomEase;

// -- persistence -------------------------------------------------------------
bool AnimCustomEasesLoad(const char *path);   // false if file missing/unreadable
bool AnimCustomEasesSave(const char *path);

// -- slots -------------------------------------------------------------------
// Add returns the runtime ease id (>= ANIM_EASE_COUNT), or -1 when full /
// name invalid / name already taken by a builtin or custom.
int  AnimCustomEaseAdd(const char *name, const AnimEasePt *pts, int ptCount);
// Replaces the shape of an existing custom in place; the name is the .cfg key
// and never changes here.
bool AnimCustomEaseUpdate(int easeId, const AnimEasePt *pts, int ptCount);
bool AnimCustomEaseRemove(int easeId);        // frees the slot
const AnimCustomEase *AnimCustomEaseGet(int easeId);   // NULL if not a used custom

// -- hide flags (work on builtins AND customs, by runtime id) ----------------
void AnimEaseSetHidden(int easeId, bool hidden);
bool AnimEaseIsHidden(int easeId);

// -- iteration for dropdowns -------------------------------------------------
// Total id range to walk: [0, AnimEaseIdRange()). Sparse: skip ids where
// AnimEaseIdValid() is false (unused custom slots).
int  AnimEaseIdRange(void);
bool AnimEaseIdValid(int easeId);

// -- eval hook for anim.c (bezier y at x=p, CSS-timing-function style) -------
float AnimCustomEaseEval(int easeId, float p); // unused slot -> p (linear)
// Same math on a point list that isn't stored yet (the graph editor previews
// its staged curve with this).
float AnimEasePtsEval(const AnimEasePt *pts, int ptCount, float p);
// Fills a 2-knot curve from the legacy single-cubic control points.
void  AnimEasePtsFromCubic(AnimEasePt *out, float x1, float y1, float x2, float y2);
const char *AnimCustomEaseName(int easeId);    // NULL if not a used custom
int  AnimCustomEaseByName(const char *name);   // id or -1

#endif
