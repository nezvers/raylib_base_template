#ifndef ANIM_EASE_CUSTOM_H
#define ANIM_EASE_CUSTOM_H

// ============================================================================
//  anim_ease_custom.h  -  user-authored cubic-bezier easings + hide flags
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
//      ease <name> <x1> <y1> <x2> <y2>   # cubic bezier, endpoints (0,0)-(1,1)
//      hide <name>                       # builtin or custom; dropdown filter
//                                        # only - hidden easings still evaluate
// ============================================================================

#include <stdbool.h>
#include "anim.h"   // ANIM_EASE_COUNT

#define ANIM_CUSTOM_EASE_MAX   32
#define ANIM_CUSTOM_NAME_MAX   24

typedef struct
{
    char  name[ANIM_CUSTOM_NAME_MAX];
    float x1, y1, x2, y2;   // the two inner bezier control points
    bool  hidden;           // filtered from selection lists (never from eval)
    bool  used;             // slot occupied
} AnimCustomEase;

// -- persistence -------------------------------------------------------------
bool AnimCustomEasesLoad(const char *path);   // false if file missing/unreadable
bool AnimCustomEasesSave(const char *path);

// -- slots -------------------------------------------------------------------
// Add returns the runtime ease id (>= ANIM_EASE_COUNT), or -1 when full /
// name invalid / name already taken by a builtin or custom.
int  AnimCustomEaseAdd(const char *name, float x1, float y1, float x2, float y2);
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

// -- eval hook for anim.c (cubic bezier y at x=p, CSS-timing-function style) -
float AnimCustomEaseEval(int easeId, float p); // unused slot -> p (linear)
const char *AnimCustomEaseName(int easeId);    // NULL if not a used custom
int  AnimCustomEaseByName(const char *name);   // id or -1

#endif
