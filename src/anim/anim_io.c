// ============================================================================
//  anim_io.c  -  .cfg serialization + name/value tables for anim.h
//
//  Grammar (one token stream, whitespace separated - same reader style as
//  settings_state.c: fscanf keys, dispatch by TextIsEqual):
//
//    doc      <name> <duration>
//    elem     <kind> <name>              # kind = text|shape|global
//      text   <quoted-single-token>      # (text elements) spaces -> '_'
//      color  <r> <g> <b> <a>
//      pos    <xFrac> <yFrac>
//      size   <xFrac> <yFrac>
//      shape  <rect|circle|square|rhombus|triangle|line>   # (shape elements)
//      outline <r> <g> <b> <a> <thickFrac>                 # (shape elements)
//      track  <prop> <keyCount>         # then keyCount x `key` lines
//        key  <t> <value> <ease>            # scalar tracks
//        key  <t> <r> <g> <b> <ease>        # colour tracks (RGB; no alpha)
//      end
//    signal   <name> <fwd|rev> <secStart> <secEnd>
//
//  Unknown leading keys are skipped (forward compatible). Missing file on load
//  leaves the doc initialized-empty.
// ============================================================================

#include "anim_io.h"
#include "raylib.h"     // TextIsEqual, TextCopy
#include <stdio.h>
#include <stddef.h>

// ---------------------------------------------------------------------------
//  Property name table (per element kind, in the order the editor lists them).
// ---------------------------------------------------------------------------
typedef struct { int prop; const char *name; } PropRow;

static const PropRow k_textProps[] = {
    { AP_T_POS_X, "pos_x" }, { AP_T_POS_Y, "pos_y" }, { AP_T_SIZE, "size" },
    { AP_T_ALPHA, "alpha" }, { AP_T_ROT, "rot" },     { AP_T_CRUMBLE, "crumble" },
    { AP_T_COLOR, "color" },
};
static const PropRow k_shapeProps[] = {
    { AP_S_POS_X, "pos_x" }, { AP_S_POS_Y, "pos_y" }, { AP_S_W, "w" },
    { AP_S_H, "h" },         { AP_S_ALPHA, "alpha" }, { AP_S_ROT, "rot" },
    { AP_S_COLOR, "color" },
    { AP_S_OUTLINE_COLOR, "outline_color" },
    { AP_S_OUTLINE, "outline" },
    { AP_S_OUTLINE_ALPHA, "outline_alpha" },
};
static const PropRow k_globalProps[] = {
    { AP_G_FADE, "fade" }, { AP_G_COLOR, "color" },
};

static const PropRow *PropsFor(int elemKind, int *count)
{
    switch (elemKind)
    {
        case AE_TEXT:   *count = (int)(sizeof(k_textProps)/sizeof(k_textProps[0]));   return k_textProps;
        case AE_SHAPE:  *count = (int)(sizeof(k_shapeProps)/sizeof(k_shapeProps[0])); return k_shapeProps;
        case AE_GLOBAL: *count = (int)(sizeof(k_globalProps)/sizeof(k_globalProps[0]));return k_globalProps;
        default:        *count = 0; return NULL;
    }
}

const char *AnimPropName(int prop)
{
    for (int kind = AE_TEXT; kind <= AE_GLOBAL; kind++)
    {
        int n; const PropRow *rows = PropsFor(kind, &n);
        for (int i = 0; i < n; i++)
            if (rows[i].prop == prop) return rows[i].name;
    }
    return "?";
}

int AnimPropByName(const char *name, int elemKind)
{
    int n; const PropRow *rows = PropsFor(elemKind, &n);
    for (int i = 0; i < n; i++)
        if (TextIsEqual(rows[i].name, name)) return rows[i].prop;
    return -1;
}

int AnimPropCountFor(int elemKind)      { int n; PropsFor(elemKind, &n); return n; }
int AnimPropAt(int elemKind, int index)
{
    int n; const PropRow *rows = PropsFor(elemKind, &n);
    if (index < 0 || index >= n) return -1;
    return rows[index].prop;
}

const char *AnimElemKindName(int kind)
{
    switch (kind)
    {
        case AE_TEXT:   return "text";
        case AE_SHAPE:  return "shape";
        case AE_GLOBAL: return "global";
        default:        return "text";
    }
}

static int ElemKindByName(const char *name)
{
    if (TextIsEqual(name, "shape"))  return AE_SHAPE;
    if (TextIsEqual(name, "global")) return AE_GLOBAL;
    return AE_TEXT;
}

