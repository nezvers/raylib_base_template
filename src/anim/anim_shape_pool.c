// ============================================================================
//  anim_shape_pool.c  -  global pool of hand-authored pixel shapes
//
//  See anim_shape_pool.h for the format and the "no colour lives here" rule.
//
//  Loading is two-rooted and order-sensitive: the read-only resources root goes
//  first so shipped shapes claim their names, then the writable user root, whose
//  same-named entries OVERWRITE the builtin in place. That is what lets a user
//  tweak a shipped shape without losing the ability to reference it by the name
//  their documents already use.
// ============================================================================

#include "anim_shape_pool.h"
#include <stdio.h>
#include <string.h>

static AnimShapeDef s_pool[ANIM_SHAPE_POOL_MAX];
static int s_skipped = 0;       // .shp files the last load could not fit

// ---------------------------------------------------------------------------
//  Slots
// ---------------------------------------------------------------------------
AnimShapeDef *AnimShapePoolGet(int slot)
{
    if (slot < 0 || slot >= ANIM_SHAPE_POOL_MAX || !s_pool[slot].used) return NULL;
    return &s_pool[slot];
}

bool AnimShapeIdValid(int slot) { return AnimShapePoolGet(slot) != NULL; }

int AnimShapePoolFindByName(const char *name)
{
    if (!name || !name[0]) return ANIM_SHAPE_MISSING;
    for (int i = 0; i < ANIM_SHAPE_POOL_MAX; i++)
        if (s_pool[i].used && TextIsEqual(s_pool[i].name, name)) return i;
    return ANIM_SHAPE_MISSING;
}

int AnimShapePoolCount(void)
{
    int n = 0;
    for (int i = 0; i < ANIM_SHAPE_POOL_MAX; i++) if (s_pool[i].used) n++;
    return n;
}

// A name is storable when it fits the slot, is one whitespace-free token (the
// .shp grammar and the filename both need that) and is not already taken.
bool AnimShapeNameOk(const char *name)
{
    if (!name || !name[0]) return false;
    int n = 0;
    for (const char *c = name; *c; c++, n++)
        if (*c == ' ' || *c == '\t' || *c == '\n' || *c == '\r' ||
            *c == '#'  || *c == '/'  || *c == '\\')
            return false;
    if (n >= ANIM_SHAPE_NAME_MAX) return false;
    return AnimShapePoolFindByName(name) == ANIM_SHAPE_MISSING;
}

static int ClampDim(int v)
{
    if (v < 1) return 1;
    if (v > ANIM_SHAPE_GRID_MAX) return ANIM_SHAPE_GRID_MAX;
    return v;
}

int AnimShapePoolAdd(const char *name, int w, int h)
{
    if (!AnimShapeNameOk(name)) return ANIM_SHAPE_MISSING;
    for (int i = 0; i < ANIM_SHAPE_POOL_MAX; i++)
    {
        if (s_pool[i].used) continue;
        // Texture handles are cleared with the rest: a fresh slot owns nothing.
        s_pool[i] = (AnimShapeDef){ 0 };
        TextCopy(s_pool[i].name, name);
        s_pool[i].w = ClampDim(w);
        s_pool[i].h = ClampDim(h);
        s_pool[i].used = true;
        return i;
    }
    return ANIM_SHAPE_MISSING;
}

// ---------------------------------------------------------------------------
//  Pixels
// ---------------------------------------------------------------------------
unsigned char AnimShapePx(const AnimShapeDef *s, int x, int y)
{
    if (!s || x < 0 || y < 0 || x >= s->w || y >= s->h) return ANIM_PX_EMPTY;
    return s->px[y * ANIM_SHAPE_GRID_MAX + x];
}

void AnimShapeSetPx(AnimShapeDef *s, int x, int y, unsigned char v)
{
    if (!s || x < 0 || y < 0 || x >= s->w || y >= s->h) return;
    if (v > ANIM_PX_OUTLINE) v = ANIM_PX_EMPTY;
    s->px[y * ANIM_SHAPE_GRID_MAX + x] = v;
}

void AnimShapeResize(AnimShapeDef *s, int w, int h)
{
    if (!s) return;
    w = ClampDim(w);
    h = ClampDim(h);
    // Rows live at a fixed ANIM_SHAPE_GRID_MAX stride, so the pixels a shrink
    // drops are simply the ones past the new extents - nothing to move. Growing
    // exposes bytes that are already zero (the slot was cleared on Add, and a
    // previous shrink left them untouched but out of reach)... except when a
    // shrink was followed by a grow, so clear the newly reachable region.
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            if (x >= s->w || y >= s->h) s->px[y * ANIM_SHAPE_GRID_MAX + x] = ANIM_PX_EMPTY;
    s->w = w;
    s->h = h;
    s->texValid = false;
}

