// ============================================================================
//  anim_io.c  -  .cfg serialization + name/value tables for anim.h
//
//  Grammar (one token stream, whitespace separated - same reader style as
//  settings_state.c: fscanf keys, dispatch by TextIsEqual):
//
//    doc      <name> <duration> [<introEnd> <outroStart>]   # trim optional
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
//    signal   <name> <length>          # written AFTER all elems (names resolve)
//      target <elemName> <prop> <keyCount>
//        key  <u> <value> <ease>           # u is 0..1 (fraction of length)
//        key  <u> <r> <g> <b> <ease>       # colour targets
//      endsig
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
// One element as `elem ... end`, indented by `ind`. Shared by AnimDocSave and
// the element library (anim_library.c) so the grammar has exactly one writer.
void AnimElemWriteCfg(FILE *f, const AnimElem *e, const char *ind)
{
    fprintf(f, "%selem %s %s\n", ind, AnimElemKindName(e->kind),
            e->name[0] ? e->name : "elem");

    if (e->kind == AE_TEXT)
    {
        char enc[ANIM_TEXT_LEN_MAX];
        EncodeText(e->text, enc, ANIM_TEXT_LEN_MAX);
        fprintf(f, "%s  text %s\n", ind, enc);
    }
    if (e->kind == AE_SHAPE)
    {
        fprintf(f, "%s  shape %s\n", ind, AnimShapeKindName(e->shapeKind));
        fprintf(f, "%s  outline %d %d %d %d %f\n", ind,
                e->outlineColor.r, e->outlineColor.g,
                e->outlineColor.b, e->outlineColor.a, e->outlineFrac);
    }

    fprintf(f, "%s  color %d %d %d %d\n", ind, e->color.r, e->color.g, e->color.b, e->color.a);
    fprintf(f, "%s  pos %f %f\n",  ind, e->posFrac.x,  e->posFrac.y);
    fprintf(f, "%s  size %f %f\n", ind, e->sizeFrac.x, e->sizeFrac.y);

    for (int j = 0; j < e->trackCount; j++)
    {
        const AnimTrack *tr = &e->tracks[j];
        fprintf(f, "%s  track %s %d\n", ind, AnimPropName(tr->prop), tr->keyCount);
        for (int k = 0; k < tr->keyCount; k++)
        {
            if (AnimPropIsColor(tr->prop))
                fprintf(f, "%s    key %f %d %d %d %s\n", ind, tr->keys[k].t,
                        tr->keys[k].cval.r, tr->keys[k].cval.g,
                        tr->keys[k].cval.b, AnimEaseName(tr->keys[k].ease));
            else
                fprintf(f, "%s    key %f %f %s\n", ind, tr->keys[k].t, tr->keys[k].value,
                        AnimEaseName(tr->keys[k].ease));
        }
    }
    fprintf(f, "%s  end\n", ind);
}

bool AnimDocSave(const AnimDoc *doc, const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) return false;

    fprintf(f, "doc %s %f %f %f\n", doc->name[0] ? doc->name : "untitled",
            doc->duration, AnimDocIntroEnd(doc), AnimDocOutroStart(doc));

    for (int i = 0; i < doc->elemCount; i++)
        AnimElemWriteCfg(f, &doc->elems[i], "");

    // Signals come AFTER every element: their targets name elements, so a
    // single forward pass can resolve those names on load.
    for (int i = 0; i < doc->signalCount; i++)
    {
        const AnimSignal *sg = &doc->signals[i];
        fprintf(f, "signal %s %f\n", sg->name[0] ? sg->name : "sig", sg->length);
        for (int j = 0; j < sg->targetCount; j++)
        {
            const AnimSigTarget *tg = &sg->targets[j];
            if (tg->elemIdx < 0 || tg->elemIdx >= doc->elemCount) continue;
            const AnimElem *e = &doc->elems[tg->elemIdx];
            fprintf(f, "  target %s %s %d\n",
                    e->name[0] ? e->name : "elem", AnimPropName(tg->prop), tg->keyCount);
            for (int k = 0; k < tg->keyCount; k++)
            {
                if (AnimPropIsColor(tg->prop))
                    fprintf(f, "    key %f %d %d %d %s\n", tg->keys[k].t,
                            tg->keys[k].cval.r, tg->keys[k].cval.g,
                            tg->keys[k].cval.b, AnimEaseName(tg->keys[k].ease));
                else
                    fprintf(f, "    key %f %f %s\n", tg->keys[k].t, tg->keys[k].value,
                            AnimEaseName(tg->keys[k].ease));
            }
        }
        fprintf(f, "  endsig\n");
    }

    fclose(f);
    return true;
}

