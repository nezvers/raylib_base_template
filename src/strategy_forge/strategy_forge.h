#ifndef STRATEGY_FORGE_H
#define STRATEGY_FORGE_H

// ============================================================================
//  strategy_forge.h  -  authoring state for .sga assets
//
//  The showcase is the gallery; this is the workshop. It opens on ONE asset,
//  edits it in memory, and writes it out through strategy_asset_io.
//
//  Opening is always explicit - the state has no meaningful "empty" mode, so
//  the showcase calls one of the Open* functions BEFORE transitioning here.
//  Enter() with nothing opened falls back to a blank asset rather than showing
//  a void, but that path is a safety net, not a workflow.
// ============================================================================

#include "../app_state/app_state.h"
#include "../strategy_asset/strategy_asset.h"
#include "../examples/strategy_test/strategy_models.h"

// Start a brand new asset: one default part, an unused name, IDLE selected.
void StrategyForgeOpenNew(void);

// Start from an existing authored asset. `a` is COPIED - the forge never holds
// a pointer into the showcase's catalog, which is rebuilt out from under it on
// every save. `remix` true gives the copy a fresh name and clears the source
// file binding, so saving cannot overwrite what was opened.
void StrategyForgeOpenAsset(const SgaAsset *a, bool remix);

// Start from a BUILT-IN model, converting ModelPart -> SgaPart through the
// legacy ColorRole mapping. Always a remix: built-ins are never writable.
void StrategyForgeOpenBuiltin(const StrategyModel *m, const char *name,
                              int category, const char *subtype);

// Where to go when the forge closes. Defaults to the showcase.
void StrategyForgeSetReturn(AppState *state);

#endif // STRATEGY_FORGE_H
