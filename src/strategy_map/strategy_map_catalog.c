// ============================================================================
//  strategy_map_catalog.c  -  see strategy_map_catalog.h
// ============================================================================

#include "strategy_map_catalog.h"
#include "strategy_map_io.h"
#include "raylib.h"
#include <string.h>

// THE array. Static, never on a stack, never copied.
static SgmMap s_maps[SGM_MAPS_MAX];
static int    s_count;
static bool   s_ready;

static void Scan(void)
{
    s_count = SgmMapLoadAll(s_maps, SGM_MAPS_MAX);
    s_ready = true;
}

void SgmCatalogLoad(void)
{
    if (s_ready) return;        // idempotent: every screen may call this
    Scan();
}

void SgmCatalogReload(void)
{
    Scan();
}

const SgmMap *SgmCatalogMaps(void)  { return s_maps; }
int           SgmCatalogCount(void) { return s_count; }
bool          SgmCatalogReady(void) { return s_ready; }

const SgmMap *SgmCatalogFind(const char *name)
{
    if ((name == NULL) || (name[0] == '\0')) return NULL;

    for (int i = 0; i < s_count; i++)
        if (strncmp(s_maps[i].name, name, SGM_NAME_MAX) == 0) return &s_maps[i];

    return NULL;
}
