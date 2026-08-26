// ============================================================================
//  strategy_catalog.h  -  the ONE in-memory set of authored assets
//
//  WHY THIS EXISTS. An SgaAsset is large by design (fixed-capacity arrays, no
//  heap): 594 KB on the desktop tier, so a full SGA_ASSETS_MAX array is ~74 MB.
//  Before this module the showcase held that array as a private file-scope
//  member and nothing else could reach it, which meant the game could only join
//  in by allocating a SECOND one. Two copies of 74 MB to show the same models is
//  not a tradeoff, it is a bug waiting to be measured.
//
//  So the array lives here, once, and everyone borrows it. The showcase, the
//  binding view and the live game all read the same assets, which also means a
//  reload is seen by all three at the same instant rather than each holding a
//  private snapshot that drifts.
//
//  BORROWED, NOT OWNED. Every accessor hands back a `const SgaAsset *` into the
//  array. Those pointers stay valid until the next Reload, which may move an
//  asset to a different slot - so hold a NAME across a reload, never a pointer.
//  StrategyCatalogFind exists precisely so a name can be turned back into a
//  pointer cheaply.
//
//  STILL HEADLESS. Like the rest of src/strategy_asset/, this module never
//  calls into the game or into raylib's UI, so it links into sga_tests. It
//  learns the game's faction palette and enum sizes through the two hooks
//  installed in StrategyCatalogLoad.
// ============================================================================

#ifndef STRATEGY_CATALOG_H
#define STRATEGY_CATALOG_H

#include "strategy_asset.h"
#include "strategy_bindings.h"
#include <stdbool.h>

// Loads the asset folder and the binding file, ONCE. Safe to call from every
// screen's Enter(): a second call is a no-op, so no screen has to know whether
// it is the first one to run. Use Reload to force a rescan.
void StrategyCatalogLoad(void);

// Rescans SGA_DIR from disk. What the showcase calls after a save or a delete.
// INVALIDATES every pointer previously handed out (see the header note).
void StrategyCatalogReload(void);

// The shared array and how much of it is live.
const SgaAsset *StrategyCatalogAssets(void);
int             StrategyCatalogCount(void);

// The asset with this name, or NULL. The safe way to re-acquire a pointer after
// a reload, and what the binding resolve is built on.
const SgaAsset *StrategyCatalogFind(const char *name);

// The asset bound to a game role, or NULL meaning "draw the built-in model".
// A role bound to a name that is not on this machine also returns NULL - the
// binding is kept (see strategy_bindings.h), the drawing just falls back.
const SgaAsset *StrategyCatalogForRole(int family, int role);

// True once Load/Reload has run. Lets a caller tell "no assets authored yet"
// apart from "the folder was never scanned".
bool StrategyCatalogReady(void);

#endif // STRATEGY_CATALOG_H
