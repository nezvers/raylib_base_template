// ============================================================================
//  strategy_asset_io.c  -  binary persistence for authored assets
//
//  Follows settings_state.c, which is the project's only binary persistence and
//  therefore its house pattern:
//
//    - an `int32_t version` as FIELD ZERO, compared for exact equality. There
//      is no magic header anywhere in this project and this does not add one.
//      A mismatch REFUSES the load rather than reading a struct whose fields
//      mean something else.
//    - the whole record written as one fwrite of sizeof(), via SimpleSave /
//      SimpleLoad (include/simple_save.h). That header is included for
//      DECLARATIONS ONLY: its implementation is already compiled into this
//      binary by examples/simple_save_example.c, and a second
//      SIMPLE_SAVE_IMPLEMENTATION would be a duplicate-symbol link error.
//    - a hand-managed, padding-free layout. settings_state.c stores its one
//      bool as an int for exactly this reason; every flag and enum here is
//      int32_t, every scalar a float, and Color is written as four bytes.
//    - post-load CLAMPING on every field. A file is untrusted input; a bad
//      partCount used raw is an out-of-bounds walk.
//
//  ON-DISK IS ITS OWN STRUCT. SgaAsset is a runtime value that will grow fields
//  with no business on disk; SgaFile is frozen by the version number. The
//  _Static_asserts below turn "someone reordered a field" into a compile error
//  instead of a corrupt file nobody notices until it is loaded.
//
//  ENDIANNESS AND LAYOUT ARE HOST-NATIVE, exactly as settings.sav is. Every
//  platform this project targets is little-endian with 4-byte float/int32, so
//  files do move between them in practice - but that is an observation, not a
//  guarantee the format makes. The version field is the hook if a byte-swapping
//  v2 is ever wanted.
// ============================================================================

#include "strategy_asset_io.h"
#include "raylib.h"
#include "simple_save.h"    // DECLARATIONS ONLY - see the note above
#include <string.h>

// Bump on ANY change to SgaFile's layout.
#define SGA_SAVE_VERSION 2

// ---------------------------------------------------------------------------
//  THE ON-DISK CAPACITIES ARE FIXED, and deliberately NOT the build's tiered
//  SGA_*_MAX values.
//
//  The record is one struct blit, so its size IS the file's size. If that size
//  followed the build tier, a desktop-written file and a Web build would
//  disagree about the length of every field after the first - and because
//  SimpleLoad reads a fixed byte count without checking the file's actual
//  length, the Web build would happily read the first 70 KB of a 461 KB file,
//  land the header correctly by luck, and return SUCCESS with every part after
//  the first silently corrupt. That is precisely the "quietly mangled model"
//  the truncation report exists to prevent, so the format must not permit it.
//
//  These are the DESKTOP tier's values, so a desktop-authored asset always
//  fits. A Web build reads the same file and reports what it had to drop
//  (SgaLoadTrunc) instead of misreading it. anim_io.c pins ANIM_SHAPE_REF_MAX
//  the same way, for the same reason: "fixed, so the same file parses
//  identically on every platform".
// ---------------------------------------------------------------------------
#define SGA_FILE_PARTS   64
#define SGA_FILE_KEYS    32
#define SGA_FILE_EASES   32

// The build must never exceed what the format can hold, or a save would drop
// data with no way to say so.
_Static_assert(SGA_PARTS_MAX <= SGA_FILE_PARTS, "SGA_PARTS_MAX exceeds the file format");
_Static_assert(SGA_KEYS_MAX  <= SGA_FILE_KEYS,  "SGA_KEYS_MAX exceeds the file format");
_Static_assert(SGA_EASES_MAX <= SGA_FILE_EASES, "SGA_EASES_MAX exceeds the file format");

// ---------------------------------------------------------------------------
//  The on-disk record
//
//  Written in declaration order with no bool and no implicit padding: every
//  member is 4 bytes or an array of them, except the colour, which is four
//  explicit bytes so its packing is stated rather than inherited from Color.
// ---------------------------------------------------------------------------
typedef struct { float x, y, z; } SgaV3Disk;

