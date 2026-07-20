// ============================================================================
//  anim_library.h  -  a reusable shelf of pre-configured ANIM ELEMENTS
//
//  An animation document (anim.h) owns elements that only exist inside it, so
//  a text style or an animated shape had to be re-authored from scratch in
//  every new document. The library fixes that: it stores whole AnimElem values
//  - base properties AND all of their tracks/keyframes - under a name, in one
//  file shared by every animation. Insert one into a document and the copy
//  arrives already animated.
//
//  Storage is the SAME `elem ... end` grammar the .cfg documents use (see
//  AnimElemWriteCfg / AnimElemReadCfgToken in anim_io.h), so there is exactly
//  one element serializer in the codebase and a library entry is a plain,
//  hand-editable text block like everything else.
//
//  Fixed capacity, no heap, plain values throughout (an AnimLibrary can be
//  memcpy'd) - same constraints as AnimDoc.
// ============================================================================

#ifndef ANIM_LIBRARY_H
#define ANIM_LIBRARY_H

#include "anim.h"
#include <stdbool.h>

#define ANIM_LIB_MAX 64          // entries in the library

// One shelved element. `name` is the LIBRARY key (what the picker lists); the
// element keeps its own elem->name, which is what it is called once inserted.
typedef struct {
    char     name[ANIM_NAME_MAX];
    AnimElem elem;
} AnimLibEntry;

typedef struct {
    AnimLibEntry entries[ANIM_LIB_MAX];
    int          count;
} AnimLibrary;

// Empty the library (no file touched).
void AnimLibraryInit(AnimLibrary *lib);

// Load/save the whole library. Load on a missing file leaves it empty and
// returns false (not an error condition - just an empty shelf).
bool AnimLibraryLoad(AnimLibrary *lib, const char *path);
bool AnimLibrarySave(const AnimLibrary *lib, const char *path);

// Shelve a copy of `e` under `name`. An existing entry with that name is
// OVERWRITTEN (the picker offers no duplicate names). Returns the entry index,
// or -1 if the library is full.
int AnimLibraryAdd(AnimLibrary *lib, const char *name, const AnimElem *e);

// Drop entry `idx` (shifts the tail down). Out-of-range indices are ignored.
void AnimLibraryRemove(AnimLibrary *lib, int idx);

// Rename entry `idx`. Ignored if the index is bad, the name is empty, or the
// name is already taken by a DIFFERENT entry. Returns true if it renamed.
bool AnimLibraryRename(AnimLibrary *lib, int idx, const char *name);

// Index of an entry by name, or -1.
int AnimLibraryFind(const AnimLibrary *lib, const char *name);

#endif // ANIM_LIBRARY_H
