// ============================================================================
//  strategy_bindings.c  -  see strategy_bindings.h
//
//  The on-disk record follows strategy_asset_io.c exactly: version as field
//  zero, one whole-struct blit through SimpleSave/SimpleLoad, fixed capacities
//  that do NOT follow the build tier, and clamping on everything read back.
//
//  There is one difference worth stating. The asset file's payload is numeric
//  and a bad value there is a geometry bug; this file's payload is entirely
//  STRINGS, and a string read off disk that is not terminated is an
//  out-of-bounds read on the first printf. So the load terminates every slot
//  unconditionally before anything looks at it - that, not the enum clamping,
//  is the load's real job here.
// ============================================================================

#include "strategy_bindings.h"
#include "strategy_asset_io.h"      // SGA_DIR
#include "raylib.h"
#include "simple_save.h"            // DECLARATIONS ONLY - implementation lives
                                    // in examples/simple_save_example.c
#include <string.h>

// Bump on ANY change to SgbFile's layout.
#define SGB_SAVE_VERSION 1

// ---------------------------------------------------------------------------
//  The live set
// ---------------------------------------------------------------------------
static SgaBindings s_bindings;

// How many roles each family really has. Zero means "not installed yet", which
// reports the capacity instead - see StrategyBindingsRoleCount.
static int s_roleCount[SGB_ROLE_FAMILY_COUNT];

static const int s_capacity[SGB_ROLE_FAMILY_COUNT] = {
    SGB_UNITS_MAX, SGB_BUILDINGS_MAX, SGB_NODES_MAX
};

SgaBindings *StrategyBindingsGet(void) { return &s_bindings; }

void StrategyBindingsClear(void)
{
    memset(&s_bindings, 0, sizeof(s_bindings));
}

void StrategyBindingsSetRoleCounts(int units, int buildings, int nodes)
{
    const int wish[SGB_ROLE_FAMILY_COUNT] = { units, buildings, nodes };
    for (int f = 0; f < SGB_ROLE_FAMILY_COUNT; f++)
    {
        int n = wish[f];
        if (n < 0) n = 0;
        if (n > s_capacity[f]) n = s_capacity[f];
        s_roleCount[f] = n;
    }
}

int StrategyBindingsRoleCount(int family)
{
    if ((family < 0) || (family >= SGB_ROLE_FAMILY_COUNT)) return 0;
    // Uninstalled reports the capacity: a headless test binds by index without
    // needing the game's enums, and the arrays are that long anyway.
    return (s_roleCount[family] > 0) ? s_roleCount[family] : s_capacity[family];
}

const char *StrategyBindingsFamilyName(int family)
{
    switch (family)
    {
        case SGB_ROLE_BUILDING: return "BUILDING";
        case SGB_ROLE_NODE:     return "RESOURCE";
        default:                return "UNIT";
    }
}

// ---------------------------------------------------------------------------
//  Slot access
//
//  One place that turns (family, role) into a char* - every accessor below goes
//  through it, so the bounds check exists exactly once. Bounds are the ARRAY
//  capacity, not the installed role count: a file written by a build whose
//  enums were longer still round-trips rather than losing its tail.
// ---------------------------------------------------------------------------
static char *Slot(int family, int role)
{
    if ((family < 0) || (family >= SGB_ROLE_FAMILY_COUNT)) return NULL;
    if ((role < 0) || (role >= s_capacity[family])) return NULL;

    switch (family)
    {
        case SGB_ROLE_UNIT:     return s_bindings.unit[role];
        case SGB_ROLE_BUILDING: return s_bindings.building[role];
        case SGB_ROLE_NODE:     return s_bindings.node[role];
        default:                return NULL;
    }
}

const char *StrategyBindingGet(int family, int role)
{
    const char *s = Slot(family, role);
    return s ? s : "";      // never NULL: callers print this directly
}

bool StrategyBindingSet(int family, int role, const char *assetName)
{
    char *s = Slot(family, role);
    if (s == NULL) return false;

    if ((assetName == NULL) || (assetName[0] == '\0'))
    {
        memset(s, 0, SGA_NAME_MAX);
        return true;
    }

    // TextCopy truncates at the destination size and terminates, which is the
    // behaviour wanted: a name too long to store is a name that could never
    // have been an asset's, since SGA_NAME_MAX bounds both.
    TextCopy(s, assetName);
    s[SGA_NAME_MAX - 1] = '\0';
    return true;
}

bool StrategyBindingIsBound(int family, int role)
{
    const char *s = Slot(family, role);
    return (s != NULL) && (s[0] != '\0');
}

const SgaAsset *StrategyBindingResolve(int family, int role,
                                       const SgaAsset *assets, int assetCount)
{
    const char *want = Slot(family, role);
    if ((want == NULL) || (want[0] == '\0')) return NULL;
    if ((assets == NULL) || (assetCount <= 0)) return NULL;

    for (int i = 0; i < assetCount; i++)
        if (strncmp(assets[i].name, want, SGA_NAME_MAX) == 0) return &assets[i];

    return NULL;    // bound but missing - the caller asks IsBound to tell apart
}