typedef struct {
    float     t;
    float     u;
    SgaV3Disk offset;       // added in v2
    SgaV3Disk rot;
    SgaV3Disk scale;
    int32_t   ease;
} SgaKeyDisk;

typedef struct {
    int32_t    pathPart;
    int32_t    keyCount;
    SgaKeyDisk keys[SGA_FILE_KEYS];
} SgaAnimDisk;

typedef struct {
    SgaV3Disk center;
    float     radiusX, radiusZ;
    float     squareness;
    SgaV3Disk rotation;
} SgaPathDisk;

typedef struct {
    char          name[SGA_NAME_MAX];
    int32_t       kind;
    int32_t       visible;

    SgaV3Disk     offset;
    SgaV3Disk     size;
    float         r0, r1, h;
    int32_t       sides;

    int32_t       tintMode;
    float         tintAmount;
    float         brightness;
    unsigned char cr, cg, cb, ca;   // Color, unpacked so the layout is explicit

    SgaPathDisk   path;
    SgaAnimDisk   anim[SGA_STATE_COUNT];
} SgaPartDisk;

typedef struct {
    char    name[SGA_EASE_NAME_MAX];
    int32_t ptCount;
    float   pts[SGA_EASE_PTS_MAX*6];    // x y ix iy ox oy per knot, flattened
} SgaEaseDisk;

typedef struct {
    int32_t     version;                // MUST stay first
    char        name[SGA_NAME_MAX];
    char        subtype[SGA_SUBTYPE_MAX];
    int32_t     category;

    int32_t     partCount;
    int32_t     easeCount;
    float       duration[SGA_STATE_COUNT];

    SgaPartDisk parts[SGA_FILE_PARTS];
    SgaEaseDisk eases[SGA_FILE_EASES];
} SgaFile;

// A reordered or retyped field changes these numbers. Catching it here is the
// difference between a compile error and a file that loads as garbage.
_Static_assert(sizeof(SgaV3Disk) == 12, "SgaV3Disk must be 3 tight floats");
_Static_assert(sizeof(SgaKeyDisk) == 48, "SgaKeyDisk layout changed - bump SGA_SAVE_VERSION");
_Static_assert(sizeof(SgaPathDisk) == 36, "SgaPathDisk layout changed - bump SGA_SAVE_VERSION");
_Static_assert(sizeof(SgaAnimDisk) == 8 + SGA_FILE_KEYS*48, "SgaAnimDisk has padding");
_Static_assert(sizeof(SgaEaseDisk) == SGA_EASE_NAME_MAX + 4 + SGA_EASE_PTS_MAX*24,
               "SgaEaseDisk layout changed - bump SGA_SAVE_VERSION");
_Static_assert(sizeof(SgaPartDisk) ==
               SGA_NAME_MAX + 8 + 24 + 12 + 4 + 12 + 4
               + sizeof(SgaPathDisk) + SGA_STATE_COUNT*sizeof(SgaAnimDisk),
               "SgaPartDisk has padding - reorder its members");
_Static_assert(sizeof(SgaFile) ==
               4 + SGA_NAME_MAX + SGA_SUBTYPE_MAX + 4 + 4 + 4
               + SGA_STATE_COUNT*4
               + SGA_FILE_PARTS*sizeof(SgaPartDisk)
               + SGA_FILE_EASES*sizeof(SgaEaseDisk),
               "SgaFile has padding - reorder its members");

// ---------------------------------------------------------------------------
//  Truncation report
//
//  The capacities are a TWO-TIER build setting: desktop is raised, Web keeps
//  the smaller values that fit its fixed 128 MB heap. So an asset authored on
//  desktop can legitimately be too big for the Web build, and dropping the
//  overflow silently would be a quietly mangled model. anim_io.c reports the
//  same thing for the same reason.
// ---------------------------------------------------------------------------
static SgaLoadTrunc s_trunc;

