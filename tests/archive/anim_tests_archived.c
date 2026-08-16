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
#include "../src/anim/anim_ease_custom.h"
#include "../src/anim/anim_io.h"
#include "../src/anim/anim_library.h"
#include "../src/anim/signal.h"
#include "../src/anim/anim_signal.h"
#include "../src/anim/anim_stage.h"
#include "../src/anim/anim_scene.h"
#include "../src/anim/anim_shape_pool.h"
#include <stdio.h>
#include <string.h>
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
// The string pool: shared text with STABLE indices, and the AP_T_STRING track
// that keys them. The stepping rule is the one genuinely new sampling behaviour.
static void TestStringPool(void)
{
    AnimDoc doc;
    AnimDocInit(&doc);
    CHECK(doc.stringCount == 0);

    int a = AnimDocAddString(&doc, "HELLO");
    int b = AnimDocAddString(&doc, "GOODBYE");
    CHECK(a == 0 && b == 1 && doc.stringCount == 2);

    // an identical string is REUSED, not duplicated (auto-add is idempotent)
    CHECK(AnimDocAddString(&doc, "HELLO") == 0);
    CHECK(doc.stringCount == 2);
    CHECK(AnimDocFindString(&doc, "GOODBYE") == 1);
    CHECK(AnimDocFindString(&doc, "nope") == -1);

    CHECK(TextIsEqual(AnimDocStringAt(&doc, 0), "HELLO"));
    CHECK(AnimDocStringAt(&doc, 99) == NULL);           // out of range
    CHECK(AnimDocStringAt(&doc, -1) == NULL);

    // --- the track keys INDICES, and they SNAP ---------------------------
    AnimElem *e = AnimDocAddElem(&doc, AE_TEXT);
    TextCopy(e->name, "t");
    TextCopy(e->text, "BASE");
    AnimTrack *tr = AnimElemAddTrack(e, AP_T_STRING);
    AnimTrackAddKey(tr, 0.0f, 0.0f, ANIM_EASE_LINEAR);   // -> "HELLO"
    AnimTrackAddKey(tr, 2.0f, 1.0f, ANIM_EASE_LINEAR);   // -> "GOODBYE"

    CHECK(AnimPropIsStepped(AP_T_STRING));
    CHECK(!AnimPropIsStepped(AP_T_POS_X) && !AnimPropIsStepped(AP_T_COLOR));

    // mid-segment must NOT be 0.5: a blended index resolves to the wrong entry
    CHECK_NEAR(AnimTrackEval(tr, 1.0f, -1.0f), 0.0f);
    CHECK_NEAR(AnimTrackEval(tr, 1.999f, -1.0f), 0.0f);
    CHECK_NEAR(AnimTrackEval(tr, 2.0f, -1.0f), 1.0f);   // snaps AT the key
    CHECK_NEAR(AnimElemProp(e, AP_T_STRING, 1.0f), 0.0f);

    CHECK(TextIsEqual(AnimElemTextAt(e, &doc, 0.0f), "HELLO"));
    CHECK(TextIsEqual(AnimElemTextAt(e, &doc, 1.9f), "HELLO"));   // holds
    CHECK(TextIsEqual(AnimElemTextAt(e, &doc, 2.0f), "GOODBYE")); // snaps
    CHECK(TextIsEqual(AnimElemTextAt(e, &doc, 9.0f), "GOODBYE")); // after last

    // --- resolver fallbacks ----------------------------------------------
    // no doc -> the element's own text; no track -> likewise
    CHECK(TextIsEqual(AnimElemTextAt(e, NULL, 0.0f), "BASE"));
    AnimElem *plain = AnimDocAddElem(&doc, AE_TEXT);
    TextCopy(plain->text, "PLAIN");
    CHECK(TextIsEqual(AnimElemTextAt(plain, &doc, 0.0f), "PLAIN"));

    // a stale index degrades to the element's own text, never blank
    AnimTrackAddKey(tr, 3.0f, 77.0f, ANIM_EASE_LINEAR);
    CHECK(TextIsEqual(AnimElemTextAt(e, &doc, 3.0f), "BASE"));
    AnimTrackRemoveKey(tr, 2);

    // --- usage counting and guarded delete --------------------------------
    CHECK(AnimDocStringUsers(&doc, 0) == 1);
    CHECK(AnimDocStringUsers(&doc, 1) == 1);
    CHECK(!AnimDocRemoveString(&doc, 0));               // in use: refused
    CHECK(doc.strings[0].used);

    int c = AnimDocAddString(&doc, "UNUSED");
    CHECK(AnimDocStringUsers(&doc, c) == 0);
    CHECK(AnimDocRemoveString(&doc, c));                // unused: removed
    CHECK(doc.stringCount == 2);                        // tail shrank back

    // --- indices are STABLE across a delete -------------------------------
    int keep = AnimDocAddString(&doc, "KEEP");          // idx 2
    int gone = AnimDocAddString(&doc, "GONE");          // idx 3
    CHECK(keep == 2 && gone == 3);
    CHECK(AnimDocRemoveString(&doc, 2));                // remove the MIDDLE one
    CHECK(TextIsEqual(AnimDocStringAt(&doc, 3), "GONE"));  // 3 did NOT shift
    CHECK(AnimDocStringAt(&doc, 2) == NULL);               // 2 is a hole
    // a new add reuses the hole rather than growing
    CHECK(AnimDocAddString(&doc, "REUSED") == 2);

    // --- GC drops unassigned EMPTY entries only ---------------------------
    int empty = AnimDocAddString(&doc, "");
    CHECK(empty >= 0);
    CHECK(AnimDocGCStrings(&doc) == 1);
    CHECK(AnimDocStringAt(&doc, empty) == NULL);
    CHECK(TextIsEqual(AnimDocStringAt(&doc, 0), "HELLO"));  // non-empty untouched
    CHECK(AnimDocGCStrings(&doc) == 0);                     // idempotent

    // --- capacity ---------------------------------------------------------
    AnimDoc full;
    AnimDocInit(&full);
    for (int i = 0; i < ANIM_STRINGS_MAX; i++)
        CHECK(AnimDocAddString(&full, TextFormat("s%d", i)) == i);
    CHECK(AnimDocAddString(&full, "overflow") == -1);
}

// The pool and a string track survive a .cfg round-trip, holes included.
static void TestStringPoolIO(void)
{
    AnimDoc doc;
    AnimDocInit(&doc);
    doc.duration = 4.0f;

    AnimDocAddString(&doc, "FIRST LINE");
    AnimDocAddString(&doc, "second  with  spaces");
    int hole = AnimDocAddString(&doc, "DOOMED");
    AnimDocAddString(&doc, "multi\nline\ntext");
    CHECK(AnimDocRemoveString(&doc, hole));         // leaves a hole at idx 2

    AnimElem *e = AnimDocAddElem(&doc, AE_TEXT);
    TextCopy(e->name, "cap");
    TextCopy(e->text, "FALLBACK");
    AnimTrack *tr = AnimElemAddTrack(e, AP_T_STRING);
    AnimTrackAddKey(tr, 0.0f, 0.0f, ANIM_EASE_LINEAR);
    AnimTrackAddKey(tr, 2.0f, 3.0f, ANIM_EASE_LINEAR);   // index 3, past the hole

    const char *path = "anim_tests_strpool_tmp.cfg";
    CHECK(AnimDocSave(&doc, path));
    AnimDoc in;
    CHECK(AnimDocLoad(&in, path));

    CHECK(in.stringCount == 4);
    CHECK(TextIsEqual(AnimDocStringAt(&in, 0), "FIRST LINE"));
    CHECK(TextIsEqual(AnimDocStringAt(&in, 1), "second  with  spaces"));
    CHECK(AnimDocStringAt(&in, 2) == NULL);              // the hole survived
    CHECK(TextIsEqual(AnimDocStringAt(&in, 3), "multi\nline\ntext"));

    // and the track still resolves to the right entries
    CHECK(in.elemCount == 1);
    CHECK(TextIsEqual(AnimElemTextAt(&in.elems[0], &in, 0.0f), "FIRST LINE"));
    CHECK(TextIsEqual(AnimElemTextAt(&in.elems[0], &in, 2.0f), "multi\nline\ntext"));

    // a second save is byte-identical (the pool does not churn, holes and all)
    const char *path2 = "anim_tests_strpool_tmp2.cfg";
    CHECK(AnimDocSave(&in, path2));
    FILE *f1 = fopen(path, "rb"), *f2 = fopen(path2, "rb");
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

    remove(path); remove(path2);

    // a document with NO pool loads exactly as before
    AnimDoc old;
    AnimDocInit(&old);
    AnimElem *oe = AnimDocAddElem(&old, AE_TEXT);
    TextCopy(oe->text, "PLAIN OLD");
    const char *p3 = "anim_tests_strpool_tmp3.cfg";
    CHECK(AnimDocSave(&old, p3));
    AnimDoc back;
    CHECK(AnimDocLoad(&back, p3));
    CHECK(back.stringCount == 0);
    CHECK(TextIsEqual(AnimElemTextAt(&back.elems[0], &back, 0.0f), "PLAIN OLD"));
    remove(p3);
}

// Clone writes DELTA keys only: an expired element is brought back by copying
// just the properties that actually differ, which is the whole point (it saves
// the 16-key / 12-track / 12-element budgets).
static void TestCloneElemState(void)
{
    AnimDoc doc;
    AnimDocInit(&doc);
    AnimElem *a = AnimDocAddElem(&doc, AE_TEXT);   // destination
    AnimElem *b = AnimDocAddElem(&doc, AE_TEXT);   // source
    TextCopy(a->name, "a"); TextCopy(b->name, "b");

    // Identical rest poses -> nothing differs -> no keys written at all.
    CHECK(AnimDocCloneElemState(&doc, 0, 2.0f, 1, 0.0f, 0.02f, false) == 0);
    CHECK(a->trackCount == 0);

    // Give the source a distinct look at t=0: position and colour differ,
    // size does not.
    b->posFrac = (Vector2){ 0.75f, 0.25f };
    b->color   = (Color){ 10, 20, 30, 255 };
    a->posFrac = (Vector2){ 0.10f, 0.10f };
    a->color   = (Color){ 99, 99, 99, 255 };
    a->sizeFrac = b->sizeFrac;                              // same -> no key

    int n = AnimDocCloneElemState(&doc, 0, 2.0f, 1, 0.0f, 0.02f, false);
    CHECK(n == 3);                                          // pos_x, pos_y, color
    CHECK(AnimElemFindTrack(a, AP_T_POS_X) != NULL);
    CHECK(AnimElemFindTrack(a, AP_T_POS_Y) != NULL);
    CHECK(AnimElemFindTrack(a, AP_T_COLOR) != NULL);
    CHECK(AnimElemFindTrack(a, AP_T_SIZE)  == NULL);        // unchanged -> untouched

    // The destination now LOOKS like the source did, at the chosen time.
    CHECK_NEAR(AnimElemProp(a, AP_T_POS_X, 2.0f), 0.75f);
    CHECK_NEAR(AnimElemProp(a, AP_T_POS_Y, 2.0f), 0.25f);
    Color ac = AnimElemColorProp(a, AP_T_COLOR, 2.0f);
    CHECK(ac.r == 10 && ac.g == 20 && ac.b == 30);

    // Re-cloning at the same time is idempotent: keys within eps are OVERWRITTEN
    // rather than doubled, so repeated use cannot exhaust the key budget.
    int before = AnimElemFindTrack(a, AP_T_POS_X)->keyCount;
    CHECK(AnimDocCloneElemState(&doc, 0, 2.0f, 1, 0.0f, 0.02f, false) == 0);  // now equal
    CHECK(AnimElemFindTrack(a, AP_T_POS_X)->keyCount == before);

    // Cross-kind is a no-op, never corruption.
    AnimDocAddElem(&doc, AE_SHAPE);
    CHECK(AnimDocCloneElemState(&doc, 0, 1.0f, 2, 0.0f, 0.02f, false) == 0);
    CHECK(AnimElemFindTrack(a, AP_S_POS_X) == NULL);        // no shape prop leaked

    // Self-clone onto the SAME instant says nothing and is refused.
    CHECK(AnimDocCloneElemState(&doc, 0, 2.0f, 0, 2.0f, 0.02f, false) == 0);

    // Onto a different time it copies the element's own pose there. `a` sits at
    // 0.10 before its t=2 key and 0.75 from it on, so cloning the EARLY pose
    // forward is a real change and brings the old look back.
    CHECK(AnimDocCloneElemState(&doc, 0, 5.0f, 0, 0.0f, 0.02f, false) > 0);
    CHECK_NEAR(AnimElemProp(a, AP_T_POS_X, 5.0f), 0.10f);   // its own t=0 pose
    CHECK_NEAR(AnimElemProp(a, AP_T_POS_X, 2.0f), 0.75f);   // the t=2 key survived
}