// ---------------------------------------------------------------------------
//  Element-scoped reader: consumes ONE token that belongs inside an `elem`
//  block (its base fields, `track`, `key`, `end`) and returns true if it was
//  one. Shared by AnimDocLoad and the element library so the grammar has
//  exactly one reader. `curElem` may be NULL (tokens are then skipped, not
//  misapplied); `*curTrack` is the open track, updated in place.
// ---------------------------------------------------------------------------
bool AnimElemReadCfgToken(FILE *f, const char *key, AnimElem *curElem,
                          AnimTrack **curTrack)
{
    if (TextIsEqual(key, "text"))
    {
        char enc[ANIM_TEXT_LEN_MAX];
        if (fscanf(f, "%63s", enc) == 1 && curElem)
        { DecodeText(enc); TextCopy(curElem->text, enc); }
    }
    else if (TextIsEqual(key, "shape"))
    {
        char s[16];
        if (fscanf(f, "%15s", s) == 1 && curElem)
            curElem->shapeKind = AnimShapeKindByName(s);
    }
    else if (TextIsEqual(key, "outline"))
    {
        int r, g, b, a; float th;
        if (fscanf(f, "%d %d %d %d %f", &r, &g, &b, &a, &th) == 5 && curElem)
        {
            curElem->outlineColor = (Color){ (unsigned char)r, (unsigned char)g,
                                             (unsigned char)b, (unsigned char)a };
            curElem->outlineFrac  = th;
        }
    }
    else if (TextIsEqual(key, "color"))
    {
        int r, g, b, a;
        if (fscanf(f, "%d %d %d %d", &r, &g, &b, &a) == 4 && curElem)
            curElem->color = (Color){ (unsigned char)r, (unsigned char)g,
                                      (unsigned char)b, (unsigned char)a };
    }
    else if (TextIsEqual(key, "pos"))
    {
        float px, py;
        if (fscanf(f, "%f %f", &px, &py) == 2 && curElem)
            curElem->posFrac = (Vector2){ px, py };
    }
    else if (TextIsEqual(key, "size"))
    {
        float sx, sy;
        if (fscanf(f, "%f %f", &sx, &sy) == 2 && curElem)
            curElem->sizeFrac = (Vector2){ sx, sy };
    }
    else if (TextIsEqual(key, "track"))
    {
        char propName[32]; int keyCount = 0;
        *curTrack = NULL;
        if (fscanf(f, "%31s %d", propName, &keyCount) == 2 && curElem)
        {
            int prop = AnimPropByName(propName, curElem->kind);
            if (prop >= 0) *curTrack = AnimElemAddTrack(curElem, prop);
        }
    }
    else if (TextIsEqual(key, "key"))
    {
        AnimTrack *tr = *curTrack;
        if (!tr)
        {
            // No open track (unknown property): the key's arity is unknowable,
            // so swallow the REST OF THE LINE rather than a guessed token count
            // - a wrong guess would re-read numbers as leading keys and desync
            // the whole stream.
            int c;
            while ((c = fgetc(f)) != EOF && c != '\n') { }
        }
        else if (AnimPropIsColor(tr->prop))
        {
            float t; int r, g, b; char easeName[32];
            if (fscanf(f, "%f %d %d %d %31s", &t, &r, &g, &b, easeName) == 5)
                AnimTrackAddColorKey(tr, t, (Color){ (unsigned char)r,
                                     (unsigned char)g, (unsigned char)b, 255 },
                                     AnimEaseByName(easeName));
        }
        else
        {
            float t, v; char easeName[32];
            if (fscanf(f, "%f %f %31s", &t, &v, easeName) == 3)
                AnimTrackAddKey(tr, t, v, AnimEaseByName(easeName));
        }
    }
    else if (TextIsEqual(key, "end"))
        *curTrack = NULL;          // element stays current until the next `elem`
    else
        return false;              // not ours

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
    AnimSignal    *curSig = NULL;   // signal block currently open
    AnimSigTarget *curTgt = NULL;   // target inside it

    while (fscanf(f, "%63s", key) == 1)
    {
        if (TextIsEqual(key, "doc"))
        {
            // The trim fields are OPTIONAL (files written before intro/outro
            // existed have only name+duration), so the rest of the LINE is
            // taken in one go and the field count decides the defaults.
            char nm[ANIM_NAME_MAX], rest[128];
            float dur = 0.0f, inEnd = 0.0f, outStart = 0.0f;
            if (fscanf(f, "%31s", nm) == 1 && fgets(rest, sizeof(rest), f))
            {
                int n = sscanf(rest, "%f %f %f", &dur, &inEnd, &outStart);
                if (n >= 1)
                {
                    TextCopy(doc->name, nm);
                    doc->duration   = dur;
                    doc->introEnd   = (n >= 3) ? inEnd : 0.0f;
                    doc->outroStart = (n >= 3) ? outStart : dur;
                }
            }
            curElem = NULL; curTrack = NULL; curSig = NULL; curTgt = NULL;
        }
        else if (TextIsEqual(key, "elem"))
        {
            char kindStr[16], nm[ANIM_NAME_MAX];
            curSig = NULL; curTgt = NULL;
            if (fscanf(f, "%15s %31s", kindStr, nm) == 2)
            {
                curElem  = AnimDocAddElem(doc, ElemKindByName(kindStr));
                curTrack = NULL;
                if (curElem) TextCopy(curElem->name, nm);
            }
        }
        else if (TextIsEqual(key, "signal"))
        {
            // signals are written after every element, so target names resolve
            char nm[ANIM_NAME_MAX]; float len = 0.0f;
            curElem = NULL; curTrack = NULL; curSig = NULL; curTgt = NULL;
            if (fscanf(f, "%31s %f", nm, &len) == 2 &&
                doc->signalCount < ANIM_SIGNALS_MAX)
            {
                curSig = &doc->signals[doc->signalCount++];
                TextCopy(curSig->name, nm);
                curSig->length      = len;
                curSig->targetCount = 0;
            }
        }
        else if (TextIsEqual(key, "target"))
        {
            // `<elemName> <prop> <keyCount>`; an unresolvable element or prop
            // drops the target (curTgt stays NULL) but still consumes the line.
            char en[ANIM_NAME_MAX], pn[32]; int kc = 0;
            curTgt = NULL;
            if (fscanf(f, "%31s %31s %d", en, pn, &kc) == 3 && curSig &&
                curSig->targetCount < ANIM_SIG_TARGETS_MAX)
            {
                int ei = -1;
                for (int i = 0; i < doc->elemCount; i++)
                    if (TextIsEqual(doc->elems[i].name, en)) { ei = i; break; }
                int prop = (ei >= 0) ? AnimPropByName(pn, doc->elems[ei].kind) : -1;
                if (ei >= 0 && prop >= 0)
                {
                    curTgt = &curSig->targets[curSig->targetCount++];
                    curTgt->elemIdx  = ei;
                    curTgt->prop     = prop;
                    curTgt->keyCount = 0;
                }
            }
        }
        else if (TextIsEqual(key, "endsig"))
        { curSig = NULL; curTgt = NULL; }
        else if (curSig && TextIsEqual(key, "key"))
        {
            // signal keys live in the signal block, NOT in an element - handle
            // them here so the element reader never sees them.
            if (curTgt && AnimPropIsColor(curTgt->prop))
            {
                float u; int r, g, b; char easeName[32];
                if (fscanf(f, "%f %d %d %d %31s", &u, &r, &g, &b, easeName) == 5 &&
                    curTgt->keyCount < ANIM_SIG_KEYS_MAX)
                {
                    AnimKey *k = &curTgt->keys[curTgt->keyCount++];
                    k->t = u; k->value = 0.0f;
                    k->cval = (Color){ (unsigned char)r, (unsigned char)g,
                                       (unsigned char)b, 255 };
                    k->ease = AnimEaseByName(easeName);
                }
            }
            else if (curTgt)
            {
                float u, v; char easeName[32];
                if (fscanf(f, "%f %f %31s", &u, &v, easeName) == 3 &&
                    curTgt->keyCount < ANIM_SIG_KEYS_MAX)
                {
                    AnimKey *k = &curTgt->keys[curTgt->keyCount++];
                    k->t = u; k->value = v; k->cval = (Color){0,0,0,0};
                    k->ease = AnimEaseByName(easeName);
                }
            }
            else
            {
                // dropped target: arity unknown, swallow the rest of the line
                int c;
                while ((c = fgetc(f)) != EOF && c != '\n') { }
            }
        }
        else if (AnimElemReadCfgToken(f, key, curElem, &curTrack))
        {
            // handled: an element-scoped token (text/shape/color/track/key/...)
        }
        else
        {
            fscanf(f, "%*s");   // unknown leading key: skip one token
        }
    }

    fclose(f);
    return true;
}