bool StrategyAssetLoadTruncated(void)
{
    return (s_trunc.parts != 0) || (s_trunc.keys != 0) || (s_trunc.eases != 0);
}

const SgaLoadTrunc *StrategyAssetLoadTrunc(void) { return &s_trunc; }
void StrategyAssetLoadTruncReset(void) { memset(&s_trunc, 0, sizeof(s_trunc)); }

int StrategyAssetLoadTruncMessage(char *out, int cap)
{
    if ((out == NULL) || (cap <= 0)) return 0;
    out[0] = '\0';

    // TextAppend dereferences its position argument unconditionally, so this
    // needs a real cursor - NULL would crash. (The showcase's stat sheet
    // documents the same trap.)
    int pos = 0;
    if (s_trunc.parts > 0)
        TextAppend(out, TextFormat("%d parts (max %d)\n",
                                   s_trunc.parts, SGA_PARTS_MAX), &pos);
    if (s_trunc.keys > 0)
        TextAppend(out, TextFormat("%d keyframes (max %d per part)\n",
                                   s_trunc.keys, SGA_KEYS_MAX), &pos);
    if (s_trunc.eases > 0)
        TextAppend(out, TextFormat("%d easing curves (max %d)\n",
                                   s_trunc.eases, SGA_EASES_MAX), &pos);

    if (pos >= cap) pos = cap - 1;
    out[pos] = '\0';
    return pos;
}

