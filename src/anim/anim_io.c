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
//    string   <idx> <escaped-single-token>  # shared text pool entry, BEFORE the
//                                        # elements: a `string` track keys these
//                                        # INDICES. The index is explicit
//                                        # because a deleted entry leaves a hole
//                                        # and the rest must not shift.
//    elem     <kind> <name>              # kind = text|shape|global
//      text   <escaped-single-token>     # (text elements) space -> '\s',
//                                        # newline -> '\n', '\' -> '\\'
//      color  <r> <g> <b> <a>
//      pos    <xFrac> <yFrac>
//      size   <xFrac> <yFrac>
//      shape  <rect|circle|square|rhombus|triangle|line>   # (shape elements)
//      outline <r> <g> <b> <a> <thickFrac>                 # (shape elements)
//      outline_style crisp                 # OPTIONAL (circle): smooth DrawRing
//                                          # outline; absent -> faceted polygon
//      crumble_fx <spin> <dir> <spread> <dist>   # OPTIONAL (text): REST POSE of
//                                          # the crumble scatter's shape, in
//                                          # degrees except dist (fraction of
//                                          # height) - what the element uses
//                                          # where no crumble key drives it.
//                                          # Written only when it differs from
//                                          # the defaults; absent -> 90 90 12
//                                          # 0.5, the values the effect was
//                                          # hardcoded to before it took params
//      track  <prop> <keyCount>         # then keyCount x `key` lines
//        key  <t> <value> <ease>            # scalar tracks
//        key  <t> <r> <g> <b> <ease>        # colour tracks (RGB; no alpha)
//        key  <t> <amount> <dir> <spread> <dist> <spin> <ease>
//                                           # the `crumble` track, ALWAYS: one
//                                           # crumble key is the whole state of
//                                           # the effect at that instant, so its
//                                           # five properties share a line
//                                           # instead of splitting into five
//                                           # blocks to keep time-aligned. Params
//                                           # left unkeyed are written from the
//                                           # rest pose, so the line is complete.
//                                           # Documents written before the
//                                           # scatter was keyable used the plain
//                                           # scalar form; they were migrated
//                                           # in place and are no longer read.
//      end
//    pause    <t> [<once>]              # doc-clock hold; playback stops here
//                                       # until a key is pressed. `once` (0/1)
//                                       # is OPTIONAL and defaults to 0 = holds
//                                       # on every loop cycle.
//    signal   <name> <length> [terminal [usesPos [posAnchor [replay]]]]
//                                       # AFTER all elems (names resolve).
//                                       # `terminal`, `usesPos`, `posAnchor`, `replay`
//                                       # (0/1) are OPTIONAL: files written
//                                       # before each existed load them as 0.
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
#include "anim_shape_pool.h"    // shape_ref name <-> live slot resolution
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
    // Serialized inside the `crumble` track's own key lines (see the grammar at
    // the top of this file), never as `track crumble_dir` blocks of their own.
    // The names still matter: signal `target` lines are per-property, and the
    // editor labels its member rows with them.
    { AP_T_CRUMBLE_DIR, "crumble_dir" }, { AP_T_CRUMBLE_SPREAD, "crumble_spread" },
    { AP_T_CRUMBLE_DIST, "crumble_dist" }, { AP_T_CRUMBLE_SPIN, "crumble_spin" },
    { AP_T_COLOR, "color" }, { AP_T_STRING, "string" },
};
static const PropRow k_shapeProps[] = {
    { AP_S_POS_X, "pos_x" }, { AP_S_POS_Y, "pos_y" }, { AP_S_W, "w" },
    { AP_S_H, "h" },         { AP_S_ALPHA, "alpha" }, { AP_S_ROT, "rot" },
    { AP_S_COLOR, "color" },
    { AP_S_OUTLINE_COLOR, "outline_color" },
    { AP_S_OUTLINE, "outline" },
    { AP_S_OUTLINE_ALPHA, "outline_alpha" },
    { AP_S_SCALE, "scale" },
    // NOT "shape": that token is already the element's base shapeKind field
    // (see AnimElemWriteCfg), and a collision would make the reader ambiguous.
    { AP_S_SHAPE, "shape_id" },
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
    // The one group whose members share a .cfg line: a crumble key carries the
    // amount AND the shape of the scatter at that instant.
    { "crumble",  { AP_T_CRUMBLE, AP_T_CRUMBLE_DIR, AP_T_CRUMBLE_SPREAD,
                    AP_T_CRUMBLE_DIST, AP_T_CRUMBLE_SPIN },              5 },
    { "string",   { AP_T_STRING },            1 },
};
static const AnimPropGroup k_shapeGroups[] = {
    { "position", { AP_S_POS_X, AP_S_POS_Y },                       2 },
    { "size",     { AP_S_W, AP_S_H },                               2 },
    { "scale",    { AP_S_SCALE },                                   1 },
    { "color",    { AP_S_COLOR, AP_S_ALPHA },                       2 },
    { "outline",  { AP_S_OUTLINE, AP_S_OUTLINE_COLOR, AP_S_OUTLINE_ALPHA }, 3 },
    { "rotation", { AP_S_ROT },                                     1 },
    // Single-member, so ZenGroupIsStepped reports true and the editor drops the
    // easing controls automatically (same as text's "string" group).
    { "shape",    { AP_S_SHAPE },                                   1 },
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
    "rect", "circle", "square", "rhombus", "triangle", "line", "custom",
};
// Appending to AnimShapeKind without a name here would be an out-of-bounds READ,
// not a compile error - the sized array makes it one.
_Static_assert(sizeof(k_shapeKindNames)/sizeof(k_shapeKindNames[0]) == SHAPE_KIND_COUNT,
               "k_shapeKindNames must have one entry per AnimShapeKind");

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
//  Text-token helpers. The reader is whitespace-delimited, so a text element's
//  string has to survive as one fscanf token: space -> '\s', newline -> '\n',
//  and a literal backslash doubles. Everything else is written as itself, so
//  '_' finally round-trips (the older scheme stored spaces AS '_' and could
//  not tell the two apart - see the migration note in the header comment).
//  Encoding can double the length, hence the *2 on every `enc` buffer.
// ---------------------------------------------------------------------------
// fscanf wants its field width as a plain literal (the preprocessor will not
// do the arithmetic), so it is spelled out and pinned to the capacity by a
// static assert - raise ANIM_TEXT_LEN_MAX and this fails the build until the
// scan width follows it.
#define ANIM_TEXT_ENC_WIDTH   "1023"
_Static_assert(ANIM_TEXT_LEN_MAX * 2 - 1 == 1023,
               "ANIM_TEXT_ENC_WIDTH must stay (ANIM_TEXT_LEN_MAX * 2 - 1)");
