// ============================================================================
//  strategy_catalog.c  -  see strategy_catalog.h
// ============================================================================

#include "strategy_catalog.h"
#include "strategy_asset_io.h"
#include "raylib.h"
#include <string.h>

// THE array. Static, never on a stack, never copied - see the header for why
// its size makes that a rule rather than a preference.
static SgaAsset s_assets[SGA_ASSETS_MAX];
static int      s_count;
static bool     s_ready;

static void Scan(void)
{
    s_count = StrategyAssetLoadAll(s_assets, SGA_ASSETS_MAX);
    s_ready = true;
}

void StrategyCatalogLoad(void)
{
    if (s_ready) return;        // idempotent: every screen may call this
    StrategyBindingsLoad();
    Scan();
}

void StrategyCatalogReload(void)
{
    Scan();
}

const SgaAsset *StrategyCatalogAssets(void) { return s_assets; }
int             StrategyCatalogCount(void)  { return s_count; }
bool            StrategyCatalogReady(void)  { return s_ready; }

const SgaAsset *StrategyCatalogFind(const char *name)
{
    if ((name == NULL) || (name[0] == '\0')) return NULL;

    for (int i = 0; i < s_count; i++)
        if (strncmp(s_assets[i].name, name, SGA_NAME_MAX) == 0) return &s_assets[i];

    return NULL;
}

const SgaAsset *StrategyCatalogForRole(int family, int role)
{
    // The resolve lives in strategy_bindings.c and takes the array as a
    // parameter - which is exactly the seam that let the array move here
    // without the binding module having to know where it ended up.
    return StrategyBindingResolve(family, role, s_assets, s_count);
}
