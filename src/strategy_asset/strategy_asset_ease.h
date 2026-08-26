// ============================================================================
//  strategy_asset_ease.h  -  baking easing curves INTO an asset
//
//  This is the seam that makes a .sga standalone, and it is the only place
//  where the asset format meets the global easing set in src/anim/.
//
//  THE PROBLEM. Custom easing ids are ANIM_EASE_COUNT + slot, and which slot a
//  curve lands in depends on what anims/_easings.cfg happened to contain when
//  it was loaded. An id written to a file is therefore a promise the next run
//  cannot keep: delete one curve from the .cfg and every id after it shifts, so
//  the asset silently animates with a DIFFERENT shape than it was authored
//  with. anim_io.c hit exactly this with shape references and answered it the
//  same way - by name, never by id.
//
//  THE ANSWER, in two parts:
//    - builtins are stored by NAME and resolved with AnimEaseByName on use, so
//      "sineOut" means sineOut forever.
//    - customs are stored as their KNOTS. The curve travels inside the asset,
//      so it plays identically on a machine that has never seen _easings.cfg,
//      or after the slot it came from is deleted or reshaped.
//
//  Everything here is a pure transform between an SgaAsset and the anim easing
//  tables. strategy_asset.c stays free of src/anim/ so the data model keeps
//  linking headless; this file is where the dependency is allowed to live.
// ============================================================================

#ifndef STRATEGY_ASSET_EASE_H
#define STRATEGY_ASSET_EASE_H

#include "strategy_asset.h"

// Find (or bake) the asset-local ease slot for a runtime easing id, returning
// the index to store in SgaKey.ease. Builtins are recorded by name; customs are
// recorded by name AND knots. Re-baking an id already present returns the
// existing slot rather than duplicating it.
//
// Returns -1 when the asset's ease table is full, which the caller should
// report - a silently dropped curve would play as linear with no explanation.
// Passing a linear/invalid id returns -1 too, which SgaKey reads as linear.
int  StrategyAssetBakeEase(SgaAsset *a, int runtimeEaseId);

// The runtime id an asset-local slot corresponds to RIGHT NOW, for the forge's
// picker to show the curve as selected. -1 when the baked curve has no live
// counterpart, which is normal and not an error: a curve baked on another
// machine still plays, it just is not in this session's list.
int  StrategyAssetEaseRuntimeId(const SgaAsset *a, int index);

// Display name of a baked slot; "linear" for an out-of-range index.
const char *StrategyAssetEaseName(const SgaAsset *a, int index);

// Drop baked curves no key references any more, compacting the table and
// repointing every key. Called at save so a long editing session does not fill
// SGA_EASES_MAX with curves the author already moved off.
void StrategyAssetCompactEases(SgaAsset *a);

// Eased progress for a baked slot, resolving builtins by name and evaluating
// customs from their own knots. This is what the forge previews with; the
// runtime path in strategy_asset.c handles the knots alone (it cannot see the
// builtin table), so a builtin-by-name slot only curves through here.
float StrategyAssetEaseApplyBaked(const SgaAsset *a, int index, float p);

#endif // STRATEGY_ASSET_EASE_H