static void EncodeText(const char *in, char *out, int cap)
{
    int o = 0;
    for (int i = 0; in[i]; i++)
    {
        char esc = 0;
        switch (in[i])
        {
            case '\\': esc = '\\'; break;
            case ' ':  esc = 's';  break;
            case '\n': esc = 'n';  break;
            case '\r': continue;                    // CRLF pastes normalise out
        }
        // stop on a whole pair, never half an escape.
        if (o + (esc ? 2 : 1) > cap - 1) break;
        if (esc) { out[o++] = '\\'; out[o++] = esc; }
        else       out[o++] = in[i];
    }
    out[o] = 0;
    if (o == 0) { TextCopy(out, "\\s"); }           // never emit an empty token
}
static void DecodeText(char *s)
{
    int o = 0;
    for (int i = 0; s[i]; i++)
    {
        if (s[i] != '\\' || !s[i+1]) { s[o++] = s[i]; continue; }
        switch (s[++i])
        {
            case 's':  s[o++] = ' ';  break;
            case 'n':  s[o++] = '\n'; break;
            case '\\': s[o++] = '\\'; break;
            default:   s[o++] = '\\'; s[o++] = s[i]; break;   // unknown: as-is
        }
    }
    s[o] = 0;
}

// ---------------------------------------------------------------------------
//  The crumble group: the one group whose members share a key line
//
//  A crumble key is the whole state of the effect at that instant - how far
//  along it is AND what the scatter looks like - so the five member tracks
//  serialize as one `track crumble` block with wide key lines instead of five
//  blocks a reader would have to keep time-aligned by hand. Order below IS the
//  field order on the line.
// ---------------------------------------------------------------------------
#define CRUMBLE_PROPS 5
static const int k_crumbleProps[CRUMBLE_PROPS] = {
    AP_T_CRUMBLE, AP_T_CRUMBLE_DIR, AP_T_CRUMBLE_SPREAD,
    AP_T_CRUMBLE_DIST, AP_T_CRUMBLE_SPIN,
};

