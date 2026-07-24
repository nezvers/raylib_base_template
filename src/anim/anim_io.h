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
#define ANIM_GROUP_PROPS 3
typedef struct { const char *name; int props[ANIM_GROUP_PROPS]; int propCount; } AnimPropGroup;

int                  AnimGroupCountFor(int elemKind);        // # groups for a kind
const AnimPropGroup *AnimGroupAt(int elemKind, int index);   // NULL if out of range
int                  AnimGroupIndexOfProp(int elemKind, int prop);  // -1 if none

const char *AnimElemKindName(int kind);   // AE_* -> "text"/"shape"/"global"

const char *AnimShapeKindName(int kind);        // SHAPE_* -> "rect" etc.
int         AnimShapeKindByName(const char *s); // unknown -> SHAPE_RECT

#endif // ANIM_IO_H
