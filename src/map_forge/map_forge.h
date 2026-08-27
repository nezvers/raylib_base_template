#ifndef MAP_FORGE_H
#define MAP_FORGE_H

// ============================================================================
//  map_forge.h  -  FORGE - MAP, the authoring state for .sgm battlefields
//
//  The asset forge builds the things on the field; this builds the field. It
//  opens on ONE map, edits it in memory, and writes it out through
//  strategy_map_io.
//
//  Opening is always explicit - the state has no meaningful "empty" mode, so
//  the caller runs one of the Open* functions BEFORE transitioning here.
//  Enter() with nothing opened falls back to a blank map rather than showing a
//  void, but that path is a safety net, not a workflow. (Same contract as
//  strategy_forge.h, for the same reason: AppStateTransition runs Enter()
//  synchronously, so there is no chance to pass anything afterwards.)
// ============================================================================

#include "../app_state/app_state.h"
#include "../strategy_map/strategy_map.h"

// Start a brand new map at the default extent, one faction, all ground.
void MapForgeOpenNew(void);

// Start from an existing authored map. `m` is COPIED - the forge never holds a
// pointer into the catalog, which is rebuilt out from under it on every save.
//
// `remix` picks which of the two authoring gestures this is, exactly as the
// asset forge's StrategyForgeOpenAsset does:
//   false - EDIT.  The map stays bound to its own file, so SAVE writes over it.
//   true  - REMIX. The copy is renamed to a free name and UNBOUND from any
//           file, so SAVE creates a new .sgm and the original is never touched.
// The rename happens here rather than at the call site because "a free name" is
// a question only the map folder can answer (SgmMapNameFree).
void MapForgeOpenMap(const SgmMap *m, bool remix);

// Open by name from the catalog. False when no such map is loaded.
bool MapForgeOpenNamed(const char *name, bool remix);

// Where to go when the forge closes. Defaults to the main menu.
void MapForgeSetReturn(AppState *state);

#endif // MAP_FORGE_H
