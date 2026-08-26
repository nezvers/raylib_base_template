// ============================================================================
//  strategy_bindings.h  -  which authored asset stands in for which game role
//
//  An .sga is a LOOK and nothing more. This file is the other half: a mapping
//  from each game role - every UnitKind, BuildingKind and NodeKind - to the
//  asset that should be drawn for it, or to nothing at all, which means "keep
//  the built-in model".
//
//  WHY A SEPARATE FILE. Keeping the mapping out of the assets is what makes a
//  whole visual set swappable as one unit: two binding files over the same
//  asset folder are two complete art directions, and neither edits a single
//  .sga. It also keeps an asset shareable on its own - handing someone a model
//  never drags your role assignments along with it.
//
//  A CATEGORY NEVER RESTRICTS A BINDING. The taxonomy in strategy_asset.h
//  exists to FIND assets, and this module deliberately does not consult it: a
//  "resource/tree" asset bound to the town hall is a supported thing to do.
//  That is the point - a warrior may look like a worker, a town hall like a
//  tree - so nothing here validates a role against a category, and the UI says
//  so in as many words rather than leaving it to look like a missing check.
//
//  NAMES, NOT INDICES. A binding stores the asset's NAME. An index into
//  whatever order the directory scan happened to return is a promise the next
//  run cannot keep: add a file, rename one, delete one, and every index after
//  it points at a different model with no way to notice. This is the same
//  name-not-id rule the baked easings follow (strategy_asset_ease.h), for the
//  same reason, and anim_io.c's shape references were the first place the
//  project hit it.
//
//  An unresolvable name is NOT an error and is NOT erased. The asset file may
//  simply be missing on this machine, and quietly dropping the binding would
//  turn "you forgot to copy a file" into "your art direction is gone". The
//  binding is kept, the role falls back to its built-in model, and the UI
//  reports the name it could not find.
// ============================================================================

#ifndef STRATEGY_BINDINGS_H
#define STRATEGY_BINDINGS_H

#include "strategy_asset.h"
#include <stdbool.h>
#include <stdint.h>

#define SGB_FILE  "bindings.sgb"    // lives in SGA_DIR, beside the assets

// The three role families. They are kept apart rather than flattened into one
// list because the game's enums are independent - UnitKind 0 and BuildingKind 0
// are both valid and mean different things.
typedef enum {
    SGB_ROLE_UNIT = 0,
    SGB_ROLE_BUILDING,
    SGB_ROLE_NODE,
    SGB_ROLE_FAMILY_COUNT
} SgbRoleFamily;

// Capacities are FIXED, not tiered, for the same reason the asset file's are
// (strategy_asset_io.c): the record is one struct blit, so its size is the
// file's size, and a size that followed the build tier would let one build
// misread another's file. They sit comfortably above the game's current enums;
// the _Static_asserts in strategy_bindings.c fail the build if a family ever
// outgrows its slot rather than letting roles fall off the end.
#define SGB_UNITS_MAX      32
#define SGB_BUILDINGS_MAX  32
#define SGB_NODES_MAX      16

typedef struct {
    // One asset name per role, empty meaning "use the built-in model". Indexed
    // by the game's own enum value, so a role keeps its slot even if the enum
    // grows - appending a UnitKind cannot repoint an existing binding.
    char unit[SGB_UNITS_MAX][SGA_NAME_MAX];
    char building[SGB_BUILDINGS_MAX][SGA_NAME_MAX];
    char node[SGB_NODES_MAX][SGA_NAME_MAX];
} SgaBindings;

// ---------------------------------------------------------------------------
//  The module owns one set, the way settings_state owns one Settings.
// ---------------------------------------------------------------------------
SgaBindings *StrategyBindingsGet(void);

// Clears every binding back to "built-in". Not a file operation - StrategyBindingsSave
// still has to be called for it to stick.
void StrategyBindingsClear(void);

// How many roles a family actually has. The game's enum counts live in
// strategy_types.h, which this module does not include (it stays headless);
// the app installs them once at startup. Unset, every family reports its
// capacity, which is what a test wants.
void StrategyBindingsSetRoleCounts(int units, int buildings, int nodes);
int  StrategyBindingsRoleCount(int family);

const char *StrategyBindingsFamilyName(int family);     // bad id -> "UNIT"

// ---------------------------------------------------------------------------
//  Reading and writing one binding
// ---------------------------------------------------------------------------
// The bound asset NAME for a role, or "" when the role uses its built-in. Never
// NULL, so a caller can print it without a guard.
const char *StrategyBindingGet(int family, int role);

// Bind a role to an asset name; NULL or "" clears the binding. Returns false
// only for an out-of-range family/role - a name that matches no asset on this
// machine is accepted on purpose (see the header note).
bool StrategyBindingSet(int family, int role, const char *assetName);

// True when this role has a name recorded, whether or not it resolves.
bool StrategyBindingIsBound(int family, int role);

// The asset a role resolves to right now, looked up BY NAME in `assets`, or
// NULL for "draw the built-in". A bound-but-missing name also returns NULL -
// use StrategyBindingIsBound to tell the two apart, which is what lets the UI
// say "bound to X, which is not here" instead of silently showing the built-in.
const SgaAsset *StrategyBindingResolve(int family, int role,
                                       const SgaAsset *assets, int assetCount);

// Every binding pointing at `oldName` is repointed to `newName`, which keeps a
// rename from silently unbinding a set. NULL/"" newName clears them instead -
// what a delete wants. Returns how many bindings changed.
int StrategyBindingsRename(const char *oldName, const char *newName);

// How many bindings name an asset that is not in `assets`. What the UI counts
// to warn "3 roles point at assets that are missing".
int StrategyBindingsMissingCount(const SgaAsset *assets, int assetCount);

// ---------------------------------------------------------------------------
//  Persistence. Same versioned whole-struct blit as the asset file.
// ---------------------------------------------------------------------------
// Loading a file that is absent is SUCCESS with everything cleared: no bindings
// yet is the normal starting state, not a failure to report.
bool StrategyBindingsLoad(void);
bool StrategyBindingsSave(void);

const char *StrategyBindingsPath(void);     // "assets_strategy/bindings.sgb"

#endif // STRATEGY_BINDINGS_H