// ---------------------------------------------------------------------------
//  File IO
// ---------------------------------------------------------------------------
static char PxChar(unsigned char v)
{
    return v == ANIM_PX_FILL ? '#' : (v == ANIM_PX_OUTLINE ? 'O' : '.');
}

static unsigned char PxVal(char c)
{
    return c == '#' ? ANIM_PX_FILL : (c == 'O' || c == 'o' ? ANIM_PX_OUTLINE
                                                           : ANIM_PX_EMPTY);
}

// Reads one .shp into the slot, which the caller has already claimed/cleared.
// Returns false on an unreadable file or a missing/invalid size.
static bool ReadShapeFile(const char *path, AnimShapeDef *s)
{
    FILE *f = fopen(path, "r");
    if (!f) return false;

    bool haveSize = false;
    char key[64];
    while (fscanf(f, "%63s", key) == 1)
    {
        if (key[0] == '#')                          // comment to end of line
        {
            int c;
            while ((c = fgetc(f)) != EOF && c != '\n') { }
        }
        else if (TextIsEqual(key, "shape"))
        {
            // The in-file name is advisory; the caller's basename wins, so that
            // a renamed file is referenced by what the directory actually shows.
            fscanf(f, "%*s");
        }
        else if (TextIsEqual(key, "size"))
        {
            int w = 0, h = 0;
            if (fscanf(f, "%d %d", &w, &h) == 2)
            {
                s->w = ClampDim(w);
                s->h = ClampDim(h);
                haveSize = true;
            }
        }
        else if (TextIsEqual(key, "rows"))
        {
            if (!haveSize) break;                   // rows without a size: bail
            char row[ANIM_SHAPE_GRID_MAX + 2];
            for (int y = 0; y < s->h; y++)
            {
                if (fscanf(f, "%129s", row) != 1) break;
                if (TextIsEqual(row, "end")) break; // short file, tolerate it
                for (int x = 0; x < s->w && row[x]; x++)
                    s->px[y * ANIM_SHAPE_GRID_MAX + x] = PxVal(row[x]);
            }
        }
        else if (TextIsEqual(key, "end"))
        {
            break;
        }
        else
        {
            // Unknown leading key: skip its (single) value, so a newer file with
            // extra fields still loads here. Same contract as anim_io.c.
            fscanf(f, "%*s");
        }
    }
    fclose(f);
    return haveSize;
}

bool AnimShapePoolSaveOne(int slot, const char *userRoot)
{
    AnimShapeDef *s = AnimShapePoolGet(slot);
    if (!s || !userRoot) return false;
    if (!DirectoryExists(userRoot)) MakeDirectory(userRoot);

    const char *path = TextFormat("%s%s.shp", userRoot, s->name);
    FILE *f = fopen(path, "w");
    if (!f) return false;

    fprintf(f, "# pixel shape - see src/anim/anim_shape_pool.h for the grammar\n");
    fprintf(f, "# '.' empty   '#' fill (AP_S_COLOR)   'O' outline (AP_S_OUTLINE_COLOR)\n");
    fprintf(f, "shape %s\n", s->name);
    fprintf(f, "size %d %d\n", s->w, s->h);
    fprintf(f, "rows\n");
    char row[ANIM_SHAPE_GRID_MAX + 1];
    for (int y = 0; y < s->h; y++)
    {
        for (int x = 0; x < s->w; x++) row[x] = PxChar(s->px[y*ANIM_SHAPE_GRID_MAX + x]);
        row[s->w] = '\0';
        fprintf(f, "%s\n", row);
    }
    fprintf(f, "end\n");
    fclose(f);

    // It now has a user copy shadowing any shipped one, so it is editable and
    // deletable from here on.
    s->builtin = false;
    return true;
}

bool AnimShapePoolDelete(int slot, const char *userRoot)
{
    AnimShapeDef *s = AnimShapePoolGet(slot);
    if (!s) return false;
    if (userRoot && !s->builtin)
    {
        const char *path = TextFormat("%s%s.shp", userRoot, s->name);
        if (FileExists(path)) remove(path);
    }
    AnimShapePoolInvalidate(slot);
    s->used = false;
    return true;
}

