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

// Save/load a single document. Return false on file error (missing file on
// load leaves `doc` initialized-empty). `path` is relative to CWD like
// settings.cfg (e.g. "anim/intro.cfg" or an absolute path).
bool AnimDocSave(const AnimDoc *doc, const char *path);
bool AnimDocLoad(AnimDoc *doc, const char *path);

// --- name <-> value tables (stable strings; used by .cfg and editor UI) -----
// Easing name<->id lookups live in anim.h (AnimEaseName/AnimEaseByName).

// Property kinds valid for a given element kind (for the "add track" dropdown).
const char *AnimPropName(int prop);                     // AP_* -> "pos_x" etc.
int         AnimPropByName(const char *name, int elemKind);  // name -> AP_* (-1)
int         AnimPropCountFor(int elemKind);             // # props for a kind
int         AnimPropAt(int elemKind, int index);        // the index-th AP_* prop

const char *AnimElemKindName(int kind);   // AE_* -> "text"/"shape"/"global"

const char *AnimShapeKindName(int kind);        // SHAPE_* -> "rect" etc.
int         AnimShapeKindByName(const char *s); // unknown -> SHAPE_RECT

#endif // ANIM_IO_H