// A property with no track must keep what it already showed. A lone key would
// override the element's base value across the WHOLE timeline, so a fresh track
// is anchored at 0 - this is not a string-only concern.
static void TestClonePreservesBase(void)
{
    AnimDoc doc;
    AnimDocInit(&doc);
    AnimElem *a = AnimDocAddElem(&doc, AE_TEXT);            // destination
    AnimElem *b = AnimDocAddElem(&doc, AE_TEXT);            // source
    a->posFrac = (Vector2){ 0.10f, 0.50f };
    b->posFrac = (Vector2){ 0.90f, 0.50f };
    a->color = b->color; a->sizeFrac = b->sizeFrac;

    CHECK(AnimDocCloneElemState(&doc, 0, 4.0f, 1, 0.0f, 0.02f, false) == 1);

    // The original property survives at the start rather than being replaced.
    CHECK_NEAR(AnimElemProp(a, AP_T_POS_X, 0.0f), 0.10f);
    CHECK_NEAR(AnimElemProp(a, AP_T_POS_X, 4.0f), 0.90f);
    CHECK_NEAR(AnimElemProp(a, AP_T_POS_X, 2.0f), 0.50f);   // interpolated between

    // With holdBefore the old pose is pinned right up to the clone, so the
    // change is a step: mid-way still reads the ORIGINAL value.
    AnimDoc d2;
    AnimDocInit(&d2);
    AnimElem *c = AnimDocAddElem(&d2, AE_TEXT);
    AnimElem *d = AnimDocAddElem(&d2, AE_TEXT);
    c->posFrac = (Vector2){ 0.10f, 0.50f };
    d->posFrac = (Vector2){ 0.90f, 0.50f };
    c->color = d->color; c->sizeFrac = d->sizeFrac;

    CHECK(AnimDocCloneElemState(&d2, 0, 4.0f, 1, 0.0f, 0.02f, true) == 1);
    CHECK_NEAR(AnimElemProp(c, AP_T_POS_X, 2.0f), 0.10f);   // held, not sliding
    CHECK_NEAR(AnimElemProp(c, AP_T_POS_X, 4.0f), 0.90f);   // and the clone landed
    CHECK(AnimElemFindTrack(c, AP_T_POS_X)->keyCount == 3); // anchor + hold + clone
}

// Cloning TEXT must write a string KEY, never overwrite the element's words for
// the whole timeline: the destination says its old thing before the clone time
// and the source's thing from it onwards. This is the whole point of the words
// being a tracked property.
static void TestCloneText(void)
{
    AnimDoc doc;
    AnimDocInit(&doc);
    AnimElem *a = AnimDocAddElem(&doc, AE_TEXT);            // destination
    AnimElem *b = AnimDocAddElem(&doc, AE_TEXT);            // source
    TextCopy(a->text, "BEFORE");
    TextCopy(b->text, "AFTER");
    a->posFrac = b->posFrac; a->color = b->color;           // only words differ

    int n = AnimDocCloneElemState(&doc, 0, 1.0f, 1, 0.0f, 0.02f, false);
    CHECK(n == 1);                                          // exactly the string key
    CHECK(AnimElemFindTrack(a, AP_T_STRING) != NULL);

    // The source's words were pooled so a key could point at them.
    CHECK(AnimDocFindString(&doc, "AFTER") >= 0);

    // Before the key the element keeps its own words; from the key on, the
    // cloned ones. The base text is NOT clobbered.
    CHECK(TextIsEqual(AnimElemTextAt(a, &doc, 0.5f), "BEFORE"));
    CHECK(TextIsEqual(AnimElemTextAt(a, &doc, 1.0f), "AFTER"));
    CHECK(TextIsEqual(AnimElemTextAt(a, &doc, 9.0f), "AFTER"));
    CHECK(TextIsEqual(a->text, "BEFORE"));

    // Stepped, so mid-segment is a whole string, never a blend of two indices.
    int at2 = AnimDocAddString(&doc, "THIRD");
    AnimTrackAddKey(AnimElemFindTrack(a, AP_T_STRING), 3.0f, (float)at2,
                    ANIM_EASE_LINEAR);
    CHECK(TextIsEqual(AnimElemTextAt(a, &doc, 2.0f), "AFTER"));
    CHECK(TextIsEqual(AnimElemTextAt(a, &doc, 3.0f), "THIRD"));

    // Re-cloning is a no-op now that the words already match at that time.
    CHECK(AnimDocCloneElemState(&doc, 0, 1.0f, 1, 0.0f, 0.02f, false) == 0);

    // Two elements wanting the same words SHARE one pool entry rather than
    // spending a second slot - the pool is 24 wide and the point is reuse.
    AnimElem *c = AnimDocAddElem(&doc, AE_TEXT);
    c->posFrac = b->posFrac; c->color = b->color;
    TextCopy(c->text, "OTHER");
    int shared = AnimDocFindString(&doc, "AFTER");
    int usersBefore = AnimDocStringUsers(&doc, shared);
    CHECK(AnimDocCloneElemState(&doc, 2, 1.0f, 1, 0.0f, 0.02f, false) == 1);
    CHECK(AnimDocStringUsers(&doc, shared) == usersBefore + 1);  // shared, not copied
    CHECK(TextIsEqual(AnimElemTextAt(c, &doc, 1.0f), "AFTER"));
    CHECK(TextIsEqual(AnimElemTextAt(c, &doc, 0.5f), "OTHER"));  // its own words held
}

// A NEW string key must carry the words the element already shows. Seeding it
// the way every other property is seeded writes the -1 "no pool entry" base
// value, and the key then resolves to nothing - the editor's "(missing string)".
static void TestStringIdxAt(void)
{
    AnimDoc doc;
    AnimDocInit(&doc);
    AnimElem *a = AnimDocAddElem(&doc, AE_TEXT);
    TextCopy(a->text, "HELLO");

    // No track yet: the element's own words join the pool and that index is it.
    int i0 = AnimDocStringIdxAt(&doc, a, 0.0f);
    CHECK(i0 >= 0);
    CHECK(TextIsEqual(AnimDocStringAt(&doc, i0), "HELLO"));

    // Idempotent - asking twice must not spend a second slot.
    int used = doc.stringCount;
    CHECK(AnimDocStringIdxAt(&doc, a, 2.0f) == i0);
    CHECK(doc.stringCount == used);

    // With a track it resolves THROUGH it, per time, not from keys[0].
    AnimTrack *tr = AnimElemAddTrack(a, AP_T_STRING);
    int i1 = AnimDocAddString(&doc, "WORLD");
    AnimTrackAddKey(tr, 0.0f, (float)i0, ANIM_EASE_LINEAR);
    AnimTrackAddKey(tr, 5.0f, (float)i1, ANIM_EASE_LINEAR);
    CHECK(AnimDocStringIdxAt(&doc, a, 1.0f) == i0);
    CHECK(AnimDocStringIdxAt(&doc, a, 5.0f) == i1);
    CHECK(AnimDocStringIdxAt(&doc, a, 9.0f) == i1);     // holds past the last key

    // A stale index (its entry deleted) degrades to the element's own words
    // rather than resolving to nothing.
    AnimElem *b = AnimDocAddElem(&doc, AE_TEXT);
    TextCopy(b->text, "OWN");
    AnimTrack *bt = AnimElemAddTrack(b, AP_T_STRING);
    AnimTrackAddKey(bt, 0.0f, 999.0f, ANIM_EASE_LINEAR);   // points nowhere
    int ib = AnimDocStringIdxAt(&doc, b, 0.0f);
    CHECK(TextIsEqual(AnimDocStringAt(&doc, ib), "OWN"));

    // A non-text element has no words to pool.
    AnimElem *s = AnimDocAddElem(&doc, AE_SHAPE);
    CHECK(AnimDocStringIdxAt(&doc, s, 0.0f) == -1);
}

// Two string keys at different times: the words SNAP at each key and hold, with
// no blend between two pool indices anywhere in the segment.
static void TestStringKeysStep(void)
{
    AnimDoc doc;
    AnimDocInit(&doc);
    AnimElem *a = AnimDocAddElem(&doc, AE_TEXT);
    TextCopy(a->text, "ONE");

    int i0 = AnimDocAddString(&doc, "ONE");
    int i1 = AnimDocAddString(&doc, "TWO");
    CHECK(i0 != i1);
    AnimTrack *tr = AnimElemAddTrack(a, AP_T_STRING);
    AnimTrackAddKey(tr, 0.0f, (float)i0, ANIM_EASE_LINEAR);
    AnimTrackAddKey(tr, 4.0f, (float)i1, ANIM_EASE_LINEAR);

    CHECK(TextIsEqual(AnimElemTextAt(a, &doc, 0.0f), "ONE"));
    CHECK(TextIsEqual(AnimElemTextAt(a, &doc, 3.9f), "ONE"));   // right up to it
    CHECK(TextIsEqual(AnimElemTextAt(a, &doc, 4.0f), "TWO"));
    CHECK(TextIsEqual(AnimElemTextAt(a, &doc, 8.0f), "TWO"));

    // Every sample across the segment is one of the two entries - never an
    // index between them, which is what interpolation would produce.
    for (int i = 0; i <= 40; i++)
    {
        float t = (float)i * 0.1f;
        int   v = (int)(AnimElemProp(a, AP_T_STRING, t) + 0.5f);
        CHECK(v == i0 || v == i1);
    }

    // Round-tripping through the file keeps both keys pointing where they did.
    const char *path = "/tmp/zen_string_keys_test.cfg";
    CHECK(AnimDocSave(&doc, path));
    AnimDoc rd;
    AnimDocInit(&rd);
    CHECK(AnimDocLoad(&rd, path));
    CHECK(rd.elemCount == 1);
    CHECK(TextIsEqual(AnimElemTextAt(&rd.elems[0], &rd, 1.0f), "ONE"));
    CHECK(TextIsEqual(AnimElemTextAt(&rd.elems[0], &rd, 5.0f), "TWO"));
    remove(path);
}


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
    // old files predate the authoring flags -> all default off.
    CHECK(!in.elems[0].sizeAbsolute && !in.elems[0].cornerMode
          && !in.elems[0].outlineCrisp);
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
    e->outlineCrisp = true;                           // crisp ring outline
    e->rotBase      = 30.0f;                          // rest-pose rotation
    e->sizeFrac     = (Vector2){ 320.0f, 180.0f };   // pixels, since absolute
    // rest-pose rotation with no track is now the base value AnimElemProp reads.
    CHECK_NEAR(AnimElemProp(e, AP_S_ROT, 0.0f), 30.0f);
    CHECK(AnimDocSave(&doc, path));

    AnimDoc in;
    CHECK(AnimDocLoad(&in, path));
    CHECK(in.elemCount == 1);
    CHECK(in.elems[0].sizeAbsolute && in.elems[0].cornerMode);
    CHECK(in.elems[0].outlineCrisp);
    CHECK_NEAR(in.elems[0].rotBase, 30.0f);
    CHECK_NEAR(in.elems[0].sizeFrac.x, 320.0f);
    CHECK_NEAR(in.elems[0].sizeFrac.y, 180.0f);
    remove(path);
}

