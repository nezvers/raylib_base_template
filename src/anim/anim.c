// ============================================================================
//  anim.c  -  authoring helpers, evaluation and playback for anim.h
//
//  Like scene_anim.c, evaluation is pure data: AnimTrackEval walks keyframes
//  and eases between them. This file only knows HOW each element kind is drawn
//  (text via DrawTextPro, shapes via DrawRectangle/Circle, global as a screen
//  fade). See anim_io.* for load/save and anim_editor for authoring.
// ============================================================================

#include "anim.h"
#include "../screen_state/screen_state.h"
#include <string.h>
#include <math.h>
#include <stddef.h>

// deterministic pseudo-random in [-1,1] from an int seed (defined below the
// draw helpers; forward-declared here for the crumble preview).
float Rand11i(int seed);

// ---------------------------------------------------------------------------
//  Authoring / defaults
// ---------------------------------------------------------------------------
void AnimDocInit(AnimDoc *doc)
{
    memset(doc, 0, sizeof(*doc));
    TextCopy(doc->name, "untitled");
    doc->duration   = 2.0f;
    doc->elemCount  = 0;
    doc->signalCount = 0;
}

void AnimElemInit(AnimElem *e, AnimElemKind kind)
{
    memset(e, 0, sizeof(*e));
    e->kind       = kind;
    e->color      = RAYWHITE;
    e->shapeKind  = SHAPE_RECT;
    e->trackCount = 0;

    switch (kind)
    {
        case AE_TEXT:
            TextCopy(e->name, "text");
            TextCopy(e->text, "TEXT");
            e->posFrac  = (Vector2){ 0.5f, 0.4f };   // x=center, y=top edge
            e->sizeFrac = (Vector2){ 0.10f, 0.10f }; // x=font size (of height)
            break;
        case AE_SHAPE:
            TextCopy(e->name, "shape");
            e->posFrac  = (Vector2){ 0.5f, 0.5f };   // center
            e->sizeFrac = (Vector2){ 0.20f, 0.20f }; // w frac, h frac
            break;
        case AE_GLOBAL:
            TextCopy(e->name, "global");
            e->color    = BLACK;                     // fade-to colour
            e->posFrac  = (Vector2){ 0.0f, 0.0f };
            e->sizeFrac = (Vector2){ 0.0f, 0.0f };
            break;
    }
}

AnimElem *AnimDocAddElem(AnimDoc *doc, AnimElemKind kind)
{
    if (doc->elemCount >= ANIM_ELEMS_MAX) return NULL;
    AnimElem *e = &doc->elems[doc->elemCount++];
    AnimElemInit(e, kind);
    return e;
}

AnimTrack *AnimElemFindTrack(AnimElem *e, int prop)
{
    for (int i = 0; i < e->trackCount; i++)
        if (e->tracks[i].prop == prop) return &e->tracks[i];
    return NULL;
}

AnimTrack *AnimElemAddTrack(AnimElem *e, int prop)
{
    if (AnimElemFindTrack(e, prop)) return NULL;      // one track per property
    if (e->trackCount >= ANIM_TRACKS_MAX) return NULL;
    AnimTrack *tr = &e->tracks[e->trackCount++];
    tr->prop     = prop;
    tr->keyCount = 0;
    return tr;
}

AnimKey *AnimTrackAddKey(AnimTrack *tr, float t, float value, EaseFn ease)
{
    if (tr->keyCount >= ANIM_KEYS_MAX) return NULL;
    AnimKey *k = &tr->keys[tr->keyCount++];
    k->t     = t;
    k->value = value;
    k->ease  = ease;
    AnimTrackSortKeys(tr);
    // The pointer may have moved after the sort; return the row that now holds t.
    for (int i = 0; i < tr->keyCount; i++)
        if (tr->keys[i].t == t && tr->keys[i].value == value) return &tr->keys[i];
    return &tr->keys[tr->keyCount - 1];
}

void AnimTrackSortKeys(AnimTrack *tr)
{
    // Insertion sort by t (keyCount is tiny; keeps keys ascending for Eval).
    for (int i = 1; i < tr->keyCount; i++)
    {
        AnimKey key = tr->keys[i];
        int j = i - 1;
        while (j >= 0 && tr->keys[j].t > key.t)
        {
            tr->keys[j + 1] = tr->keys[j];
            j--;
        }
        tr->keys[j + 1] = key;
    }
}

