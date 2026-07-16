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
    CHECK(in.elemCount == 2);
    CHECK(TextIsEqual(in.elems[0].text, "HELLO WORLD"));
    CHECK(in.elems[0].color.a == 200);
    CHECK(in.elems[0].trackCount == 2);
    CHECK(in.elems[0].tracks[0].keyCount == 2);
    CHECK(in.elems[0].tracks[0].keys[1].ease == ANIM_EASE_BOUNCE_OUT);
    CHECK(in.elems[0].tracks[1].keys[0].ease == ANIM_EASE_BACK_IN);
    CHECK(in.elems[1].kind == AE_SHAPE && in.elems[1].shapeKind == SHAPE_CIRCLE);
    CHECK_NEAR(in.elems[1].sizeFrac.y, 0.2f);
    CHECK(in.signalCount == 2);
    CHECK(in.signals[1].dir == ANIM_REV);
    CHECK_NEAR(in.signals[1].sectionStart, 0.5f);
    CHECK_NEAR(in.signals[1].sectionEnd, 3.0f);

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
    TestEase();
    TestKeys();
    TestDoc();
    TestIO();
    TestSignals();

    printf("anim_tests: %d checks, %d failed\n", s_checks, s_fails);
    return s_fails ? 1 : 0;
}