// The four SHAPE members (everything but the amount, which owns the block).
static bool CrumbleShapeProp(int prop)
{
    for (int i = 1; i < CRUMBLE_PROPS; i++)
        if (k_crumbleProps[i] == prop) return true;
    return false;
}

// The key index in `tr` at time t, or -1. Exact float compare is right here:
// the times being matched were written by the same group edit, not typed.
static int CrumbleKeyAt(const AnimElem *e, int prop, float t)
{
    AnimTrack *tr = AnimElemFindTrack((AnimElem *)e, prop);
    if (!tr) return -1;
    for (int k = 0; k < tr->keyCount; k++)
        if (tr->keys[k].t == t) return k;
    return -1;
}

// One `track crumble <n>` block, wide form. `n` is the UNION of the members'
// key times: group editing keys them as a unit, but a union costs little and
// means a ragged member can never silently drop a key on save.
static void WriteCrumbleTrack(FILE *f, const char *ind, const AnimElem *e,
                              const AnimTrack *amount)
{
    float times[ANIM_KEYS_MAX];
    int   n = 0;
    for (int i = 0; i < CRUMBLE_PROPS; i++)
    {
        AnimTrack *tr = (i == 0) ? (AnimTrack *)amount
                                 : AnimElemFindTrack((AnimElem *)e, k_crumbleProps[i]);
        for (int k = 0; tr && k < tr->keyCount && n < ANIM_KEYS_MAX; k++)
        {
            bool dup = false;
            for (int q = 0; q < n && !dup; q++) dup = (times[q] == tr->keys[k].t);
            if (!dup) times[n++] = tr->keys[k].t;
        }
    }
    for (int i = 1; i < n; i++)         // insertion sort: n <= ANIM_KEYS_MAX
    {
        float v = times[i]; int j = i - 1;
        while (j >= 0 && times[j] > v) { times[j+1] = times[j]; j--; }
        times[j+1] = v;
    }

    fprintf(f, "%s  track %s %d\n", ind, AnimPropName(AP_T_CRUMBLE), n);
    for (int i = 0; i < n; i++)
    {
        float t = times[i];
        fprintf(f, "%s    key %f", ind, t);
        for (int p = 0; p < CRUMBLE_PROPS; p++)
        {
            int prop = k_crumbleProps[p];
            int k    = CrumbleKeyAt(e, prop, t);
            // No key on this member here (ragged): write what the element
            // actually shows at t, so the reload renders identically.
            AnimTrack *tr = AnimElemFindTrack((AnimElem *)e, prop);
            fprintf(f, " %f", k >= 0 ? tr->keys[k].value : AnimElemProp(e, prop, t));
        }
        int ka = CrumbleKeyAt(e, AP_T_CRUMBLE, t);
        fprintf(f, " %s\n", AnimEaseName(ka >= 0 ? amount->keys[ka].ease
                                                 : ANIM_EASE_LINEAR));
    }
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
        char enc[ANIM_TEXT_LEN_MAX * 2];
        EncodeText(e->text, enc, sizeof(enc));
        fprintf(f, "%s  text %s\n", ind, enc);
    }
    if (e->kind == AE_SHAPE)
    {
        fprintf(f, "%s  shape %s\n", ind, AnimShapeKindName(e->shapeKind));
        if (e->shapeName[0])
            fprintf(f, "%s  shape_name %s\n", ind, e->shapeName);
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

    // Crumble scatter shape, written as one token only when it differs from the
    // defaults. Four fields that almost always hold their default would be four
    // lines of noise on every text element, and skipping them keeps documents
    // authored before the effect was parameterized byte-identical on a re-save.
    if (e->kind == AE_TEXT &&
        (e->crumbleRot    != 90.0f || e->crumbleDir  != 90.0f ||
         e->crumbleSpread != 12.0f || e->crumbleDist != 0.5f))
        fprintf(f, "%s  crumble_fx %f %f %f %f\n", ind, e->crumbleRot,
                e->crumbleDir, e->crumbleSpread, e->crumbleDist);

    // An AP_S_SHAPE key holds a pool SLOT INDEX, which is a runtime-only handle:
    // the pool is global, it is not part of this document, and its slots are
    // assigned in whatever order the shapes/ directory happened to enumerate.
    // So every index the track uses is written out with the NAME it stood for,
    // and the reader maps saved index -> name -> whatever slot that name has NOW
    // (see the fixup at the element's `end`). Names are the durable reference;
    // indices survive only as long as one run.
    for (int j = 0; j < e->trackCount; j++)
    {
        const AnimTrack *tr = &e->tracks[j];
        if (tr->prop != AP_S_SHAPE) continue;
        for (int k = 0; k < tr->keyCount; k++)
        {
            int slot = (int)(tr->keys[k].value + 0.5f);
            const AnimShapeDef *sd = AnimShapePoolGet(slot);
            if (!sd) continue;                          // unresolvable: nothing to name
            bool dupe = false;                          // one line per distinct slot
            for (int p = 0; p < k && !dupe; p++)
                dupe = ((int)(tr->keys[p].value + 0.5f) == slot);
            if (!dupe) fprintf(f, "%s  shape_ref %d %s\n", ind, slot, sd->name);
        }
    }

    for (int j = 0; j < e->trackCount; j++)
    {
        const AnimTrack *tr = &e->tracks[j];
        // The crumble group serializes as ONE block whose key lines carry all
        // five member values; the four shape members have no block of their own.
        if (CrumbleShapeProp(tr->prop)) continue;
        if (tr->prop == AP_T_CRUMBLE) { WriteCrumbleTrack(f, ind, e, tr); continue; }
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

    // The string pool comes BEFORE the elements: an element's `string` track
    // keys pool INDICES, so the entries must exist by the time they are read.
    // The index is written explicitly because a deleted entry leaves a hole and
    // the surviving indices must not shift (see AnimString).
    for (int i = 0; i < doc->stringCount; i++)
    {
        if (!doc->strings[i].used) continue;
        char enc[ANIM_TEXT_LEN_MAX * 2];
        EncodeText(doc->strings[i].text, enc, sizeof(enc));
        fprintf(f, "string %d %s\n", i, enc);
    }

    for (int i = 0; i < doc->elemCount; i++)
        AnimElemWriteCfg(f, &doc->elems[i], "");

    // Pause markers are doc-level and reference nothing, so they can sit
    // anywhere; after the elements keeps the file reading top-down in time.
    for (int i = 0; i < doc->pauseCount; i++)
        fprintf(f, "pause %f %d\n", doc->pauses[i].t,
                doc->pauses[i].once ? 1 : 0);

    // Signals come AFTER every element: their targets name elements, so a
    // single forward pass can resolve those names on load.
    for (int i = 0; i < doc->signalCount; i++)
    {
        const AnimSignal *sg = &doc->signals[i];
        fprintf(f, "signal %s %f %d %d %d %d\n", sg->name[0] ? sg->name : "sig",
                sg->length, sg->terminal ? 1 : 0, sg->usesPos ? 1 : 0,
                sg->posAnchor ? 1 : 0, sg->replay ? 1 : 0);

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
// Saved-index -> name map for the element currently being read, filled by its
// `shape_ref` lines and consumed by the fixup at its `end`. File-static because
// both callers (AnimDocLoad and the element library) open the `elem` block
// themselves and only ever have ONE element in flight; the `end` handler clears
// it, so a document whose elements have no shape_ref lines never sees a stale
// entry. Not thread-safe, in keeping with the rest of this reader.
// Sized by ANIM_SHAPE_REF_MAX, NOT by this build's pool size: the indices in a
// file are the SAVING build's slot numbers, and a 512-slot desktop build writes
// indices an 8-slot web build could never allocate. Rejecting those on the index
// would drop the line before its NAME - the thing that actually resolves - was
// ever read, losing shapes the web build does have.
static char s_shapeRefs[ANIM_SHAPE_REF_MAX][ANIM_SHAPE_NAME_MAX];
static bool s_shapeRefUsed[ANIM_SHAPE_REF_MAX];
// fscanf wants its field width as a literal, so the "%23s" reads below are
// pinned to the buffer here - raise the name cap and this fails the build until
// those widths follow it.
_Static_assert(ANIM_SHAPE_NAME_MAX == 24,
               "the %23s shape-name scan widths must stay ANIM_SHAPE_NAME_MAX-1");
// A build that can ALLOCATE a slot this reader cannot ACCEPT would write files
// it could not read back. Raise ANIM_SHAPE_REF_MAX with the pool if that day
// comes - it costs 24 bytes per index and nothing else.
_Static_assert(ANIM_SHAPE_POOL_MAX <= ANIM_SHAPE_REF_MAX,
               "ANIM_SHAPE_REF_MAX must cover every slot index this build can write");

// Rewrites every AP_S_SHAPE key from the index it had WHEN SAVED to the slot the
// same-named shape occupies NOW. A name the live pool does not have (the shape
// was deleted or renamed) becomes ANIM_SHAPE_MISSING, which draws the
// placeholder - deliberately, rather than silently resolving to whatever shape
// inherited that slot number.
static void FixupShapeRefs(AnimElem *e)
{
    if (e)
        for (int j = 0; j < e->trackCount; j++)
        {
            AnimTrack *tr = &e->tracks[j];
            if (tr->prop != AP_S_SHAPE) continue;
            for (int k = 0; k < tr->keyCount; k++)
            {
                int saved = (int)(tr->keys[k].value + 0.5f);
                if (saved < 0 || saved >= ANIM_SHAPE_REF_MAX || !s_shapeRefUsed[saved])
                {
                    tr->keys[k].value = (float)ANIM_SHAPE_MISSING;
                    continue;
                }
                tr->keys[k].value = (float)AnimShapePoolFindByName(s_shapeRefs[saved]);
            }
        }

    for (int i = 0; i < ANIM_SHAPE_REF_MAX; i++) s_shapeRefUsed[i] = false;
}

// ---------------------------------------------------------------------------
//  TRUNCATION REPORT. The capacities in anim.h are a two-tier build setting:
//  CMake raises KEYS/TRACKS/ELEMS/STRINGS on desktop and leaves the Web build
//  on the smaller values that fit its fixed 128 MB heap. A document authored on
//  desktop can therefore exceed what a Web build can hold, and every over-cap
//  add below returns NULL/-1 and is DROPPED - which used to be silent, so a
//  half-loaded animation looked like a corrupt one.
//
//  These counters record what the last AnimDocLoad had to drop, so a caller
//  (the zen editor) can say "this animation needs the desktop build" instead of
//  showing a mangled document with no explanation. Reset at the top of every
//  AnimDocLoad; file-static and not thread-safe, in keeping with this reader.
//
//  NOTE ON `tracks`: it cannot fire TODAY. The Web tier's ANIM_TRACKS_MAX (12)
//  exactly equals the largest per-kind property count (TEXT has 12), which is
//  the invariant anim.h documents and the archived suite asserts - so no
//  well-formed .cfg can present a 13th track. It is counted anyway because a
//  new property would break that tie, and a silent drop is exactly what this
//  whole mechanism exists to prevent.
// ---------------------------------------------------------------------------
static AnimLoadTrunc s_trunc;

void AnimDocLoadTruncReset(void) { s_trunc = (AnimLoadTrunc){ 0 }; }

const AnimLoadTrunc *AnimDocLoadTrunc(void) { return &s_trunc; }

bool AnimDocLoadTruncated(void)
{
    return s_trunc.elems || s_trunc.tracks || s_trunc.keys || s_trunc.strings ||
           s_trunc.signals || s_trunc.sigTargets || s_trunc.pauses;
}

int AnimDocLoadTruncMessage(char *out, int cap)
{
    if (!out || cap <= 0) return 0;
    out[0] = '\0';
    if (!AnimDocLoadTruncated()) return 0;

    // One line per exceeded capacity, "<n> <what> (max <cap>)", so the reader
    // learns BOTH how much was lost and which limit to blame.
    struct { int n; const char *what; int cap; } rows[] = {
        { s_trunc.elems,      "elements",       ANIM_ELEMS_MAX       },
        { s_trunc.tracks,     "tracks",         ANIM_TRACKS_MAX      },
        { s_trunc.keys,       "keyframes",      ANIM_KEYS_MAX        },
        { s_trunc.strings,    "text strings",   ANIM_STRINGS_MAX     },
        { s_trunc.signals,    "signals",        ANIM_SIGNALS_MAX     },
        { s_trunc.sigTargets, "signal targets", ANIM_SIG_TARGETS_MAX },
        { s_trunc.pauses,     "pause markers",  ANIM_PAUSES_MAX      },
    };
    int len = 0;
    for (int i = 0; i < (int)(sizeof rows / sizeof rows[0]); i++)
    {
        if (rows[i].n <= 0) continue;
        const char *line = TextFormat("%s%d %s (max %d)", len ? "\n" : "",
                                      rows[i].n, rows[i].what, rows[i].cap);
        int add = TextLength(line);
        if (len + add >= cap) break;            // out of room: stop cleanly
        TextCopy(out + len, line);
        len += add;
    }
    return len;
}

bool AnimElemReadCfgToken(FILE *f, const char *key, AnimElem *curElem,
                          AnimTrack **curTrack)
{
    if (TextIsEqual(key, "text"))
    {
        // width is (sizeof enc - 1) spelled out: fscanf needs it as a literal.
        char enc[ANIM_TEXT_LEN_MAX * 2];
        if (fscanf(f, "%" ANIM_TEXT_ENC_WIDTH "s", enc) == 1 && curElem)
        { DecodeText(enc); TextCopy(curElem->text, enc); }
    }
    else if (TextIsEqual(key, "shape"))
    {
        char s[16];
        if (fscanf(f, "%15s", s) == 1 && curElem)
            curElem->shapeKind = AnimShapeKindByName(s);
    }
    else if (TextIsEqual(key, "shape_name"))
    {
        // SHAPE_CUSTOM rest pose, by name. Absent in older files and in shapes
        // that are not custom - AnimElemInit's empty string stands there.
        char s[ANIM_SHAPE_NAME_MAX];
        if (fscanf(f, "%23s", s) == 1 && curElem) TextCopy(curElem->shapeName, s);
    }
    else if (TextIsEqual(key, "shape_ref"))
    {
        // Pool index -> name, as of the save. Recorded now, applied at `end`
        // once every track of this element has been read.
        int idx = -1; char s[ANIM_SHAPE_NAME_MAX];
        if (fscanf(f, "%d %23s", &idx, s) == 2 &&
            idx >= 0 && idx < ANIM_SHAPE_REF_MAX)
        {
            TextCopy(s_shapeRefs[idx], s);
            s_shapeRefUsed[idx] = true;
        }
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
    else if (TextIsEqual(key, "crumble_fx"))
    {
        // absent in older files - AnimElemInit's defaults stand, and those are
        // exactly the constants the crumble effect used before it took
        // parameters, so an old document crumbles the way it was authored to.
        float r, d, sp, ds;
        if (fscanf(f, "%f %f %f %f", &r, &d, &sp, &ds) == 4 && curElem)
        {
            curElem->crumbleRot    = r;
            curElem->crumbleDir    = d;
            curElem->crumbleSpread = sp;
            curElem->crumbleDist   = ds;
        }
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
            if (prop >= 0)
            {
                *curTrack = AnimElemAddTrack(curElem, prop);
                // AnimElemAddTrack also returns NULL for a DUPLICATE property,
                // which is a malformed file, not a capacity problem - so the
                // count is gated on the cap itself. Either way the track is
                // dropped and its `key` lines fall into the no-open-track
                // branch below, which swallows them safely.
                if (!*curTrack && curElem->trackCount >= ANIM_TRACKS_MAX)
                    s_trunc.tracks++;
            }
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
        else if (tr->prop == AP_T_CRUMBLE)
        {
            // A crumble key is wide: one line carries all five members, in
            // k_crumbleProps order, and they fan out into the member tracks
            // here. Reading the line whole (rather than fscanf-ing tokens)
            // keeps a malformed key from desyncing the rest of the stream.
            char line[256];
            if (!fgets(line, sizeof line, f)) return true;
            float t, v[CRUMBLE_PROPS]; char easeName[32];
            int n = sscanf(line, "%f %f %f %f %f %f %31s", &t, &v[0], &v[1],
                           &v[2], &v[3], &v[4], easeName);
            if (n == 1 + CRUMBLE_PROPS + 1)     // t + members + ease
            {
                int ease = AnimEaseByName(easeName);
                for (int i = 0; i < CRUMBLE_PROPS; i++)
                {
                    AnimTrack *mt = (i == 0) ? tr : NULL;
                    if (!mt && curElem)
                    {
                        mt = AnimElemFindTrack(curElem, k_crumbleProps[i]);
                        if (!mt) mt = AnimElemAddTrack(curElem, k_crumbleProps[i]);
                    }
                    if (mt && !AnimTrackAddKey(mt, t, v[i], ease))
                        s_trunc.keys++;         // ANIM_KEYS_MAX full
                    else if (!mt && curElem &&
                             curElem->trackCount >= ANIM_TRACKS_MAX)
                        s_trunc.tracks++;       // ANIM_TRACKS_MAX full
                }
            }
        }
        else if (AnimPropIsColor(tr->prop))
        {
            float t; int r, g, b; char easeName[32];
            if (fscanf(f, "%f %d %d %d %31s", &t, &r, &g, &b, easeName) == 5 &&
                !AnimTrackAddColorKey(tr, t, (Color){ (unsigned char)r,
                                      (unsigned char)g, (unsigned char)b, 255 },
                                      AnimEaseByName(easeName)))
                s_trunc.keys++;                 // ANIM_KEYS_MAX full
        }
        else
        {
            float t, v; char easeName[32];
            if (fscanf(f, "%f %f %31s", &t, &v, easeName) == 3 &&
                !AnimTrackAddKey(tr, t, v, AnimEaseByName(easeName)))
                s_trunc.keys++;                 // ANIM_KEYS_MAX full
        }
    }
    else if (TextIsEqual(key, "end"))
    {
        // Every track is in by now, so the saved shape indices can be remapped
        // onto the live pool.
        FixupShapeRefs(curElem);
        *curTrack = NULL;          // element stays current until the next `elem`
    }
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
    AnimDocLoadTruncReset();

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
                else s_trunc.elems++;   // ANIM_ELEMS_MAX full: this element and
                                        // every token in it are dropped
            }
        }
        else if (TextIsEqual(key, "string"))
        {
            // Doc-level and written before the elements, so a `string` track's
            // indices resolve on this single forward pass.
            curElem = NULL; curTrack = NULL;
            curSig = NULL; curTgt = NULL; curPos = NULL;

            int idx = -1;
            char enc[ANIM_TEXT_LEN_MAX * 2];
            int scanned = fscanf(f, "%d %" ANIM_TEXT_ENC_WIDTH "s", &idx, enc);
            if (scanned == 2 && idx >= ANIM_STRINGS_MAX) s_trunc.strings++;
            if (scanned == 2 && idx >= 0 && idx < ANIM_STRINGS_MAX)
            {
                DecodeText(enc);
                // Placed AT its stored index, holes and all - the indices are
                // what the tracks reference and must survive a round-trip.
                TextCopy(doc->strings[idx].text, enc);
                doc->strings[idx].used = true;
                if (idx >= doc->stringCount) doc->stringCount = idx + 1;
            }
        }
        else if (TextIsEqual(key, "pause"))
        {
            // Doc-level, like `doc`: close any open element/signal context so a
            // marker written between blocks cannot be read as part of one.
            curElem = NULL; curTrack = NULL;
            curSig = NULL; curTgt = NULL; curPos = NULL;

            // `once` is OPTIONAL (as with `doc` and `signal`): take the rest of
            // the LINE and let the field count decide the default.
            char rest[64];
            float t = 0.0f; int once = 0;
            if (fgets(rest, sizeof(rest), f))
            {
                int n = sscanf(rest, "%f %d", &t, &once);
                if (n >= 1 && doc->pauseCount < ANIM_PAUSES_MAX)
                {
                    AnimPause *p = AnimDocAddPause(doc, t, 1e-4f);
                    if (p) p->once = (n >= 2) && (once != 0);
                }
                else if (n >= 1) s_trunc.pauses++;   // ANIM_PAUSES_MAX full
            }
        }
        else if (TextIsEqual(key, "signal"))
        {
            // signals are written after every element, so target names resolve.
            // `terminal` and `usesPos` are OPTIONAL (files written before either
            // existed end the line earlier), so the rest of the LINE is taken in
            // one go and the field count decides the defaults - as `doc` above.
            char nm[ANIM_NAME_MAX], rest[64];
            float len = 0.0f; int term = 0, uspos = 0, panch = 0, rep = 0;
            curElem = NULL; curTrack = NULL; curSig = NULL; curTgt = NULL;
            curPos = NULL;
            if (fscanf(f, "%31s", nm) == 1 && fgets(rest, sizeof(rest), f))
            {
                int n = sscanf(rest, "%f %d %d %d %d", &len, &term, &uspos,
                               &panch, &rep);
                if (n >= 1 && doc->signalCount >= ANIM_SIGNALS_MAX)
                    s_trunc.signals++;               // ANIM_SIGNALS_MAX full
                if (n >= 1 && doc->signalCount < ANIM_SIGNALS_MAX)
                {
                    curSig = &doc->signals[doc->signalCount++];
                    TextCopy(curSig->name, nm);
                    curSig->length      = len;
                    curSig->terminal    = (n >= 2) && (term != 0);
                    curSig->usesPos     = (n >= 3) && (uspos != 0);
                    curSig->posAnchor   = (n >= 4) && (panch != 0);
                    curSig->replay      = (n >= 5) && (rep != 0);
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
                (curSig->targetCount < ANIM_SIG_TARGETS_MAX ||
                 (s_trunc.sigTargets++, false)))    // full: count it, drop it
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