// Loads every .shp in one root. `builtin` marks where they came from; a name
// already present is OVERWRITTEN in place, keeping its slot index.
static int LoadRoot(const char *root, bool builtin)
{
    if (!root || !DirectoryExists(root)) return 0;

    int loaded = 0;
    FilePathList fl = LoadDirectoryFilesEx(root, ".shp", false);
    for (unsigned int i = 0; i < fl.count; i++)
    {
        const char *base = GetFileNameWithoutExt(fl.paths[i]);
        if (!base || !base[0] || base[0] == '_') continue;

        int slot = AnimShapePoolFindByName(base);
        if (slot == ANIM_SHAPE_MISSING)
        {
            // Claim a slot. Add() rejects names that are too long or malformed,
            // which is exactly the filter we want on filenames too.
            slot = AnimShapePoolAdd(base, 1, 1);
            if (slot == ANIM_SHAPE_MISSING)
            {
                // Pool full, or a filename Add rejects (too long / malformed).
                // Either way this file is on disk and not in the pool, which is
                // the one failure a person needs told about.
                s_skipped++;
                continue;
            }
        }
        else
        {
            AnimShapePoolInvalidate(slot);
        }

        AnimShapeDef *s = &s_pool[slot];
        // Keep the slot claimed but clear the pixels: a reload is a full replace.
        memset(s->px, 0, sizeof(s->px));
        s->w = 1;
        s->h = 1;
        if (!ReadShapeFile(fl.paths[i], s))
        {
            s->used = false;                            // unreadable: free it
            continue;
        }
        TextCopy(s->name, base);
        s->builtin = builtin;
        s->used = true;
        loaded++;
    }
    UnloadDirectoryFiles(fl);
    return loaded;
}

int AnimShapePoolLoadAll(const char *resRoot, const char *userRoot)
{
    AnimShapePoolUnloadTextures();
    memset(s_pool, 0, sizeof(s_pool));
    s_skipped = 0;

    int n = LoadRoot(resRoot, true);
    n += LoadRoot(userRoot, false);
    return n;
}

int AnimShapePoolSkipped(void) { return s_skipped; }

// ---------------------------------------------------------------------------
//  Render cache
//
//  Two ALPHA-only stencils per shape, one per ink. Drawn as two DrawTexturePro
//  calls tinted with the element's live colours, which is what keeps colour
//  fully animatable from zen. One texture per ink rather than one packed
//  texture plus a shader, because the anim layer has no shader infrastructure
//  and a shader would break the headless anim_tests build.
//
//  Baking goes through LoadTextureFromImage on a CPU-side Image, NOT a render
//  target, so it is safe inside main.c's BeginTextureMode block (which cannot
//  nest). It happens lazily on the draw path only, so anim_tests - which never
//  draws - never needs a GL context.
// ---------------------------------------------------------------------------
static Texture2D BakeStencil(const AnimShapeDef *s, unsigned char ink)
{
    Image img = GenImageColor(s->w, s->h, BLANK);
    ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_GRAY_ALPHA);
    unsigned char *p = (unsigned char *)img.data;   // 2 bytes/px: grey, alpha
    for (int y = 0; y < s->h; y++)
    {
        for (int x = 0; x < s->w; x++)
        {
            bool on = (s->px[y*ANIM_SHAPE_GRID_MAX + x] == ink);
            int i = (y * s->w + x) * 2;
            p[i]   = 255;                           // white, so the tint is exact
            p[i+1] = on ? 255 : 0;
        }
    }
    Texture2D t = LoadTextureFromImage(img);
    UnloadImage(img);
    // POINT filtering is what keeps it crisply pixelated at any scale.
    if (t.id) SetTextureFilter(t, TEXTURE_FILTER_POINT);
    return t;
}

bool AnimShapePoolTextures(int slot, Texture2D *fill, Texture2D *line)
{
    AnimShapeDef *s = AnimShapePoolGet(slot);
    if (!s) return false;
    if (!s->texValid)
    {
        if (s->fillTex.id) UnloadTexture(s->fillTex);
        if (s->lineTex.id) UnloadTexture(s->lineTex);
        s->fillTex  = BakeStencil(s, ANIM_PX_FILL);
        s->lineTex  = BakeStencil(s, ANIM_PX_OUTLINE);
        s->texValid = true;
    }
    if (fill) *fill = s->fillTex;
    if (line) *line = s->lineTex;
    return true;
}

void AnimShapePoolInvalidate(int slot)
{
    if (slot < 0 || slot >= ANIM_SHAPE_POOL_MAX) return;
    AnimShapeDef *s = &s_pool[slot];
    if (s->fillTex.id) { UnloadTexture(s->fillTex); s->fillTex = (Texture2D){ 0 }; }
    if (s->lineTex.id) { UnloadTexture(s->lineTex); s->lineTex = (Texture2D){ 0 }; }
    s->texValid = false;
}

void AnimShapePoolUnloadTextures(void)
{
    for (int i = 0; i < ANIM_SHAPE_POOL_MAX; i++) AnimShapePoolInvalidate(i);
}
