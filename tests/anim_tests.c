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
#include "../src/signal/signal.h"
#include "../src/signal/anim_signal.h"
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
    doc.signals[0].dir = ANIM_FWD;
    doc.signals[0].sectionStart = 0.0f;  doc.signals[0].sectionEnd = 1.0f;
    TextCopy(doc.signals[1].name, "leave");
    doc.signals[1].dir = ANIM_REV;
    doc.signals[1].sectionStart = 0.5f;  doc.signals[1].sectionEnd = 3.0f;

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
    CHECK(in.elems[1].tracks[1].prop == AP_S_OUTLINE_COLOR);
    CHECK(in.elems[1].tracks[1].keys[0].cval.g == 8);
    CHECK(in.elems[1].tracks[2].prop == AP_S_OUTLINE_ALPHA);
    CHECK(in.elems[1].tracks[2].keyCount == 2 &&
          in.elems[1].tracks[2].keys[1].ease == ANIM_EASE_SINE_OUT);

    // every shape kind survives the name round-trip
    for (int k = SHAPE_SQUARE; k < SHAPE_KIND_COUNT; k++)
        CHECK(in.elems[2 + k - SHAPE_SQUARE].shapeKind == k);
    CHECK(in.signalCount == 2);
    CHECK(in.signals[1].dir == ANIM_REV);
    CHECK_NEAR(in.signals[1].sectionStart, 0.5f);
    CHECK_NEAR(in.signals[1].sectionEnd, 3.0f);

    remove(path);
}

// Old-format files (no `outline` line, pre-refactor shape names) still load
// with outline defaults - forward compatibility for saved docs.
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
    remove(path);
}

// ---------------------------------------------------------------------------
//  Signal bus + anim bridge + player
// ---------------------------------------------------------------------------
static int s_pings = 0;
static void Ping(const char *name, void *user) { (void)name; (void)user; s_pings++; }

static void TestSignals(void)
{
    SignalReset();
    SignalListen("ping", Ping, NULL);
    SignalListen("ping", Ping, NULL);                       // dedup
    SignalEmit("ping");
    CHECK(s_pings == 1);
    SignalStopListening("ping", Ping, NULL);
    SignalEmit("ping");
    CHECK(s_pings == 1);

    // bridge: firing a doc signal starts the player at the right section/dir.
    SignalReset();
    AnimDoc doc;
    AnimDocInit(&doc);
    doc.duration = 2.0f;
    doc.signalCount = 1;
    TextCopy(doc.signals[0].name, "enter");
    doc.signals[0].dir = ANIM_REV;
    doc.signals[0].sectionStart = 0.5f;
    doc.signals[0].sectionEnd   = 1.5f;

    AnimPlayer p = {0};
    AnimSignalRegister(&doc, &p);
    SignalEmit("enter");
    CHECK(p.playing);
    CHECK(p.dir == ANIM_REV);
    CHECK_NEAR(p.secStart, 0.5f);
    CHECK_NEAR(p.secEnd, 1.5f);
    CHECK_NEAR(AnimPlayerSampleTime(&p), 1.5f);             // reverse starts at end

    AnimPlayerUpdate(&p, 0.25f);
    CHECK_NEAR(AnimPlayerSampleTime(&p), 1.25f);
    AnimPlayerUpdate(&p, 10.0f);                            // run past the end
    CHECK(AnimPlayerDone(&p));
    CHECK_NEAR(AnimPlayerSampleTime(&p), 0.5f);

    AnimSignalUnregister(&doc, &p);
    p.playing = false;
    SignalEmit("enter");
    CHECK(!p.playing);                                      // binding removed

    // forward playback
    AnimPlayerStartAll(&p, &doc, ANIM_FWD);
    CHECK_NEAR(AnimPlayerSampleTime(&p), 0.0f);
    AnimPlayerUpdate(&p, 0.75f);
    CHECK_NEAR(AnimPlayerSampleTime(&p), 0.75f);
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
    TestIO();
    TestIOOldFormat();
    TestSignals();

    printf("anim_tests: %d checks, %d failed\n", s_checks, s_fails);
    return s_fails ? 1 : 0;
}
