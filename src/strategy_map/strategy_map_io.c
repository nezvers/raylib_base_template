// ============================================================================
//  strategy_map_io.c  -  the .sgm on-disk format
//
//  HOUSE PATTERN, copied from strategy_asset_io.c which copied it from
//  settings_state.c:
//    - int32_t version as FIELD ZERO, compared for exact equality. There is no
//      magic header anywhere in this project and this file does not add one.
//    - the whole record written as one fwrite of sizeof(), via SimpleSave /
//      SimpleLoad.
//    - a padding-free hand-managed layout, so the file is the struct.
//    - every field clamped on the way back in, because a file on disk is
//      untrusted input even when this program wrote it.
// ============================================================================

#include "strategy_map_io.h"
#include "raylib.h"
#include <string.h>
#include <stdint.h>

// simple_save.h is included for DECLARATIONS ONLY - its implementation is
// compiled exactly once, by simple_save_example.c. Defining the
// implementation macro here would be a duplicate-symbol link error.
#include "simple_save.h"

#define SGM_SAVE_VERSION 2

// ---------------------------------------------------------------------------
//  THE ON-DISK CAPACITIES ARE FIXED, and deliberately NOT the build's tiered
//  SGM_*_MAX values.
//
//  The record is one struct blit, so its size IS the file's size. If that size
//  followed the build tier, a desktop-written file and a Web build would
//  disagree about the length of every field after the first - and because
//  SimpleLoad reads a fixed byte count without checking the file's actual
//  length, the Web build would read the first slice of a larger file, land the
//  header correctly by luck, and return SUCCESS with the whole grid silently
//  shifted. That is precisely the "quietly cropped battlefield" the truncation
//  report exists to prevent, so the format must not permit it.
//
//  These are the DESKTOP tier's values, so a desktop-authored map always fits.
//  A Web build reads the same file and reports what it had to drop
//  (SgmLoadTrunc) instead of misreading it. strategy_asset_io.c and anim_io.c
//  pin their formats the same way, for the same reason.
// ---------------------------------------------------------------------------
#define SGM_FILE_GRID    256
#define SGM_FILE_TILES   (SGM_FILE_GRID*SGM_FILE_GRID)
#define SGM_FILE_PLACES  1024

// The build must never exceed what the format can hold, or a save would drop
// data with no way to say so.
_Static_assert(SGM_GRID_MAX   <= SGM_FILE_GRID,   "SGM_GRID_MAX exceeds the file format");
_Static_assert(SGM_PLACES_MAX <= SGM_FILE_PLACES, "SGM_PLACES_MAX exceeds the file format");

// ---------------------------------------------------------------------------
//  The on-disk record
//
//  Written in declaration order with no bool and no implicit padding: every
//  member is 4 bytes or an array of them, except the tile and placement
//  records, whose bytes are explicit so their packing is stated rather than
//  inherited.
// ---------------------------------------------------------------------------
typedef struct {
    uint8_t terrain, height, flags, variant;
} SgmTileDisk;

typedef struct {
    int16_t tileX, tileZ;
    uint8_t family, kind, faction, amount;
} SgmPlaceDisk;

typedef struct {
    int16_t tileX, tileZ;
    uint8_t colorIndex, reserved;
} SgmStartDisk;

_Static_assert(sizeof(SgmTileDisk)  == 4, "SgmTileDisk layout changed - bump SGM_SAVE_VERSION");
_Static_assert(sizeof(SgmPlaceDisk) == 8, "SgmPlaceDisk layout changed - bump SGM_SAVE_VERSION");
_Static_assert(sizeof(SgmStartDisk) == 6, "SgmStartDisk layout changed - bump SGM_SAVE_VERSION");

typedef struct {
    int32_t version;            // FIELD ZERO, always
    char    name[SGM_NAME_MAX];
    char    desc[SGM_DESC_MAX];

    int32_t gridW, gridH;
    int32_t factionCount;
    int32_t gridStyle;
    int32_t placeCount;

    SgmStartDisk starts[SGM_FACTIONS_MAX];

    // The grid is stored at the FILE's stride (SGM_FILE_GRID), not the map's,
    // so a row's position in the file never depends on the authored width.
    SgmTileDisk  tiles[SGM_FILE_TILES];
    SgmPlaceDisk places[SGM_FILE_PLACES];
} SgmFile;

// ---------------------------------------------------------------------------
//  Truncation report
// ---------------------------------------------------------------------------
static SgmLoadTrunc s_trunc = { 0 };

bool SgmMapLoadTruncated(void)
{
    return (s_trunc.gridW > 0) || (s_trunc.gridH > 0) || (s_trunc.places > 0);
}

const SgmLoadTrunc *SgmMapLoadTrunc(void) { return &s_trunc; }