int StrategyBindingsRename(const char *oldName, const char *newName)
{
    if ((oldName == NULL) || (oldName[0] == '\0')) return 0;

    int n = 0;
    for (int f = 0; f < SGB_ROLE_FAMILY_COUNT; f++)
        for (int r = 0; r < s_capacity[f]; r++)
        {
            char *s = Slot(f, r);
            if ((s == NULL) || (strncmp(s, oldName, SGA_NAME_MAX) != 0)) continue;

            if ((newName == NULL) || (newName[0] == '\0')) memset(s, 0, SGA_NAME_MAX);
            else { TextCopy(s, newName); s[SGA_NAME_MAX - 1] = '\0'; }
            n++;
        }

    return n;
}

int StrategyBindingsMissingCount(const SgaAsset *assets, int assetCount)
{
    int n = 0;
    for (int f = 0; f < SGB_ROLE_FAMILY_COUNT; f++)
        for (int r = 0; r < StrategyBindingsRoleCount(f); r++)
        {
            if (!StrategyBindingIsBound(f, r)) continue;
            if (StrategyBindingResolve(f, r, assets, assetCount) == NULL) n++;
        }

    return n;
}

// ---------------------------------------------------------------------------
//  The on-disk record
//
//  A plain mirror of SgaBindings with the version in front. It is spelled out
//  as its own struct rather than embedding SgaBindings so that adding a runtime
//  field later cannot change the file's layout by accident - the same split the
//  asset file makes between SgaAsset and SgaFile.
// ---------------------------------------------------------------------------
typedef struct {
    int32_t version;
    char    unit[SGB_UNITS_MAX][SGA_NAME_MAX];
    char    building[SGB_BUILDINGS_MAX][SGA_NAME_MAX];
    char    node[SGB_NODES_MAX][SGA_NAME_MAX];
} SgbFile;

// char arrays pack with no padding, so the size is exactly the sum. Asserting
// it turns a reordered or inserted field into a compile error rather than a
// file that loads as garbage.
_Static_assert(sizeof(SgbFile) == sizeof(int32_t) +
               (SGB_UNITS_MAX + SGB_BUILDINGS_MAX + SGB_NODES_MAX)*SGA_NAME_MAX,
               "SgbFile has unexpected padding - the on-disk layout changed");

const char *StrategyBindingsPath(void)
{
    return TextFormat("%s/%s", SGA_DIR, SGB_FILE);
}

// ---------------------------------------------------------------------------
//  Load
// ---------------------------------------------------------------------------
bool StrategyBindingsLoad(void)
{
    StrategyBindingsClear();

    const char *path = StrategyBindingsPath();

    // No file is the normal starting state, not a failure. Reporting it as one
    // would put a scary message in front of every first run.
    if (!FileExists(path)) return true;

    SgbFile *f = (SgbFile *)MemAlloc(sizeof(SgbFile));
    if (f == NULL) return false;
    memset(f, 0, sizeof(*f));

    if (!SimpleLoad(path, (char *)f, sizeof(*f)))
    {
        MemFree(f);
        TraceLog(LOG_WARNING, "BINDINGS: could not read %s", path);
        return false;
    }

    if (f->version != SGB_SAVE_VERSION)
    {
        TraceLog(LOG_WARNING, "BINDINGS: %s is version %d, expected %d - ignored",
                 path, f->version, SGB_SAVE_VERSION);
        MemFree(f);
        return false;
    }

    // Terminate EVERY slot before anything reads one. A file is untrusted
    // input, and an unterminated name here is an out-of-bounds read the first
    // time it is printed - a far more immediate problem than a bad enum.
    for (int i = 0; i < SGB_UNITS_MAX; i++)     f->unit[i][SGA_NAME_MAX - 1]     = '\0';
    for (int i = 0; i < SGB_BUILDINGS_MAX; i++) f->building[i][SGA_NAME_MAX - 1] = '\0';
    for (int i = 0; i < SGB_NODES_MAX; i++)     f->node[i][SGA_NAME_MAX - 1]     = '\0';

    memcpy(s_bindings.unit,     f->unit,     sizeof(s_bindings.unit));
    memcpy(s_bindings.building, f->building, sizeof(s_bindings.building));
    memcpy(s_bindings.node,     f->node,     sizeof(s_bindings.node));

    MemFree(f);
    return true;
}

// ---------------------------------------------------------------------------
//  Save
// ---------------------------------------------------------------------------
bool StrategyBindingsSave(void)
{
    if (!DirectoryExists(SGA_DIR)) MakeDirectory(SGA_DIR);

    SgbFile *f = (SgbFile *)MemAlloc(sizeof(SgbFile));
    if (f == NULL) return false;
    memset(f, 0, sizeof(*f));

    f->version = SGB_SAVE_VERSION;
    memcpy(f->unit,     s_bindings.unit,     sizeof(f->unit));
    memcpy(f->building, s_bindings.building, sizeof(f->building));
    memcpy(f->node,     s_bindings.node,     sizeof(f->node));

    bool ok = SimpleSave(StrategyBindingsPath(), (char *)f, sizeof(*f));
    MemFree(f);
    return ok;
}
