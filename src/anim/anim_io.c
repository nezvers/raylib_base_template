// ============================================================================
//  anim_io.c  -  .cfg serialization + name/value tables for anim.h
//
//  Grammar (one token stream, whitespace separated - same reader style as
//  settings_state.c: fscanf keys, dispatch by TextIsEqual):
//
//    doc      <name> <duration> [<introEnd> <outroStart>
//                                [<loopBlend> <loopSmooth>]]
//                                       # trim optional; the loop-blend pair is
//                                       # optional too - files written before it
//                                       # existed load as smooth with the
//                                       # ANIM_LOOP_BLEND_DEFAULT length.
//    elem     <kind> <name>              # kind = text|shape|global
//      text   <quoted-single-token>      # (text elements) spaces -> '_'
//      color  <r> <g> <b> <a>
//      pos    <xFrac> <yFrac>
//      size   <xFrac> <yFrac>
//      shape  <rect|circle|square|rhombus|triangle|line>   # (shape elements)
//      outline <r> <g> <b> <a> <thickFrac>                 # (shape elements)
//      outline_style crisp                 # OPTIONAL (circle): smooth DrawRing
//                                          # outline; absent -> faceted polygon
//      track  <prop> <keyCount>         # then keyCount x `key` lines
//        key  <t> <value> <ease>            # scalar tracks
//        key  <t> <r> <g> <b> <ease>        # colour tracks (RGB; no alpha)
//      end
//    signal   <name> <length> [terminal [usesPos]]
//                                       # AFTER all elems (names resolve).
//                                       # `terminal` and `usesPos` (0/1) are
//                                       # OPTIONAL: files written before either
//                                       # existed load them as 0.
//      posparam <elemName> <slot> <keyCount>   # OPTIONAL: Mouse-Position bind
//        poskey <u> <offX> <offY> <ease>       # slot 0=center/P0, 1=P1 (corner)
//      seq    <mult> <usesSeq> <targetCount> <keyCount>   # OPTIONAL: sequence
//        seqtarget <elemName> <prop>           # scalar prop the offset adds to
//        seqkey <u> <amt> <ease>               # 0..1 envelope of the offset
//      target <elemName> <prop> <keyCount>
//                                       # (a trailing number written by an older
//                                       #  build - the dropped seqStep - is
//                                       #  ignored, so old files still load.)
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
    { AP_S_SCALE, "scale" },
};
static const PropRow k_globalProps[] = {
    { AP_G_FADE, "fade" }, { AP_G_COLOR, "color" },
    { AP_G_BG_ALPHA, "bg_alpha" }, { AP_G_BG_COLOR, "bg_color" },
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

// ---------------------------------------------------------------------------
//  Property groups (editor UX): each groups the member AP_* props that share one
//  logical target. Presentation only - storage/.cfg stay per-prop.
// ---------------------------------------------------------------------------
static const AnimPropGroup k_textGroups[] = {
    { "position", { AP_T_POS_X, AP_T_POS_Y }, 2 },
    { "size",     { AP_T_SIZE },              1 },
    { "color",    { AP_T_COLOR, AP_T_ALPHA }, 2 },
    { "rotation", { AP_T_ROT },               1 },
    { "crumble",  { AP_T_CRUMBLE },           1 },
};
static const AnimPropGroup k_shapeGroups[] = {
    { "position", { AP_S_POS_X, AP_S_POS_Y },                       2 },
    { "size",     { AP_S_W, AP_S_H },                               2 },
    { "scale",    { AP_S_SCALE },                                   1 },
    { "color",    { AP_S_COLOR, AP_S_ALPHA },                       2 },
    { "outline",  { AP_S_OUTLINE, AP_S_OUTLINE_COLOR, AP_S_OUTLINE_ALPHA }, 3 },
    { "rotation", { AP_S_ROT },                                     1 },
};
static const AnimPropGroup k_globalGroups[] = {
    { "fade",       { AP_G_FADE, AP_G_COLOR },        2 },
    { "background", { AP_G_BG_COLOR, AP_G_BG_ALPHA }, 2 },
};

static const AnimPropGroup *GroupsFor(int elemKind, int *count)
{
    switch (elemKind)
    {
        case AE_TEXT:   *count = (int)(sizeof(k_textGroups)/sizeof(k_textGroups[0]));   return k_textGroups;
        case AE_SHAPE:  *count = (int)(sizeof(k_shapeGroups)/sizeof(k_shapeGroups[0])); return k_shapeGroups;
        case AE_GLOBAL: *count = (int)(sizeof(k_globalGroups)/sizeof(k_globalGroups[0]));return k_globalGroups;
        default:        *count = 0; return NULL;
    }
}

int AnimGroupCountFor(int elemKind) { int n; GroupsFor(elemKind, &n); return n; }

const AnimPropGroup *AnimGroupAt(int elemKind, int index)
{
    int n; const AnimPropGroup *g = GroupsFor(elemKind, &n);
    if (index < 0 || index >= n) return NULL;
    return &g[index];
}

int AnimGroupIndexOfProp(int elemKind, int prop)
{
    int n; const AnimPropGroup *g = GroupsFor(elemKind, &n);
    for (int i = 0; i < n; i++)
        for (int j = 0; j < g[i].propCount; j++)
            if (g[i].props[j] == prop) return i;
    return -1;
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
        // its own token, not a third field on `size`, so files written here
        // still load in readers that predate scale.
        fprintf(f, "%s  scale %f\n", ind, e->scaleFrac);
    }
    if (e->kind == AE_GLOBAL)
    {
        fprintf(f, "%s  bg %d %d %d %d\n", ind,
                e->bgColor.r, e->bgColor.g, e->bgColor.b, e->bgColor.a);
    }

    fprintf(f, "%s  color %d %d %d %d\n", ind, e->color.r, e->color.g, e->color.b, e->color.a);
    fprintf(f, "%s  pos %f %f\n",  ind, e->posFrac.x,  e->posFrac.y);
    fprintf(f, "%s  size %f %f\n", ind, e->sizeFrac.x, e->sizeFrac.y);

    // Authoring flags, each its own optional token so files written here still
    // load in readers that predate them (absent -> the AnimElemInit default).
    if (e->sizeAbsolute)     fprintf(f, "%s  unit abs\n", ind);
    if (e->cornerMode)       fprintf(f, "%s  anchor corners\n", ind);
    if (e->outlineCrisp)     fprintf(f, "%s  outline_style crisp\n", ind);
    if (e->rotBase != 0.0f)  fprintf(f, "%s  rot %f\n", ind, e->rotBase);

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

    fprintf(f, "doc %s %f %f %f %f %d\n", doc->name[0] ? doc->name : "untitled",
            doc->duration, AnimDocIntroEnd(doc), AnimDocOutroStart(doc),
            doc->loopBlend, doc->loopSmooth ? 1 : 0);

    for (int i = 0; i < doc->elemCount; i++)
        AnimElemWriteCfg(f, &doc->elems[i], "");

    // Signals come AFTER every element: their targets name elements, so a
    // single forward pass can resolve those names on load.
    for (int i = 0; i < doc->signalCount; i++)
    {
        const AnimSignal *sg = &doc->signals[i];
        fprintf(f, "signal %s %f %d %d\n", sg->name[0] ? sg->name : "sig",
                sg->length, sg->terminal ? 1 : 0, sg->usesPos ? 1 : 0);

        // Mouse-Position bindings (the "--params--" section)
        for (int j = 0; j < sg->posParamCount; j++)
        {
            const AnimSigPosParam *pp = &sg->posParams[j];
            if (pp->elemIdx < 0 || pp->elemIdx >= doc->elemCount) continue;
            const AnimElem *e = &doc->elems[pp->elemIdx];
            fprintf(f, "  posparam %s %d %d\n",
                    e->name[0] ? e->name : "elem", pp->slot, pp->keyCount);
            for (int k = 0; k < pp->keyCount; k++)
                fprintf(f, "    poskey %f %f %f %s\n", pp->keys[k].t,
                        pp->keys[k].offX, pp->keys[k].offY,
                        AnimEaseName(pp->keys[k].ease));
        }

        // Sequence offset (the "--sequence--" section)
        if (sg->usesSeq || sg->seqTargetCount > 0 || sg->seqKeyCount > 0)
        {
            fprintf(f, "  seq %f %d %d %d\n", sg->seqMult, sg->usesSeq ? 1 : 0,
                    sg->seqTargetCount, sg->seqKeyCount);
            for (int j = 0; j < sg->seqTargetCount; j++)
            {
                const AnimSigSeqTarget *st = &sg->seqTargets[j];
                if (st->elemIdx < 0 || st->elemIdx >= doc->elemCount) continue;
                const AnimElem *e = &doc->elems[st->elemIdx];
                fprintf(f, "    seqtarget %s %s\n",
                        e->name[0] ? e->name : "elem", AnimPropName(st->prop));
            }
            for (int j = 0; j < sg->seqKeyCount; j++)
                fprintf(f, "    seqkey %f %f %s\n", sg->seqKeys[j].t,
                        sg->seqKeys[j].amt, AnimEaseName(sg->seqKeys[j].ease));
        }

        for (int j = 0; j < sg->targetCount; j++)
        {
            const AnimSigTarget *tg = &sg->targets[j];
            if (tg->elemIdx < 0 || tg->elemIdx >= doc->elemCount) continue;
            const AnimElem *e = &doc->elems[tg->elemIdx];
            fprintf(f, "  target %s %s %d\n",
                    e->name[0] ? e->name : "elem", AnimPropName(tg->prop),
                    tg->keyCount);
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
    else if (TextIsEqual(key, "scale"))
    {
        // absent in files written before scale existed - AnimElemInit's 1.0
        // stands in that case, so those load at their authored size.
        float s;
        if (fscanf(f, "%f", &s) == 1 && curElem) curElem->scaleFrac = s;
    }
    else if (TextIsEqual(key, "unit"))
    {
        // absent in older files - AnimElemInit's false (fraction) stands.
        char u[8];
        if (fscanf(f, "%7s", u) == 1 && curElem)
            curElem->sizeAbsolute = TextIsEqual(u, "abs");
    }
    else if (TextIsEqual(key, "anchor"))
    {
        char a[16];
        if (fscanf(f, "%15s", a) == 1 && curElem)
            curElem->cornerMode = TextIsEqual(a, "corners");
    }
    else if (TextIsEqual(key, "outline_style"))
    {
        // absent in older files - AnimElemInit's false (polygon) stands.
        char s[16];
        if (fscanf(f, "%15s", s) == 1 && curElem)
            curElem->outlineCrisp = TextIsEqual(s, "crisp");
    }
    else if (TextIsEqual(key, "rot"))
    {
        // elem-level rest-pose rotation (distinct from a `track rot` block,
        // whose prop name only appears AFTER the `track` keyword).
        float r;
        if (fscanf(f, "%f", &r) == 1 && curElem) curElem->rotBase = r;
    }
    else if (TextIsEqual(key, "bg"))
    {
        int r, g, b, a;
        if (fscanf(f, "%d %d %d %d", &r, &g, &b, &a) == 4 && curElem)
            curElem->bgColor = (Color){ (unsigned char)r, (unsigned char)g,
                                        (unsigned char)b, (unsigned char)a };
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
    AnimSigPosParam *curPos = NULL; // Mouse-Position binding inside it

    while (fscanf(f, "%63s", key) == 1)
    {
        if (TextIsEqual(key, "doc"))
        {
            // The trim fields are OPTIONAL (files written before intro/outro
            // existed have only name+duration), and so is the loop-blend pair
            // after them, so the rest of the LINE is taken in one go and the
            // field count decides the defaults.
            char nm[ANIM_NAME_MAX], rest[128];
            float dur = 0.0f, inEnd = 0.0f, outStart = 0.0f;
            float blend = ANIM_LOOP_BLEND_DEFAULT; int smooth = 1;
            if (fscanf(f, "%31s", nm) == 1 && fgets(rest, sizeof(rest), f))
            {
                int n = sscanf(rest, "%f %f %f %f %d", &dur, &inEnd, &outStart,
                               &blend, &smooth);
                if (n >= 1)
                {
                    TextCopy(doc->name, nm);
                    doc->duration   = dur;
                    doc->introEnd   = (n >= 3) ? inEnd : 0.0f;
                    doc->outroStart = (n >= 3) ? outStart : dur;
                    // A file predating the pair keeps AnimDocInit's defaults
                    // (smooth, ANIM_LOOP_BLEND_DEFAULT) - the sscanf above left
                    // the locals holding exactly those.
                    doc->loopBlend  = blend;
                    doc->loopSmooth = (smooth != 0);
                }
            }
            curElem = NULL; curTrack = NULL; curSig = NULL; curTgt = NULL;
            curPos = NULL;
        }
        else if (TextIsEqual(key, "elem"))
        {
            char kindStr[16], nm[ANIM_NAME_MAX];
            curSig = NULL; curTgt = NULL; curPos = NULL;
            if (fscanf(f, "%15s %31s", kindStr, nm) == 2)
            {
                curElem  = AnimDocAddElem(doc, ElemKindByName(kindStr));
                curTrack = NULL;
                if (curElem) TextCopy(curElem->name, nm);
            }
        }
        else if (TextIsEqual(key, "signal"))
        {
            // signals are written after every element, so target names resolve.
            // `terminal` and `usesPos` are OPTIONAL (files written before either
            // existed end the line earlier), so the rest of the LINE is taken in
            // one go and the field count decides the defaults - as `doc` above.
            char nm[ANIM_NAME_MAX], rest[64];
            float len = 0.0f; int term = 0, uspos = 0;
            curElem = NULL; curTrack = NULL; curSig = NULL; curTgt = NULL;
            curPos = NULL;
            if (fscanf(f, "%31s", nm) == 1 && fgets(rest, sizeof(rest), f))
            {
                int n = sscanf(rest, "%f %d %d", &len, &term, &uspos);
                if (n >= 1 && doc->signalCount < ANIM_SIGNALS_MAX)
                {
                    curSig = &doc->signals[doc->signalCount++];
                    TextCopy(curSig->name, nm);
                    curSig->length      = len;
                    curSig->terminal    = (n >= 2) && (term != 0);
                    curSig->usesPos     = (n >= 3) && (uspos != 0);
                    curSig->targetCount = 0;
                    // new collections default empty (files may omit them)
                    curSig->usesSeq        = false;
                    curSig->seqMult        = 0.0f;
                    curSig->posParamCount  = 0;
                    curSig->seqTargetCount = 0;
                    curSig->seqKeyCount    = 0;
                }
            }
        }
        else if (TextIsEqual(key, "posparam"))
        {
            // `<elemName> <slot> <keyCount>`: a Mouse-Position binding. Unknown
            // element drops it (curPos stays NULL) but still consumes the line.
            char en[ANIM_NAME_MAX], rest[64]; int slot = 0, kc = 0;
            curPos = NULL; curTgt = NULL;
            if (fscanf(f, "%31s", en) == 1 && fgets(rest, sizeof(rest), f) &&
                sscanf(rest, "%d %d", &slot, &kc) >= 1 && curSig &&
                curSig->posParamCount < ANIM_SIG_POS_MAX)
            {
                int ei = -1;
                for (int i = 0; i < doc->elemCount; i++)
                    if (TextIsEqual(doc->elems[i].name, en)) { ei = i; break; }
                if (ei >= 0)
                {
                    curPos = &curSig->posParams[curSig->posParamCount++];
                    curPos->elemIdx  = ei;
                    curPos->slot     = slot;
                    curPos->keyCount = 0;
                }
            }
        }
        else if (TextIsEqual(key, "poskey"))
        {
            float u, ox, oy; char easeName[32];
            if (curPos && fscanf(f, "%f %f %f %31s", &u, &ox, &oy, easeName) == 4 &&
                curPos->keyCount < ANIM_SIG_KEYS_MAX)
            {
                AnimPosKey *k = &curPos->keys[curPos->keyCount++];
                k->t = u; k->offX = ox; k->offY = oy;
                k->ease = AnimEaseByName(easeName);
            }
            else   // dropped binding (or full): swallow the rest of the line
            { int c; while ((c = fgetc(f)) != EOF && c != '\n') { } }
        }
        else if (TextIsEqual(key, "seq"))
        {
            // `<mult> <usesSeq> <targetCount> <keyCount>`: the sequence header.
            // Counts are informational; the seqtarget/seqkey lines fill in.
            char rest[64]; float mult = 0.0f; int us = 0, tc = 0, kc = 0;
            curPos = NULL; curTgt = NULL;
            if (fgets(rest, sizeof(rest), f) &&
                sscanf(rest, "%f %d %d %d", &mult, &us, &tc, &kc) >= 1 && curSig)
            {
                curSig->seqMult        = mult;
                curSig->usesSeq        = (us != 0);
                curSig->seqTargetCount = 0;
                curSig->seqKeyCount    = 0;
            }
        }
        else if (curSig && TextIsEqual(key, "seqtarget"))
        {
            char en[ANIM_NAME_MAX], pn[32];
            if (fscanf(f, "%31s %31s", en, pn) == 2 &&
                curSig->seqTargetCount < ANIM_SIG_SEQ_TARGETS)
            {
                int ei = -1;
                for (int i = 0; i < doc->elemCount; i++)
                    if (TextIsEqual(doc->elems[i].name, en)) { ei = i; break; }
                int prop = (ei >= 0) ? AnimPropByName(pn, doc->elems[ei].kind) : -1;
                if (ei >= 0 && prop >= 0)
                {
                    AnimSigSeqTarget *st =
                        &curSig->seqTargets[curSig->seqTargetCount++];
                    st->elemIdx = ei; st->prop = prop;
                }
            }
        }
        else if (curSig && TextIsEqual(key, "seqkey"))
        {
            float u, amt; char easeName[32];
            if (fscanf(f, "%f %f %31s", &u, &amt, easeName) == 3 &&
                curSig->seqKeyCount < ANIM_SIG_SEQ_KEYS)
            {
                AnimSeqKey *k = &curSig->seqKeys[curSig->seqKeyCount++];
                k->t = u; k->amt = amt; k->ease = AnimEaseByName(easeName);
            }
        }
        else if (TextIsEqual(key, "target"))
        {
            // `<elemName> <prop> <keyCount>`; an unresolvable element or prop
            // drops the target (curTgt stays NULL) but still consumes the line.
            // A trailing number (an older build's dropped seqStep) is read into
            // the rest of the LINE and ignored, so old files still load.
            char en[ANIM_NAME_MAX], pn[32], rest[64];
            int kc = 0;
            curTgt = NULL; curPos = NULL;
            if (fscanf(f, "%31s %31s", en, pn) == 2 && fgets(rest, sizeof(rest), f) &&
                sscanf(rest, "%d", &kc) >= 1 && curSig &&
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
        { curSig = NULL; curTgt = NULL; curPos = NULL; }
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
