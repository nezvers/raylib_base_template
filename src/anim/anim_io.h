// ============================================================================
//  anim_io.h  -  save / load an AnimDoc as a human-readable .cfg
//
//  Format follows the project's only other persistence (settings_state.c):
//  plain-text, line based, fopen + fprintf / fscanf. It is intentionally
//  hand-editable and diff-able (no JSON dependency, no binary). See anim_io.c
//  for the exact grammar; a saved file round-trips through Load -> Save byte
//  for byte (aside from float formatting).
//
//  Also exposes the name<->value lookup tables the editor uses for dropdowns:
//  easing functions, property kinds and element kinds all have stable string
//  names so the .cfg (and the editor UI) can talk about them by name.
// ============================================================================

#ifndef ANIM_IO_H
#define ANIM_IO_H

#include "anim.h"
#include <stdbool.h>
#include <stdio.h>      // FILE, for the shared element reader/writer below

// Save/load a single document. Return false on file error (missing file on
// load leaves `doc` initialized-empty). `path` is relative to CWD like
// settings.cfg (e.g. "anim/intro.cfg" or an absolute path).
bool AnimDocSave(const AnimDoc *doc, const char *path);
bool AnimDocLoad(AnimDoc *doc, const char *path);

// --- did the last load FIT? -------------------------------------------------
// The capacities in anim.h are a TWO-TIER build setting: CMakeLists.txt raises
// KEYS/TRACKS/ELEMS/STRINGS on desktop and leaves the Web build on the smaller
// values that fit its fixed 128 MB emscripten heap. So a document authored in
// the desktop editor can legitimately be too big for the web build, and
// AnimDocLoad still returns TRUE for it - it loads what fits and DROPS the
// rest, which without this would be a silently mangled animation.
//
// Call AnimDocLoadTruncated() right after a successful AnimDocLoad to find out,
// and AnimDocLoadTruncMessage() to render what was lost for the user. Valid
// only until the next AnimDocLoad (each one resets the counts).
typedef struct {
    int elems;       // elements past ANIM_ELEMS_MAX (with everything inside)
    int tracks;      // tracks past ANIM_TRACKS_MAX
    int keys;        // keyframes past ANIM_KEYS_MAX
    int strings;     // shared strings whose saved index is >= ANIM_STRINGS_MAX
    int signals;     // signals past ANIM_SIGNALS_MAX
    int sigTargets;  // signal targets past ANIM_SIG_TARGETS_MAX
    int pauses;      // pause markers past ANIM_PAUSES_MAX
} AnimLoadTrunc;

bool AnimDocLoadTruncated(void);          // true if ANY of the above is nonzero
const AnimLoadTrunc *AnimDocLoadTrunc(void);   // the per-capacity breakdown
void AnimDocLoadTruncReset(void);         // AnimDocLoad calls this itself

// Renders the breakdown into `out` as one "<n> <what> (max <cap>)" line per
// exceeded capacity. Returns the length written (0, with out[0]='\0', when the
// last load fit).
int AnimDocLoadTruncMessage(char *out, int cap);

// --- shared `elem ... end` grammar (one writer, one reader) -----------------
// Used by AnimDocSave/Load AND by the element library (anim_library.*), so an
// element serializes identically wherever it is stored.

// Write one element as an `elem ... end` block, every line prefixed by `ind`.
void AnimElemWriteCfg(FILE *f, const AnimElem *e, const char *ind);

// Consume ONE already-read token `key` if it belongs inside an `elem` block
// (base fields, `track`, `key`, `end`), applying it to `curElem` and the open
// `*curTrack`. Returns false if the token is not element-scoped, leaving the
// stream untouched so the caller can handle it. `curElem` may be NULL: tokens
// are then consumed but discarded (keeps the stream in sync).
bool AnimElemReadCfgToken(FILE *f, const char *key, AnimElem *curElem,
                          AnimTrack **curTrack);

// --- name <-> value tables (stable strings; used by .cfg and editor UI) -----
// Easing name<->id lookups live in anim.h (AnimEaseName/AnimEaseByName).

// Property kinds valid for a given element kind (for the "add track" dropdown).
const char *AnimPropName(int prop);                     // AP_* -> "pos_x" etc.
int         AnimPropByName(const char *name, int elemKind);  // name -> AP_* (-1)
int         AnimPropCountFor(int elemKind);             // # props for a kind
int         AnimPropAt(int elemKind, int index);        // the index-th AP_* prop

// --- property GROUPS (editor UX: fewer, consolidated track targets) ---------
// A group bundles the fine-grained AP_* props that belong to one logical target
// (Position = pos_x+pos_y, Color = color+alpha, ...). Purely a presentation +
// coordinated-editing layer: the underlying per-prop tracks and the .cfg format
// are unchanged. `props` lists the member AP_* values (up to ANIM_GROUP_PROPS).
// 5 = the "crumble" group: amount + the four params that shape the scatter.
#define ANIM_GROUP_PROPS 5
typedef struct { const char *name; int props[ANIM_GROUP_PROPS]; int propCount; } AnimPropGroup;

int                  AnimGroupCountFor(int elemKind);        // # groups for a kind
const AnimPropGroup *AnimGroupAt(int elemKind, int index);   // NULL if out of range
int                  AnimGroupIndexOfProp(int elemKind, int prop);  // -1 if none

const char *AnimElemKindName(int kind);   // AE_* -> "text"/"shape"/"global"

const char *AnimShapeKindName(int kind);        // SHAPE_* -> "rect" etc.
int         AnimShapeKindByName(const char *s); // unknown -> SHAPE_RECT

#endif // ANIM_IO_H