// ---------------------------------------------------------------------------
//  Evaluation
// ---------------------------------------------------------------------------
float AnimTrackEval(const AnimTrack *tr, float t, float missing)
{
    if (!tr || tr->keyCount == 0) return missing;
    if (t <= tr->keys[0].t)                return tr->keys[0].value;
    if (t >= tr->keys[tr->keyCount-1].t)   return tr->keys[tr->keyCount-1].value;

    // Find the segment [a, b] containing t (keys are sorted ascending).
    for (int i = 1; i < tr->keyCount; i++)
    {
        const AnimKey *b = &tr->keys[i];
        if (t <= b->t)
        {
            const AnimKey *a = &tr->keys[i - 1];
            float span = b->t - a->t;
            float p    = (span > 0.0f) ? (t - a->t) / span : 1.0f;
            if (b->ease) p = b->ease(p);       // right key's ease shapes segment
            return a->value + (b->value - a->value) * p;
        }
    }
    return tr->keys[tr->keyCount-1].value;      // unreachable, keeps compiler happy
}

// Element base value for a property (the rest pose when no track drives it).
static float ElemBaseProp(const AnimElem *e, int prop)
{
    switch (prop)
    {
        case AP_T_POS_X: case AP_S_POS_X: return e->posFrac.x;
        case AP_T_POS_Y: case AP_S_POS_Y: return e->posFrac.y;
        case AP_T_SIZE:  case AP_S_W:     return e->sizeFrac.x;
        case AP_S_H:                      return e->sizeFrac.y;
        case AP_T_ALPHA: case AP_S_ALPHA: return 1.0f;
        case AP_T_ROT:   case AP_S_ROT:   return 0.0f;
        case AP_T_CRUMBLE:                return 0.0f;
        case AP_G_FADE:                   return 0.0f;
        default:                          return 0.0f;
    }
}

float AnimElemProp(const AnimElem *e, int prop, float t)
{
    const AnimTrack *tr = NULL;
    for (int i = 0; i < e->trackCount; i++)
        if (e->tracks[i].prop == prop) { tr = &e->tracks[i]; break; }
    if (!tr) return ElemBaseProp(e, prop);
    return AnimTrackEval(tr, t, ElemBaseProp(e, prop));
}

float AnimDocMaxKeyTime(const AnimDoc *doc)
{
    float d = 0.0f;
    for (int i = 0; i < doc->elemCount; i++)
        for (int j = 0; j < doc->elems[i].trackCount; j++)
        {
            const AnimTrack *tr = &doc->elems[i].tracks[j];
            for (int k = 0; k < tr->keyCount; k++)
                if (tr->keys[k].t > d) d = tr->keys[k].t;
        }
    return d;
}

// ---------------------------------------------------------------------------
//  Drawing (game space). One helper per element kind.
// ---------------------------------------------------------------------------
static float TextSpacingFor(float sizePx)
{
    return sizePx / (float)GetFontDefault().baseSize;
}

static void DrawTextElem(const AnimElem *e, float t, Vector2 game)
{
    float alpha = AnimElemProp(e, AP_T_ALPHA, t);
    if (alpha <= 0.0f) return;

    float cxF   = AnimElemProp(e, AP_T_POS_X, t);
    float topF  = AnimElemProp(e, AP_T_POS_Y, t);
    float sizeF = AnimElemProp(e, AP_T_SIZE,  t);
    float rot   = AnimElemProp(e, AP_T_ROT,   t);
    float crumble = AnimElemProp(e, AP_T_CRUMBLE, t);

    float sizePx  = fmaxf(1.0f, game.y * sizeF);
    float spacing = TextSpacingFor(sizePx);
    Font  font    = GetFontDefault();

    Vector2 box   = MeasureTextEx(font, e->text, sizePx, spacing);
    float left    = game.x * cxF - box.x * 0.5f;   // cxF = center
    float top     = game.y * topF;
    Color col     = Fade(e->color, alpha * (1.0f - crumble));
    if (col.a == 0) return;

    if (crumble > 0.0f)
    {
        // Simple crumble preview: scatter glyphs downward by `crumble`. This is
        // an editor-friendly approximation (deterministic, no physics state) -
        // enough to see the effect on the timeline; runtime players can add the
        // full particle sim later if wanted.
        float scale = sizePx / (float)font.baseSize;
        float penX  = left;
        int   idx   = 0;
        for (const char *c = e->text; *c; c++)
        {
            int cp = (unsigned char)*c;
            GlyphInfo gi = GetGlyphInfo(font, cp);
            float advance = (gi.advanceX == 0 ? gi.image.width : gi.advanceX) * scale
                          + spacing;
            if (cp != ' ')
            {
                char buf[2]  = { (char)cp, 0 };
                float fall   = crumble * crumble * game.y * 0.5f;
                float drift  = sinf((float)idx * 12.9898f) * crumble * game.x * 0.05f;
                Vector2 gsz  = MeasureTextEx(font, buf, sizePx, spacing);
                Vector2 org  = { gsz.x * 0.5f, gsz.y * 0.5f };
                Vector2 ctr  = { penX + org.x + drift, top + org.y + fall };
                float   grot = rot + crumble * (Rand11i(idx) * 90.0f);
                DrawTextPro(font, buf, ctr, org, grot, sizePx, spacing, col);
                idx++;
            }
            penX += advance;
        }
        return;
    }

    if (rot != 0.0f)
    {
        Vector2 org = { box.x * 0.5f, box.y * 0.5f };
        Vector2 ctr = { left + org.x, top + org.y };
        DrawTextPro(font, e->text, ctr, org, rot, sizePx, spacing, col);
    }
    else
    {
        DrawTextEx(font, e->text, (Vector2){ left, top }, sizePx, spacing, col);
    }
}