// SMOOTH LOOP: the tail of a looping cycle eases back into the loop-start pose,
// so the wrap has nothing to jump over. Exercised through AnimLoopBlendBegin,
// which is the same window a draw installs.
static void TestLoopBlend(void)
{
    AnimDoc doc; AnimDocInit(&doc);
    doc.duration   = 5.0f;
    doc.outroStart = 5.0f;
    doc.introEnd   = 0.0f;
    doc.loopSmooth = true;
    doc.loopBlend  = 1.0f;                       // blend over [4,5]

    AnimElem *e = AnimDocAddElem(&doc, AE_SHAPE);
    AnimTrack *w = AnimElemAddTrack(e, AP_S_W);
    AnimTrackAddKey(w, 0.0f, 0.1f, ANIM_EASE_LINEAR);   // loop-start pose
    AnimTrackAddKey(w, 4.0f, 1.6f, ANIM_EASE_LINEAR);   // last key, then held

    // Without the window nothing changes: the value is held to the very end and
    // the wrap back to 0.1 is the snap this feature exists to remove.
    CHECK_NEAR(AnimElemProp(e, AP_S_W, 5.0f), 1.6f);

    AnimLoopBlendBegin(&doc, true);
    CHECK_NEAR(AnimElemProp(e, AP_S_W, 4.0f), 1.6f);    // window opens here: as authored
    CHECK_NEAR(AnimElemProp(e, AP_S_W, 5.0f), 0.1f);    // at the wrap: the loop-start pose
    float mid = AnimElemProp(e, AP_S_W, 4.5f);          // and strictly between the two
    CHECK(mid < 1.6f && mid > 0.1f);
    AnimLoopBlendEnd();

    // A one-shot play must still show its authored tail...
    AnimLoopBlendBegin(&doc, false);
    CHECK_NEAR(AnimElemProp(e, AP_S_W, 5.0f), 1.6f);
    AnimLoopBlendEnd();

    // ...and so must a looping one whose document opts out.
    doc.loopSmooth = false;
    AnimLoopBlendBegin(&doc, true);
    CHECK_NEAR(AnimElemProp(e, AP_S_W, 5.0f), 1.6f);
    AnimLoopBlendEnd();

    // The blend can never be longer than the cycle it belongs to: an oversized
    // value is clamped, not allowed to reach back before the loop start.
    doc.loopSmooth = true;
    doc.loopBlend  = 99.0f;
    AnimLoopBlendBegin(&doc, true);
    CHECK_NEAR(AnimElemProp(e, AP_S_W, 0.0f), 0.1f);    // loop start is untouched
    CHECK_NEAR(AnimElemProp(e, AP_S_W, 5.0f), 0.1f);
    AnimLoopBlendEnd();
}

// The smooth-loop pair round-trips, and a .cfg written before it existed loads
// as smooth with the default blend (so old documents get the fix for free).
static void TestLoopBlendIO(void)
{
    const char *path = "anim_tests_loop_tmp.cfg";
    AnimDoc doc; AnimDocInit(&doc);
    CHECK(doc.loopSmooth);                                 // default: on
    CHECK_NEAR(doc.loopBlend, ANIM_LOOP_BLEND_DEFAULT);
    doc.loopSmooth = false;
    doc.loopBlend  = 0.75f;
    CHECK(AnimDocSave(&doc, path));

    AnimDoc in;
    CHECK(AnimDocLoad(&in, path));
    CHECK(!in.loopSmooth);
    CHECK_NEAR(in.loopBlend, 0.75f);
    remove(path);

    // short `doc` line (pre-loop-blend file) -> the defaults
    FILE *f = fopen(path, "w");
    CHECK(f != NULL);
    if (!f) return;
    fprintf(f, "doc old 2.0 0.0 2.0\n");
    fclose(f);
    CHECK(AnimDocLoad(&in, path));
    CHECK(in.loopSmooth);
    CHECK_NEAR(in.loopBlend, ANIM_LOOP_BLEND_DEFAULT);
    remove(path);
}

// SEQUENCE OFFSET: a signal adds seq * seqMult * envelope(u) to its seq targets,
// eased in per the envelope keys and stacked on top of whatever else drives them.
static void TestSignalSeqStep(void)
{
    AnimDoc doc; AnimDocInit(&doc);
    AnimElem *e = AnimDocAddElem(&doc, AE_SHAPE);
    e->sizeFrac = (Vector2){ 0.1f, 0.1f };
    e->posFrac  = (Vector2){ 0.5f, 0.5f };

    doc.signalCount = 1;
    AnimSignal *sg = &doc.signals[0];
    TextCopy(sg->name, "fan");
    sg->length  = 1.0f;
    sg->usesSeq = true;
    sg->seqMult = 0.03f;                              // each instance +0.03 * seq
    sg->seqTargetCount = 1;
    sg->seqTargets[0] = (AnimSigSeqTarget){ 0, AP_S_W };
    sg->seqKeyCount = 1;
    sg->seqKeys[0] = (AnimSeqKey){ 0.55f, 1.0f, ANIM_EASE_LINEAR };  // full at 0.55

    // at/after u=0.55 the envelope is full, so the offset is exactly seq*mult
    for (int seq = 0; seq <= 2; seq++)
    {
        AnimSignalPlayer sp = {0};
        sp.seq = seq;                                // owned by the instance
        AnimSignalPlayerStart(&sp, sg, &doc, 0.0f, NULL);
        CHECK(sp.seq == seq);                        // Start must not clear it
        AnimSignalPlayerUpdate(&sp, 0.55f);          // u = 0.55, envelope = 1
        CHECK_NEAR(AnimSignalPlayerSeqOffset(&sp, 0, AP_S_W), 0.03f*(float)seq);
        // a prop that is NOT a seq target is never offset
        CHECK_NEAR(AnimSignalPlayerSeqOffset(&sp, 0, AP_S_H), 0.0f);
    }

    // the envelope EASES in: at half the key's u the offset is half (linear key)
    AnimSignalPlayer sp = {0};
    sp.seq = 2;
    AnimSignalPlayerStart(&sp, sg, &doc, 0.0f, NULL);
    AnimSignalPlayerUpdate(&sp, 0.275f);             // half of 0.55
    CHECK_NEAR(AnimSignalPlayerSeqOffset(&sp, 0, AP_S_W), 0.03f*2.0f*0.5f);

    // seq 0 never offsets, and a negative multiplier is allowed
    sp.seq = 0;
    AnimSignalPlayerStart(&sp, sg, &doc, 0.0f, NULL);
    AnimSignalPlayerUpdate(&sp, 0.9f);
    CHECK_NEAR(AnimSignalPlayerSeqOffset(&sp, 0, AP_S_W), 0.0f);
    sg->seqMult = -10.0f; sp.seq = 3;
    AnimSignalPlayerStart(&sp, sg, &doc, 0.0f, NULL);
    AnimSignalPlayerUpdate(&sp, 0.9f);
    CHECK_NEAR(AnimSignalPlayerSeqOffset(&sp, 0, AP_S_W), -30.0f);
}

// usesPos / usesSeq / seqMult, a posparam+poskey, and a seq block round-trip;
// and a signal/target line written before any of this still loads (all the new
// collections empty, usesSeq off), with a stray old seqStep token ignored.
static void TestSignalSeqStepIO(void)
{
    const char *path = "anim_tests_seq_tmp.cfg";
    AnimDoc doc; AnimDocInit(&doc);
    AnimElem *e = AnimDocAddElem(&doc, AE_SHAPE);
    TextCopy(e->name, "box");

    doc.signalCount = 1;
    AnimSignal *sg = &doc.signals[0];
    TextCopy(sg->name, "fan");
    sg->length  = 1.0f;
    sg->usesPos = true;
    sg->posAnchor = true;
    sg->replay = true;
    sg->usesSeq = true;
    sg->seqMult = -0.04f;
    // a Mouse-Position binding on the shape's center, one key with an offset
    sg->posParamCount = 1;
    sg->posParams[0] = (AnimSigPosParam){0};
    sg->posParams[0].elemIdx = 0; sg->posParams[0].slot = 1;
    sg->posParams[0].keyCount = 1;
    sg->posParams[0].keys[0] = (AnimPosKey){ 1.0f, 0.1f, -0.2f, ANIM_EASE_SINE_OUT };
    // a sequence target + envelope key
    sg->seqTargetCount = 1;
    sg->seqTargets[0] = (AnimSigSeqTarget){ 0, AP_S_W };
    sg->seqKeyCount = 1;
    sg->seqKeys[0] = (AnimSeqKey){ 0.55f, 1.0f, ANIM_EASE_SINE_IN };
    // one plain target too, to be sure the sections coexist
    sg->targetCount = 1;
    sg->targets[0] = (AnimSigTarget){0};
    sg->targets[0].elemIdx = 0; sg->targets[0].prop = AP_S_H;
    sg->targets[0].keyCount = 1;
    sg->targets[0].keys[0] = (AnimKey){ 1.0f, 0.5f, (Color){0,0,0,0}, ANIM_EASE_LINEAR };
    CHECK(AnimDocSave(&doc, path));

    AnimDoc in;
    CHECK(AnimDocLoad(&in, path));
    AnimSignal *g = &in.signals[0];
    CHECK(in.signalCount == 1 && g->targetCount == 1);
    CHECK(g->usesPos && g->usesSeq && g->posAnchor && g->replay);
    CHECK_NEAR(g->seqMult, -0.04f);
    CHECK(g->posParamCount == 1 && g->posParams[0].slot == 1);
    CHECK(g->posParams[0].keyCount == 1);
    CHECK_NEAR(g->posParams[0].keys[0].offX, 0.1f);
    CHECK_NEAR(g->posParams[0].keys[0].offY, -0.2f);
    CHECK(g->seqTargetCount == 1 && g->seqTargets[0].prop == AP_S_W);
    CHECK(g->seqKeyCount == 1);
    CHECK_NEAR(g->seqKeys[0].t, 0.55f);
    remove(path);

    // pre-change file: short signal + a target line still carrying an old
    // seqStep column (must be ignored), no params/seq blocks at all.
    FILE *f = fopen(path, "w");
    CHECK(f != NULL);
    if (!f) return;
    fprintf(f, "doc old 2.0\n"
               "elem shape box\n"
               "  pos 0.5 0.5\n"
               "  end\n"
               "signal fan 1.0\n"
               "  target box w 1 -0.04\n"
               "    key 1.0 0.5 linear\n"
               "  endsig\n");
    fclose(f);
    CHECK(AnimDocLoad(&in, path));
    g = &in.signals[0];
    CHECK(in.signalCount == 1 && g->targetCount == 1);
    CHECK(!g->usesPos && !g->usesSeq && !g->posAnchor && !g->replay);
    CHECK(g->posParamCount == 0 && g->seqTargetCount == 0 && g->seqKeyCount == 0);
    CHECK(g->targets[0].keyCount == 1);
    CHECK_NEAR(g->targets[0].keys[0].value, 0.5f);
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
    // the editor authors SIGNAL targets in groups too: one authored track costs
    // one target slot per member, so the budget has to hold whole groups.
    CHECK(ANIM_SIG_TARGETS_MAX >= ANIM_GROUP_PROPS);
    CHECK(ANIM_SIG_TARGETS_MAX >= ANIM_ELEMS_MAX);
}

// A signal whose targets were authored as GROUPS (shape outline = 3 props,
// position = 2) survives save/load with its props, keys, u values and eases -
// the .cfg is still per-property, so this guards the grouped editing model
// against the storage it writes through.
static void TestIOSignalGroupTargets(void)
{
    AnimDoc doc;
    AnimDocInit(&doc);
    TextCopy(doc.name, "siggroups");
    doc.duration = 2.0f;

    AnimElem *s = AnimDocAddElem(&doc, AE_SHAPE);
    TextCopy(s->name, "box");

    // the shape `outline` group (outline, outline_color, outline_alpha) plus
    // `position` (pos_x, pos_y): 5 targets from two authored tracks
    const int props[] = { AP_S_OUTLINE, AP_S_OUTLINE_COLOR, AP_S_OUTLINE_ALPHA,
                          AP_S_POS_X, AP_S_POS_Y };
    doc.signalCount = 1;
    TextCopy(doc.signals[0].name, "flash");
    doc.signals[0].length      = 0.75f;
    doc.signals[0].targetCount = 5;
    for (int i = 0; i < 5; i++)
    {
        AnimSigTarget *tg = &doc.signals[0].targets[i];
        *tg = (AnimSigTarget){0};
        tg->elemIdx  = 0;
        tg->prop     = props[i];
        tg->keyCount = 2;                       // one GROUP key at each u
        tg->keys[0] = (AnimKey){ 0.25f, 0.1f*(i+1), (Color){ 10, 20, 30, 255 },
                                 ANIM_EASE_LINEAR };
        tg->keys[1] = (AnimKey){ 1.00f, 0.2f*(i+1), (Color){ 40, 50, 60, 255 },
                                 ANIM_EASE_BACK_IN };
    }

    const char *path = "anim_tests_siggroup.cfg";
    CHECK(AnimDocSave(&doc, path));

    AnimDoc in;
    CHECK(AnimDocLoad(&in, path));
    CHECK(in.signalCount == 1);
    CHECK(in.signals[0].targetCount == 5);
    for (int i = 0; i < 5; i++)
    {
        AnimSigTarget *tg = &in.signals[0].targets[i];
        CHECK(tg->prop == props[i]);
        CHECK(tg->keyCount == 2);
        CHECK_NEAR(tg->keys[0].t, 0.25f);       // shared group key times
        CHECK_NEAR(tg->keys[1].t, 1.00f);
        CHECK(tg->keys[1].ease == ANIM_EASE_BACK_IN);
        if (AnimPropIsColor(props[i])) CHECK(tg->keys[1].cval.g == 50);
        else                           CHECK_NEAR(tg->keys[1].value, 0.2f*(i+1));
    }
    remove(path);
}

