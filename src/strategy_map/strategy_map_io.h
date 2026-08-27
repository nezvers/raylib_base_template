// ============================================================================
//  strategy_map_io.h  -  save / load an SgmMap as a versioned binary file
//
//  Same shape as strategy_asset_io.h, and for the same reasons: a map is
//  produced and consumed only by the map forge and the game, and it is meant to
//  be copied between machines as one self-contained file. See the .c for the
//  on-disk layout and the rules that keep it readable.
//
//  Files live in a CWD-relative, writable directory created on demand - the
//  same convention as ZEN_ANIM_DIR and SGA_DIR, and deliberately NOT under
//  RESOURCES_PATH, which is read-only (and a preloaded virtual FS on Web).
// ============================================================================

#ifndef STRATEGY_MAP_IO_H
#define STRATEGY_MAP_IO_H

#include "strategy_map.h"
#include <stdbool.h>

#define SGM_DIR  "maps_strategy"
#define SGM_EXT  ".sgm"

// Builds "maps_strategy/<name>.sgm". Returns a TextFormat ring buffer - copy it
// if it must outlive the next few calls.
const char *SgmMapPath(const char *name);

bool SgmMapSave(const SgmMap *m, const char *path);
bool SgmMapSaveNamed(const SgmMap *m, const char *name);
bool SgmMapLoad(SgmMap *m, const char *path);
bool SgmMapDelete(const char *name);
bool SgmMapNameFree(const char *name);

// Loads every .sgm in SGM_DIR into `out`, up to `max`. Returns how many landed.
// Each map's name is taken from its FILENAME, so the file is the identity.
int  SgmMapLoadAll(SgmMap *out, int max);

// --- did the last load FIT? -------------------------------------------------
// The grid cap is a two-tier build setting (CMake raises it off Web), so a
// desktop-authored map can legitimately be too big for the Web build. A load
// that overflows still SUCCEEDS - it keeps what fits - which without this would
// be a silently cropped battlefield. Valid until the next load.
typedef struct {
    int gridW;      // columns past SGM_GRID_MAX
    int gridH;      // rows past SGM_GRID_MAX
    int places;     // placements past SGM_PLACES_MAX
} SgmLoadTrunc;

bool SgmMapLoadTruncated(void);
const SgmLoadTrunc *SgmMapLoadTrunc(void);
void SgmMapLoadTruncReset(void);

// Renders the breakdown as one "<n> <what> (max <cap>)" line per exceeded
// capacity. Returns the length written (0, with out[0] = '\0', when it fit).
int  SgmMapLoadTruncMessage(char *out, int cap);

#endif // STRATEGY_MAP_IO_H