// Shape kind <-> stable .cfg name (order matches AnimShapeKind).
static const char *k_shapeKindNames[SHAPE_KIND_COUNT] = {
    "rect", "circle", "square", "rhombus", "triangle", "line",
};

const char *AnimShapeKindName(int kind)
{
    if (kind < 0 || kind >= SHAPE_KIND_COUNT) return k_shapeKindNames[SHAPE_RECT];
    return k_shapeKindNames[kind];
}

int AnimShapeKindByName(const char *name)
{
    for (int i = 0; i < SHAPE_KIND_COUNT; i++)
        if (TextIsEqual(k_shapeKindNames[i], name)) return i;
    return SHAPE_RECT;    // unknown -> rect (old-file compatible)
}

// ---------------------------------------------------------------------------
//  Text-token helpers: the reader is whitespace-delimited, so a text element's
//  string is stored with spaces as '_' (and '_' as itself is fine - simplest
//  scheme that keeps the file a single fscanf token stream).
// ---------------------------------------------------------------------------
static void EncodeText(const char *in, char *out, int cap)
{
    int i = 0;
    for (; in[i] && i < cap - 1; i++) out[i] = (in[i] == ' ') ? '_' : in[i];
    out[i] = 0;
    if (i == 0) { out[0] = '_'; out[1] = 0; }   // never emit an empty token
}
static void DecodeText(char *s)
{
    for (int i = 0; s[i]; i++) if (s[i] == '_') s[i] = ' ';
}

// ---------------------------------------------------------------------------
//  Save
// ---------------------------------------------------------------------------
bool AnimDocSave(const AnimDoc *doc, const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) return false;

    fprintf(f, "doc %s %f\n", doc->name[0] ? doc->name : "untitled", doc->duration);

    for (int i = 0; i < doc->elemCount; i++)
    {
        const AnimElem *e = &doc->elems[i];
        fprintf(f, "elem %s %s\n", AnimElemKindName(e->kind),
                e->name[0] ? e->name : "elem");

        if (e->kind == AE_TEXT)
        {
            char enc[ANIM_TEXT_LEN_MAX];
            EncodeText(e->text, enc, ANIM_TEXT_LEN_MAX);
            fprintf(f, "  text %s\n", enc);
        }
        if (e->kind == AE_SHAPE)
        {
            fprintf(f, "  shape %s\n", AnimShapeKindName(e->shapeKind));
            fprintf(f, "  outline %d %d %d %d %f\n",
                    e->outlineColor.r, e->outlineColor.g,
                    e->outlineColor.b, e->outlineColor.a, e->outlineFrac);
        }

        fprintf(f, "  color %d %d %d %d\n", e->color.r, e->color.g, e->color.b, e->color.a);
        fprintf(f, "  pos %f %f\n",  e->posFrac.x,  e->posFrac.y);
        fprintf(f, "  size %f %f\n", e->sizeFrac.x, e->sizeFrac.y);

        for (int j = 0; j < e->trackCount; j++)
        {
            const AnimTrack *tr = &e->tracks[j];
            fprintf(f, "  track %s %d\n", AnimPropName(tr->prop), tr->keyCount);
            for (int k = 0; k < tr->keyCount; k++)
            {
                if (AnimPropIsColor(tr->prop))
                    fprintf(f, "    key %f %d %d %d %s\n", tr->keys[k].t,
                            tr->keys[k].cval.r, tr->keys[k].cval.g,
                            tr->keys[k].cval.b, AnimEaseName(tr->keys[k].ease));
                else
                    fprintf(f, "    key %f %f %s\n", tr->keys[k].t, tr->keys[k].value,
                            AnimEaseName(tr->keys[k].ease));
            }
        }
        fprintf(f, "  end\n");
    }

    for (int i = 0; i < doc->signalCount; i++)
    {
        const AnimSignal *sg = &doc->signals[i];
        fprintf(f, "signal %s %s %f %f\n", sg->name[0] ? sg->name : "sig",
                sg->dir == ANIM_REV ? "rev" : "fwd", sg->sectionStart, sg->sectionEnd);
    }

    fclose(f);
    return true;
}

