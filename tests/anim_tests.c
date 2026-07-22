// ============================================================================
//  anim_tests.c  -  headless checks for the anim / anim_io / signal modules
//
//  No window is opened: only raylib's text helpers are used, and the one
//  graphics-facing dependency (ScreenStateTargetSize) is stubbed below, so the
//  binary runs in CI / over ssh. Build: the `anim_tests` cmake target
//  (desktop only). Exit code 0 = all checks pass.
// ============================================================================

#include "raylib.h"
#include "../src/anim/anim.h"
#include "../src/anim/anim_io.h"
#include "../src/anim/anim_library.h"
#include "../src/signal/signal.h"
#include "../src/signal/anim_signal.h"
#include "../src/anim_stage/anim_stage.h"
#include "../src/anim_stage/anim_scene.h"
#include <stdio.h>
#include <math.h>

// anim.c needs this for AnimDocDraw; the tests never draw, a fixed size is fine.
Vector2 ScreenStateTargetSize(void) { return (Vector2){ 1280, 720 }; }

static int s_checks = 0, s_fails = 0;
#define CHECK(cond) do { \
    s_checks++; \
    if (!(cond)) { s_fails++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

#define CHECK_NEAR(a, b) CHECK(fabsf((a) - (b)) < 0.0001f)

// ---------------------------------------------------------------------------
//  Track evaluation
// ---------------------------------------------------------------------------
static void TestEval(void)
{
    AnimTrack tr = { AP_T_ALPHA, {{0}}, 0 };
    CHECK_NEAR(AnimTrackEval(&tr, 0.5f, 7.0f), 7.0f);       // empty -> missing

    AnimTrackAddKey(&tr, 1.0f, 0.0f, ANIM_EASE_LINEAR);
    AnimTrackAddKey(&tr, 2.0f, 1.0f, ANIM_EASE_LINEAR);
    CHECK_NEAR(AnimTrackEval(&tr, 0.0f, 0), 0.0f);          // before first
    CHECK_NEAR(AnimTrackEval(&tr, 5.0f, 0), 1.0f);          // after last
    CHECK_NEAR(AnimTrackEval(&tr, 1.5f, 0), 0.5f);          // linear midpoint

    tr.keys[1].ease = ANIM_EASE_SINE_OUT;                   // eased segment
    CHECK_NEAR(AnimTrackEval(&tr, 1.5f, 0), AnimEaseApply(ANIM_EASE_SINE_OUT, 0.5f));

    AnimTrack z = { AP_T_ALPHA, {{0}}, 0 };                 // zero-span segment
    AnimTrackAddKey(&z, 1.0f, 0.0f, ANIM_EASE_LINEAR);
    AnimTrackAddKey(&z, 1.0f, 5.0f, ANIM_EASE_LINEAR);
    float v = AnimTrackEval(&z, 1.0f, 0);
    CHECK(v == 0.0f || v == 5.0f);                          // no NaN/crash
}

static void TestSegment(void)
{
    AnimTrack tr = { AP_T_ALPHA, {{0}}, 0 };
    int i0 = -1, i1 = -1;
    CHECK(!AnimTrackSegment(&tr, 0.5f, &i0, &i1));          // empty -> false

    AnimTrackAddKey(&tr, 1.0f, 0.0f, ANIM_EASE_LINEAR);
    AnimTrackAddKey(&tr, 2.0f, 1.0f, ANIM_EASE_LINEAR);
    CHECK(AnimTrackSegment(&tr, 0.0f, &i0, &i1) && i0 == 0 && i1 == 0);  // before
    CHECK(AnimTrackSegment(&tr, 1.5f, &i0, &i1) && i0 == 0 && i1 == 1);  // mid
    CHECK(AnimTrackSegment(&tr, 5.0f, &i0, &i1) && i0 == 1 && i1 == 1);  // after
}

// ---------------------------------------------------------------------------
//  Colour tracks
// ---------------------------------------------------------------------------
static void TestEvalColor(void)
{
    CHECK(AnimPropIsColor(AP_T_COLOR) && AnimPropIsColor(AP_S_COLOR) &&
          AnimPropIsColor(AP_G_COLOR) && !AnimPropIsColor(AP_T_ALPHA));

    AnimTrack tr = { AP_S_COLOR, {{0}}, 0 };
    Color miss = { 1, 2, 3, 4 };
    Color got  = AnimTrackEvalColor(&tr, 0.5f, miss);
    CHECK(got.r == 1 && got.a == 4);                        // empty -> missing

    AnimTrackAddColorKey(&tr, 0.0f, (Color){ 0, 0, 0, 255 },      ANIM_EASE_LINEAR);
    AnimTrackAddColorKey(&tr, 1.0f, (Color){ 200, 100, 50, 255 }, ANIM_EASE_LINEAR);
    got = AnimTrackEvalColor(&tr, 0.5f, miss);
    CHECK(got.r == 100 && got.g == 50 && got.b == 25);
    CHECK(got.a == 4);                                      // alpha from missing
    got = AnimTrackEvalColor(&tr, -1.0f, miss);             // clamp before first
    CHECK(got.r == 0 && got.a == 4);

    // overshooting ease must clamp channels, not wrap the unsigned char.
    tr.keys[1].ease = ANIM_EASE_BACK_OUT;
    for (float t = 0.0f; t <= 1.0f; t += 0.05f)
    {
        Color c = AnimTrackEvalColor(&tr, t, miss);
        CHECK(c.r <= 255 && c.g <= 255);                    // uchar can't be <0;
    }                                                        // wrap would show high

    // element colour: no track -> base, colour track -> eval
    AnimElem e;
    AnimElemInit(&e, AE_SHAPE);
    e.color = (Color){ 9, 9, 9, 9 };
    got = AnimElemColor(&e, 0.5f);
    CHECK(got.r == 9 && got.a == 9);
    AnimTrack *ct = AnimElemAddTrack(&e, AP_S_COLOR);
    AnimTrackAddColorKey(ct, 0.0f, (Color){ 100, 0, 0, 255 }, ANIM_EASE_LINEAR);
    got = AnimElemColor(&e, 0.5f);
    CHECK(got.r == 100 && got.a == 9);                      // alpha stays base
}

static void TestColorKeyTimeMove(void)
{
    // regression: SetKeyTime must carry cval through the re-insert.
    AnimTrack tr = { AP_S_COLOR, {{0}}, 0 };
    AnimTrackAddColorKey(&tr, 0.0f, (Color){ 11, 22, 33, 44 }, ANIM_EASE_LINEAR);
    AnimTrackAddColorKey(&tr, 1.0f, (Color){ 55, 66, 77, 88 }, ANIM_EASE_LINEAR);
    int ni = AnimTrackSetKeyTime(&tr, 0, 1.5f);             // drag past neighbour
    CHECK(ni == 1);
    CHECK(tr.keys[1].cval.r == 11 && tr.keys[1].cval.a == 44);
    CHECK(tr.keys[0].cval.r == 55);

    // WriteColorKeyAt: within eps updates cval (ease kept), outside inserts.
    AnimKey *u = AnimTrackWriteColorKeyAt(&tr, 1.01f, (Color){ 1, 1, 1, 1 }, 0.02f);
    CHECK(tr.keyCount == 2 && u->cval.r == 1);
    AnimTrackWriteColorKeyAt(&tr, 0.5f, (Color){ 2, 2, 2, 2 }, 0.02f);
    CHECK(tr.keyCount == 3 && tr.keys[0].cval.r == 2 && tr.keys[1].cval.r == 1);
}

// ---------------------------------------------------------------------------
//  Shape outline props + per-prop colour evaluation
// ---------------------------------------------------------------------------
static void TestShapeProps(void)
{
    CHECK(AnimPropIsColor(AP_S_OUTLINE_COLOR));
    CHECK(!AnimPropIsColor(AP_S_OUTLINE) && !AnimPropIsColor(AP_S_OUTLINE_ALPHA));
    CHECK(AnimPropByName("outline", AE_SHAPE) == AP_S_OUTLINE);
    CHECK(AnimPropByName("outline_color", AE_SHAPE) == AP_S_OUTLINE_COLOR);
    CHECK(AnimPropByName("outline_alpha", AE_SHAPE) == AP_S_OUTLINE_ALPHA);
    CHECK(AnimPropByName("outline", AE_TEXT) == -1);         // shape-only

    // shape kind name round-trip; unknown falls back to rect (old-file compat)
    for (int i = 0; i < SHAPE_KIND_COUNT; i++)
        CHECK(AnimShapeKindByName(AnimShapeKindName(i)) == i);
    CHECK(AnimShapeKindByName("nonsense") == SHAPE_RECT);

    // base fallback: outline off / fully opaque by default
    AnimElem e;
    AnimElemInit(&e, AE_SHAPE);
    CHECK_NEAR(AnimElemProp(&e, AP_S_OUTLINE, 0.5f), 0.0f);
    CHECK_NEAR(AnimElemProp(&e, AP_S_OUTLINE_ALPHA, 0.5f), 1.0f);
    e.outlineFrac = 0.01f;
    CHECK_NEAR(AnimElemProp(&e, AP_S_OUTLINE, 0.5f), 0.01f);
    CHECK_NEAR(AnimPropMax(AP_S_OUTLINE), 0.05f);

    // scale: rest pose is 1 (authored size), and a 0 - what an old .cfg or a
    // zeroed struct yields - must read as 1, never collapse the shape.
    CHECK_NEAR(AnimElemProp(&e, AP_S_SCALE, 0.5f), 1.0f);
    e.scaleFrac = 0.0f;
    CHECK_NEAR(AnimElemProp(&e, AP_S_SCALE, 0.5f), 1.0f);
    e.scaleFrac = 2.5f;
    CHECK_NEAR(AnimElemProp(&e, AP_S_SCALE, 0.5f), 2.5f);
    e.scaleFrac = 1.0f;
    CHECK(!AnimPropIsColor(AP_S_SCALE));
    CHECK(AnimPropByName("scale", AE_SHAPE) == AP_S_SCALE);

    // regression: base alpha (no track) comes from the colour's A channel
    e.color.a = 0;
    CHECK_NEAR(AnimElemProp(&e, AP_S_ALPHA, 0.5f), 0.0f);
    e.color.a = 128;
    CHECK_NEAR(AnimElemProp(&e, AP_S_ALPHA, 0.5f), 128.0f/255.0f);
    e.outlineColor.a = 51;
    CHECK_NEAR(AnimElemProp(&e, AP_S_OUTLINE_ALPHA, 0.5f), 0.2f);

    // per-prop colour: fill and outline tracks must NOT alias each other
    e.color        = (Color){ 1, 1, 1, 255 };
    e.outlineColor = (Color){ 2, 2, 2, 255 };
    Color got = AnimElemColorProp(&e, AP_S_OUTLINE_COLOR, 0.0f);
    CHECK(got.r == 2);                                       // base fallback
    AnimTrack *fc = AnimElemAddTrack(&e, AP_S_COLOR);
    AnimTrack *oc = AnimElemAddTrack(&e, AP_S_OUTLINE_COLOR);
    AnimTrackAddColorKey(fc, 0.0f, (Color){ 100, 0, 0, 255 }, ANIM_EASE_LINEAR);
    AnimTrackAddColorKey(oc, 0.0f, (Color){ 0, 200, 0, 255 }, ANIM_EASE_LINEAR);
    CHECK(AnimElemColorProp(&e, AP_S_COLOR, 0.5f).r == 100);
    CHECK(AnimElemColorProp(&e, AP_S_OUTLINE_COLOR, 0.5f).g == 200);
    CHECK(AnimElemColor(&e, 0.5f).r == 100);                 // primary = fill

    // every shape property fits in ANIM_TRACKS_MAX
    AnimElem s;
    AnimElemInit(&s, AE_SHAPE);
    int n = AnimPropCountFor(AE_SHAPE);
    for (int i = 0; i < n; i++)
        CHECK(AnimElemAddTrack(&s, AnimPropAt(AE_SHAPE, i)) != NULL);
    CHECK(s.trackCount == n);
}

static void TestTrackCap(void)
{
    // every text property (incl. colour) fits in ANIM_TRACKS_MAX.
    AnimElem e;
    AnimElemInit(&e, AE_TEXT);
    int n = AnimPropCountFor(AE_TEXT);
    for (int i = 0; i < n; i++)
        CHECK(AnimElemAddTrack(&e, AnimPropAt(AE_TEXT, i)) != NULL);
    CHECK(e.trackCount == n);
}

// ---------------------------------------------------------------------------
//  Ease tables
// ---------------------------------------------------------------------------
static void TestEase(void)
{
    CHECK(AnimEaseByName("sineOut") == ANIM_EASE_SINE_OUT);
    CHECK(AnimEaseByName("nonsense") == ANIM_EASE_LINEAR);
    CHECK(AnimEaseByName("linear") == ANIM_EASE_LINEAR);
    for (int i = 0; i < AnimEaseCount(); i++)
        CHECK(AnimEaseByName(AnimEaseName(i)) == i);        // name round-trip
    CHECK_NEAR(AnimEaseApply(ANIM_EASE_LINEAR, 0.3f), 0.3f);
    CHECK_NEAR(AnimEaseApply(-5, 0.3f), 0.3f);              // bad id -> linear
    CHECK_NEAR(AnimEaseApply(ANIM_EASE_COUNT + 3, 0.3f), 0.3f);
    CHECK_NEAR(AnimEaseApply(ANIM_EASE_SINE_OUT, 0.0f), 0.0f);
    CHECK_NEAR(AnimEaseApply(ANIM_EASE_SINE_OUT, 1.0f), 1.0f);
}

// ---------------------------------------------------------------------------
//  Key editing helpers
// ---------------------------------------------------------------------------
static void TestKeys(void)
{
    AnimTrack tr = { AP_S_POS_X, {{0}}, 0 };

    // AddKey inserts sorted and returns the exact slot.
    AnimKey *k2 = AnimTrackAddKey(&tr, 2.0f, 20.0f, ANIM_EASE_LINEAR);
    AnimKey *k0 = AnimTrackAddKey(&tr, 0.0f, 0.0f, ANIM_EASE_LINEAR);
    AnimKey *k1 = AnimTrackAddKey(&tr, 1.0f, 10.0f, ANIM_EASE_SINE_IN);
    CHECK(tr.keyCount == 3);
    CHECK(k0 == &tr.keys[0] && k0->t == 0.0f);
    CHECK(k1 == &tr.keys[1] && k1->value == 10.0f && k1->ease == ANIM_EASE_SINE_IN);
    (void)k2;   // k2's pointer is stale after later inserts shifted the rows
    CHECK(tr.keys[2].t == 2.0f && tr.keys[2].value == 20.0f);
    CHECK(tr.keys[0].t <= tr.keys[1].t && tr.keys[1].t <= tr.keys[2].t);

    // capacity: fill to ANIM_KEYS_MAX, the next add fails.
    while (tr.keyCount < ANIM_KEYS_MAX)
        AnimTrackAddKey(&tr, 3.0f + tr.keyCount, 0, ANIM_EASE_LINEAR);
    CHECK(AnimTrackAddKey(&tr, 99.0f, 0, ANIM_EASE_LINEAR) == NULL);

    // SetKeyTime: drag key 0 past key 1, index follows, order stays sorted.
    AnimTrack dr = { AP_S_POS_X, {{0}}, 0 };
    AnimTrackAddKey(&dr, 0.0f, 111.0f, ANIM_EASE_LINEAR);
    AnimTrackAddKey(&dr, 1.0f, 222.0f, ANIM_EASE_LINEAR);
    AnimTrackAddKey(&dr, 2.0f, 333.0f, ANIM_EASE_LINEAR);
    int ni = AnimTrackSetKeyTime(&dr, 0, 1.5f);
    CHECK(ni == 1);
    CHECK_NEAR(dr.keys[1].value, 111.0f);                   // the dragged key
    CHECK(dr.keys[0].t <= dr.keys[1].t && dr.keys[1].t <= dr.keys[2].t);
    CHECK(AnimTrackSetKeyTime(&dr, 99, 0.0f) == -1);        // bad index

    // WriteKeyAt: within eps updates value (ease kept), outside inserts.
    AnimTrack wr = { AP_S_POS_X, {{0}}, 0 };
    AnimTrackAddKey(&wr, 1.0f, 5.0f, ANIM_EASE_BACK_OUT);
    AnimKey *u = AnimTrackWriteKeyAt(&wr, 1.01f, 9.0f, 0.02f);
    CHECK(wr.keyCount == 1 && u->value == 9.0f && u->ease == ANIM_EASE_BACK_OUT);
    AnimTrackWriteKeyAt(&wr, 1.5f, 7.0f, 0.02f);
    CHECK(wr.keyCount == 2 && wr.keys[1].ease == ANIM_EASE_LINEAR);

    // RemoveKey shifts down.
    AnimTrackRemoveKey(&wr, 0);
    CHECK(wr.keyCount == 1 && wr.keys[0].value == 7.0f);
}

// ---------------------------------------------------------------------------
//  Doc / element helpers
// ---------------------------------------------------------------------------
static void TestDoc(void)
{
    AnimDoc doc;
    AnimDocInit(&doc);
    AnimElem *a = AnimDocAddElem(&doc, AE_TEXT);
    AnimElem *b = AnimDocAddElem(&doc, AE_SHAPE);
    (void)b;
    AnimDocAddElem(&doc, AE_GLOBAL);
    CHECK(doc.elemCount == 3);

    // base fallback: no track -> base field / implied default
    a->posFrac.x = 0.33f;
    CHECK_NEAR(AnimElemProp(a, AP_T_POS_X, 0.7f), 0.33f);
    CHECK_NEAR(AnimElemProp(a, AP_T_ALPHA, 0.7f), 1.0f);

    AnimTrack *tr = AnimElemAddTrack(a, AP_T_ALPHA);
    CHECK(AnimElemAddTrack(a, AP_T_ALPHA) == NULL);         // one per property
    AnimTrackAddKey(tr, 0.0f, 0.0f, ANIM_EASE_LINEAR);
    AnimTrackAddKey(tr, 1.5f, 1.0f, ANIM_EASE_LINEAR);
    CHECK_NEAR(AnimDocMaxKeyTime(&doc), 1.5f);

    AnimDocRemoveElem(&doc, 0);
    CHECK(doc.elemCount == 2 && doc.elems[0].kind == AE_SHAPE);

    // slider ranges
    CHECK_NEAR(AnimPropMin(AP_T_ROT), -360.0f);
    CHECK_NEAR(AnimPropMax(AP_S_ROT),  360.0f);
    CHECK_NEAR(AnimPropMax(AP_T_ALPHA), 1.0f);
    CHECK_NEAR(AnimPropMin(AP_T_POS_X), -1.0f);   // off-screen keying (slide-in)
    CHECK_NEAR(AnimPropMax(AP_S_POS_Y),  2.0f);
    CHECK_NEAR(AnimPropMin(AP_S_W), 0.0f);
}

static void TestRemoveTrack(void)
{
    AnimElem e;
    AnimElemInit(&e, AE_TEXT);
    AnimElemAddTrack(&e, AP_T_POS_X);
    AnimElemAddTrack(&e, AP_T_POS_Y);
    AnimElemAddTrack(&e, AP_T_ALPHA);
    CHECK(e.trackCount == 3);

    AnimElemRemoveTrack(&e, 1);                             // middle: later shift
    CHECK(e.trackCount == 2);
    CHECK(e.tracks[0].prop == AP_T_POS_X && e.tracks[1].prop == AP_T_ALPHA);

    AnimElemRemoveTrack(&e, -1);                            // out of range: no-op
    AnimElemRemoveTrack(&e, 2);
    CHECK(e.trackCount == 2);

    AnimElemRemoveTrack(&e, 1);
    AnimElemRemoveTrack(&e, 0);
    CHECK(e.trackCount == 0);

    // deleting down to the LAST key is legal - empty track -> base value
    AnimTrack *tr = AnimElemAddTrack(&e, AP_T_ALPHA);
    AnimTrackAddKey(tr, 0.0f, 0.25f, ANIM_EASE_LINEAR);
    AnimTrackAddKey(tr, 1.0f, 0.75f, ANIM_EASE_LINEAR);
    AnimTrackRemoveKey(tr, 1);
    AnimTrackRemoveKey(tr, 0);
    CHECK(tr->keyCount == 0);
    e.color.a = 255;
    CHECK_NEAR(AnimElemProp(&e, AP_T_ALPHA, 0.5f), 1.0f);   // base fallback
}

// ---------------------------------------------------------------------------
//  Element reorder / duplicate (element order == draw order)
// ---------------------------------------------------------------------------
static void TestMoveDuplicateElem(void)
{
    AnimDoc doc;
    AnimDocInit(&doc);
    TextCopy(AnimDocAddElem(&doc, AE_TEXT)->name,  "a");
    TextCopy(AnimDocAddElem(&doc, AE_SHAPE)->name, "b");
    TextCopy(AnimDocAddElem(&doc, AE_TEXT)->name,  "c");
    CHECK(doc.elemCount == 3);

    AnimDocMoveElem(&doc, 1, -1);                           // b up: b a c
    CHECK(TextIsEqual(doc.elems[0].name, "b") &&
          TextIsEqual(doc.elems[1].name, "a"));
    AnimDocMoveElem(&doc, 0, +1);                           // back: a b c
    CHECK(TextIsEqual(doc.elems[0].name, "a") &&
          TextIsEqual(doc.elems[1].name, "b"));

    AnimDocMoveElem(&doc, 0, -1);                           // top edge: no-op
    AnimDocMoveElem(&doc, 2, +1);                           // bottom edge: no-op
    AnimDocMoveElem(&doc, -1, +1);                          // bad index: no-op
    CHECK(doc.elemCount == 3);
    CHECK(TextIsEqual(doc.elems[0].name, "a") &&
          TextIsEqual(doc.elems[2].name, "c"));

    // duplicate carries tracks + keys and lands directly after the source
    AnimTrack *tr = AnimElemAddTrack(&doc.elems[0], AP_T_ALPHA);
    AnimTrackAddKey(tr, 0.0f, 0.0f, ANIM_EASE_LINEAR);
    AnimTrackAddKey(tr, 1.0f, 1.0f, ANIM_EASE_SINE_OUT);

    AnimElem *dup = AnimDocDuplicateElem(&doc, 0);
    CHECK(dup != NULL && doc.elemCount == 4);
    CHECK(dup == &doc.elems[1]);                            // inserted after src
    CHECK(TextIsEqual(dup->name, "a_2"));                   // uniquified
    CHECK(dup->trackCount == 1 && dup->tracks[0].keyCount == 2);
    CHECK(dup->tracks[0].keys[1].ease == ANIM_EASE_SINE_OUT);
    CHECK(TextIsEqual(doc.elems[2].name, "b"));             // tail shifted, intact

    AnimElem *dup2 = AnimDocDuplicateElem(&doc, 0);         // "a" again -> a_3
    CHECK(dup2 != NULL && TextIsEqual(dup2->name, "a_3"));

    CHECK(AnimDocDuplicateElem(&doc, -1) == NULL);          // bad index
    while (doc.elemCount < ANIM_ELEMS_MAX) AnimDocAddElem(&doc, AE_TEXT);
    CHECK(AnimDocDuplicateElem(&doc, 0) == NULL);           // full doc
}

// ---------------------------------------------------------------------------
//  IO round-trip
// ---------------------------------------------------------------------------
static void TestIO(void)
{
    const char *path = "anim_tests_tmp.cfg";

    AnimDoc doc;
    AnimDocInit(&doc);
    TextCopy(doc.name, "roundtrip");
    doc.duration = 3.25f;

    AnimElem *t = AnimDocAddElem(&doc, AE_TEXT);
    TextCopy(t->name, "title");
    TextCopy(t->text, "HELLO WORLD");                       // space encoding
    t->color = (Color){ 10, 20, 30, 200 };
    t->posFrac = (Vector2){ 0.25f, 0.5f };
    AnimTrack *tr = AnimElemAddTrack(t, AP_T_ALPHA);
    AnimTrackAddKey(tr, 0.0f, 0.0f, ANIM_EASE_LINEAR);
    AnimTrackAddKey(tr, 1.0f, 1.0f, ANIM_EASE_BOUNCE_OUT);
    AnimTrack *tp = AnimElemAddTrack(t, AP_T_POS_Y);
    AnimTrackAddKey(tp, 0.5f, 0.9f, ANIM_EASE_BACK_IN);

    AnimElem *s = AnimDocAddElem(&doc, AE_SHAPE);
    s->shapeKind = SHAPE_CIRCLE;
    s->sizeFrac = (Vector2){ 0.4f, 0.2f };
    s->outlineColor = (Color){ 130, 150, 180, 255 };
    s->outlineFrac  = 0.004f;
    s->scaleFrac    = 1.75f;
    AnimTrack *sc = AnimElemAddTrack(s, AP_S_COLOR);
    AnimTrackAddColorKey(sc, 0.0f, (Color){ 10, 20, 30, 40 },     ANIM_EASE_LINEAR);
    AnimTrackAddColorKey(sc, 1.0f, (Color){ 250, 200, 150, 255 }, ANIM_EASE_SINE_OUT);
    AnimTrack *oc = AnimElemAddTrack(s, AP_S_OUTLINE_COLOR);
    AnimTrackAddColorKey(oc, 0.5f, (Color){ 7, 8, 9, 255 }, ANIM_EASE_LINEAR);
    AnimTrack *oa = AnimElemAddTrack(s, AP_S_OUTLINE_ALPHA);
    AnimTrackAddKey(oa, 0.0f, 1.0f, ANIM_EASE_LINEAR);
    AnimTrackAddKey(oa, 2.0f, 0.0f, ANIM_EASE_SINE_OUT);

    // one elem per remaining shape kind so every kind name round-trips
    for (int k = SHAPE_SQUARE; k < SHAPE_KIND_COUNT; k++)
    {
        AnimElem *x = AnimDocAddElem(&doc, AE_SHAPE);
        x->shapeKind = k;
    }

    doc.signalCount = 2;
    TextCopy(doc.signals[0].name, "enter");
    doc.signals[0].length      = 1.5f;
    doc.signals[0].targetCount = 0;

    TextCopy(doc.signals[1].name, "leave");
    doc.signals[1].length      = 2.0f;
    doc.signals[1].targetCount = 2;
    // scalar target on elem 0 (a text element)
    doc.signals[1].targets[0] = (AnimSigTarget){0};
    doc.signals[1].targets[0].elemIdx  = 0;
    doc.signals[1].targets[0].prop     = AP_T_POS_Y;
    doc.signals[1].targets[0].keyCount = 2;
    doc.signals[1].targets[0].keys[0] =
        (AnimKey){ 0.5f, 0.25f, (Color){0,0,0,0}, ANIM_EASE_SINE_OUT };
    doc.signals[1].targets[0].keys[1] =
        (AnimKey){ 1.0f, 0.90f, (Color){0,0,0,0}, ANIM_EASE_BACK_IN };
    // colour target on elem 0 (keys carry cval, not value)
    doc.signals[1].targets[1] = (AnimSigTarget){0};
    doc.signals[1].targets[1].elemIdx  = 0;
    doc.signals[1].targets[1].prop     = AP_T_COLOR;
    doc.signals[1].targets[1].keyCount = 1;
    doc.signals[1].targets[1].keys[0] =
        (AnimKey){ 1.0f, 0.0f, (Color){ 7, 8, 9, 255 }, ANIM_EASE_LINEAR };

    CHECK(AnimDocSave(&doc, path));

    AnimDoc in;
    CHECK(AnimDocLoad(&in, path));
    CHECK(TextIsEqual(in.name, "roundtrip"));
    CHECK_NEAR(in.duration, 3.25f);
    CHECK(in.elemCount == 2 + (SHAPE_KIND_COUNT - SHAPE_SQUARE));
    CHECK(TextIsEqual(in.elems[0].text, "HELLO WORLD"));
    CHECK(in.elems[0].color.a == 200);
    CHECK(in.elems[0].trackCount == 2);
    CHECK(in.elems[0].tracks[0].keyCount == 2);
    CHECK(in.elems[0].tracks[0].keys[1].ease == ANIM_EASE_BOUNCE_OUT);
    CHECK(in.elems[0].tracks[1].keys[0].ease == ANIM_EASE_BACK_IN);
    CHECK(in.elems[1].kind == AE_SHAPE && in.elems[1].shapeKind == SHAPE_CIRCLE);
    CHECK_NEAR(in.elems[1].sizeFrac.y, 0.2f);
    CHECK(in.elems[1].trackCount == 3 && in.elems[1].tracks[0].prop == AP_S_COLOR);
    CHECK(in.elems[1].tracks[0].keyCount == 2);
    CHECK(in.elems[1].tracks[0].keys[0].cval.b == 30);
    CHECK(in.elems[1].tracks[0].keys[1].cval.r == 250 &&
          in.elems[1].tracks[0].keys[1].cval.a == 255);
    CHECK(in.elems[1].tracks[0].keys[1].ease == ANIM_EASE_SINE_OUT);

    // outline base + tracks round-trip
    CHECK(in.elems[1].outlineColor.r == 130 && in.elems[1].outlineColor.b == 180);
    CHECK_NEAR(in.elems[1].outlineFrac, 0.004f);
    CHECK_NEAR(in.elems[1].scaleFrac, 1.75f);
    CHECK(in.elems[1].tracks[1].prop == AP_S_OUTLINE_COLOR);
    CHECK(in.elems[1].tracks[1].keys[0].cval.g == 8);
    CHECK(in.elems[1].tracks[2].prop == AP_S_OUTLINE_ALPHA);
    CHECK(in.elems[1].tracks[2].keyCount == 2 &&
          in.elems[1].tracks[2].keys[1].ease == ANIM_EASE_SINE_OUT);

    // every shape kind survives the name round-trip
    for (int k = SHAPE_SQUARE; k < SHAPE_KIND_COUNT; k++)
        CHECK(in.elems[2 + k - SHAPE_SQUARE].shapeKind == k);
    CHECK(in.signalCount == 2);
    CHECK_NEAR(in.signals[0].length, 1.5f);
    CHECK(in.signals[0].targetCount == 0);

    CHECK_NEAR(in.signals[1].length, 2.0f);
    CHECK(in.signals[1].targetCount == 2);
    // targets are stored BY ELEMENT NAME and resolved back to an index
    CHECK(in.signals[1].targets[0].elemIdx == 0);
    CHECK(in.signals[1].targets[0].prop == AP_T_POS_Y);
    CHECK(in.signals[1].targets[0].keyCount == 2);
    CHECK_NEAR(in.signals[1].targets[0].keys[0].t, 0.5f);      // normalized u
    CHECK_NEAR(in.signals[1].targets[0].keys[1].value, 0.90f);
    CHECK(in.signals[1].targets[0].keys[1].ease == ANIM_EASE_BACK_IN);
    CHECK(in.signals[1].targets[1].prop == AP_T_COLOR);
    CHECK(in.signals[1].targets[1].keys[0].cval.r == 7);
    CHECK(in.signals[1].targets[1].keys[0].cval.b == 9);

    remove(path);
}

// A target naming an element that no longer exists must drop cleanly, and the
// rest of the file must keep parsing (the reader has to consume the orphan's
// key lines rather than letting their numbers desync the token stream).
static void TestIOSignalOrphanTarget(void)
{
    const char *path = "anim_tests_orphan.cfg";
    FILE *f = fopen(path, "w");
    CHECK(f != NULL);
    if (!f) return;
    fprintf(f, "doc orphan 2.0\n"
               "elem text keeper\n"
               "  color 1 2 3 255\n"
               "  pos 0.5 0.5\n"
               "  size 0.1 0.1\n"
               "  end\n"
               "signal s1 1.0\n"
               "  target ghost pos_y 2\n"          // element does not exist
               "    key 0.500000 0.250000 linear\n"
               "    key 1.000000 0.750000 sineOut\n"
               "  target keeper pos_x 1\n"         // this one must survive
               "    key 1.000000 0.800000 linear\n"
               "  endsig\n"
               "signal s2 0.5\n"
               "  endsig\n");
    fclose(f);

    AnimDoc in;
    CHECK(AnimDocLoad(&in, path));
    CHECK(in.elemCount == 1);
    CHECK(in.signalCount == 2);                    // both signals still parsed
    CHECK(in.signals[0].targetCount == 1);         // orphan dropped
    CHECK(in.signals[0].targets[0].prop == AP_T_POS_X);
    CHECK(in.signals[0].targets[0].keyCount == 1);
    CHECK_NEAR(in.signals[0].targets[0].keys[0].value, 0.8f);
    CHECK(TextIsEqual(in.signals[1].name, "s2"));  // stream stayed in sync
    CHECK_NEAR(in.signals[1].length, 0.5f);
    remove(path);
}

// Old-format files (no `outline` or `scale` line, pre-refactor shape names)
// still load with those defaults - forward compatibility for saved docs.
static void TestIOOldFormat(void)
{
    const char *path = "anim_tests_old_tmp.cfg";
    FILE *f = fopen(path, "w");
    CHECK(f != NULL);
    if (!f) return;
    fprintf(f, "doc old 2.0\n"
               "elem shape box\n"
               "  shape circle\n"
               "  color 10 20 30 255\n"
               "  pos 0.5 0.5\n"
               "  size 0.2 0.2\n"
               "  end\n");
    fclose(f);

    AnimDoc in;
    CHECK(AnimDocLoad(&in, path));
    CHECK(in.elemCount == 1 && in.elems[0].shapeKind == SHAPE_CIRCLE);
    CHECK_NEAR(in.elems[0].outlineFrac, 0.0f);              // default: off
    CHECK(in.elems[0].outlineColor.r == 245);               // RAYWHITE default
    // no `scale` line either -> authored size, not a collapsed shape
    CHECK_NEAR(in.elems[0].scaleFrac, 1.0f);
    CHECK_NEAR(AnimElemProp(&in.elems[0], AP_S_SCALE, 0.0f), 1.0f);
    // no trim fields on the doc line -> whole clock is played
    CHECK_NEAR(AnimDocIntroEnd(&in), 0.0f);
    CHECK_NEAR(AnimDocOutroStart(&in), 2.0f);
    CHECK_NEAR(AnimDocPlayLen(&in), 2.0f);
    // old files predate the authoring flags -> both default off.
    CHECK(!in.elems[0].sizeAbsolute && !in.elems[0].cornerMode);
    remove(path);
}

// The per-element authoring flags (sizeAbsolute, cornerMode) round-trip through
// .cfg, and absolute sizing changes what a shape's extent evaluates to on screen
// (via the draw path's unit branch, mirrored here by hand).
static void TestAuthoringFlags(void)
{
    const char *path = "anim_tests_flags_tmp.cfg";
    AnimDoc doc; AnimDocInit(&doc);
    AnimElem *e = AnimDocAddElem(&doc, AE_SHAPE);
    e->sizeAbsolute = true;
    e->cornerMode   = true;
    e->rotBase      = 30.0f;                          // rest-pose rotation
    e->sizeFrac     = (Vector2){ 320.0f, 180.0f };   // pixels, since absolute
    // rest-pose rotation with no track is now the base value AnimElemProp reads.
    CHECK_NEAR(AnimElemProp(e, AP_S_ROT, 0.0f), 30.0f);
    CHECK(AnimDocSave(&doc, path));

    AnimDoc in;
    CHECK(AnimDocLoad(&in, path));
    CHECK(in.elemCount == 1);
    CHECK(in.elems[0].sizeAbsolute && in.elems[0].cornerMode);
    CHECK_NEAR(in.elems[0].rotBase, 30.0f);
    CHECK_NEAR(in.elems[0].sizeFrac.x, 320.0f);
    CHECK_NEAR(in.elems[0].sizeFrac.y, 180.0f);
    remove(path);
}

// Every AP_* prop of each element kind belongs to exactly one group, and every
// group member is a real prop of that kind. Guards the group tables against a
// prop being unreachable in the grouped inspector/timeline.
static void TestGroupCoverage(void)
{
    for (int kind = AE_TEXT; kind <= AE_GLOBAL; kind++)
    {
        // each prop maps to exactly one group
        int propN = AnimPropCountFor(kind);
        for (int i = 0; i < propN; i++)
        {
            int prop = AnimPropAt(kind, i);
            CHECK(AnimGroupIndexOfProp(kind, prop) >= 0);
        }
        // each group member is a valid prop of the kind, and group membership is
        // unique (no prop in two groups)
        int grpN = AnimGroupCountFor(kind);
        for (int g = 0; g < grpN; g++)
        {
            const AnimPropGroup *grp = AnimGroupAt(kind, g);
            CHECK(grp && grp->propCount >= 1 && grp->propCount <= ANIM_GROUP_PROPS);
            for (int j = 0; j < grp->propCount; j++)
                CHECK(AnimGroupIndexOfProp(kind, grp->props[j]) == g);
        }
    }
}

// Intro/outro trim: accessors clamp, the section round-trips through .cfg, and
// a looping player replays [introEnd, outroStart) after its first pass.
static void TestTrim(void)
{
    AnimDoc doc;
    AnimDocInit(&doc);
    doc.duration = 10.0f;

    // unset outro (0) means "whole clock"; that is what old docs deserialize to
    doc.outroStart = 0.0f;
    CHECK_NEAR(AnimDocOutroStart(&doc), 10.0f);
    CHECK_NEAR(AnimDocPlayLen(&doc), 10.0f);

    // out-of-range values are clamped by the accessors, not stored raw
    doc.outroStart = 20.0f;  CHECK_NEAR(AnimDocOutroStart(&doc), 10.0f);
    doc.outroStart = 6.0f;
    doc.introEnd   = 9.0f;   CHECK_NEAR(AnimDocIntroEnd(&doc), 6.0f);  // can't cross
    doc.introEnd   = -1.0f;  CHECK_NEAR(AnimDocIntroEnd(&doc), 0.0f);
    doc.introEnd   = 2.0f;
    CHECK_NEAR(AnimDocPlayLen(&doc), 4.0f);

    // round-trip
    const char *path = "anim_tests_trim_tmp.cfg";
    CHECK(AnimDocSave(&doc, path));
    AnimDoc in;
    CHECK(AnimDocLoad(&in, path));
    CHECK_NEAR(in.duration, 10.0f);
    CHECK_NEAR(in.introEnd, 2.0f);
    CHECK_NEAR(in.outroStart, 6.0f);
    remove(path);

    // player: StartAll stops at the outro, never sampling the trimmed tail
    AnimPlayer p = {0};
    AnimPlayerStartAll(&p, &doc, ANIM_FWD);
    CHECK_NEAR(p.secEnd, 6.0f);
    p.loop = false;
    for (int i = 0; i < 100; i++)
    {
        AnimPlayerUpdate(&p, 0.1f);
        CHECK(AnimPlayerSampleTime(&p) <= 6.0f + 0.0001f);
    }
    CHECK(AnimPlayerDone(&p));
    CHECK_NEAR(AnimPlayerSampleTime(&p), 6.0f);

    // looping: the first pass includes the intro, every later cycle starts at
    // introEnd and stays inside [introEnd, outroStart)
    AnimPlayerStartAll(&p, &doc, ANIM_FWD);
    p.loop = true;
    CHECK(!p.introDone);
    CHECK_NEAR(AnimPlayerSampleTime(&p), 0.0f);     // intro plays on pass one
    for (int i = 0; i < 30; i++) AnimPlayerUpdate(&p, 0.25f);   // 7.5s > 6s
    CHECK(p.introDone);
    for (int i = 0; i < 200; i++)
    {
        AnimPlayerUpdate(&p, 0.1f);
        float t = AnimPlayerSampleTime(&p);
        CHECK(t >= 2.0f - 0.0001f && t <= 6.0f + 0.0001f);
    }
}

// Save -> load -> save must be byte-stable: the shared elem writer/reader
// (AnimElemWriteCfg / AnimElemReadCfgToken) is the single grammar, so a second
// pass over its own output has to reproduce it exactly.
static void TestIOIdempotent(void)
{
    const char *p1 = "anim_tests_idem1.cfg", *p2 = "anim_tests_idem2.cfg";

    AnimDoc doc;
    AnimDocInit(&doc);
    TextCopy(doc.name, "idem");
    doc.duration = 3.0f;

    AnimElem *t = AnimDocAddElem(&doc, AE_TEXT);
    TextCopy(t->name, "title");
    TextCopy(t->text, "TWO WORDS");                         // exercises the _ encoding
    AnimTrack *a = AnimElemAddTrack(t, AP_T_ALPHA);
    AnimTrackAddKey(a, 0.0f, 0.0f, ANIM_EASE_LINEAR);
    AnimTrackAddKey(a, 1.5f, 1.0f, ANIM_EASE_BOUNCE_OUT);
    AnimTrack *tc = AnimElemAddTrack(t, AP_T_COLOR);        // colour track: 5-token keys
    AnimTrackAddColorKey(tc, 0.0f, (Color){ 1, 2, 3, 255 }, ANIM_EASE_LINEAR);
    AnimTrackAddColorKey(tc, 2.0f, (Color){ 9, 8, 7, 255 }, ANIM_EASE_SINE_OUT);

    AnimElem *s = AnimDocAddElem(&doc, AE_SHAPE);
    TextCopy(s->name, "box");
    s->shapeKind   = SHAPE_RHOMBUS;
    s->outlineFrac = 0.02f;
    AnimElemAddTrack(s, AP_S_OUTLINE_COLOR);

    CHECK(AnimDocSave(&doc, p1));

    AnimDoc back;
    CHECK(AnimDocLoad(&back, p1));
    CHECK(back.elemCount == 2);
    CHECK(TextIsEqual(back.elems[0].text, "TWO WORDS"));    // decoded back
    CHECK(back.elems[0].trackCount == 2);
    CHECK(back.elems[0].tracks[1].keys[1].cval.r == 9);
    CHECK(back.elems[1].shapeKind == SHAPE_RHOMBUS);
    CHECK(AnimDocSave(&back, p2));

    // compare the two files byte for byte
    FILE *f1 = fopen(p1, "rb"), *f2 = fopen(p2, "rb");
    CHECK(f1 && f2);
    if (f1 && f2)
    {
        int c1, c2, same = 1;
        do { c1 = fgetc(f1); c2 = fgetc(f2); if (c1 != c2) { same = 0; break; } }
        while (c1 != EOF);
        CHECK(same);
    }
    if (f1) fclose(f1);
    if (f2) fclose(f2);
    remove(p1); remove(p2);
}

// ---------------------------------------------------------------------------
//  Element library: CRUD + round-trip through the shared elem grammar
// ---------------------------------------------------------------------------
static void TestLibrary(void)
{
    const char *path = "anim_tests_lib.cfg";

    AnimLibrary lib;
    AnimLibraryInit(&lib);
    CHECK(lib.count == 0);
    CHECK(AnimLibraryFind(&lib, "nope") == -1);

    AnimElem e;
    AnimElemInit(&e, AE_SHAPE);
    TextCopy(e.name, "box");
    e.shapeKind   = SHAPE_TRIANGLE;
    e.outlineFrac = 0.03f;
    e.color       = (Color){ 10, 20, 30, 200 };
    AnimTrack *tr = AnimElemAddTrack(&e, AP_S_POS_Y);
    AnimTrackAddKey(tr, 0.0f, 0.1f, ANIM_EASE_LINEAR);
    AnimTrackAddKey(tr, 1.0f, 0.9f, ANIM_EASE_BACK_OUT);

    CHECK(AnimLibraryAdd(&lib, "neon_box", &e) == 0);
    CHECK(lib.count == 1);
    CHECK(AnimLibraryFind(&lib, "neon_box") == 0);

    // same name overwrites in place rather than appending a duplicate
    e.outlineFrac = 0.05f;
    CHECK(AnimLibraryAdd(&lib, "neon_box", &e) == 0);
    CHECK(lib.count == 1);
    CHECK_NEAR(lib.entries[0].elem.outlineFrac, 0.05f);

    AnimElem t;
    AnimElemInit(&t, AE_TEXT);
    TextCopy(t.text, "TWO WORDS");
    CHECK(AnimLibraryAdd(&lib, "title_style", &t) == 1);

    // rename rules
    CHECK(!AnimLibraryRename(&lib, 1, "neon_box"));          // name taken
    CHECK(!AnimLibraryRename(&lib, 1, ""));                  // empty
    CHECK(!AnimLibraryRename(&lib, 9, "x"));                 // bad index
    CHECK(AnimLibraryRename(&lib, 1, "big_title"));
    CHECK(TextIsEqual(lib.entries[1].name, "big_title"));

    CHECK(AnimLibrarySave(&lib, path));

    AnimLibrary back;
    CHECK(AnimLibraryLoad(&back, path));
    CHECK(back.count == 2);
    CHECK(TextIsEqual(back.entries[0].name, "neon_box"));
    CHECK(back.entries[0].elem.kind == AE_SHAPE);
    CHECK(back.entries[0].elem.shapeKind == SHAPE_TRIANGLE);
    CHECK(back.entries[0].elem.color.a == 200);
    CHECK(back.entries[0].elem.trackCount == 1);             // tracks survive
    CHECK(back.entries[0].elem.tracks[0].keyCount == 2);
    CHECK(back.entries[0].elem.tracks[0].keys[1].ease == ANIM_EASE_BACK_OUT);
    CHECK(back.entries[1].elem.kind == AE_TEXT);             // kind re-inited
    CHECK(TextIsEqual(back.entries[1].elem.text, "TWO WORDS"));

    AnimLibraryRemove(&back, 0);
    CHECK(back.count == 1 && TextIsEqual(back.entries[0].name, "big_title"));
    AnimLibraryRemove(&back, 5);                             // out of range: no-op
    CHECK(back.count == 1);

    // missing file -> empty library, false
    AnimLibrary miss;
    CHECK(!AnimLibraryLoad(&miss, "anim_tests_no_such_lib.cfg"));
    CHECK(miss.count == 0);

    remove(path);
}

// ---------------------------------------------------------------------------
//  Signal bus + anim bridge + player
// ---------------------------------------------------------------------------
static int s_pings = 0;
static void Ping(const char *name, void *user, const SignalParams *p) { (void)name; (void)user; (void)p; s_pings++; }

static void TestSignals(void)
{
    SignalReset();
    SignalListen("ping", Ping, NULL);
    SignalListen("ping", Ping, NULL);                       // dedup
    SignalEmit("ping", NULL);
    CHECK(s_pings == 1);
    SignalStopListening("ping", Ping, NULL);
    SignalEmit("ping", NULL);
    CHECK(s_pings == 1);

    // bridge: firing a doc signal starts the signal player on that signal.
    SignalReset();
    AnimDoc doc;
    AnimDocInit(&doc);
    doc.duration = 2.0f;
    AnimElem *e = AnimDocAddElem(&doc, AE_TEXT);
    e->posFrac = (Vector2){ 0.5f, 0.20f };                  // live pose to ease FROM

    doc.signalCount = 1;
    TextCopy(doc.signals[0].name, "enter");
    doc.signals[0].length      = 2.0f;
    doc.signals[0].targetCount = 1;
    doc.signals[0].targets[0] = (AnimSigTarget){0};
    doc.signals[0].targets[0].elemIdx  = 0;
    doc.signals[0].targets[0].prop     = AP_T_POS_Y;
    doc.signals[0].targets[0].keyCount = 1;
    doc.signals[0].targets[0].keys[0] =
        (AnimKey){ 1.0f, 0.80f, (Color){0,0,0,0}, ANIM_EASE_LINEAR };

    float docTime = 0.0f;
    AnimSignalPlayer sp = {0};
    AnimSignalRegister(&doc, &sp, &docTime);
    SignalEmit("enter", NULL);
    CHECK(sp.playing);
    CHECK_NEAR(sp.fromValue[0], 0.20f);                     // captured live pose

    float v = 0.0f;
    CHECK(AnimSignalPlayerEval(&sp, 0, AP_T_POS_Y, &v, NULL));
    CHECK_NEAR(v, 0.20f);                                   // u=0 -> the capture
    CHECK(!AnimSignalPlayerEval(&sp, 0, AP_T_POS_X, &v, NULL));   // untargeted
    CHECK(!AnimSignalPlayerEval(&sp, 1, AP_T_POS_Y, &v, NULL));   // other element

    AnimSignalPlayerUpdate(&sp, 1.0f);                      // halfway
    CHECK(AnimSignalPlayerEval(&sp, 0, AP_T_POS_Y, &v, NULL));
    CHECK_NEAR(v, 0.50f);                                   // linear 0.2 -> 0.8

    AnimSignalPlayerUpdate(&sp, 10.0f);                     // past the end
    CHECK(AnimSignalPlayerDone(&sp));
    CHECK(!AnimSignalPlayerEval(&sp, 0, AP_T_POS_Y, &v, NULL));   // idle: no drive

    AnimSignalUnregister(&doc, &sp);
    sp.playing = false;
    SignalEmit("enter", NULL);
    CHECK(!sp.playing);                                     // binding removed

    // length 0 = instant: lands on the final key the moment it starts
    doc.signals[0].length = 0.0f;
    AnimSignalPlayerStart(&sp, &doc.signals[0], &doc, 0.0f, NULL);
    CHECK(AnimSignalPlayerEval(&sp, 0, AP_T_POS_Y, &v, NULL));
    CHECK_NEAR(v, 0.80f);

    // normalized keys rescale with the length: same u, twice the wall time
    doc.signals[0].length = 4.0f;
    AnimSignalPlayerStart(&sp, &doc.signals[0], &doc, 0.0f, NULL);
    AnimSignalPlayerUpdate(&sp, 2.0f);                      // u = 0.5 again
    CHECK(AnimSignalPlayerEval(&sp, 0, AP_T_POS_Y, &v, NULL));
    CHECK_NEAR(v, 0.50f);

    // a NULL / empty signal leaves the player idle rather than half-started
    AnimSignalPlayerStart(&sp, NULL, &doc, 0.0f, NULL);
    CHECK(AnimSignalPlayerDone(&sp));

    // the plain AnimPlayer (still used for whole-doc playback) is unaffected
    AnimPlayer p = {0};
    AnimPlayerStartAll(&p, &doc, ANIM_FWD);
    CHECK_NEAR(AnimPlayerSampleTime(&p), 0.0f);
    AnimPlayerUpdate(&p, 0.75f);
    CHECK_NEAR(AnimPlayerSampleTime(&p), 0.75f);
}

// Signal targets address elements BY INDEX, so every reshuffle of doc.elems
// must be mirrored onto them or a signal silently drives the wrong element.
static void TestSignalTargetRemap(void)
{
    AnimDoc doc;
    AnimDocInit(&doc);
    TextCopy(AnimDocAddElem(&doc, AE_TEXT)->name,  "a");    // 0
    TextCopy(AnimDocAddElem(&doc, AE_TEXT)->name,  "b");    // 1
    TextCopy(AnimDocAddElem(&doc, AE_TEXT)->name,  "c");    // 2

    doc.signalCount = 1;
    AnimSignal *sg = &doc.signals[0];
    TextCopy(sg->name, "s");
    sg->length = 1.0f;
    sg->targetCount = 3;
    for (int i = 0; i < 3; i++)
    {
        sg->targets[i] = (AnimSigTarget){0};
        sg->targets[i].elemIdx = i;              // one target per element
        sg->targets[i].prop    = AP_T_POS_Y;
    }

    // move: targets follow their elements through the swap
    AnimDocMoveElem(&doc, 0, +1);                // a <-> b  => b a c
    CHECK(sg->targets[0].elemIdx == 1);          // "a" is now at 1
    CHECK(sg->targets[1].elemIdx == 0);          // "b" is now at 0
    CHECK(sg->targets[2].elemIdx == 2);          // "c" untouched
    AnimDocMoveElem(&doc, 1, -1);                // back to a b c
    CHECK(sg->targets[0].elemIdx == 0 && sg->targets[1].elemIdx == 1);

    // duplicate at 0 inserts at 1: targets at/after 1 shift up
    CHECK(AnimDocDuplicateElem(&doc, 0) != NULL);   // a a_2 b c
    CHECK(sg->targets[0].elemIdx == 0);             // "a" stays at 0
    CHECK(sg->targets[1].elemIdx == 2);             // "b" pushed to 2
    CHECK(sg->targets[2].elemIdx == 3);             // "c" pushed to 3
    AnimDocRemoveElem(&doc, 1);                     // drop the copy: a b c
    CHECK(sg->targets[1].elemIdx == 1 && sg->targets[2].elemIdx == 2);

    // remove a TARGETED element: its target is dropped, later ones shift down
    AnimDocRemoveElem(&doc, 1);                  // remove "b" => a c
    CHECK(doc.elemCount == 2);
    CHECK(sg->targetCount == 2);                 // the "b" target is gone
    CHECK(sg->targets[0].elemIdx == 0);          // "a"
    CHECK(sg->targets[1].elemIdx == 1);          // "c", decremented from 2

    // an out-of-range target must not be evaluated or crash the player
    sg->targets[1].elemIdx = 99;
    sg->targets[1].keyCount = 1;
    sg->targets[1].keys[0] = (AnimKey){ 1.0f, 1.0f, (Color){0,0,0,0}, ANIM_EASE_LINEAR };
    AnimSignalPlayer sp = {0};
    AnimSignalPlayerStart(&sp, sg, &doc, 0.0f, NULL);
    float v = 0.0f;
    CHECK(!AnimSignalPlayerEval(&sp, 1, AP_T_POS_Y, &v, NULL));   // idx 1 != 99
}

// ---------------------------------------------------------------------------
//  Terminal signal flag: round-trips, and defaults false on pre-flag files.
// ---------------------------------------------------------------------------
static void TestSignalTerminalIO(void)
{
    const char *path = "anim_tests_term.cfg";

    AnimDoc doc;
    AnimDocInit(&doc);
    TextCopy(AnimDocAddElem(&doc, AE_TEXT)->name, "a");
    doc.signalCount = 2;
    TextCopy(doc.signals[0].name, "ends");
    doc.signals[0].length   = 1.5f;
    doc.signals[0].terminal = true;
    doc.signals[0].targetCount = 0;
    TextCopy(doc.signals[1].name, "blip");
    doc.signals[1].length   = 0.5f;
    doc.signals[1].terminal = false;
    doc.signals[1].targetCount = 0;

    CHECK(AnimDocSave(&doc, path));
    AnimDoc in;
    CHECK(AnimDocLoad(&in, path));
    CHECK(in.signalCount == 2);
    CHECK(in.signals[0].terminal);                  // true survives
    CHECK(!in.signals[1].terminal);                 // false survives
    CHECK_NEAR(in.signals[0].length, 1.5f);         // length still parses
    remove(path);

    // a file written BEFORE the flag existed: `signal <name> <length>` only
    const char *old = "anim_tests_term_old.cfg";
    FILE *f = fopen(old, "w");
    CHECK(f != NULL);
    if (!f) return;
    fprintf(f, "doc d 2.0\n"
               "elem text a\n"
               "  text hi\n"
               "  end\n"
               "signal legacy 0.750000\n");
    fclose(f);

    AnimDoc oldDoc;
    CHECK(AnimDocLoad(&oldDoc, old));
    CHECK(oldDoc.signalCount == 1);
    CHECK(TextIsEqual(oldDoc.signals[0].name, "legacy"));
    CHECK_NEAR(oldDoc.signals[0].length, 0.75f);
    CHECK(!oldDoc.signals[0].terminal);             // absent -> false
    remove(old);
}

// ---------------------------------------------------------------------------
//  The playback stage: looping, terminal-signal shutdown, layering.
// ---------------------------------------------------------------------------
static int s_doneCalls = 0;
static void OnStageDone(void *user) { (void)user; s_doneCalls++; }

// Write a doc the stage can load by name from anims/ (its fixed lookup dir).
static void WriteStageAnim(const char *name, bool terminal)
{
    AnimDoc doc;
    AnimDocInit(&doc);
    doc.duration   = 2.0f;
    doc.introEnd   = 0.0f;
    doc.outroStart = 2.0f;

    AnimElem *e = AnimDocAddElem(&doc, AE_TEXT);
    TextCopy(e->name, "a");
    AnimTrack *tr = AnimElemAddTrack(e, AP_T_ALPHA);
    AnimTrackAddKey(tr, 0.0f, 0.0f, ANIM_EASE_LINEAR);
    AnimTrackAddKey(tr, 2.0f, 1.0f, ANIM_EASE_LINEAR);

    doc.signalCount = 1;
    AnimSignal *sg = &doc.signals[0];
    TextCopy(sg->name, "stage_end");
    sg->length      = 1.0f;
    sg->terminal    = terminal;
    sg->targetCount = 1;
    sg->targets[0] = (AnimSigTarget){0};
    sg->targets[0].elemIdx  = 0;
    sg->targets[0].prop     = AP_T_ALPHA;
    sg->targets[0].keyCount = 1;
    sg->targets[0].keys[0]  = (AnimKey){ 1.0f, 0.0f, (Color){0,0,0,0}, ANIM_EASE_LINEAR };

    if (!DirectoryExists("anims")) MakeDirectory("anims");
    AnimDocSave(&doc, TextFormat("anims/%s.cfg", name));
}

static void TestStage(void)
{
    SignalReset();
    AnimStageReset();
    WriteStageAnim("_test_loop", true);

    // --- looping never ends on its own ------------------------------------
    AnimHandle h = AnimStagePlay("_test_loop", true, 0);
    CHECK(h != ANIM_HANDLE_NONE);
    CHECK(AnimStageAlive(h));
    s_doneCalls = 0;
    AnimStageSetDoneCallback(h, OnStageDone, NULL);
    for (int i = 0; i < 300; i++) AnimStageUpdate(0.05f);   // 15s >> 2s duration
    CHECK(AnimStageAlive(h));                    // still looping
    CHECK(s_doneCalls == 0);

    // --- a terminal signal ends it, but only after its full length --------
    CHECK(!AnimStageEndsOnCurrentSignal(h));     // nothing running yet
    SignalEmit("stage_end", NULL);
    CHECK(AnimStageEndsOnCurrentSignal(h));      // armed: safe to wait on done
    AnimStageUpdate(0.5f);                       // half of the 1.0s signal
    CHECK(AnimStageAlive(h));                    // NOT cut off mid-transition
    for (int i = 0; i < 20; i++) AnimStageUpdate(0.05f);
    CHECK(!AnimStageAlive(h));                   // ended at the signal's end
    CHECK(s_doneCalls == 1);                     // reported exactly once
    CHECK(AnimStageActiveCount() == 0);

    // a stale handle is inert, and Stop on it must not re-fire the callback
    AnimStageStop(h);
    CHECK(s_doneCalls == 1);

    // --- a NON-terminal signal leaves the loop running --------------------
    WriteStageAnim("_test_plain", false);
    AnimHandle p = AnimStagePlay("_test_plain", true, 0);
    CHECK(p != ANIM_HANDLE_NONE);
    SignalEmit("stage_end", NULL);
    CHECK(!AnimStageEndsOnCurrentSignal(p));     // playing, but not an ENDING
    for (int i = 0; i < 60; i++) AnimStageUpdate(0.05f);
    CHECK(AnimStageAlive(p));                    // signal ended, playback did not
    AnimStageStopAll();
    CHECK(!AnimStageAlive(p));
    CHECK(AnimStageActiveCount() == 0);

    // --- one-shot stops by itself -----------------------------------------
    AnimHandle o = AnimStagePlay("_test_plain", false, 0);
    CHECK(AnimStageAlive(o));
    for (int i = 0; i < 60; i++) AnimStageUpdate(0.05f);   // 3s > 2s duration
    CHECK(!AnimStageAlive(o));

    // --- layering: drawn low layer first, ties keep start order -----------
    AnimStageStopAll();
    AnimHandle top = AnimStagePlay("_test_plain", true, 5);
    AnimHandle bot = AnimStagePlay("_test_plain", true, 1);
    AnimHandle mid = AnimStagePlay("_test_plain", true, 5);   // ties with `top`
    CHECK(top != ANIM_HANDLE_NONE && bot != ANIM_HANDLE_NONE);
    CHECK(mid != ANIM_HANDLE_NONE);
    int order[ANIM_STAGE_SLOTS_MAX];
    int n = AnimStageDrawOrder(order, ANIM_STAGE_SLOTS_MAX);
    CHECK(n == 3);
    CHECK(order[0] == AnimStageSlotOf(bot));     // layer 1 first (drawn behind)
    CHECK(order[1] == AnimStageSlotOf(top));     // layer 5, started earlier
    CHECK(order[2] == AnimStageSlotOf(mid));     // layer 5, started later
    AnimStageStopAll();

    // --- an INSTANT terminal signal (length 0) still ends the instance ----
    // It completes inside its first update, so the completion edge is the only
    // thing that can catch it - and a caller must not be told to wait for it.
    {
        AnimDoc d;
        AnimDocInit(&d);
        d.duration = 2.0f; d.outroStart = 2.0f;
        TextCopy(AnimDocAddElem(&d, AE_TEXT)->name, "a");
        d.signalCount = 1;
        TextCopy(d.signals[0].name, "snap");
        d.signals[0].length      = 0.0f;         // instant
        d.signals[0].terminal    = true;
        d.signals[0].targetCount = 1;
        d.signals[0].targets[0] = (AnimSigTarget){0};
        d.signals[0].targets[0].elemIdx  = 0;
        d.signals[0].targets[0].prop     = AP_T_ALPHA;
        d.signals[0].targets[0].keyCount = 1;
        d.signals[0].targets[0].keys[0]  =
            (AnimKey){ 1.0f, 0.0f, (Color){0,0,0,0}, ANIM_EASE_LINEAR };
        AnimDocSave(&d, "anims/_test_snap.cfg");

        AnimHandle s = AnimStagePlay("_test_snap", true, 0);
        CHECK(s != ANIM_HANDLE_NONE);
        SignalEmit("snap", NULL);
        AnimStageUpdate(0.016f);
        CHECK(!AnimStageAlive(s));               // ended on the first update
        AnimStageStopAll();
        remove("anims/_test_snap.cfg");
    }

    // --- a signal with NO targets never plays, so nothing waits on it -----
    {
        AnimDoc d;
        AnimDocInit(&d);
        d.duration = 2.0f; d.outroStart = 2.0f;
        TextCopy(AnimDocAddElem(&d, AE_TEXT)->name, "a");
        d.signalCount = 1;
        TextCopy(d.signals[0].name, "empty");
        d.signals[0].length      = 1.0f;
        d.signals[0].terminal    = true;
        d.signals[0].targetCount = 0;            // nothing to drive
        AnimDocSave(&d, "anims/_test_empty.cfg");

        AnimHandle s = AnimStagePlay("_test_empty", true, 0);
        CHECK(s != ANIM_HANDLE_NONE);
        SignalEmit("empty", NULL);
        // never armed: the caller must be told NOT to wait, or it would hang
        CHECK(!AnimStageEndsOnCurrentSignal(s));
        AnimStageStopAll();
        remove("anims/_test_empty.cfg");
    }

    // --- a start delay holds the instance back without drawing it ---------
    // While waiting it is ALIVE and holds its slot, but is not in the draw
    // order and its clock does not move.
    {
        AnimStageStopAll();
        AnimHandle d = AnimStagePlayEx("_test_plain", true, 0, 1.0f);
        CHECK(d != ANIM_HANDLE_NONE);
        CHECK(AnimStageAlive(d));                // alive from the moment played
        CHECK(AnimStageActiveCount() == 1);      // and it costs a slot already

        int ord[ANIM_STAGE_SLOTS_MAX];
        CHECK(AnimStageDrawOrder(ord, ANIM_STAGE_SLOTS_MAX) == 0);  // invisible
        for (int i = 0; i < 10; i++) AnimStageUpdate(0.05f);        // 0.5s < 1s
        CHECK(AnimStageAlive(d));
        CHECK(AnimStageDrawOrder(ord, ANIM_STAGE_SLOTS_MAX) == 0);  // still not
        for (int i = 0; i < 11; i++) AnimStageUpdate(0.05f);        // past 1.0s
        CHECK(AnimStageDrawOrder(ord, ANIM_STAGE_SLOTS_MAX) == 1);  // now drawn
        CHECK(ord[0] == AnimStageSlotOf(d));
        AnimStageStopAll();
    }

    // --- the delay shifts the whole life of a one-shot by exactly itself ---
    // A 2s doc played with a 1s delay is still running at 2.5s (a plain one
    // would have finished at 2s) and is gone by 3.5s. This is what proves the
    // clock is HELD during the wait rather than merely hidden.
    {
        AnimStageStopAll();
        AnimHandle a = AnimStagePlayEx("_test_plain", false, 0, 1.0f);
        CHECK(a != ANIM_HANDLE_NONE);
        for (int i = 0; i < 50; i++) AnimStageUpdate(0.05f);   // t = 2.5s
        CHECK(AnimStageAlive(a));                              // delayed, so on
        for (int i = 0; i < 20; i++) AnimStageUpdate(0.05f);   // t = 3.5s
        CHECK(!AnimStageAlive(a));
    }

    // --- the stagger survives uneven frames -------------------------------
    // The remainder of the frame that ends a wait is spent on the animation, so
    // two copies started 1.0s apart still end 1.0s apart even when no frame
    // boundary lands on either delay. Fed deliberately ragged dt.
    {
        AnimStageStopAll();
        AnimHandle first  = AnimStagePlayEx("_test_plain", false, 0, 0.0f);
        AnimHandle second = AnimStagePlayEx("_test_plain", false, 0, 1.0f);
        CHECK(first != ANIM_HANDLE_NONE && second != ANIM_HANDLE_NONE);

        // Run to just under 2s: neither has finished (the first ends AT 2s).
        // The bound leaves room for a whole frame, so the loop cannot step
        // PAST 2.0s and end `first` before the check below.
        float t = 0.0f;
        while (t + 0.07f < 1.97f) { AnimStageUpdate(0.07f); t += 0.07f; }
        CHECK(AnimStageAlive(first));
        CHECK(AnimStageAlive(second));

        // Cross 2s: the undelayed one ends, the delayed one keeps going.
        while (t < 2.5f) { AnimStageUpdate(0.07f); t += 0.07f; }
        CHECK(!AnimStageAlive(first));
        CHECK(AnimStageAlive(second));

        // Cross 3s (= its 1s delay + 2s duration): now the delayed one ends.
        while (t < 3.2f) { AnimStageUpdate(0.07f); t += 0.07f; }
        CHECK(!AnimStageAlive(second));
        CHECK(AnimStageActiveCount() == 0);
    }

    // --- stopping a still-waiting instance reports done and frees the slot -
    {
        AnimStageStopAll();
        s_doneCalls = 0;
        AnimHandle w = AnimStagePlayEx("_test_plain", true, 0, 5.0f);
        CHECK(w != ANIM_HANDLE_NONE);
        AnimStageSetDoneCallback(w, OnStageDone, NULL);
        AnimStageUpdate(0.05f);                  // still deep in the wait
        CHECK(AnimStageAlive(w));
        AnimStageStop(w);
        CHECK(!AnimStageAlive(w));
        CHECK(s_doneCalls == 1);
        CHECK(AnimStageActiveCount() == 0);
    }

    // --- a non-positive delay is exactly AnimStagePlay ---------------------
    {
        AnimStageStopAll();
        AnimHandle z = AnimStagePlayEx("_test_plain", true, 0, -1.0f);
        CHECK(z != ANIM_HANDLE_NONE);
        int ord[ANIM_STAGE_SLOTS_MAX];
        CHECK(AnimStageDrawOrder(ord, ANIM_STAGE_SLOTS_MAX) == 1);  // drawn now
        AnimStageStopAll();
    }

    // --- a missing file must not occupy a slot ----------------------------
    CHECK(AnimStagePlay("_test_does_not_exist", true, 0) == ANIM_HANDLE_NONE);
    CHECK(AnimStageActiveCount() == 0);

    remove("anims/_test_loop.cfg");
    remove("anims/_test_plain.cfg");
    AnimStageReset();
    SignalReset();
}

// The animation the main menu asks for by name must actually be loadable from
// anims/, and must declare the signal the menu emits to end it - otherwise the
// integration silently degrades to "no overlay" with nothing to point at.
// Skipped when run outside the repo root (anims/ is CWD-relative).
static void TestMenuAnimPresent(void)
{
    AnimDoc doc;
    if (!AnimDocLoad(&doc, "anims/signal_test.cfg")) return;   // not at repo root
    CHECK(doc.elemCount > 0);

    bool found = false;
    for (int i = 0; i < doc.signalCount; i++)
        if (TextIsEqual(doc.signals[i].name, "TV-out")) found = true;
    CHECK(found);       // main_menu.c's MENU_END_SIGNAL
}

// ---------------------------------------------------------------------------
//  Signal POSITION parameter: an absolute pos re-anchors a POS target so the
//  authored transition plays out at the param location instead of its authored
//  one; no param = byte-identical to before.
// ---------------------------------------------------------------------------
static void TestSignalPosParam(void)
{
    AnimDoc doc;
    AnimDocInit(&doc);
    doc.duration = 2.0f;
    AnimElem *e = AnimDocAddElem(&doc, AE_SHAPE);
    e->posFrac = (Vector2){ 0.5f, 0.5f };

    // A signal whose pos_x/pos_y targets rest at the authored center (0.5,0.5).
    doc.signalCount = 1;
    AnimSignal *sg = &doc.signals[0];
    TextCopy(sg->name, "place");
    sg->length      = 1.0f;
    sg->targetCount = 2;
    sg->targets[0] = (AnimSigTarget){0};
    sg->targets[0].elemIdx = 0; sg->targets[0].prop = AP_S_POS_X;
    sg->targets[0].keyCount = 1;
    sg->targets[0].keys[0] = (AnimKey){ 1.0f, 0.5f, (Color){0,0,0,0}, ANIM_EASE_LINEAR };
    sg->targets[1] = (AnimSigTarget){0};
    sg->targets[1].elemIdx = 0; sg->targets[1].prop = AP_S_POS_Y;
    sg->targets[1].keyCount = 1;
    sg->targets[1].keys[0] = (AnimKey){ 1.0f, 0.5f, (Color){0,0,0,0}, ANIM_EASE_LINEAR };

    // --- no param: lands at the authored 0.5/0.5 (no-op proof) -------------
    AnimSignalPlayer sp = {0};
    AnimSignalPlayerStart(&sp, sg, &doc, 0.0f, NULL);
    AnimSignalPlayerUpdate(&sp, 0.9f);                 // near end, still playing
    float vx = 0.0f, vy = 0.0f;
    CHECK(AnimSignalPlayerEval(&sp, 0, AP_S_POS_X, &vx, NULL));
    CHECK(AnimSignalPlayerEval(&sp, 0, AP_S_POS_Y, &vy, NULL));
    CHECK_NEAR(vx, 0.5f);
    CHECK_NEAR(vy, 0.5f);

    // --- with an absolute pos param: re-anchored to it ---------------------
    SignalParams pp = { .pos = { 0.8f, 0.2f }, .hasPos = true };
    AnimSignalPlayerStart(&sp, sg, &doc, 0.0f, &pp);
    AnimSignalPlayerUpdate(&sp, 0.9f);
    CHECK(AnimSignalPlayerEval(&sp, 0, AP_S_POS_X, &vx, NULL));
    CHECK(AnimSignalPlayerEval(&sp, 0, AP_S_POS_Y, &vy, NULL));
    CHECK_NEAR(vx, 0.8f);                              // authored 0.5 + (0.8-0.5)
    CHECK_NEAR(vy, 0.2f);

    // hasPos false leaves non-pos targets untouched: alpha param never applies.
    // (only pos_x/pos_y are re-anchored; nothing else is.)
}

// A signal param carried through the whole emit path (bus -> bridge -> player).
static void TestSignalEmitParam(void)
{
    SignalReset();
    AnimDoc doc;
    AnimDocInit(&doc);
    doc.duration = 2.0f;
    AnimElem *e = AnimDocAddElem(&doc, AE_SHAPE);
    e->posFrac = (Vector2){ 0.5f, 0.5f };

    doc.signalCount = 1;
    AnimSignal *sg = &doc.signals[0];
    TextCopy(sg->name, "spawn");
    sg->length      = 1.0f;
    sg->targetCount = 1;
    sg->targets[0] = (AnimSigTarget){0};
    sg->targets[0].elemIdx = 0; sg->targets[0].prop = AP_S_POS_X;
    sg->targets[0].keyCount = 1;
    sg->targets[0].keys[0] = (AnimKey){ 1.0f, 0.5f, (Color){0,0,0,0}, ANIM_EASE_LINEAR };

    float docTime = 0.0f;
    AnimSignalPlayer sp = {0};
    AnimSignalRegister(&doc, &sp, &docTime);

    SignalParams pp = { .pos = { 0.9f, 0.1f }, .hasPos = true };
    SignalEmit("spawn", &pp);
    CHECK(sp.playing);
    CHECK(sp.param.hasPos);
    AnimSignalPlayerUpdate(&sp, 0.9f);
    float vx = 0.0f;
    CHECK(AnimSignalPlayerEval(&sp, 0, AP_S_POS_X, &vx, NULL));
    CHECK_NEAR(vx, 0.9f);                              // emit's param reached Eval

    AnimSignalUnregister(&doc, &sp);
}

// ---------------------------------------------------------------------------
//  Declarative scene layer: play a table, emit across all matching rows, and
//  the terminal-then-done flow.
// ---------------------------------------------------------------------------
static void TestScene(void)
{
    SignalReset();
    AnimStageReset();
    WriteStageAnim("_test_scene", false);   // non-terminal signal "stage_end"

    static const AnimStageEntry ENTRIES[] = {
        { .anim="_test_scene", .loop=true, .delay=0.0f, .layer=0, .tag=10,
          .signals={ {"stage_end", false} }, .signalCount=1 },
        { .anim="_test_scene", .loop=true, .delay=0.5f, .layer=1, .tag=11,
          .signals={ {"stage_end", false} }, .signalCount=1 },
    };
    AnimStageScene sc;
    AnimScenePlay(&sc, ENTRIES, 2);

    CHECK(AnimStageActiveCount() == 2);     // both rows became instances
    CHECK(AnimSceneAlive(&sc));

    // the delayed row is alive but not yet on the draw list (still waiting)
    int order[ANIM_STAGE_SLOTS_MAX];
    CHECK(AnimStageDrawOrder(order, ANIM_STAGE_SLOTS_MAX) == 1);
    AnimStageUpdate(0.6f);                   // clears the 0.5s delay
    CHECK(AnimStageDrawOrder(order, ANIM_STAGE_SLOTS_MAX) == 2);

    AnimSceneStop(&sc);
    CHECK(!AnimSceneAlive(&sc));
    CHECK(AnimStageActiveCount() == 0);
}

// AnimSceneEmit reaches EVERY row declaring the name, and ONLY those. Observed
// through a TERMINAL signal: a matching row is armed to end (EndsOnCurrentSignal
// true), a non-matching row is not.
static void TestSceneEmitAll(void)
{
    SignalReset();
    AnimStageReset();
    WriteStageAnim("_test_scene2", true);   // "stage_end" is terminal here

    static const AnimStageEntry ENTRIES[] = {
        { .anim="_test_scene2", .loop=true, .delay=0.0f, .layer=0, .tag=0,
          .signals={ {"stage_end", false} }, .signalCount=1 },
        { .anim="_test_scene2", .loop=true, .delay=0.0f, .layer=0, .tag=1,
          .signals={ {"stage_end", false} }, .signalCount=1 },
        { .anim="_test_scene2", .loop=true, .delay=0.0f, .layer=0, .tag=2,
          .signals={ {"other", false} }, .signalCount=1 },   // does NOT declare it
    };
    AnimStageScene sc;
    AnimScenePlay(&sc, ENTRIES, 3);

    AnimSceneEmit(&sc, "stage_end", NULL);
    CHECK(AnimStageEndsOnCurrentSignal(sc.handles[0]));   // matched -> armed
    CHECK(AnimStageEndsOnCurrentSignal(sc.handles[1]));   // matched -> armed
    CHECK(!AnimStageEndsOnCurrentSignal(sc.handles[2]));  // not declared -> inert

    AnimSceneStop(&sc);
}

// Terminal emit across a scene: onDone fires exactly once after all wind down,
// and immediately when nothing is armed.
static int s_sceneDone = 0;
static void OnSceneDone(void *user) { (void)user; s_sceneDone++; }

static void TestSceneTerminal(void)
{
    SignalReset();
    AnimStageReset();
    WriteStageAnim("_test_term", true);     // terminal "stage_end"

    static const AnimStageEntry ENTRIES[] = {
        { .anim="_test_term", .loop=true, .delay=0.0f, .layer=0, .tag=0,
          .signals={ {"stage_end", false} }, .signalCount=1 },
        { .anim="_test_term", .loop=true, .delay=0.0f, .layer=0, .tag=1,
          .signals={ {"stage_end", false} }, .signalCount=1 },
    };
    AnimStageScene sc;
    AnimScenePlay(&sc, ENTRIES, 2);

    s_sceneDone = 0;
    AnimSceneEmitTerminal(&sc, "stage_end", NULL, OnSceneDone, NULL);
    CHECK(s_sceneDone == 0);                 // two armed, none done yet
    CHECK(AnimSceneAlive(&sc));
    for (int i = 0; i < 30; i++) AnimStageUpdate(0.05f);  // > 1.0s signal length
    CHECK(!AnimSceneAlive(&sc));
    CHECK(s_sceneDone == 1);                 // fired exactly once, after both

    // nothing to arm (no row names it) -> onDone immediately
    AnimStageReset();
    AnimScenePlay(&sc, ENTRIES, 2);
    s_sceneDone = 0;
    AnimSceneEmitTerminal(&sc, "no_such_signal", NULL, OnSceneDone, NULL);
    CHECK(s_sceneDone == 1);
    AnimSceneStop(&sc);
}

int main(void)
{
    TestEval();
    TestSegment();
    TestEvalColor();
    TestColorKeyTimeMove();
    TestShapeProps();
    TestTrackCap();
    TestEase();
    TestKeys();
    TestDoc();
    TestRemoveTrack();
    TestMoveDuplicateElem();
    TestIO();
    TestIOOldFormat();
    TestAuthoringFlags();
    TestGroupCoverage();
    TestTrim();
    TestIOIdempotent();
    TestIOSignalOrphanTarget();
    TestLibrary();
    TestSignals();
    TestSignalTargetRemap();
    TestSignalTerminalIO();
    TestStage();
    TestMenuAnimPresent();
    TestSignalPosParam();
    TestSignalEmitParam();
    TestScene();
    TestSceneEmitAll();
    TestSceneTerminal();

    printf("anim_tests: %d checks, %d failed\n", s_checks, s_fails);
    return s_fails ? 1 : 0;
}