static void DrawShapeElem(const AnimElem *e, float t, Vector2 game)
{
    float alpha = AnimElemProp(e, AP_S_ALPHA, t);
    if (alpha <= 0.0f) return;

    float cxF = AnimElemProp(e, AP_S_POS_X, t);
    float cyF = AnimElemProp(e, AP_S_POS_Y, t);
    float wF  = AnimElemProp(e, AP_S_W,     t);
    float hF  = AnimElemProp(e, AP_S_H,     t);
    float rot = AnimElemProp(e, AP_S_ROT,   t);

    Color col = Fade(e->color, alpha);
    if (col.a == 0) return;

    float cx = game.x * cxF;
    float cy = game.y * cyF;
    float w  = game.x * wF;
    float h  = game.y * hF;

    if (e->shapeKind == SHAPE_CIRCLE)
    {
        DrawCircle((int)cx, (int)cy, fmaxf(w, h) * 0.5f, col);
    }
    else
    {
        // Rectangle about its center, rotatable.
        Rectangle rec = { cx, cy, w, h };
        Vector2   org = { w * 0.5f, h * 0.5f };
        DrawRectanglePro(rec, org, rot, col);
    }
}

static void DrawGlobalElem(const AnimElem *e, float t, Vector2 game)
{
    float fade = AnimElemProp(e, AP_G_FADE, t);
    if (fade <= 0.0f) return;
    DrawRectangle(0, 0, (int)game.x, (int)game.y, Fade(e->color, fade));
}

void AnimDocDraw(const AnimDoc *doc, float t)
{
    Vector2 game = ScreenStateTargetSize();
    for (int i = 0; i < doc->elemCount; i++)
    {
        const AnimElem *e = &doc->elems[i];
        switch (e->kind)
        {
            case AE_TEXT:   DrawTextElem(e, t, game);   break;
            case AE_SHAPE:  DrawShapeElem(e, t, game);  break;
            case AE_GLOBAL: DrawGlobalElem(e, t, game); break;
        }
    }
}

// deterministic pseudo-random in [-1,1] from an int seed (repeatable scatter);
// same trick as scene_anim.c's Rand11, exposed here for the crumble preview.
float Rand11i(int seed)
{
    float s = sinf((float)seed * 12.9898f) * 43758.5455f;
    return 2.0f * (s - floorf(s)) - 1.0f;
}

// ---------------------------------------------------------------------------
//  Player
// ---------------------------------------------------------------------------
void AnimPlayerStart(AnimPlayer *p, const AnimDoc *doc, int dir,
                     float secStart, float secEnd)
{
    p->doc      = doc;
    p->dir      = dir;
    p->secStart = secStart;
    p->secEnd   = secEnd;
    p->clock    = 0.0f;
    p->playing  = true;
    // loop is left as the caller set it (default false via zeroed struct).
}

void AnimPlayerStartAll(AnimPlayer *p, const AnimDoc *doc, int dir)
{
    AnimPlayerStart(p, doc, dir, 0.0f, doc ? doc->duration : 0.0f);
}

static float SectionLen(const AnimPlayer *p)
{
    float len = p->secEnd - p->secStart;
    return len > 0.0f ? len : 0.0f;
}

void AnimPlayerUpdate(AnimPlayer *p, float dt)
{
    if (!p->doc || !p->playing) return;
    p->clock += dt;
    float len = SectionLen(p);
    if (p->clock >= len)
    {
        if (p->loop) p->clock = fmodf(p->clock, len > 0.0f ? len : 1.0f);
        else       { p->clock = len; p->playing = false; }
    }
}

bool AnimPlayerDone(const AnimPlayer *p)
{
    return !p->playing;
}

float AnimPlayerSampleTime(const AnimPlayer *p)
{
    // Forward: secStart -> secEnd. Reverse: secEnd -> secStart (doc runs back).
    return (p->dir == ANIM_REV) ? (p->secEnd - p->clock)
                                : (p->secStart + p->clock);
}

void AnimPlayerDraw(const AnimPlayer *p)
{
    if (!p->doc) return;
    AnimDocDraw(p->doc, AnimPlayerSampleTime(p));
}