// ---------------------------------------------------------------------------
//  Clamping. Every value that came off disk goes through one of these.
// ---------------------------------------------------------------------------
static int32_t ClampI(int32_t v, int32_t lo, int32_t hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static float ClampF(float v, float lo, float hi)
{
    // NaN fails every comparison, so it would slip through an ordinary range
    // test as "already in range" and then poison every multiply downstream.
    if (!(v == v)) return lo;
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static SgaV3Disk V3Out(Vector3 v) { return (SgaV3Disk){ v.x, v.y, v.z }; }

static Vector3 V3In(SgaV3Disk v, float limit)
{
    return (Vector3){ ClampF(v.x, -limit, limit),
                      ClampF(v.y, -limit, limit),
                      ClampF(v.z, -limit, limit) };
}

#define SGA_COORD_MAX  1000.0f      // a model is a few units tall; this is slack

// ---------------------------------------------------------------------------
//  Paths
// ---------------------------------------------------------------------------
const char *StrategyAssetPath(const char *name)
{
    return TextFormat("%s/%s%s", SGA_DIR, name, SGA_EXT);
}

static void EnsureDir(void)
{
    if (!DirectoryExists(SGA_DIR)) MakeDirectory(SGA_DIR);
}

// ---------------------------------------------------------------------------
//  Save
// ---------------------------------------------------------------------------
bool StrategyAssetSave(const SgaAsset *a, const char *path)
{
    if ((a == NULL) || (path == NULL)) return false;

    // Heap-free everywhere else, but on the desktop tier SgaFile is far too big
    // for the stack, and this runs once per explicit save.
    SgaFile *f = (SgaFile *)MemAlloc(sizeof(SgaFile));
    if (f == NULL) return false;
    memset(f, 0, sizeof(*f));

    f->version = SGA_SAVE_VERSION;
    TextCopy(f->name, a->name);
    TextCopy(f->subtype, a->subtype);
    f->category = ClampI(a->category, 0, SGA_CATEGORY_COUNT - 1);
    f->partCount = ClampI(a->partCount, 0, SGA_FILE_PARTS);
    f->easeCount = ClampI(a->easeCount, 0, SGA_FILE_EASES);

    for (int s = 0; s < SGA_STATE_COUNT; s++) f->duration[s] = a->duration[s];

    for (int i = 0; i < f->partCount; i++)
    {
        const SgaPart *p = &a->parts[i];
        SgaPartDisk *d = &f->parts[i];

        TextCopy(d->name, p->name);
        d->kind = p->kind;
        d->visible = p->visible ? 1 : 0;
        d->offset = V3Out(p->offset);
        d->size = V3Out(p->size);
        d->r0 = p->r0; d->r1 = p->r1; d->h = p->h;
        d->sides = p->sides;
        d->tintMode = p->tintMode;
        d->tintAmount = p->tintAmount;
        d->brightness = p->brightness;
        d->cr = p->color.r; d->cg = p->color.g;
        d->cb = p->color.b; d->ca = p->color.a;

        d->path.center = V3Out(p->path.center);
        d->path.radiusX = p->path.radiusX;
        d->path.radiusZ = p->path.radiusZ;
        d->path.squareness = p->path.squareness;
        d->path.rotation = V3Out(p->path.rotation);

        for (int s = 0; s < SGA_STATE_COUNT; s++)
        {
            const SgaPartAnim *an = &p->anim[s];
            SgaAnimDisk *ad = &d->anim[s];

            ad->pathPart = an->pathPart;
            ad->keyCount = ClampI(an->keyCount, 0, SGA_FILE_KEYS);

            for (int k = 0; k < ad->keyCount; k++)
            {
                ad->keys[k].t = an->keys[k].t;
                ad->keys[k].u = an->keys[k].u;
                ad->keys[k].offset = V3Out(an->keys[k].offset);
                ad->keys[k].rot = V3Out(an->keys[k].rot);
                ad->keys[k].scale = V3Out(an->keys[k].scale);
                ad->keys[k].ease = an->keys[k].ease;
            }
        }
    }

    for (int i = 0; i < f->easeCount; i++)
    {
        const SgaEase *e = &a->eases[i];
        SgaEaseDisk *d = &f->eases[i];

        TextCopy(d->name, e->name);
        d->ptCount = ClampI(e->ptCount, 0, SGA_EASE_PTS_MAX);

        for (int k = 0; k < d->ptCount; k++)
        {
            const SgaEasePt *q = &e->pts[k];
            float *o = &d->pts[k*6];
            o[0] = q->x;  o[1] = q->y;
            o[2] = q->ix; o[3] = q->iy;
            o[4] = q->ox; o[5] = q->oy;
        }
    }

    EnsureDir();
    bool ok = SimpleSave(path, (char *)f, sizeof(*f));
    MemFree(f);
    return ok;
}

bool StrategyAssetSaveNamed(const SgaAsset *a, const char *name)
{
    if ((a == NULL) || (name == NULL) || (name[0] == '\0')) return false;
    return StrategyAssetSave(a, StrategyAssetPath(name));
}

// ---------------------------------------------------------------------------
//  Load
// ---------------------------------------------------------------------------
bool StrategyAssetLoad(SgaAsset *a, const char *path)
{
    if ((a == NULL) || (path == NULL)) return false;

    StrategyAssetLoadTruncReset();

    SgaFile *f = (SgaFile *)MemAlloc(sizeof(SgaFile));
    if (f == NULL) return false;
    memset(f, 0, sizeof(*f));

    // SimpleLoad reads a fixed byte count and does NOT check the file's actual
    // length, so a short file would be read as a full record with whatever was
    // already in the buffer standing in for the rest. Check the size first: the
    // format is one fixed-size blit, so anything but an exact match is not one
    // of our files (or is a truncated copy of one).
    if ((int)GetFileLength(path) != (int)sizeof(*f))
    {
        TraceLog(LOG_WARNING, "SGA: %s is %d bytes, expected %d - not this format",
                 path, (int)GetFileLength(path), (int)sizeof(*f));
        MemFree(f);
        return false;
    }

    if (!SimpleLoad(path, (char *)f, sizeof(*f)))
    {
        MemFree(f);
        return false;
    }

    // Exact equality, and a refusal rather than a best-effort read: a record
    // from another version means something else, field for field.
    if (f->version != SGA_SAVE_VERSION)
    {
        TraceLog(LOG_WARNING, "SGA: %s is version %d, this build reads %d",
                 path, f->version, SGA_SAVE_VERSION);
        MemFree(f);
        return false;
    }

    memset(a, 0, sizeof(*a));

    // TextCopy stops at the destination size, but the terminator still has to
    // be forced: the bytes on disk may never have had one.
    TextCopy(a->name, f->name);
    a->name[SGA_NAME_MAX - 1] = '\0';
    TextCopy(a->subtype, f->subtype);
    a->subtype[SGA_SUBTYPE_MAX - 1] = '\0';

    a->category = ClampI(f->category, 0, SGA_CATEGORY_COUNT - 1);

    int32_t nParts = ClampI(f->partCount, 0, SGA_FILE_PARTS);
    if (nParts > SGA_PARTS_MAX)
    {
        s_trunc.parts = nParts - SGA_PARTS_MAX;
        nParts = SGA_PARTS_MAX;
    }
    a->partCount = nParts;

    int32_t nEases = ClampI(f->easeCount, 0, SGA_FILE_EASES);
    if (nEases > SGA_EASES_MAX)
    {
        s_trunc.eases = nEases - SGA_EASES_MAX;
        nEases = SGA_EASES_MAX;
    }
    a->easeCount = nEases;

    for (int s = 0; s < SGA_STATE_COUNT; s++)
        a->duration[s] = ClampF(f->duration[s], 0.0f, 600.0f);

    for (int i = 0; i < a->partCount; i++)
    {
        const SgaPartDisk *d = &f->parts[i];
        SgaPart *p = &a->parts[i];

        TextCopy(p->name, d->name);
        p->name[SGA_NAME_MAX - 1] = '\0';

        p->kind = ClampI(d->kind, 0, SGA_KIND_COUNT - 1);
        p->visible = (d->visible != 0) ? 1 : 0;
        p->offset = V3In(d->offset, SGA_COORD_MAX);
        p->size = V3In(d->size, SGA_COORD_MAX);
        p->r0 = ClampF(d->r0, 0.0f, SGA_COORD_MAX);
        p->r1 = ClampF(d->r1, 0.0f, SGA_COORD_MAX);
        p->h  = ClampF(d->h,  0.0f, SGA_COORD_MAX);

        // Fewer than 3 sides is not a solid - raylib would draw nothing.
        p->sides = ClampI(d->sides, 3, 64);

        p->tintMode = ClampI(d->tintMode, 0, SGA_TINT_COUNT - 1);
        p->tintAmount = ClampF(d->tintAmount, 0.0f, 1.0f);
        p->brightness = ClampF(d->brightness, -1.0f, 1.0f);
        p->color = (Color){ d->cr, d->cg, d->cb, d->ca };

        p->path.center = V3In(d->path.center, SGA_COORD_MAX);
        p->path.radiusX = ClampF(d->path.radiusX, 0.0f, SGA_COORD_MAX);
        p->path.radiusZ = ClampF(d->path.radiusZ, 0.0f, SGA_COORD_MAX);
        p->path.squareness = ClampF(d->path.squareness, 0.0f, 1.0f);
        p->path.rotation = V3In(d->path.rotation, 360.0f);

        for (int s = 0; s < SGA_STATE_COUNT; s++)
        {
            const SgaAnimDisk *ad = &d->anim[s];
            SgaPartAnim *an = &p->anim[s];

            // A path reference past the parts that actually loaded would index
            // out of bounds. Clamped to "no path" rather than to part 0, which
            // would silently attach the motion to the wrong thing.
            an->pathPart = ((ad->pathPart >= 0) && (ad->pathPart < a->partCount))
                           ? ad->pathPart : -1;

            int32_t nKeys = ClampI(ad->keyCount, 0, SGA_FILE_KEYS);
            if (nKeys > SGA_KEYS_MAX)
            {
                s_trunc.keys += nKeys - SGA_KEYS_MAX;
                nKeys = SGA_KEYS_MAX;
            }
            an->keyCount = nKeys;

            for (int k = 0; k < nKeys; k++)
            {
                an->keys[k].t = ClampF(ad->keys[k].t, 0.0f, 600.0f);
                an->keys[k].u = ClampF(ad->keys[k].u, -10.0f, 10.0f);
                an->keys[k].offset = V3In(ad->keys[k].offset, 1000.0f);
                an->keys[k].rot = V3In(ad->keys[k].rot, 3600.0f);
                an->keys[k].scale = V3In(ad->keys[k].scale, 100.0f);
                an->keys[k].ease = ((ad->keys[k].ease >= 0) &&
                                    (ad->keys[k].ease < a->easeCount))
                                   ? ad->keys[k].ease : -1;
            }
        }
    }

    for (int i = 0; i < a->easeCount; i++)
    {
        const SgaEaseDisk *d = &f->eases[i];
        SgaEase *e = &a->eases[i];

        TextCopy(e->name, d->name);
        e->name[SGA_EASE_NAME_MAX - 1] = '\0';
        e->ptCount = ClampI(d->ptCount, 0, SGA_EASE_PTS_MAX);

        for (int k = 0; k < e->ptCount; k++)
        {
            const float *o = &d->pts[k*6];
            e->pts[k].x  = ClampF(o[0], -10.0f, 10.0f);
            e->pts[k].y  = ClampF(o[1], -10.0f, 10.0f);
            e->pts[k].ix = ClampF(o[2], -10.0f, 10.0f);
            e->pts[k].iy = ClampF(o[3], -10.0f, 10.0f);
            e->pts[k].ox = ClampF(o[4], -10.0f, 10.0f);
            e->pts[k].oy = ClampF(o[5], -10.0f, 10.0f);
        }

        // A one-knot curve is not a curve. Demote it to "builtin by name",
        // which degrades to linear when the name resolves to nothing.
        if (e->ptCount < 2) e->ptCount = 0;
    }

    MemFree(f);

    // height/radius are DERIVED, never read from the file - a stored pair goes
    // stale the moment a part moves.
    StrategyAssetMeasure(a);
    return true;
}

bool StrategyAssetDelete(const char *name)
{
    if ((name == NULL) || (name[0] == '\0')) return false;

    const char *path = StrategyAssetPath(name);
    if (!FileExists(path)) return false;

    SimpleDelete(path);
    return !FileExists(path);
}

bool StrategyAssetNameFree(const char *name)
{
    if ((name == NULL) || (name[0] == '\0')) return false;
    return !FileExists(StrategyAssetPath(name));
}

// ---------------------------------------------------------------------------
//  Directory scan
// ---------------------------------------------------------------------------
int StrategyAssetLoadAll(SgaAsset *out, int max)
{
    if ((out == NULL) || (max <= 0)) return 0;

    EnsureDir();
    if (!DirectoryExists(SGA_DIR)) return 0;

    int n = 0;
    FilePathList fl = LoadDirectoryFilesEx(SGA_DIR, SGA_EXT, false);

    for (unsigned int i = 0; (i < fl.count) && (n < max); i++)
    {
        const char *base = GetFileName(fl.paths[i]);

        // The leading-underscore convention, shared with anims/_easings.cfg:
        // such files are module data, not content, and stay out of listings.
        if ((base == NULL) || (base[0] == '_')) continue;

        if (StrategyAssetLoad(&out[n], fl.paths[i]))
        {
            // The FILE is the identity, not the name field inside it - two
            // files could otherwise both claim to be "worker", and the second
            // would be unreachable by name.
            TextCopy(out[n].name, GetFileNameWithoutExt(fl.paths[i]));
            out[n].name[SGA_NAME_MAX - 1] = '\0';
            n++;
        }
    }

    UnloadDirectoryFiles(fl);
    return n;
}