void SgmMapLoadTruncReset(void) { s_trunc = (SgmLoadTrunc){ 0 }; }

int SgmMapLoadTruncMessage(char *out, int cap)
{
    if ((out == NULL) || (cap <= 0)) return 0;
    out[0] = '\0';
    if (!SgmMapLoadTruncated()) return 0;

    // TextAppend dereferences its position argument unconditionally, so this
    // needs a real cursor - NULL would crash. (strategy_asset_io.c documents
    // the same trap.)
    int pos = 0;
    if (s_trunc.gridW > 0)
        TextAppend(out, TextFormat("%d columns (max %d)\n",
                                   s_trunc.gridW, SGM_GRID_MAX), &pos);
    if (s_trunc.gridH > 0)
        TextAppend(out, TextFormat("%d rows (max %d)\n",
                                   s_trunc.gridH, SGM_GRID_MAX), &pos);
    if (s_trunc.places > 0)
        TextAppend(out, TextFormat("%d placements (max %d)\n",
                                   s_trunc.places, SGM_PLACES_MAX), &pos);

    if (pos >= cap) pos = cap - 1;
    out[pos] = '\0';
    return pos;
}

// ---------------------------------------------------------------------------
//  Paths
// ---------------------------------------------------------------------------
static int ClampI(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

const char *SgmMapPath(const char *name)
{
    return TextFormat("%s/%s%s", SGM_DIR, name, SGM_EXT);
}

static void EnsureDir(void)
{
    if (!DirectoryExists(SGM_DIR)) MakeDirectory(SGM_DIR);
}

// ---------------------------------------------------------------------------
//  Save
// ---------------------------------------------------------------------------
bool SgmMapSave(const SgmMap *m, const char *path)
{
    if ((m == NULL) || (path == NULL)) return false;

    // Heap-free everywhere else, but SgmFile is ~70 KB - too big for a 1 MB Web
    // stack, and this runs once per explicit save.
    SgmFile *f = (SgmFile *)MemAlloc(sizeof(SgmFile));
    if (f == NULL) return false;
    memset(f, 0, sizeof(*f));

    f->version = SGM_SAVE_VERSION;
    TextCopy(f->name, m->name);
    TextCopy(f->desc, m->desc);

    f->gridW        = ClampI(m->gridW, 0, SGM_FILE_GRID);
    f->gridH        = ClampI(m->gridH, 0, SGM_FILE_GRID);
    f->factionCount = ClampI(m->factionCount, 1, SGM_FACTIONS_MAX);
    f->gridStyle    = ClampI(m->gridStyle, 0, SGM_GRID_STYLE_COUNT - 1);
    f->placeCount   = ClampI(m->placeCount, 0, SGM_FILE_PLACES);

    for (int i = 0; i < SGM_FACTIONS_MAX; i++)
    {
        f->starts[i].tileX      = m->starts[i].tileX;
        f->starts[i].tileZ      = m->starts[i].tileZ;
        f->starts[i].colorIndex = m->starts[i].colorIndex;
    }

    // Row by row at the FILE's stride, so the file layout is independent of the
    // authored width.
    for (int z = 0; z < f->gridH; z++)
    {
        for (int x = 0; x < f->gridW; x++)
        {
            const SgmTile *t = &m->tiles[z*m->gridW + x];
            SgmTileDisk *d = &f->tiles[z*SGM_FILE_GRID + x];
            d->terrain = t->terrain;
            d->height  = t->height;
            d->flags   = t->flags;
            d->variant = t->variant;
        }
    }

    for (int i = 0; i < f->placeCount; i++)
    {
        const SgmPlacement *p = &m->places[i];
        SgmPlaceDisk *d = &f->places[i];
        d->tileX   = p->tileX;
        d->tileZ   = p->tileZ;
        d->family  = p->family;
        d->kind    = p->kind;
        d->faction = p->faction;
        d->amount  = p->amount;
    }

    EnsureDir();
    bool ok = SimpleSave(path, (char *)f, sizeof(*f));
    MemFree(f);
    return ok;
}

bool SgmMapSaveNamed(const SgmMap *m, const char *name)
{
    if ((m == NULL) || (name == NULL) || (name[0] == '\0')) return false;
    return SgmMapSave(m, SgmMapPath(name));
}

// ---------------------------------------------------------------------------
//  Load
// ---------------------------------------------------------------------------
bool SgmMapLoad(SgmMap *m, const char *path)
{
    if ((m == NULL) || (path == NULL)) return false;
    if (!FileExists(path)) return false;

    SgmMapLoadTruncReset();

    SgmFile *f = (SgmFile *)MemAlloc(sizeof(SgmFile));
    if (f == NULL) return false;
    memset(f, 0, sizeof(*f));

    if (!SimpleLoad(path, (char *)f, sizeof(*f))) { MemFree(f); return false; }

    if (f->version != SGM_SAVE_VERSION)
    {
        TraceLog(LOG_WARNING, "SGM: %s is version %d, expected %d",
                 path, f->version, SGM_SAVE_VERSION);
        MemFree(f);
        return false;
    }

    // Start from a known-good document so anything the file does not cover is
    // sane rather than whatever was in `m` before.
    SgmMapInit(m, f->name, SGM_GRID_MAX, SGM_GRID_MAX);
    TextCopy(m->desc, f->desc);

    int fileW = ClampI(f->gridW, 0, SGM_FILE_GRID);
    int fileH = ClampI(f->gridH, 0, SGM_FILE_GRID);

    // What this build can actually hold. A bigger file is CROPPED, and the
    // shortfall is reported rather than silently applied.
    int w = ClampI(fileW, 0, SGM_GRID_MAX);
    int h = ClampI(fileH, 0, SGM_GRID_MAX);
    if (fileW > w) s_trunc.gridW = fileW;
    if (fileH > h) s_trunc.gridH = fileH;

    m->gridW = w;
    m->gridH = h;
    m->factionCount = ClampI(f->factionCount, 1, SGM_FACTIONS_MAX);
    m->gridStyle    = ClampI(f->gridStyle, 0, SGM_GRID_STYLE_COUNT - 1);

    for (int i = 0; i < SGM_FACTIONS_MAX; i++)
    {
        m->starts[i].tileX      = (int16_t)ClampI(f->starts[i].tileX, 0, (w > 0) ? w - 1 : 0);
        m->starts[i].tileZ      = (int16_t)ClampI(f->starts[i].tileZ, 0, (h > 0) ? h - 1 : 0);
        m->starts[i].colorIndex = f->starts[i].colorIndex;
    }

    for (int z = 0; z < h; z++)
    {
        for (int x = 0; x < w; x++)
        {
            const SgmTileDisk *d = &f->tiles[z*SGM_FILE_GRID + x];
            SgmTile *t = &m->tiles[z*w + x];
            t->terrain = (uint8_t)ClampI(d->terrain, 0, SGM_TERRAIN_COUNT - 1);
            t->height  = (uint8_t)ClampI(d->height, 0, SGM_HEIGHT_MAX);
            t->flags   = (uint8_t)(d->flags & (SGM_TILE_PASSABLE | SGM_TILE_BUILDABLE));
            t->variant = d->variant;
        }
    }

    int fileP = ClampI(f->placeCount, 0, SGM_FILE_PLACES);
    m->placeCount = 0;
    for (int i = 0; i < fileP; i++)
    {
        const SgmPlaceDisk *d = &f->places[i];

        // Drop what this build cannot hold, and what the cropped grid no longer
        // contains - a placement outside the extent has nowhere to stand.
        if (m->placeCount >= SGM_PLACES_MAX) { s_trunc.places = fileP; break; }
        if (!SgmInBounds(m, d->tileX, d->tileZ)) continue;

        SgmPlacement *p = &m->places[m->placeCount++];
        p->tileX   = d->tileX;
        p->tileZ   = d->tileZ;
        p->family  = (uint8_t)ClampI(d->family, 0, SGM_PLACE_COUNT - 1);
        p->kind    = d->kind;
        p->faction = d->faction;
        p->amount  = d->amount;
    }

    MemFree(f);
    return true;
}

// ---------------------------------------------------------------------------
//  Directory
// ---------------------------------------------------------------------------
bool SgmMapDelete(const char *name)
{
    if ((name == NULL) || (name[0] == '\0')) return false;
    const char *path = SgmMapPath(name);
    if (!FileExists(path)) return false;

    SimpleDelete(path);
    return !FileExists(path);
}

bool SgmMapNameFree(const char *name)
{
    if ((name == NULL) || (name[0] == '\0')) return false;
    return !FileExists(SgmMapPath(name));
}

int SgmMapLoadAll(SgmMap *out, int max)
{
    if ((out == NULL) || (max <= 0)) return 0;
    if (!DirectoryExists(SGM_DIR)) return 0;

    FilePathList files = LoadDirectoryFilesEx(SGM_DIR, SGM_EXT, false);
    int n = 0;

    for (unsigned int i = 0; (i < files.count) && (n < max); i++)
    {
        if (!SgmMapLoad(&out[n], files.paths[i])) continue;

        // The FILE is the identity - a map renamed on disk is renamed, whatever
        // the record says. Same rule as .sga.
        TextCopy(out[n].name, GetFileNameWithoutExt(files.paths[i]));
        n++;
    }

    UnloadDirectoryFiles(files);
    return n;
}
