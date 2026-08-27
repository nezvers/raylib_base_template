// ============================================================================
//  strategy_map_catalog.h  -  the ONE in-memory set of authored maps
//
//  Same reasoning as strategy_catalog.h, one tier smaller. An SgmMap is ~256 KB
//  on the desktop tier, so a full SGM_MAPS_MAX array is ~2 MB - not the 74 MB
//  the asset catalog was built to stop being duplicated, but the map picker,
//  the forge and the game all want the same list, and a shared one means a save
//  is seen by all three at once instead of each holding a snapshot that drifts.
//
//  BORROWED, NOT OWNED. Every accessor hands back a `const SgmMap *` into the
//  array. Those pointers stay valid until the next Reload, which may move a map
//  to a different slot - so hold a NAME across a reload, never a pointer.
//  SgmCatalogFind turns a name back into a pointer cheaply.
//
//  HEADLESS, like the rest of src/strategy_map/: no UI calls, so it links into
//  map_tests.
// ============================================================================

#ifndef STRATEGY_MAP_CATALOG_H
#define STRATEGY_MAP_CATALOG_H

#include "strategy_map.h"
#include <stdbool.h>

// Loads the map folder ONCE. Safe to call from every screen's Enter(): a second
// call is a no-op, so no screen has to know whether it is the first to run.
void SgmCatalogLoad(void);

// Rescans SGM_DIR from disk. What the forge calls after a save or a delete.
// INVALIDATES every pointer previously handed out (see the header note).
void SgmCatalogReload(void);

// The shared array and how much of it is live.
const SgmMap *SgmCatalogMaps(void);
int           SgmCatalogCount(void);

// The map with this name, or NULL. The safe way to re-acquire a pointer after a
// reload.
const SgmMap *SgmCatalogFind(const char *name);

// True once Load/Reload has run. Lets a caller tell "no maps authored yet"
// apart from "the folder was never scanned".
bool SgmCatalogReady(void);

#endif // STRATEGY_MAP_CATALOG_H