// ---------------------------------------------------------------------------
//  Load. Single fscanf token stream; `elem`/`signal`/`track` open contexts.
// ---------------------------------------------------------------------------
bool AnimDocLoad(AnimDoc *doc, const char *path)
{
    AnimDocInit(doc);   // empty + defaults; stays this way if the file is absent

    FILE *f = fopen(path, "r");
    if (!f) return false;

    char     key[64];
    AnimElem *curElem  = NULL;   // element currently being filled
    AnimTrack *curTrack = NULL;  // track currently being filled

    while (fscanf(f, "%63s", key) == 1)
    {
        if (TextIsEqual(key, "doc"))
        {
            char nm[ANIM_NAME_MAX];
            if (fscanf(f, "%31s %f", nm, &doc->duration) == 2) TextCopy(doc->name, nm);
            curElem = NULL; curTrack = NULL;
        }
        else if (TextIsEqual(key, "elem"))
        {
            char kindStr[16], nm[ANIM_NAME_MAX];
            if (fscanf(f, "%15s %31s", kindStr, nm) == 2)
            {
                curElem  = AnimDocAddElem(doc, ElemKindByName(kindStr));
                curTrack = NULL;
                if (curElem) TextCopy(curElem->name, nm);
            }
        }
        else if (TextIsEqual(key, "text") && curElem)
        {
            char enc[ANIM_TEXT_LEN_MAX];
            if (fscanf(f, "%63s", enc) == 1) { DecodeText(enc); TextCopy(curElem->text, enc); }
        }
        else if (TextIsEqual(key, "shape") && curElem)
        {
            char s[16];
            if (fscanf(f, "%15s", s) == 1)
                curElem->shapeKind = AnimShapeKindByName(s);
        }
        else if (TextIsEqual(key, "outline") && curElem)
        {
            int r, g, b, a; float th;
            if (fscanf(f, "%d %d %d %d %f", &r, &g, &b, &a, &th) == 5)
            {
                curElem->outlineColor = (Color){ (unsigned char)r, (unsigned char)g,
                                                 (unsigned char)b, (unsigned char)a };
                curElem->outlineFrac  = th;
            }
        }
        else if (TextIsEqual(key, "color") && curElem)
        {
            int r, g, b, a;
            if (fscanf(f, "%d %d %d %d", &r, &g, &b, &a) == 4)
                curElem->color = (Color){ (unsigned char)r, (unsigned char)g,
                                          (unsigned char)b, (unsigned char)a };
        }
        else if (TextIsEqual(key, "pos") && curElem)
            fscanf(f, "%f %f", &curElem->posFrac.x, &curElem->posFrac.y);
        else if (TextIsEqual(key, "size") && curElem)
            fscanf(f, "%f %f", &curElem->sizeFrac.x, &curElem->sizeFrac.y);
        else if (TextIsEqual(key, "track") && curElem)
        {
            char propName[32]; int keyCount = 0;
            if (fscanf(f, "%31s %d", propName, &keyCount) == 2)
            {
                int prop = AnimPropByName(propName, curElem->kind);
                curTrack = (prop >= 0) ? AnimElemAddTrack(curElem, prop) : NULL;
            }
        }
        else if (TextIsEqual(key, "key") && curTrack)
        {
            if (AnimPropIsColor(curTrack->prop))
            {
                float t; int r, g, b; char easeName[32];
                if (fscanf(f, "%f %d %d %d %31s", &t, &r, &g, &b, easeName) == 5)
                    AnimTrackAddColorKey(curTrack,
                        t, (Color){ (unsigned char)r, (unsigned char)g,
                                    (unsigned char)b, 255 },
                        AnimEaseByName(easeName));
            }
            else
            {
                float t, v; char easeName[32];
                if (fscanf(f, "%f %f %31s", &t, &v, easeName) == 3)
                    AnimTrackAddKey(curTrack, t, v, AnimEaseByName(easeName));
            }
        }
        else if (TextIsEqual(key, "end"))
        {
            curTrack = NULL;   // element stays current until the next `elem`
        }
        else if (TextIsEqual(key, "signal"))
        {
            char nm[ANIM_NAME_MAX], dirStr[8]; float a, b;
            if (fscanf(f, "%31s %7s %f %f", nm, dirStr, &a, &b) == 4 &&
                doc->signalCount < ANIM_SIGNALS_MAX)
            {
                AnimSignal *sg = &doc->signals[doc->signalCount++];
                TextCopy(sg->name, nm);
                sg->dir          = TextIsEqual(dirStr, "rev") ? ANIM_REV : ANIM_FWD;
                sg->sectionStart = a;
                sg->sectionEnd   = b;
            }
            curElem = NULL; curTrack = NULL;
        }
        else
        {
            fscanf(f, "%*s");   // unknown leading key: skip one token
        }
    }

    fclose(f);
    return true;
}
