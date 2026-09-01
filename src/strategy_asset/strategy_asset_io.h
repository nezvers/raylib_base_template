// ============================================================================
//  strategy_asset_io.h  -  save / load an SgaAsset as a versioned binary file
//
//  Unlike the animation .cfg files, which are deliberately plain text so they
//  stay hand-editable and diffable, an authored asset is a BINARY record: it is
//  produced and consumed only by the forge and the showcase, and it is meant to
//  be copied between projects as one self-contained file. See strategy_asset_io.c
//  for the on-disk layout and the rules that keep it readable.
//
//  Files live in a CWD-relative, writable directory created on demand - the
//  same convention as ZEN_ANIM_DIR, and deliberately NOT under RESOURCES_PATH,
//  which is read-only (and preloaded into a virtual FS on Web).
// ============================================================================

#ifndef STRATEGY_ASSET_IO_H
#define STRATEGY_ASSET_IO_H

#include "strategy_asset.h"
#include <stdbool.h>

#define SGA_DIR  "assets_strategy"
#define SGA_EXT  ".sga"

// Builds "assets_strategy/<name>.sga". Returns a TextFormat ring buffer - copy
// it if it must outlive the next few calls.
const char *StrategyAssetPath(const char *name);

bool StrategyAssetSave(const SgaAsset *a, const char *path);
bool StrategyAssetSaveNamed(const SgaAsset *a, const char *name);
bool StrategyAssetLoad(SgaAsset *a, const char *path);
bool StrategyAssetDelete(const char *name);
bool StrategyAssetNameFree(const char *name);

// Loads every .sga in SGA_DIR into `out`, up to `max`. Returns how many landed.
// Each asset's name is taken from its FILENAME, so the file is the identity.
int  StrategyAssetLoadAll(SgaAsset *out, int max);

// --- did the last load FIT? -------------------------------------------------
// The capacities are a two-tier build setting (CMake raises them off Web), so a
// desktop-authored asset can legitimately be too big for the Web build. A load
// that overflows still SUCCEEDS - it keeps what fits - which without this would
// be a silently mangled model. Valid until the next load.
typedef struct {
    int parts;      // parts past SGA_PARTS_MAX
    int keys;       // keyframes past SGA_KEYS_MAX, summed over every part/state
    int eases;      // baked curves past SGA_EASES_MAX
} SgaLoadTrunc;

bool StrategyAssetLoadTruncated(void);
const SgaLoadTrunc *StrategyAssetLoadTrunc(void);
void StrategyAssetLoadTruncReset(void);

// Renders the breakdown as one "<n> <what> (max <cap>)" line per exceeded
// capacity. Returns the length written (0, with out[0] = '\0', when it fit).
int  StrategyAssetLoadTruncMessage(char *out, int cap);

#endif // STRATEGY_ASSET_IO_H