// Intro/outro trim: accessors clamp, the section round-trips through .cfg, and
// a looping player replays [introEnd, outroStart) after its first pass.
// Pause markers: sorted insert, the crossing query the runtime and the editor
// share, retiming, and a .cfg round-trip.
static void TestPauseMarkers(void)
{
    AnimDoc doc;
    AnimDocInit(&doc);
    doc.duration = 10.0f;

    // inserted out of order, stored ascending
    CHECK(AnimDocAddPause(&doc, 4.0f, 0.01f) != NULL);
    CHECK(AnimDocAddPause(&doc, 1.0f, 0.01f) != NULL);
    CHECK(AnimDocAddPause(&doc, 7.0f, 0.01f) != NULL);
    CHECK(doc.pauseCount == 3);
    CHECK_NEAR(doc.pauses[0].t, 1.0f);
    CHECK_NEAR(doc.pauses[1].t, 4.0f);
    CHECK_NEAR(doc.pauses[2].t, 7.0f);

    // a marker already within eps wins: no invisible duplicates
    CHECK(AnimDocAddPause(&doc, 4.005f, 0.01f) == NULL);
    CHECK(doc.pauseCount == 3);
    CHECK(AnimDocPauseAt(&doc, 4.005f, 0.01f) == 1);
    CHECK(AnimDocPauseAt(&doc, 5.0f, 0.01f) == -1);

    // crossing is half-open (from, to]: a marker exactly AT `from` was already
    // served, so resuming from it must not re-fire it.
    CHECK(AnimDocNextPause(&doc, 0.0f, 2.0f) == 0);
    CHECK(AnimDocNextPause(&doc, 1.0f, 3.0f) == -1);        // at `from` -> skipped
    CHECK(AnimDocNextPause(&doc, 1.0f, 4.0f) == 1);         // at `to`   -> caught
    CHECK(AnimDocNextPause(&doc, 0.0f, 9.0f) == 0);         // earliest wins
    CHECK(AnimDocNextPause(&doc, 8.0f, 9.0f) == -1);
    CHECK(AnimDocNextPause(&doc, 5.0f, 5.0f) == -1);        // empty range

    // retiming keeps the array sorted and reports the new slot
    int ni = AnimDocSetPauseTime(&doc, 0, 6.0f);            // 1 -> 6: now middle
    CHECK(ni == 1);
    CHECK_NEAR(doc.pauses[0].t, 4.0f);
    CHECK_NEAR(doc.pauses[1].t, 6.0f);
    CHECK_NEAR(doc.pauses[2].t, 7.0f);
    CHECK(AnimDocSetPauseTime(&doc, 99, 1.0f) == -1);       // bad index: no-op

    doc.pauses[1].once = true;

    // round-trip, including the optional `once` flag
    const char *path = "anim_tests_pause_tmp.cfg";
    CHECK(AnimDocSave(&doc, path));
    AnimDoc in;
    CHECK(AnimDocLoad(&in, path));
    CHECK(in.pauseCount == 3);
    CHECK_NEAR(in.pauses[0].t, 4.0f);
    CHECK_NEAR(in.pauses[1].t, 6.0f);
    CHECK_NEAR(in.pauses[2].t, 7.0f);
    CHECK(!in.pauses[0].once && in.pauses[1].once && !in.pauses[2].once);
    remove(path);

    // capacity is a hard stop, not an overrun
    AnimDocInit(&doc);
    for (int i = 0; i < ANIM_PAUSES_MAX; i++)
        CHECK(AnimDocAddPause(&doc, (float)i, 0.01f) != NULL);
    CHECK(doc.pauseCount == ANIM_PAUSES_MAX);
    CHECK(AnimDocAddPause(&doc, 99.0f, 0.01f) == NULL);
    CHECK(doc.pauseCount == ANIM_PAUSES_MAX);

    AnimDocRemovePause(&doc, 0);
    CHECK(doc.pauseCount == ANIM_PAUSES_MAX - 1);
    CHECK_NEAR(doc.pauses[0].t, 1.0f);                      // shifted down

    // a doc with no markers behaves exactly as before
    AnimDocInit(&doc);
    CHECK(AnimDocNextPause(&doc, 0.0f, 100.0f) == -1);
}

// AnimPlayerSeek parks the clock on an exact doc time, in both directions.
static void TestPlayerSeek(void)
{
    AnimDoc doc;
    AnimDocInit(&doc);
    doc.duration = 10.0f;
    doc.outroStart = 10.0f;

    AnimPlayer p = {0};
    AnimPlayerStartAll(&p, &doc, ANIM_FWD);
    AnimPlayerSeek(&p, 3.5f);
    CHECK_NEAR(AnimPlayerSampleTime(&p), 3.5f);
    AnimPlayerSeek(&p, -5.0f);                              // clamped low
    CHECK_NEAR(AnimPlayerSampleTime(&p), 0.0f);
    AnimPlayerSeek(&p, 99.0f);                              // clamped high
    CHECK_NEAR(AnimPlayerSampleTime(&p), 10.0f);

    AnimPlayerStartAll(&p, &doc, ANIM_REV);
    AnimPlayerSeek(&p, 3.5f);
    CHECK_NEAR(AnimPlayerSampleTime(&p), 3.5f);             // inverse holds
}

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
    TextCopy(t->text, "TWO WORDS");                         // exercises the space escape
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

