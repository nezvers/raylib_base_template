// ============================================================================
//  anim_library.c  -  element library persistence (see header)
//
//  File grammar (a flat token stream, same reader style as anim_io.c):
//
//    entry <libName>          # opens an entry; the next `elem` block is its
//    elem  <kind> <name>      # element, read by the SHARED element reader
//      ...                    # (base fields / track / key)
//      end
//
//  Unknown leading keys are skipped, so the format is forward compatible.
// ============================================================================

#include "anim_library.h"
#include "anim_io.h"        // AnimElemWriteCfg / AnimElemReadCfgToken
#include "raylib.h"         // TextIsEqual, TextCopy
#include <stdio.h>
#include <stddef.h>

void AnimLibraryInit(AnimLibrary *lib)
{
    if (lib) lib->count = 0;
}

int AnimLibraryFind(const AnimLibrary *lib, const char *name)
{
    if (!lib || !name) return -1;
    for (int i = 0; i < lib->count; i++)
        if (TextIsEqual(lib->entries[i].name, name)) return i;
    return -1;
}

int AnimLibraryAdd(AnimLibrary *lib, const char *name, const AnimElem *e)
{
    if (!lib || !e || !name || !name[0]) return -1;

    int at = AnimLibraryFind(lib, name);     // same name -> overwrite in place
    if (at < 0)
    {
        if (lib->count >= ANIM_LIB_MAX) return -1;
        at = lib->count++;
    }
    TextCopy(lib->entries[at].name, name);
    lib->entries[at].elem = *e;              // plain value: tracks come along
    return at;
}

void AnimLibraryRemove(AnimLibrary *lib, int idx)
{
    if (!lib || idx < 0 || idx >= lib->count) return;
    for (int i = idx; i < lib->count - 1; i++) lib->entries[i] = lib->entries[i + 1];
    lib->count--;
}

bool AnimLibraryRename(AnimLibrary *lib, int idx, const char *name)
{
    if (!lib || idx < 0 || idx >= lib->count) return false;
    if (!name || !name[0]) return false;
    int other = AnimLibraryFind(lib, name);
    if (other >= 0 && other != idx) return false;    // name already taken
    TextCopy(lib->entries[idx].name, name);
    return true;
}

bool AnimLibrarySave(const AnimLibrary *lib, const char *path)
{
    if (!lib) return false;
    FILE *f = fopen(path, "w");
    if (!f) return false;

    for (int i = 0; i < lib->count; i++)
    {
        fprintf(f, "entry %s\n", lib->entries[i].name);
        AnimElemWriteCfg(f, &lib->entries[i].elem, "  ");
    }

    fclose(f);
    return true;
}

bool AnimLibraryLoad(AnimLibrary *lib, const char *path)
{
    if (!lib) return false;
    AnimLibraryInit(lib);           // stays empty if the file is absent

    FILE *f = fopen(path, "r");
    if (!f) return false;

    char       key[64];
    AnimElem  *curElem  = NULL;     // element of the entry being filled
    AnimTrack *curTrack = NULL;

    while (fscanf(f, "%63s", key) == 1)
    {
        if (TextIsEqual(key, "entry"))
        {
            char nm[ANIM_NAME_MAX];
            curElem = NULL; curTrack = NULL;
            if (fscanf(f, "%31s", nm) == 1 && lib->count < ANIM_LIB_MAX)
            {
                AnimLibEntry *en = &lib->entries[lib->count++];
                TextCopy(en->name, nm);
                AnimElemInit(&en->elem, AE_TEXT);   // real kind set by `elem`
                curElem = &en->elem;
            }
        }
        else if (TextIsEqual(key, "elem"))
        {
            // The entry's element: re-init to the stated kind, keep the slot.
            char kindStr[16], nm[ANIM_NAME_MAX];
            curTrack = NULL;
            if (fscanf(f, "%15s %31s", kindStr, nm) == 2 && curElem)
            {
                int kind = AE_TEXT;
                if      (TextIsEqual(kindStr, "shape"))  kind = AE_SHAPE;
                else if (TextIsEqual(kindStr, "global")) kind = AE_GLOBAL;
                AnimElemInit(curElem, (AnimElemKind)kind);
                TextCopy(curElem->name, nm);
            }
        }
        else if (AnimElemReadCfgToken(f, key, curElem, &curTrack))
        {
            // handled by the shared element reader
        }
        else fscanf(f, "%*s");      // unknown leading key: skip one token
    }

    fclose(f);
    return true;
}