// A crumble key carries the whole state of the effect - the amount AND the four
// params that shape the scatter - on one line. Round-trips through save/load,
// and a re-save is byte-identical (the wide form has to be stable too).
static void TestIOCrumbleKeys(void)
{
    const char *p1 = "anim_tests_crumble1.cfg", *p2 = "anim_tests_crumble2.cfg";

    AnimDoc doc;
    AnimDocInit(&doc);
    TextCopy(doc.name, "crumble");
    doc.duration = 2.0f;

    AnimElem *t = AnimDocAddElem(&doc, AE_TEXT);
    TextCopy(t->name, "title");
    TextCopy(t->text, "GONE");

    // the five members keyed as a group would key them: same two times.
    const int props[5] = { AP_T_CRUMBLE, AP_T_CRUMBLE_DIR, AP_T_CRUMBLE_SPREAD,
                           AP_T_CRUMBLE_DIST, AP_T_CRUMBLE_SPIN };
    const float v0[5] = { 0.0f,  90.0f,  12.0f, 0.5f,  90.0f };
    const float v1[5] = { 1.0f, 270.0f, 180.0f, 1.5f, 360.0f };
    for (int i = 0; i < 5; i++)
    {
        AnimTrack *tr = AnimElemAddTrack(t, props[i]);
        CHECK(tr != NULL);
        AnimTrackAddKey(tr, 0.0f, v0[i], ANIM_EASE_LINEAR);
        AnimTrackAddKey(tr, 1.0f, v1[i], ANIM_EASE_SINE_OUT);
    }

    CHECK(AnimDocSave(&doc, p1));

    AnimDoc back;
    CHECK(AnimDocLoad(&back, p1));
    CHECK(back.elemCount == 1);
    CHECK(back.elems[0].trackCount == 5);
    for (int i = 0; i < 5; i++)
    {
        AnimTrack *tr = AnimElemFindTrack(&back.elems[0], props[i]);
        CHECK(tr != NULL);
        if (!tr) continue;
        CHECK(tr->keyCount == 2);
        CHECK_NEAR(tr->keys[0].value, v0[i]);
        CHECK_NEAR(tr->keys[1].value, v1[i]);
        CHECK(tr->keys[0].ease == ANIM_EASE_LINEAR);
        CHECK(tr->keys[1].ease == ANIM_EASE_SINE_OUT);
    }
    // and the shape is genuinely animated, not stuck on the element's rest pose
    // (mid-segment, so eased - the bound is what matters, not the exact value)
    float midDir = AnimElemProp(&back.elems[0], AP_T_CRUMBLE_DIR, 0.5f);
    CHECK(midDir > 90.0f && midDir < 270.0f);
    CHECK_NEAR(AnimElemProp(&back.elems[0], AP_T_CRUMBLE_DIR, 1.0f), 270.0f);

    CHECK(AnimDocSave(&back, p2));
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

// A crumble track with only the AMOUNT keyed still writes the wide line: the
// four shape members come from the element's rest pose. This is what migrated
// every pre-existing document, so a param nobody keyed keeps its authored look.
static void TestIOCrumbleRestPoseFill(void)
{
    AnimDoc doc;
    AnimDocInit(&doc);
    TextCopy(doc.name, "crumblefill");
    doc.duration = 2.0f;

    AnimElem *e = AnimDocAddElem(&doc, AE_TEXT);
    TextCopy(e->name, "title");
    TextCopy(e->text, "GONE");
    e->crumbleDir = 270.0f; e->crumbleSpread = 30.0f;
    e->crumbleDist = 1.25f; e->crumbleRot    = 45.0f;
    AnimTrack *tr = AnimElemAddTrack(e, AP_T_CRUMBLE);
    AnimTrackAddKey(tr, 0.0f, 0.0f, ANIM_EASE_LINEAR);
    AnimTrackAddKey(tr, 1.0f, 1.0f, ANIM_EASE_LINEAR);

    const char *path = "anim_tests_crumble_fill.cfg";
    CHECK(AnimDocSave(&doc, path));

    AnimDoc back;
    CHECK(AnimDocLoad(&back, path));
    CHECK(back.elemCount == 1);
    AnimElem *b = &back.elems[0];
    CHECK(b->trackCount == 5);                      // all five members now exist
    CHECK_NEAR(AnimElemProp(b, AP_T_CRUMBLE, 1.0f), 1.0f);
    // the rest pose was baked into every key, so it reads back unchanged
    CHECK_NEAR(AnimElemProp(b, AP_T_CRUMBLE_DIR,    0.5f), 270.0f);
    CHECK_NEAR(AnimElemProp(b, AP_T_CRUMBLE_SPREAD, 0.5f),  30.0f);
    CHECK_NEAR(AnimElemProp(b, AP_T_CRUMBLE_DIST,   0.5f),   1.25f);
    CHECK_NEAR(AnimElemProp(b, AP_T_CRUMBLE_SPIN,   0.5f),  45.0f);

    // and the fill is stable: a second save is byte-identical to the first
    const char *out = "anim_tests_crumble_fill2.cfg";
    CHECK(AnimDocSave(&back, out));
    FILE *f1 = fopen(path, "rb"), *f2 = fopen(out, "rb");
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
    remove(out);
    remove(path);
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

// Write a stage anim carrying a pause marker at `pauseT`.
static void WritePauseAnim(const char *name, float pauseT, bool once, bool terminal)
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

    AnimPause *p = AnimDocAddPause(&doc, pauseT, 0.01f);
    if (p) p->once = once;

    doc.signalCount = 1;
    AnimSignal *sg = &doc.signals[0];
    TextCopy(sg->name, "pause_sig");
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

// The runtime hold. Headless, GetKeyPressed() always returns 0, so a paused
// instance stays paused for the whole test - which is exactly what makes the
// isolation assertions below meaningful.
static void TestStagePause(void)
{
    SignalReset();
    AnimStageReset();
    WritePauseAnim("_test_pause", 1.0f, false, false);
    WriteStageAnim("_test_nopause", false);

    // --- playback holds ON the marker, not past it ------------------------
    AnimHandle h = AnimStagePlay("_test_pause", true, 0);
    CHECK(h != ANIM_HANDLE_NONE);
    CHECK(!AnimStagePaused(h));
    for (int i = 0; i < 40; i++) AnimStageUpdate(0.05f);   // 2s >> the 1s marker
    CHECK(AnimStagePaused(h));
    CHECK(AnimStageAlive(h));                    // held, NOT finished

    // it stays held for as long as no key arrives, and never runs out
    for (int i = 0; i < 200; i++) AnimStageUpdate(0.05f);
    CHECK(AnimStagePaused(h));
    CHECK(AnimStageAlive(h));

    // --- ISOLATION: a second instance is untouched by the first's hold ----
    AnimHandle other = AnimStagePlay("_test_nopause", false, 0);
    CHECK(other != ANIM_HANDLE_NONE);
    CHECK(!AnimStagePaused(other));
    for (int i = 0; i < 60; i++) AnimStageUpdate(0.05f);   // 3s > its 2s length
    CHECK(!AnimStageAlive(other));               // ran to completion while h held
    CHECK(AnimStagePaused(h));                   // and h is still holding

    // --- ISOLATION: a signal keeps running on the PAUSED instance ---------
    // A signal plays on its own clock as an override, so a doc-clock hold must
    // not stall it. Terminal here, so completing it must still end the instance.
    AnimStageStopAll();
    WritePauseAnim("_test_pause_term", 1.0f, false, true);
    AnimHandle t = AnimStagePlay("_test_pause_term", true, 0);
    for (int i = 0; i < 40; i++) AnimStageUpdate(0.05f);
    CHECK(AnimStagePaused(t));                   // parked on the marker

    SignalEmit("pause_sig", NULL);
    CHECK(AnimStageEndsOnCurrentSignal(t));      // the signal armed a shutdown
    AnimStageUpdate(0.5f);                       // half the 1.0s signal
    CHECK(AnimStageAlive(t));                    // not cut off mid-transition
    for (int i = 0; i < 20; i++) AnimStageUpdate(0.05f);
    CHECK(!AnimStageAlive(t));                   // signal ran to its end and ended
                                                 // it, THROUGH the pause
    // --- a doc with no markers never pauses -------------------------------
    AnimStageStopAll();
    AnimHandle n = AnimStagePlay("_test_nopause", true, 0);
    for (int i = 0; i < 200; i++) AnimStageUpdate(0.05f);
    CHECK(AnimStageAlive(n) && !AnimStagePaused(n));

    AnimStageStopAll();
    remove("anims/_test_pause.cfg");
    remove("anims/_test_pause_term.cfg");
    remove("anims/_test_nopause.cfg");
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
// ---------------------------------------------------------------------------
//  Signal POSITION parameter (the "--params--" section): a Mouse-Position
//  binding eases a position slot from the live pose to mouse + per-key offset.
//  usesPos off, or no position emitted, drives nothing.
// ---------------------------------------------------------------------------
static void TestSignalPosParam(void)
{
    AnimDoc doc;
    AnimDocInit(&doc);
    doc.duration = 2.0f;
    AnimElem *e = AnimDocAddElem(&doc, AE_SHAPE);
    e->posFrac = (Vector2){ 0.5f, 0.5f };

    // A signal that binds the shape's CENTER to the mouse, one key at u=1 with a
    // +0.1 x offset (so the end lands on mouse.x + 0.1, mouse.y + 0).
    doc.signalCount = 1;
    AnimSignal *sg = &doc.signals[0];
    TextCopy(sg->name, "place");
    sg->length = 1.0f;
    sg->posParamCount = 1;
    sg->posParams[0] = (AnimSigPosParam){0};
    sg->posParams[0].elemIdx = 0; sg->posParams[0].slot = 0;   // center
    sg->posParams[0].keyCount = 1;
    sg->posParams[0].keys[0] = (AnimPosKey){ 1.0f, 0.1f, 0.0f, ANIM_EASE_LINEAR };

    float vx = 0.0f, vy = 0.0f;
    SignalParams pp = { .pos = { 0.3f, 0.4f }, .hasPos = true };

    // --- usesPos off: an emitted position is ignored (nothing drives pos) ---
    AnimSignalPlayer sp = {0};
    AnimSignalPlayerStart(&sp, sg, &doc, 0.0f, &pp);
    AnimSignalPlayerUpdate(&sp, 0.9f);
    CHECK(!AnimSignalPlayerEval(&sp, 0, AP_S_POS_X, &vx, NULL));

    // --- declared + given: EASED from the live pose into mouse+offset -------
    sg->usesPos = true;
    AnimSignalPlayerStart(&sp, sg, &doc, 0.0f, &pp);

    // u=0 holds the live pose: firing must not teleport.
    CHECK(AnimSignalPlayerEval(&sp, 0, AP_S_POS_X, &vx, NULL));
    CHECK(AnimSignalPlayerEval(&sp, 0, AP_S_POS_Y, &vy, NULL));
    CHECK_NEAR(vx, 0.5f);
    CHECK_NEAR(vy, 0.5f);

    // part-way: linear ease from live (0.5) toward target (mouse.x+off = 0.4).
    AnimSignalPlayerUpdate(&sp, 0.5f);
    CHECK(AnimSignalPlayerEval(&sp, 0, AP_S_POS_X, &vx, NULL));
    CHECK_NEAR(vx, 0.5f + (0.4f - 0.5f)*0.5f);        // 0.45

    // by the last instant it is ON mouse + offset: x = 0.3+0.1, y = 0.4+0.
    AnimSignalPlayerStart(&sp, sg, &doc, 0.0f, &pp);
    AnimSignalPlayerUpdate(&sp, 0.9999f);
    CHECK(AnimSignalPlayerEval(&sp, 0, AP_S_POS_X, &vx, NULL));
    CHECK(AnimSignalPlayerEval(&sp, 0, AP_S_POS_Y, &vy, NULL));
    CHECK_NEAR(vx, 0.4f);
    CHECK_NEAR(vy, 0.4f);

    // --- usesPos on but NO position emitted: the binding does nothing -------
    AnimSignalPlayerStart(&sp, sg, &doc, 0.0f, NULL);
    AnimSignalPlayerUpdate(&sp, 0.9f);
    CHECK(!AnimSignalPlayerEval(&sp, 0, AP_S_POS_X, &vx, NULL));
}

// A corners-mode shape exposes TWO position slots; binding the SECOND corner
// (P1) to the mouse recomputes center+size so P1 lands on it while P0 holds.
static void TestSignalPosCorner(void)
{
    AnimDoc doc;
    AnimDocInit(&doc);
    doc.duration = 2.0f;
    AnimElem *e = AnimDocAddElem(&doc, AE_SHAPE);
    e->cornerMode = true;
    e->posFrac  = (Vector2){ 0.5f, 0.5f };
    e->sizeFrac = (Vector2){ 0.2f, 0.2f };            // P0=(0.4,0.4) P1=(0.6,0.6)
    e->scaleFrac = 1.0f;

    // AnimGeomToCorner agrees with the base corners.
    Vector2 p0 = AnimGeomToCorner(e, 0), p1 = AnimGeomToCorner(e, 1);
    CHECK_NEAR(p0.x, 0.4f); CHECK_NEAR(p0.y, 0.4f);
    CHECK_NEAR(p1.x, 0.6f); CHECK_NEAR(p1.y, 0.6f);

    doc.signalCount = 1;
    AnimSignal *sg = &doc.signals[0];
    TextCopy(sg->name, "corner");
    sg->length  = 1.0f;
    sg->usesPos = true;
    sg->posParamCount = 1;
    sg->posParams[0] = (AnimSigPosParam){0};
    sg->posParams[0].elemIdx = 0; sg->posParams[0].slot = 1;   // P1
    sg->posParams[0].keyCount = 1;
    sg->posParams[0].keys[0] = (AnimPosKey){ 1.0f, 0.0f, 0.0f, ANIM_EASE_LINEAR };

    SignalParams pp = { .pos = { 0.8f, 0.9f }, .hasPos = true };
    AnimSignalPlayer sp = {0};
    AnimSignalPlayerStart(&sp, sg, &doc, 0.0f, &pp);

    // u=0: the whole shape is still at its live pose (no teleport).
    float cx, cy, w, h;
    AnimSignalPlayerUpdate(&sp, 0.0f);
    CHECK(AnimSignalPlayerEval(&sp, 0, AP_S_POS_X, &cx, NULL));
    CHECK(AnimSignalPlayerEval(&sp, 0, AP_S_POS_Y, &cy, NULL));
    CHECK(AnimSignalPlayerEval(&sp, 0, AP_S_W, &w, NULL));
    CHECK(AnimSignalPlayerEval(&sp, 0, AP_S_H, &h, NULL));
    CHECK_NEAR(cx, 0.5f); CHECK_NEAR(cy, 0.5f);
    CHECK_NEAR(w, 0.2f);  CHECK_NEAR(h, 0.2f);

    // end: P1 -> (0.8,0.9), P0 held at (0.4,0.4). center=(0.6,0.65) w=0.4 h=0.5.
    AnimSignalPlayerUpdate(&sp, 0.9999f);
    CHECK(AnimSignalPlayerEval(&sp, 0, AP_S_POS_X, &cx, NULL));
    CHECK(AnimSignalPlayerEval(&sp, 0, AP_S_POS_Y, &cy, NULL));
    CHECK(AnimSignalPlayerEval(&sp, 0, AP_S_W, &w, NULL));
    CHECK(AnimSignalPlayerEval(&sp, 0, AP_S_H, &h, NULL));
    CHECK_NEAR(cx, 0.6f);  CHECK_NEAR(cy, 0.65f);
    CHECK_NEAR(w, 0.4f);   CHECK_NEAR(h, 0.5f);
}

// Spawn-anchor mode (AnimSignal.posAnchor): the binding no longer REPLACES the
// slot (PosParamEval steps aside) - instead AnimSignalPlayerPosAnchor returns a
// constant POS translation `mouse - authored_point(0)`, so the authored motion
// plays but the element is born at the cursor.
static void TestSignalPosAnchor(void)
{
    AnimDoc doc;
    AnimDocInit(&doc);
    doc.duration = 2.0f;
    AnimElem *e = AnimDocAddElem(&doc, AE_SHAPE);
    e->posFrac = (Vector2){ 0.5f, 0.5f };

    doc.signalCount = 1;
    AnimSignal *sg = &doc.signals[0];
    TextCopy(sg->name, "anchored");
    sg->length    = 1.0f;
    sg->usesPos   = true;
    sg->posAnchor = true;                                 // <-- spawn-anchor
    sg->posParamCount = 1;
    sg->posParams[0] = (AnimSigPosParam){0};
    sg->posParams[0].elemIdx = 0; sg->posParams[0].slot = 0;   // center
    sg->posParams[0].keyCount = 1;
    // offset keys are IGNORED in anchor mode - prove it by giving a fat offset.
    sg->posParams[0].keys[0] = (AnimPosKey){ 1.0f, 0.5f, 0.5f, ANIM_EASE_LINEAR };

    SignalParams pp = { .pos = { 0.3f, 0.4f }, .hasPos = true };
    AnimSignalPlayer sp = {0};
    AnimSignalPlayerStart(&sp, sg, &doc, 0.0f, &pp);

    // PosParamEval must NOT drive the slot in anchor mode (no plain target here).
    float v = 0.0f;
    CHECK(!AnimSignalPlayerEval(&sp, 0, AP_S_POS_X, &v, NULL));

    // The additive anchor is a CONSTANT `mouse - authored_point(0)`, offset-free.
    // authored center at fire = (0.5,0.5), mouse = (0.3,0.4).
    CHECK_NEAR(AnimSignalPlayerPosAnchor(&sp, 0, AP_S_POS_X), -0.2f);
    CHECK_NEAR(AnimSignalPlayerPosAnchor(&sp, 0, AP_S_POS_Y), -0.1f);
    // constant across the whole signal (not eased toward a key).
    AnimSignalPlayerUpdate(&sp, 0.5f);
    CHECK_NEAR(AnimSignalPlayerPosAnchor(&sp, 0, AP_S_POS_X), -0.2f);
    // only POS is translated - size/other props get nothing.
    CHECK_NEAR(AnimSignalPlayerPosAnchor(&sp, 0, AP_S_W), 0.0f);

    // usesPos on but no position emitted -> no anchor.
    AnimSignalPlayerStart(&sp, sg, &doc, 0.0f, NULL);
    CHECK_NEAR(AnimSignalPlayerPosAnchor(&sp, 0, AP_S_POS_X), 0.0f);

    // posAnchor off -> falls back to replace mode, anchor contributes nothing.
    sg->posAnchor = false;
    AnimSignalPlayerStart(&sp, sg, &doc, 0.0f, &pp);
    CHECK_NEAR(AnimSignalPlayerPosAnchor(&sp, 0, AP_S_POS_X), 0.0f);
    CHECK(AnimSignalPlayerEval(&sp, 0, AP_S_POS_X, &v, NULL));   // replace is back

    // Corner binding: the reference point is the CORNER at fire, not the center.
    // P1 of a 0.2-size shape centered at 0.5 is (0.6,0.6); mouse (0.8,0.9) ->
    // anchor (0.2,0.3) translating the whole element (size unchanged).
    AnimDoc dc; AnimDocInit(&dc); dc.duration = 2.0f;
    AnimElem *ce = AnimDocAddElem(&dc, AE_SHAPE);
    ce->cornerMode = true;
    ce->posFrac  = (Vector2){ 0.5f, 0.5f };
    ce->sizeFrac = (Vector2){ 0.2f, 0.2f };
    ce->scaleFrac = 1.0f;
    dc.signalCount = 1;
    AnimSignal *cs = &dc.signals[0];
    TextCopy(cs->name, "canchor");
    cs->length = 1.0f; cs->usesPos = true; cs->posAnchor = true;
    cs->posParamCount = 1;
    cs->posParams[0] = (AnimSigPosParam){0};
    cs->posParams[0].elemIdx = 0; cs->posParams[0].slot = 1;   // P1
    cs->posParams[0].keyCount = 1;
    cs->posParams[0].keys[0] = (AnimPosKey){ 1.0f, 0.0f, 0.0f, ANIM_EASE_LINEAR };
    SignalParams cp = { .pos = { 0.8f, 0.9f }, .hasPos = true };
    AnimSignalPlayer csp = {0};
    AnimSignalPlayerStart(&csp, cs, &dc, 0.0f, &cp);
    CHECK_NEAR(AnimSignalPlayerPosAnchor(&csp, 0, AP_S_POS_X), 0.2f);
    CHECK_NEAR(AnimSignalPlayerPosAnchor(&csp, 0, AP_S_POS_Y), 0.3f);
    CHECK_NEAR(AnimSignalPlayerPosAnchor(&csp, 0, AP_S_W), 0.0f);   // size untouched
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
    sg->usesPos     = true;      // this signal is authored to be placed
    sg->posParamCount = 1;
    sg->posParams[0] = (AnimSigPosParam){0};
    sg->posParams[0].elemIdx = 0; sg->posParams[0].slot = 0;  // center
    sg->posParams[0].keyCount = 1;
    sg->posParams[0].keys[0] = (AnimPosKey){ 1.0f, 0.0f, 0.0f, ANIM_EASE_LINEAR };

    float docTime = 0.0f;
    AnimSignalPlayer sp = {0};
    AnimSignalRegister(&doc, &sp, &docTime);

    SignalParams pp = { .pos = { 0.9f, 0.1f }, .hasPos = true };
    SignalEmit("spawn", &pp);
    CHECK(sp.playing);
    CHECK(sp.param.hasPos);
    AnimSignalPlayerUpdate(&sp, 0.9999f);
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
    remove("anims/_test_scene.cfg");
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
    remove("anims/_test_scene2.cfg");
}

// Restart-on-fire (AnimSignal.replay): a non-looping instance runs once, is then
// HELD on its last frame (not deactivated) so a later emit rewinds its timeline
// to u=0. Without the flag the same one-shot would have gone inert.
static void WriteReplayAnim(const char *name, bool replay)
{
    AnimDoc doc;
    AnimDocInit(&doc);
    doc.duration = 1.0f; doc.introEnd = 0.0f; doc.outroStart = 1.0f;
    AnimElem *e = AnimDocAddElem(&doc, AE_TEXT);
    TextCopy(e->name, "a");
    AnimTrack *tr = AnimElemAddTrack(e, AP_T_ALPHA);
    AnimTrackAddKey(tr, 0.0f, 0.0f, ANIM_EASE_LINEAR);
    AnimTrackAddKey(tr, 1.0f, 1.0f, ANIM_EASE_LINEAR);

    doc.signalCount = 1;
    AnimSignal *sg = &doc.signals[0];
    TextCopy(sg->name, "trig");
    sg->length = 0.2f;
    sg->replay = replay;          // the field under test - no targets needed
    if (!DirectoryExists("anims")) MakeDirectory("anims");
    AnimDocSave(&doc, TextFormat("anims/%s.cfg", name));
}

static void TestStageReplay(void)
{
    SignalReset();
    AnimStageReset();

    // --- without replay: a one-shot deactivates when its timeline runs out ----
    WriteReplayAnim("_test_noreplay", false);
    AnimHandle n = AnimStagePlay("_test_noreplay", false, 0);
    CHECK(AnimStageAlive(n));
    for (int i = 0; i < 30; i++) AnimStageUpdate(0.05f);   // 1.5s > 1.0s duration
    CHECK(!AnimStageAlive(n));                             // gone, as before
    CHECK(AnimStageActiveCount() == 0);

    // --- with replay: the one-shot is HELD, and an emit rewinds it to u=0 -----
    WriteReplayAnim("_test_replay", true);
    AnimHandle h = AnimStagePlay("_test_replay", false, 0);
    CHECK(AnimStageAlive(h));
    for (int i = 0; i < 30; i++) AnimStageUpdate(0.05f);   // run past the end
    CHECK(AnimStageAlive(h));                              // HELD, not deactivated
    CHECK(AnimStageActiveCount() == 1);
    CHECK(AnimStagePlayhead(h) >= 0.9f);                   // parked at the end

    AnimStageEmit(h, "trig", NULL);                        // restart-on-fire
    CHECK(AnimStagePlayhead(h) <= 0.05f);                  // rewound to the start
    for (int i = 0; i < 6; i++) AnimStageUpdate(0.05f);    // 0.3s in
    CHECK(AnimStageAlive(h));
    CHECK(AnimStagePlayhead(h) > 0.2f && AnimStagePlayhead(h) < 0.4f);

    for (int i = 0; i < 20; i++) AnimStageUpdate(0.05f);   // run past the end again
    CHECK(AnimStageAlive(h));                              // still HELD
    AnimStageStopAll();
    CHECK(AnimStageActiveCount() == 0);
    remove("anims/_test_noreplay.cfg");
    remove("anims/_test_replay.cfg");
}

// AnimStageEntry.startOnSignal: the row is spawned on load (so it is on the
// signal bus) but held DORMANT - not advanced, not drawn, and it does NOT play
// itself out - until the first matching AnimSceneEmit wakes it.
static void TestSceneStartOnSignal(void)
{
    SignalReset();
    AnimStageReset();
    WriteReplayAnim("_test_sos", false);   // 1.0s one-shot, signal "trig", no replay

    static const AnimStageEntry ENTRIES[] = {
        { .anim="_test_sos", .loop=false, .layer=0, .tag=0,
          .signals={ {"trig", false} }, .signalCount=1 },                   // auto-start
        { .anim="_test_sos", .loop=false, .layer=1, .tag=1, .startOnSignal=true,
          .signals={ {"trig", false} }, .signalCount=1 },                   // dormant
    };
    AnimStageScene sc;
    AnimScenePlay(&sc, ENTRIES, 2);

    int order[ANIM_STAGE_SLOTS_MAX];
    CHECK(AnimStageActiveCount() == 2);                             // both spawned
    CHECK(AnimStageDrawOrder(order, ANIM_STAGE_SLOTS_MAX) == 1);    // only the control drawn
    CHECK(AnimStagePlayhead(sc.handles[1]) <= 0.001f);             // dormant at u=0

    // Run past the 1.0s duration: the control one-shot plays out and deactivates,
    // the armed row is NEVER advanced so it neither plays nor self-destructs.
    for (int i = 0; i < 30; i++) AnimStageUpdate(0.05f);           // 1.5s
    CHECK(!AnimStageAlive(sc.handles[0]));                          // control gone
    CHECK(AnimStageAlive(sc.handles[1]));                          // armed still held
    CHECK(AnimStageActiveCount() == 1);
    CHECK(AnimStageDrawOrder(order, ANIM_STAGE_SLOTS_MAX) == 0);    // still not drawn
    CHECK(AnimStagePlayhead(sc.handles[1]) <= 0.001f);            // still at u=0

    // First matching emit wakes it: now drawn and advancing from the start.
    AnimSceneEmit(&sc, "trig", NULL);
    CHECK(AnimStageDrawOrder(order, ANIM_STAGE_SLOTS_MAX) == 1);    // now on screen
    for (int i = 0; i < 6; i++) AnimStageUpdate(0.05f);            // 0.3s in
    CHECK(AnimStageAlive(sc.handles[1]));
    CHECK(AnimStagePlayhead(sc.handles[1]) > 0.2f);               // timeline advancing

    AnimSceneStop(&sc);
    CHECK(AnimStageActiveCount() == 0);
    remove("anims/_test_sos.cfg");
}

// REPRO: a usesPos "ripple" emitted across three same-anim instances must start
// the signal on ALL of them, not just the first.
static void TestSceneEmitPosAll(void)
{
    SignalReset();
    AnimStageReset();

    // a doc with a usesPos ripple: center posparam + a w target so it plays
    AnimDoc doc; AnimDocInit(&doc);
    doc.duration = 5.0f; doc.introEnd = 0.0f; doc.outroStart = 5.0f;
    AnimElem *e = AnimDocAddElem(&doc, AE_SHAPE);
    TextCopy(e->name, "box");
    doc.signalCount = 1;
    AnimSignal *sg = &doc.signals[0];
    TextCopy(sg->name, "ripple");
    sg->length = 0.8f; sg->usesPos = true;
    sg->posParamCount = 1;
    sg->posParams[0] = (AnimSigPosParam){0};
    sg->posParams[0].elemIdx = 0; sg->posParams[0].slot = 1;   // center
    sg->posParams[0].keyCount = 1;
    sg->posParams[0].keys[0] = (AnimPosKey){ 1.0f, 0.0f, 0.0f, ANIM_EASE_SINE_OUT };
    sg->targetCount = 1;
    sg->targets[0] = (AnimSigTarget){0};
    sg->targets[0].elemIdx = 0; sg->targets[0].prop = AP_S_W;
    sg->targets[0].keyCount = 1;
    sg->targets[0].keys[0] = (AnimKey){ 0.4f, 0.7f, (Color){0,0,0,0}, ANIM_EASE_SINE_OUT };
    if (!DirectoryExists("anims")) MakeDirectory("anims");
    AnimDocSave(&doc, "anims/_test_ripple.cfg");

    static const AnimStageEntry ENTRIES[] = {
        { .anim="_test_ripple", .loop=true, .delay=0.0f, .layer=0, .tag=1,
          .signals={ {"ripple", true} }, .signalCount=1 },
        { .anim="_test_ripple", .loop=true, .delay=0.0f, .layer=0, .tag=2,
          .signals={ {"ripple", true} }, .signalCount=1 },
        { .anim="_test_ripple", .loop=true, .delay=0.0f, .layer=0, .tag=3,
          .signals={ {"ripple", true} }, .signalCount=1 },
    };
    AnimStageScene sc;
    AnimScenePlay(&sc, ENTRIES, 3);
    CHECK(AnimStageActiveCount() == 3);

    SignalParams p = { .pos = { 0.25f, 0.75f }, .hasPos = true };
    AnimSceneEmit(&sc, "ripple", &p);

    // every instance must now be running the ripple override
    CHECK(AnimStageSignalPlaying(sc.handles[0]));
    CHECK(AnimStageSignalPlaying(sc.handles[1]));
    CHECK(AnimStageSignalPlaying(sc.handles[2]));

    AnimSceneStop(&sc);
    remove("anims/_test_ripple.cfg");
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
    remove("anims/_test_term.cfg");
}

// ---------------------------------------------------------------------------
//  Custom easings: add/eval, name resolve, hide flags, .cfg roundtrip.
// ---------------------------------------------------------------------------
static void TestCustomEases(void)
{
    const char *path = "anim_tests_ease_tmp.cfg";
    remove(path);
    AnimCustomEasesLoad(path);              // missing file -> empty set

    // unknown name degrades to linear (the deleted-easing story).
    CHECK(AnimEaseByName("no_such_ease") == ANIM_EASE_LINEAR);

    // add: id lands in the custom range, resolves both ways.
    AnimEasePt pts[ANIM_EASE_PTS_MAX];
    AnimEasePtsFromCubic(pts, 0.25f, 0.1f, 0.25f, 1.0f);
    int id = AnimCustomEaseAdd("testEase", pts, 2);
    CHECK(id >= ANIM_EASE_COUNT);
    CHECK(AnimEaseByName("testEase") == id);
    CHECK(TextIsEqual(AnimEaseName(id), "testEase"));
    CHECK(AnimEaseIdValid(id) && !AnimEaseIdValid(id + 1));

    // duplicate and builtin names refuse.
    AnimEasePt flat[ANIM_EASE_PTS_MAX];
    AnimEasePtsFromCubic(flat, 0.0f, 0.0f, 1.0f, 1.0f);
    CHECK(AnimCustomEaseAdd("testEase", flat, 2) == -1);
    CHECK(AnimCustomEaseAdd("sineOut", flat, 2) == -1);

    // eval: endpoints exact, midpoint eased above linear for this ease-out
    // curve, monotone-ish interior stays in a sane band.
    CHECK_NEAR(AnimEaseApply(id, 0.0f), 0.0f);
    CHECK_NEAR(AnimEaseApply(id, 1.0f), 1.0f);
    float mid = AnimEaseApply(id, 0.5f);
    CHECK(mid > 0.5f && mid < 1.0f);
    // the linear-handles curve IS linear.
    AnimEasePt linPts[ANIM_EASE_PTS_MAX];
    AnimEasePtsFromCubic(linPts, 0.25f, 0.25f, 0.75f, 0.75f);
    int lin = AnimCustomEaseAdd("testLin", linPts, 2);
    CHECK(fabsf(AnimEaseApply(lin, 0.3f) - 0.3f) < 0.01f);

    // hidden easings still evaluate; flag reads back for builtins + customs.
    AnimEaseSetHidden(id, true);
    AnimEaseSetHidden(ANIM_EASE_BOUNCE_OUT, true);
    CHECK(AnimEaseIsHidden(id) && AnimEaseIsHidden(ANIM_EASE_BOUNCE_OUT));
    CHECK_NEAR(AnimEaseApply(id, 1.0f), 1.0f);

    // roundtrip: save, wipe (load nonexistent), reload -> same values + flags.
    CHECK(AnimCustomEasesSave(path));
    AnimCustomEasesLoad("no_such_file.cfg");
    CHECK(AnimEaseByName("testEase") == ANIM_EASE_LINEAR);      // wiped
    CHECK(!AnimEaseIsHidden(ANIM_EASE_BOUNCE_OUT));
    CHECK(AnimCustomEasesLoad(path));
    int rid = AnimEaseByName("testEase");
    CHECK(rid >= ANIM_EASE_COUNT);
    CHECK_NEAR(AnimEaseApply(rid, 0.5f), mid);
    CHECK(AnimEaseIsHidden(rid) && AnimEaseIsHidden(ANIM_EASE_BOUNCE_OUT));

    // a doc key saved with a custom ease survives the trip by name.
    AnimDoc doc;
    AnimDocInit(&doc);
    AnimElem *e = AnimDocAddElem(&doc, AE_SHAPE);
    AnimTrack *tr = AnimElemAddTrack(e, AP_S_POS_X);
    AnimTrackAddKey(tr, 0.0f, 0.0f, ANIM_EASE_LINEAR);
    AnimTrackAddKey(tr, 1.0f, 1.0f, rid);
    const char *dpath = "anim_tests_ease_doc_tmp.cfg";
    CHECK(AnimDocSave(&doc, dpath));
    AnimDoc back;
    CHECK(AnimDocLoad(&back, dpath));
    CHECK(back.elems[0].tracks[0].keys[1].ease == rid);

    // wipe + reload the DOC only: the custom set is gone, so the name is
    // unknown and the key degrades to linear instead of exploding.
    AnimCustomEasesLoad("no_such_file.cfg");
    CHECK(AnimDocLoad(&back, dpath));
    CHECK(back.elems[0].tracks[0].keys[1].ease == ANIM_EASE_LINEAR);

    remove(path);
    remove(dpath);
}

// ---------------------------------------------------------------------------
//  Multi-segment easings: knots, hold steps, legacy lines, roundtrip.
// ---------------------------------------------------------------------------
static void TestMultiSegmentEases(void)
{
    const char *path = "anim_tests_ease_multi_tmp.cfg";
    remove(path);
    AnimCustomEasesLoad(path);              // empty set

    // three knots: the curve passes through every knot at its own x.
    AnimEasePt pts[ANIM_EASE_PTS_MAX] = {
        { 0.0f, 0.0f,  0.0f, 0.0f,   0.1f,  0.3f },
        { 0.5f, 1.2f, -0.1f, 0.1f,   0.1f, -0.1f },
        { 1.0f, 1.0f, -0.2f, 0.05f,  0.0f,  0.0f },
    };
    int id = AnimCustomEaseAdd("multiTest", pts, 3);
    CHECK(id >= ANIM_EASE_COUNT);
    CHECK_NEAR(AnimEaseApply(id, 0.0f), 0.0f);
    CHECK_NEAR(AnimEaseApply(id, 1.0f), 1.0f);
    CHECK(fabsf(AnimEaseApply(id, 0.5f) - 1.2f) < 0.02f);   // the middle knot
    // overshoot above 1 really happens between the knots.
    CHECK(AnimEaseApply(id, 0.45f) > 1.0f);

    // knots out of order are pushed back into ascending x on store.
    AnimEasePt bad[ANIM_EASE_PTS_MAX] = {
        { 0.0f, 0.0f, 0,0, 0,0 },
        { 0.8f, 0.5f, 0,0, 0,0 },
        { 0.3f, 0.7f, 0,0, 0,0 },       // behind its neighbour
        { 1.0f, 1.0f, 0,0, 0,0 },
    };
    int bid = AnimCustomEaseAdd("multiOrder", bad, 4);
    const AnimCustomEase *bc = AnimCustomEaseGet(bid);
    CHECK(bc && bc->ptCount == 4);
    CHECK(bc->pts[2].x >= bc->pts[1].x);
    CHECK_NEAR(bc->pts[0].x, 0.0f);
    CHECK_NEAR(bc->pts[3].x, 1.0f);

    // two knots at the same x = a hold: the value jumps at the seam.
    AnimEasePt step[ANIM_EASE_PTS_MAX] = {
        { 0.0f, 0.0f, 0,0, 0,0 },
        { 0.5f, 0.0f, 0,0, 0,0 },
        { 0.5f, 1.0f, 0,0, 0,0 },
        { 1.0f, 1.0f, 0,0, 0,0 },
    };
    int sid = AnimCustomEaseAdd("multiStep", step, 4);
    CHECK(fabsf(AnimEaseApply(sid, 0.25f)) < 0.01f);
    CHECK(fabsf(AnimEaseApply(sid, 0.75f) - 1.0f) < 0.01f);

    // roundtrip: save + reload keeps the shape.
    float before = AnimEaseApply(id, 0.35f);
    CHECK(AnimCustomEasesSave(path));
    AnimCustomEasesLoad("no_such_file.cfg");
    CHECK(AnimEaseByName("multiTest") == ANIM_EASE_LINEAR);
    CHECK(AnimCustomEasesLoad(path));
    int rid = AnimEaseByName("multiTest");
    CHECK(rid >= ANIM_EASE_COUNT);
    const AnimCustomEase *rc = AnimCustomEaseGet(rid);
    CHECK(rc && rc->ptCount == 3);
    CHECK(fabsf(AnimEaseApply(rid, 0.35f) - before) < 0.001f);

    // a hand-written legacy line (4 floats) still loads as a 2-knot curve.
    FILE *f = fopen(path, "w");
    CHECK(f != NULL);
    if (f)
    {
        fprintf(f, "ease legacyTest 0.25 0.25 0.75 0.75\n");
        fclose(f);
    }
    CHECK(AnimCustomEasesLoad(path));
    int lid = AnimEaseByName("legacyTest");
    CHECK(lid >= ANIM_EASE_COUNT);
    const AnimCustomEase *lc = AnimCustomEaseGet(lid);
    CHECK(lc && lc->ptCount == 2);
    CHECK(fabsf(AnimEaseApply(lid, 0.3f) - 0.3f) < 0.01f);   // linear handles

    remove(path);
}

// ---------------------------------------------------------------------------
//  Auto-key starting a track: the t=0 key must capture the ORIGINAL pose, so
//  the edit animates instead of flattening the property to one value. This is
//  the ordering the zen editor's untracked auto-key path relies on.
// ---------------------------------------------------------------------------
static void TestAutoKeyStartsTrack(void)
{
    const float EPS = 0.02f;
    AnimDoc doc;
    AnimDocInit(&doc);
    AnimElem *e = AnimDocAddElem(&doc, AE_SHAPE);
    e->posFrac.x = 0.25f;                       // rest pose before the edit

    float playhead = 1.0f, edited = 0.80f;

    CHECK(AnimElemFindTrack(e, AP_S_POS_X) == NULL);
    AnimTrack *tr = AnimElemAddTrack(e, AP_S_POS_X);
    CHECK(tr != NULL);
    // seed t=0 BEFORE the base field moves
    if (tr->keyCount == 0)
        AnimTrackAddKey(tr, 0.0f, AnimElemProp(e, tr->prop, 0.0f), ANIM_EASE_LINEAR);
    e->posFrac.x = edited;
    AnimTrackWriteKeyAt(tr, playhead, edited, EPS);

    CHECK(tr->keyCount == 2);
    CHECK_NEAR(tr->keys[0].t, 0.0f);
    CHECK(fabsf(tr->keys[0].value - 0.25f) < 0.01f);    // original, not the edit
    CHECK(fabsf(tr->keys[1].value - 0.80f) < 0.01f);
    // and it genuinely animates in between
    float mid = AnimElemProp(e, AP_S_POS_X, 0.5f);
    CHECK(mid > 0.26f && mid < 0.79f);
}

// A text element's string is one whitespace-delimited fscanf token, so every
// space, newline and backslash has to survive as an escape. Getting this wrong
// does not fail loudly - it truncates the text at the first space on load.
static void TestTextEscapeIO(void)
{
    const char *p = "anim_tests_text.cfg";
    const char *nasty = "line one\nsecond_line has _ and \\ chars\nthird";

    AnimDoc doc;
    AnimDocInit(&doc);
    TextCopy(doc.name, "textio");
    AnimElem *t = AnimDocAddElem(&doc, AE_TEXT);
    TextCopy(t->text, nasty);
    CHECK(AnimDocSave(&doc, p));

    // the value must still be a single token: no raw whitespace after "text ".
    FILE *f = fopen(p, "rb");
    CHECK(f != NULL);
    if (f)
    {
        char line[ANIM_TEXT_LEN_MAX * 2 + 64];
        bool seen = false;
        while (fgets(line, sizeof(line), f))
        {
            const char *k = strstr(line, "text ");
            if (!k) continue;
            seen = true;
            for (const char *c = k + 5; *c && *c != '\n'; c++)
                CHECK(*c != ' ' && *c != '\t');     // would split the token
        }
        CHECK(seen);
        fclose(f);
    }

    AnimDoc back;
    CHECK(AnimDocLoad(&back, p));
    CHECK(back.elemCount == 1);
    CHECK(TextIsEqual(back.elems[0].text, nasty));  // byte-identical round trip

    // '_' is data now, not an encoded space (the pre-escape format lost this).
    AnimDocInit(&doc);
    TextCopy(doc.name, "underscore");
    AnimElem *u = AnimDocAddElem(&doc, AE_TEXT);
    TextCopy(u->text, "keep_me");
    CHECK(AnimDocSave(&doc, p));
    CHECK(AnimDocLoad(&back, p));
    CHECK(TextIsEqual(back.elems[0].text, "keep_me"));

    remove(p);
}

// ---------------------------------------------------------------------------
//  Pixel shape pool  (anim_shape_pool.c)
//
//  Textures are never touched here: baking is lazy and lives on the draw path,
//  so this runs without a GL context like the rest of the suite.
// ---------------------------------------------------------------------------
static void TestShapePool(void)
{
    const char *root = "anim_tests_shapes/";

    // -- slots ---------------------------------------------------------------
    AnimShapePoolLoadAll(NULL, NULL);                   // wipe
    CHECK(AnimShapePoolCount() == 0);

    int a = AnimShapePoolAdd("fangs_upper", 6, 4);
    CHECK(a >= 0);
    CHECK(AnimShapeIdValid(a));
    CHECK(AnimShapePoolFindByName("fangs_upper") == a);
    CHECK(AnimShapePoolFindByName("nope") == ANIM_SHAPE_MISSING);
    CHECK(AnimShapePoolAdd("fangs_upper", 4, 4) == ANIM_SHAPE_MISSING);  // dupe
    CHECK(AnimShapePoolAdd("has space", 4, 4) == ANIM_SHAPE_MISSING);
    CHECK(AnimShapePoolAdd("", 4, 4) == ANIM_SHAPE_MISSING);

    // Oversize dims clamp rather than overrun the fixed grid.
    int big = AnimShapePoolAdd("big", 9999, -3);
    CHECK(AnimShapePoolGet(big)->w == ANIM_SHAPE_GRID_MAX);
    CHECK(AnimShapePoolGet(big)->h == 1);
    AnimShapePoolDelete(big, NULL);
    CHECK(!AnimShapeIdValid(big));

    // -- pixels --------------------------------------------------------------
    AnimShapeDef *s = AnimShapePoolGet(a);
    AnimShapeSetPx(s, 0, 0, ANIM_PX_FILL);
    AnimShapeSetPx(s, 5, 3, ANIM_PX_OUTLINE);
    AnimShapeSetPx(s, 99, 99, ANIM_PX_FILL);            // out of bounds: ignored
    CHECK(AnimShapePx(s, 0, 0) == ANIM_PX_FILL);
    CHECK(AnimShapePx(s, 5, 3) == ANIM_PX_OUTLINE);
    CHECK(AnimShapePx(s, 1, 1) == ANIM_PX_EMPTY);
    CHECK(AnimShapePx(s, 99, 99) == ANIM_PX_EMPTY);     // reads clamp too

    // -- round trip ----------------------------------------------------------
    CHECK(AnimShapePoolSaveOne(a, root));
    AnimShapePoolLoadAll(NULL, root);
    int b = AnimShapePoolFindByName("fangs_upper");
    CHECK(b >= 0);
    AnimShapeDef *r = AnimShapePoolGet(b);
    CHECK(r->w == 6 && r->h == 4);
    CHECK(AnimShapePx(r, 0, 0) == ANIM_PX_FILL);
    CHECK(AnimShapePx(r, 5, 3) == ANIM_PX_OUTLINE);
    CHECK(AnimShapePx(r, 2, 2) == ANIM_PX_EMPTY);
    CHECK(!r->builtin);                                 // came from the user root

    // -- resize preserves the overlap ---------------------------------------
    AnimShapeResize(r, 3, 2);
    CHECK(r->w == 3 && r->h == 2);
    CHECK(AnimShapePx(r, 0, 0) == ANIM_PX_FILL);        // still inside
    AnimShapeResize(r, 6, 4);
    CHECK(AnimShapePx(r, 5, 3) == ANIM_PX_EMPTY);       // shrink really dropped it

    // -- unknown keys skip, per the forward-compat contract ------------------
    const char *odd = "anim_tests_shapes/odd.shp";
    FILE *f = fopen(odd, "w");
    CHECK(f != NULL);
    fprintf(f, "# a comment line\n");
    fprintf(f, "future_field 42\n");                    // unknown: skipped
    fprintf(f, "shape odd\nsize 3 2\nrows\n#.#\n.O.\nend\n");
    fclose(f);

    AnimShapePoolLoadAll(NULL, root);
    int o = AnimShapePoolFindByName("odd");
    CHECK(o >= 0);
    CHECK(AnimShapePoolGet(o)->w == 3 && AnimShapePoolGet(o)->h == 2);
    CHECK(AnimShapePx(AnimShapePoolGet(o), 0, 0) == ANIM_PX_FILL);
    CHECK(AnimShapePx(AnimShapePoolGet(o), 1, 1) == ANIM_PX_OUTLINE);
    CHECK(AnimShapePoolCount() == 2);

    // -- delete removes the file too ----------------------------------------
    CHECK(AnimShapePoolDelete(o, root));
    CHECK(!FileExists(odd));
    AnimShapePoolLoadAll(NULL, root);
    CHECK(AnimShapePoolCount() == 1);

    remove("anim_tests_shapes/fangs_upper.shp");

    // -- the pool fills at ANIM_SHAPE_POOL_MAX, wherever that is set ----------
    // The count is a BUILD-TIME knob (8 on web, 512 on desktop - see
    // anim_shape_pool.h), so this asserts the boundary rather than a number:
    // every slot up to the limit must be claimable and addressable, and the one
    // past it must fail. Guards the old hardcoded 8 from creeping back in as an
    // array bound somewhere.
    AnimShapePoolLoadAll(NULL, NULL);
    bool allAdded = true, allAddressable = true;
    for (int i = 0; i < ANIM_SHAPE_POOL_MAX; i++)
    {
        int slot = AnimShapePoolAdd(TextFormat("bulk_%d", i), 2, 2);
        if (slot == ANIM_SHAPE_MISSING) { allAdded = false; break; }
        AnimShapeSetPx(AnimShapePoolGet(slot), 1, 1, ANIM_PX_FILL);
    }
    CHECK(allAdded);
    CHECK(AnimShapePoolCount() == ANIM_SHAPE_POOL_MAX);
    for (int i = 0; i < ANIM_SHAPE_POOL_MAX; i++)
    {
        int slot = AnimShapePoolFindByName(TextFormat("bulk_%d", i));
        if (slot < 0 || AnimShapePx(AnimShapePoolGet(slot), 1, 1) != ANIM_PX_FILL)
        { allAddressable = false; break; }
    }
    CHECK(allAddressable);
    CHECK(AnimShapePoolAdd("one_too_many", 2, 2) == ANIM_SHAPE_MISSING);

    AnimShapePoolLoadAll(NULL, NULL);
}

// ---------------------------------------------------------------------------
//  Shape references survive pool renumbering
//
//  A saved AP_S_SHAPE key holds a runtime slot index, which means nothing on the
//  next run. The .cfg carries `shape_ref <idx> <name>` so the reader can put the
//  key back on the right SHAPE rather than the right NUMBER.
// ---------------------------------------------------------------------------
static void TestShapeRefIO(void)
{
    const char *path = "anim_tests_shaperef.cfg";

    AnimShapePoolLoadAll(NULL, NULL);
    int alpha = AnimShapePoolAdd("alpha", 2, 2);
    int beta  = AnimShapePoolAdd("beta",  2, 2);
    CHECK(alpha == 0 && beta == 1);                 // first two free slots

    AnimDoc doc;
    AnimDocInit(&doc);
    AnimElem *e = AnimDocAddElem(&doc, AE_SHAPE);
    e->shapeKind = SHAPE_CUSTOM;
    TextCopy(e->shapeName, "beta");
    AnimTrack *tr = AnimElemAddTrack(e, AP_S_SHAPE);
    AnimTrackAddKey(tr, 0.0f, (float)alpha, ANIM_EASE_LINEAR);
    AnimTrackAddKey(tr, 1.0f, (float)beta,  ANIM_EASE_LINEAR);
    CHECK(AnimDocSave(&doc, path));

    // Rebuild the pool with the names in the OTHER order, as a differently
    // sorted shapes/ directory would.
    AnimShapePoolLoadAll(NULL, NULL);
    int beta2  = AnimShapePoolAdd("beta",  2, 2);
    int alpha2 = AnimShapePoolAdd("alpha", 2, 2);
    CHECK(beta2 == 0 && alpha2 == 1);               // indices genuinely swapped

    AnimDoc back;
    CHECK(AnimDocLoad(&back, path));
    CHECK(back.elems[0].shapeKind == SHAPE_CUSTOM);
    CHECK(TextIsEqual(back.elems[0].shapeName, "beta"));
    AnimTrack *bt = NULL;
    for (int i = 0; i < back.elems[0].trackCount; i++)
        if (back.elems[0].tracks[i].prop == AP_S_SHAPE) bt = &back.elems[0].tracks[i];
    CHECK(bt != NULL);
    // The keys follow the NAMES, not the numbers they were saved as.
    CHECK((int)bt->keys[0].value == alpha2);
    CHECK((int)bt->keys[1].value == beta2);

    // A name the pool no longer has resolves to MISSING (placeholder), never to
    // whatever shape inherited that slot number.
    AnimShapePoolLoadAll(NULL, NULL);
    AnimShapePoolAdd("beta", 2, 2);                 // "alpha" is gone
    CHECK(AnimDocLoad(&back, path));
    bt = NULL;
    for (int i = 0; i < back.elems[0].trackCount; i++)
        if (back.elems[0].tracks[i].prop == AP_S_SHAPE) bt = &back.elems[0].tracks[i];
    CHECK(bt != NULL);
    CHECK((int)bt->keys[0].value == ANIM_SHAPE_MISSING);
    CHECK((int)bt->keys[1].value == AnimShapePoolFindByName("beta"));

    // AP_S_SHAPE is stepped: it snaps at each key rather than interpolating
    // between two pool indices, which would land on an unrelated third shape.
    CHECK(AnimPropIsStepped(AP_S_SHAPE));
    AnimTrack st = { AP_S_SHAPE, {{0}}, 0 };
    AnimTrackAddKey(&st, 0.0f, 0.0f, ANIM_EASE_LINEAR);
    AnimTrackAddKey(&st, 1.0f, 2.0f, ANIM_EASE_LINEAR);
    CHECK_NEAR(AnimTrackEval(&st, 0.5f, 0), 0.0f);  // holds, no midpoint
    CHECK_NEAR(AnimTrackEval(&st, 1.0f, 0), 2.0f);  // snaps

    // "custom" round-trips as a shape kind name.
    CHECK(TextIsEqual(AnimShapeKindName(SHAPE_CUSTOM), "custom"));
    CHECK(AnimShapeKindByName("custom") == SHAPE_CUSTOM);

    remove(path);

    // -- a saved index this build could never allocate ------------------------
    // The pool size differs per platform (8 on web, 512 on desktop), so a .cfg
    // written by a desktop build carries slot numbers a web build has no slot
    // for. Those lines must still resolve BY NAME: the index is only a handle
    // into the file's own shape_ref table, never a bound on this pool. Written
    // by hand, since no build can save an index beyond its own pool.
    const char *far = "anim_tests_shaperef_far.cfg";
    AnimShapePoolLoadAll(NULL, NULL);
    int gamma = AnimShapePoolAdd("gamma", 2, 2);
    CHECK(gamma == 0);

    FILE *cf = fopen(far, "w");
    CHECK(cf != NULL);
    fprintf(cf, "doc far 1.000000 0.000000 1.000000 0.300000 0\n");
    fprintf(cf, "elem shape blob\n");
    fprintf(cf, "  shape custom\n");
    fprintf(cf, "  shape_name gamma\n");
    fprintf(cf, "  shape_ref %d gamma\n", ANIM_SHAPE_REF_MAX - 1);
    fprintf(cf, "  track shape_id 1\n");
    fprintf(cf, "    key 0.000000 %d linear\n", ANIM_SHAPE_REF_MAX - 1);
    fprintf(cf, "  end\n");
    fclose(cf);

    AnimDoc farDoc;
    CHECK(AnimDocLoad(&farDoc, far));
    CHECK(farDoc.elemCount == 1);
    AnimTrack *ft = NULL;
    for (int i = 0; i < farDoc.elems[0].trackCount; i++)
        if (farDoc.elems[0].tracks[i].prop == AP_S_SHAPE) ft = &farDoc.elems[0].tracks[i];
    CHECK(ft != NULL);
    CHECK((int)ft->keys[0].value == gamma);         // resolved by NAME, not index
    remove(far);

    // Past ANIM_SHAPE_REF_MAX there is no table entry to hold the name, so the
    // key resolves to MISSING (the placeholder) instead of reading off the end.
    cf = fopen(far, "w");
    CHECK(cf != NULL);
    fprintf(cf, "doc far 1.000000 0.000000 1.000000 0.300000 0\n");
    fprintf(cf, "elem shape blob\n");
    fprintf(cf, "  shape custom\n");
    fprintf(cf, "  shape_name gamma\n");
    fprintf(cf, "  shape_ref %d gamma\n", ANIM_SHAPE_REF_MAX);
    fprintf(cf, "  track shape_id 1\n");
    fprintf(cf, "    key 0.000000 %d linear\n", ANIM_SHAPE_REF_MAX);
    fprintf(cf, "  end\n");
    fclose(cf);

    CHECK(AnimDocLoad(&farDoc, far));
    ft = NULL;
    for (int i = 0; i < farDoc.elems[0].trackCount; i++)
        if (farDoc.elems[0].tracks[i].prop == AP_S_SHAPE) ft = &farDoc.elems[0].tracks[i];
    CHECK(ft != NULL);
    CHECK((int)ft->keys[0].value == ANIM_SHAPE_MISSING);

    remove(far);
    AnimShapePoolLoadAll(NULL, NULL);
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
    TestCloneElemState();
    TestClonePreservesBase();
    TestCloneText();
    TestStringIdxAt();
    TestStringKeysStep();
    TestStringPool();
    TestStringPoolIO();
    TestIO();
    TestIOOldFormat();
    TestAuthoringFlags();
    TestLoopBlend();
    TestLoopBlendIO();
    TestSignalSeqStep();
    TestSignalSeqStepIO();
    TestGroupCoverage();
    TestTrim();
    TestPauseMarkers();
    TestPlayerSeek();
    TestIOIdempotent();
    TestIOCrumbleKeys();
    TestIOCrumbleRestPoseFill();
    TestIOSignalOrphanTarget();
    TestIOSignalGroupTargets();
    TestLibrary();
    TestSignals();
    TestSignalTargetRemap();
    TestSignalTerminalIO();
    TestStage();
    TestStagePause();
    TestSignalPosParam();
    TestSignalPosCorner();
    TestSignalPosAnchor();
    TestSignalEmitParam();
    TestScene();
    TestSceneEmitAll();
    TestStageReplay();
    TestSceneStartOnSignal();
    TestSceneEmitPosAll();
    TestSceneTerminal();
    TestCustomEases();
    TestMultiSegmentEases();
    TestAutoKeyStartsTrack();
    TestTextEscapeIO();
    TestShapePool();
    TestShapeRefIO();

    printf("anim_tests: %d checks, %d failed\n", s_checks, s_fails);
    return s_fails ? 1 : 0;
}
