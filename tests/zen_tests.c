// ============================================================================
//  zen_tests.c  -  headless checks for the zen editor's group/key editing
//
//  Companion to tests/archive/anim_tests_archived.c (the frozen legacy suite -
//  build and RUN it, but do not open it: it is 3k lines and only burns context).
//  New checks belong HERE, and this file is meant to stay small.
//
//  No window is opened. zen_groups.c is linked in directly; the handful of UI
//  symbols it references are provided below, so the binary runs over ssh / CI.
//  Build: the `zen_tests` cmake target (desktop only). Exit code 0 = all pass.
// ============================================================================

#include "raylib.h"
#include "../src/anim/anim.h"
#include "../src/anim/anim_io.h"
#include "../src/anim_editor_zen/zen_internal.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

// anim.c needs this for AnimDocDraw; the tests never draw, a fixed size is fine.
Vector2 ScreenStateTargetSize(void) { return (Vector2){ 1280, 720 }; }

// ---------------------------------------------------------------------------
//  Editor symbols zen_groups.c links against. The two ZenEnsureZero* helpers
//  are copied verbatim from zen_widgets.c (they are four lines each and
//  ZenGroupWriteKey's behaviour depends on them); the rest are pure UI and are
//  no-ops here.
// ---------------------------------------------------------------------------
ZenCtx zen;

void ZenEnsureZeroKey(AnimElem *e, AnimTrack *tr)
{
    if (tr->keyCount == 0)
        AnimTrackAddKey(tr, 0.0f, AnimElemProp(e, tr->prop, 0.0f), ANIM_EASE_LINEAR);
}
void ZenEnsureZeroColorKey(AnimElem *e, AnimTrack *tr)
{
    if (tr->keyCount == 0)
        AnimTrackAddColorKey(tr, 0.0f, AnimElemColorProp(e, tr->prop, 0.0f),
                             ANIM_EASE_LINEAR);
}
void ZenMouseReflow(void) {}
void ZenTrackModalOpen(int elem, int gi) { (void)elem; (void)gi; }
void ZenTrackModalSync(void) {}
const char *ZenTextPreview(const char *s, int maxChars) { (void)maxChars; return s; }

static int s_checks = 0, s_fails = 0;
#define CHECK(cond) do { \
    s_checks++; \
    if (!(cond)) { s_fails++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

#define CHECK_NEAR(a, b) CHECK(fabsf((a) - (b)) < 0.0001f)

// ---------------------------------------------------------------------------
//  Helpers mirroring what the timeline's set-drag does in DrawTimeline().
// ---------------------------------------------------------------------------

// A one-shot shift: keys are still at their snapshot times, so cur == base.
static void ShiftSet(AnimElem *e, int gi, const float *orig, int n, float delta)
{
    ZenGroupMoveKeySetTo(e, gi, orig, orig, n, delta);
}

// A whole drag gesture: `frames` successive deltas off the SAME press-time
// snapshot, exactly as DrawTimeline re-applies it every frame. `cur` tracks
// where the keys actually are, which is what the lookup must match.
static void DragSet(AnimElem *e, int gi, const float *orig, int n,
                    const float *deltas, int frames)
{
    float cur[ZEN_GROUP_TIMES_MAX];
    for (int i = 0; i < n; i++) cur[i] = orig[i];
    for (int f = 0; f < frames; f++)
    {
        ZenGroupMoveKeySetTo(e, gi, cur, orig, n, deltas[f]);
        for (int i = 0; i < n; i++) cur[i] = orig[i] + deltas[f];
    }
}

// Edge clamp: block the whole set at 0 / dur, never squash its spacing.
static float ClampDelta(const float *orig, int n, float delta, float dur)
{
    float lo = orig[0], hi = orig[0];
    for (int i = 1; i < n; i++)
    {
        if (orig[i] < lo) lo = orig[i];
        if (orig[i] > hi) hi = orig[i];
    }
    if (delta < -lo) delta = -lo;
    if (delta > dur - hi) delta = dur - hi;
    return delta;
}

// The group a property belongs to. Group indices are per-kind and not stable,
// so every test looks its lane up from the prop it wrote rather than hardcoding.
static int GroupOfProp(AnimElem *e, int prop)
{
    for (int i = 0, n = AnimGroupCountFor(e->kind); i < n; i++)
    {
        const AnimPropGroup *g = AnimGroupAt(e->kind, i);
        for (int m = 0; g && m < g->propCount; m++)
            if (g->props[m] == prop) return i;
    }
    return -1;
}

static AnimElem *MakeElem(AnimDoc *doc, const float *times, int n, int prop)
{
    memset(doc, 0, sizeof(*doc));
    doc->duration = 10.0f;
    doc->elemCount = 1;
    AnimElem *e = &doc->elems[0];
    e->kind = AE_TEXT;
    strcpy(e->name, "e");
    AnimTrack *tr = AnimElemAddTrack(e, prop);
    for (int i = 0; i < n; i++) AnimTrackAddKey(tr, times[i], (float)i, ANIM_EASE_LINEAR);
    return e;
}

// ---------------------------------------------------------------------------
//  Multi-key drag: rigid shift of a selected set
// ---------------------------------------------------------------------------
static void TestSetShiftRight(void)
{
    float orig[] = { 1.0f, 2.0f, 3.5f };
    AnimDoc doc; AnimElem *e = MakeElem(&doc, orig, 3, AP_T_ALPHA);
    int gi = GroupOfProp(e, AP_T_ALPHA);
    CHECK(gi >= 0);

    ShiftSet(e, gi, orig, 3, 1.25f);

    float t[ZEN_GROUP_TIMES_MAX];
    int n = ZenGroupKeyTimes(e, gi, t);
    CHECK(n == 3);
    CHECK_NEAR(t[0], 2.25f);
    CHECK_NEAR(t[1], 3.25f);
    CHECK_NEAR(t[2], 4.75f);

    // the underlying track stays sorted and keeps its key count
    AnimTrack *tr = AnimElemFindTrack(e, AP_T_ALPHA);
    CHECK(tr->keyCount == 3);
    for (int i = 1; i < tr->keyCount; i++) CHECK(tr->keys[i-1].t <= tr->keys[i].t);
}

static void TestSetShiftLeft(void)
{
    float orig[] = { 2.0f, 3.0f, 4.5f };
    AnimDoc doc; AnimElem *e = MakeElem(&doc, orig, 3, AP_T_ALPHA);
    int gi = GroupOfProp(e, AP_T_ALPHA);

    ShiftSet(e, gi, orig, 3, -1.5f);

    float t[ZEN_GROUP_TIMES_MAX];
    int n = ZenGroupKeyTimes(e, gi, t);
    CHECK(n == 3);
    CHECK_NEAR(t[0], 0.5f);
    CHECK_NEAR(t[1], 1.5f);
    CHECK_NEAR(t[2], 3.0f);
}

// Spacing must survive any shift - that is the whole point of a rigid move.
static void TestSetShiftPreservesGaps(void)
{
    float orig[] = { 1.0f, 1.4f, 3.0f };
    AnimDoc doc; AnimElem *e = MakeElem(&doc, orig, 3, AP_T_ALPHA);
    int gi = GroupOfProp(e, AP_T_ALPHA);

    ShiftSet(e, gi, orig, 3, 2.0f);

    float t[ZEN_GROUP_TIMES_MAX];
    ZenGroupKeyTimes(e, gi, t);
    CHECK_NEAR(t[1] - t[0], orig[1] - orig[0]);
    CHECK_NEAR(t[2] - t[1], orig[2] - orig[1]);
}

// ---------------------------------------------------------------------------
//  Edge clamping
// ---------------------------------------------------------------------------
static void TestClampDeltaAtZero(void)
{
    float orig[] = { 1.0f, 2.0f, 3.5f };
    // dragging 5s to the left can only travel 1s: the leading key blocks at 0
    CHECK_NEAR(ClampDelta(orig, 3, -5.0f, 10.0f), -1.0f);
    // an unobstructed drag is untouched
    CHECK_NEAR(ClampDelta(orig, 3, -0.5f, 10.0f), -0.5f);
}

static void TestClampDeltaAtDuration(void)
{
    float orig[] = { 1.0f, 2.0f, 3.5f };
    // trailing key is at 3.5, duration 10 -> at most 6.5s of travel
    CHECK_NEAR(ClampDelta(orig, 3, 99.0f, 10.0f), 6.5f);
    CHECK_NEAR(ClampDelta(orig, 3, 2.0f, 10.0f), 2.0f);
}

// Clamped drags stay in range AND keep their spacing (the failure mode we are
// guarding against is keys piling up on the boundary).
static void TestClampedShiftKeepsSpacing(void)
{
    float orig[] = { 1.0f, 2.0f, 3.5f };
    AnimDoc doc; AnimElem *e = MakeElem(&doc, orig, 3, AP_T_ALPHA);
    int gi = GroupOfProp(e, AP_T_ALPHA);

    float d = ClampDelta(orig, 3, -5.0f, doc.duration);
    ShiftSet(e, gi, orig, 3, d);

    float t[ZEN_GROUP_TIMES_MAX];
    int n = ZenGroupKeyTimes(e, gi, t);
    CHECK(n == 3);
    CHECK_NEAR(t[0], 0.0f);
    CHECK_NEAR(t[1], 1.0f);
    CHECK_NEAR(t[2], 2.5f);
    for (int i = 0; i < n; i++) CHECK(t[i] >= 0.0f && t[i] <= doc.duration);
}

// ---------------------------------------------------------------------------
//  Collisions: shifting a set onto a key that is NOT part of it
// ---------------------------------------------------------------------------
static void TestSetShiftOntoStationaryKey(void)
{
    // keys at 1, 2 and a stationary one at 4; shift {1,2} right by 2 so that
    // the trailing member lands exactly on 4.
    float all[] = { 1.0f, 2.0f, 4.0f };
    AnimDoc doc; AnimElem *e = MakeElem(&doc, all, 3, AP_T_ALPHA);
    int gi = GroupOfProp(e, AP_T_ALPHA);

    float sel[] = { 1.0f, 2.0f };
    ShiftSet(e, gi, sel, 2, 2.0f);

    // Two keys now share t=4. The group view dedups them at ZEN_AUTOKEY_EPS,
    // so the timeline shows 2 diamonds (3 and 4) rather than a duplicate.
    float t[ZEN_GROUP_TIMES_MAX];
    int n = ZenGroupKeyTimes(e, gi, t);
    CHECK(n == 2);
    CHECK_NEAR(t[0], 3.0f);
    CHECK_NEAR(t[1], 4.0f);

    // the track itself is still sorted and has not lost or gained keys
    AnimTrack *tr = AnimElemFindTrack(e, AP_T_ALPHA);
    CHECK(tr->keyCount == 3);
    for (int i = 1; i < tr->keyCount; i++) CHECK(tr->keys[i-1].t <= tr->keys[i].t);
}

// Shifting {1,2} right by 1 makes the first member land on the second member's
// ORIGINAL time. Resolving indices before writing keeps that unambiguous.
static void TestSetShiftOntoOwnMember(void)
{
    float orig[] = { 1.0f, 2.0f };
    AnimDoc doc; AnimElem *e = MakeElem(&doc, orig, 2, AP_T_ALPHA);
    int gi = GroupOfProp(e, AP_T_ALPHA);

    ShiftSet(e, gi, orig, 2, 1.0f);

    float t[ZEN_GROUP_TIMES_MAX];
    int n = ZenGroupKeyTimes(e, gi, t);
    CHECK(n == 2);
    CHECK_NEAR(t[0], 2.0f);
    CHECK_NEAR(t[1], 3.0f);
}

// Same, mirrored: shifting left onto a member's own original time.
static void TestSetShiftLeftOntoOwnMember(void)
{
    float orig[] = { 2.0f, 3.0f };
    AnimDoc doc; AnimElem *e = MakeElem(&doc, orig, 2, AP_T_ALPHA);
    int gi = GroupOfProp(e, AP_T_ALPHA);

    ShiftSet(e, gi, orig, 2, -1.0f);

    float t[ZEN_GROUP_TIMES_MAX];
    int n = ZenGroupKeyTimes(e, gi, t);
    CHECK(n == 2);
    CHECK_NEAR(t[0], 1.0f);
    CHECK_NEAR(t[1], 2.0f);
}

// A real drag re-applies the shift from the snapshot EVERY frame, with a
// growing delta. Sliding the set across an unselected key must not make a
// member "stick" to it - the regression that made a drag visibly release the
// selection partway through. Guards the index-resolved atomic pass.
static void TestDragFramesAcrossStationaryKey(void)
{
    float all[] = { 1.0f, 2.0f, 5.0f };     // 5.0 is NOT selected
    AnimDoc doc; AnimElem *e = MakeElem(&doc, all, 3, AP_T_ALPHA);
    int gi = GroupOfProp(e, AP_T_ALPHA);

    float sel[] = { 1.0f, 2.0f };
    // sweep the pair right, straight over the stationary key at 5.0
    float deltas[40];
    for (int f = 0; f < 40; f++) deltas[f] = 0.1f * (float)(f + 1);
    DragSet(e, gi, sel, 2, deltas, 40);

    // the pair ends at 5 and 6; the stationary key is still at 5.
    AnimTrack *tr = AnimElemFindTrack(e, AP_T_ALPHA);
    CHECK(tr->keyCount == 3);
    float t[ZEN_GROUP_TIMES_MAX];
    int n = ZenGroupKeyTimes(e, gi, t);
    CHECK(n == 2);
    CHECK_NEAR(t[0], 5.0f);
    CHECK_NEAR(t[1], 6.0f);
}

// Same sweep, moving left across a stationary key.
static void TestDragFramesLeftAcrossStationaryKey(void)
{
    float all[] = { 1.0f, 5.0f, 6.0f };     // 1.0 is NOT selected
    AnimDoc doc; AnimElem *e = MakeElem(&doc, all, 3, AP_T_ALPHA);
    int gi = GroupOfProp(e, AP_T_ALPHA);

    float sel[] = { 5.0f, 6.0f };
    float deltas[40];
    for (int f = 0; f < 40; f++) deltas[f] = -0.1f * (float)(f + 1);
    DragSet(e, gi, sel, 2, deltas, 40);

    AnimTrack *tr = AnimElemFindTrack(e, AP_T_ALPHA);
    CHECK(tr->keyCount == 3);
    float t[ZEN_GROUP_TIMES_MAX];
    int n = ZenGroupKeyTimes(e, gi, t);
    CHECK(n == 2);
    CHECK_NEAR(t[0], 1.0f);
    CHECK_NEAR(t[1], 2.0f);
}

// ---------------------------------------------------------------------------
//  Multi-prop groups move in lockstep (a group key is the union of its members)
// ---------------------------------------------------------------------------
static void TestSetShiftMovesEveryMember(void)
{
    AnimDoc doc;
    memset(&doc, 0, sizeof(doc));
    doc.duration = 10.0f;
    doc.elemCount = 1;
    AnimElem *e = &doc.elems[0];
    e->kind = AE_TEXT;
    strcpy(e->name, "e");

    int gi = GroupOfProp(e, AP_T_POS_X);
    CHECK(gi >= 0);
    const AnimPropGroup *g = AnimGroupAt(e->kind, gi);
    CHECK(g && g->propCount >= 2);          // Position is x+y

    float orig[] = { 1.0f, 2.0f };
    for (int m = 0; m < g->propCount; m++)
    {
        AnimTrack *tr = AnimElemAddTrack(e, g->props[m]);
        for (int i = 0; i < 2; i++)
            AnimTrackAddKey(tr, orig[i], (float)i, ANIM_EASE_LINEAR);
    }

    ShiftSet(e, gi, orig, 2, 1.5f);

    // every member track, not just the first, followed the shift
    for (int m = 0; m < g->propCount; m++)
    {
        AnimTrack *tr = AnimElemFindTrack(e, g->props[m]);
        CHECK(tr && tr->keyCount == 2);
        CHECK_NEAR(tr->keys[0].t, 2.5f);
        CHECK_NEAR(tr->keys[1].t, 3.5f);
    }
}

// A shift must not disturb the values or eases it carries.
static void TestSetShiftKeepsValues(void)
{
    float orig[] = { 1.0f, 2.0f, 3.0f };
    AnimDoc doc; AnimElem *e = MakeElem(&doc, orig, 3, AP_T_ALPHA);
    int gi = GroupOfProp(e, AP_T_ALPHA);

    AnimTrack *tr = AnimElemFindTrack(e, AP_T_ALPHA);
    tr->keys[1].ease = ANIM_EASE_QUAD_INOUT;

    ShiftSet(e, gi, orig, 3, 0.75f);

    CHECK_NEAR(tr->keys[0].value, 0.0f);
    CHECK_NEAR(tr->keys[1].value, 1.0f);
    CHECK_NEAR(tr->keys[2].value, 2.0f);
    CHECK(tr->keys[1].ease == ANIM_EASE_QUAD_INOUT);
}

// A single-key "set" is just the ordinary drag; it must not regress.
static void TestSingleKeySetIsPlainMove(void)
{
    float orig[] = { 2.0f };
    AnimDoc doc; AnimElem *e = MakeElem(&doc, orig, 1, AP_T_ALPHA);
    int gi = GroupOfProp(e, AP_T_ALPHA);

    ShiftSet(e, gi, orig, 1, 1.0f);

    float t[ZEN_GROUP_TIMES_MAX];
    int n = ZenGroupKeyTimes(e, gi, t);
    CHECK(n == 1);
    CHECK_NEAR(t[0], 3.0f);
}

// ---------------------------------------------------------------------------
//  Multi-key delete / clone (the timeline right-click menu's set scope)
// ---------------------------------------------------------------------------
static void TestDeleteSetRemovesOnlySelected(void)
{
    float orig[] = { 1.0f, 2.0f, 3.0f, 4.0f };
    AnimDoc doc; AnimElem *e = MakeElem(&doc, orig, 4, AP_T_ALPHA);
    int gi = GroupOfProp(e, AP_T_ALPHA);

    float sel[] = { 2.0f, 4.0f };
    ZenGroupDeleteKeySet(e, gi, sel, 2);

    float t[ZEN_GROUP_TIMES_MAX];
    int n = ZenGroupKeyTimes(e, gi, t);
    CHECK(n == 2);
    CHECK_NEAR(t[0], 1.0f);
    CHECK_NEAR(t[1], 3.0f);
}

// The removals shift the array; passing times in any order must still land on
// the keys the user picked, never on their neighbours.
static void TestDeleteSetUnorderedTimes(void)
{
    float orig[] = { 1.0f, 2.0f, 3.0f, 4.0f, 5.0f };
    AnimDoc doc; AnimElem *e = MakeElem(&doc, orig, 5, AP_T_ALPHA);
    int gi = GroupOfProp(e, AP_T_ALPHA);

    float sel[] = { 5.0f, 1.0f, 3.0f };
    ZenGroupDeleteKeySet(e, gi, sel, 3);

    float t[ZEN_GROUP_TIMES_MAX];
    int n = ZenGroupKeyTimes(e, gi, t);
    CHECK(n == 2);
    CHECK_NEAR(t[0], 2.0f);
    CHECK_NEAR(t[1], 4.0f);
}

// The earliest selected key lands on the playhead; the rest keep their gaps.
static void TestCloneSetAnchorsEarliestAtDest(void)
{
    float orig[] = { 1.0f, 1.5f, 3.0f };
    AnimDoc doc; AnimElem *e = MakeElem(&doc, orig, 3, AP_T_ALPHA);
    int gi = GroupOfProp(e, AP_T_ALPHA);

    int done = ZenGroupCloneKeySet(e, gi, orig, 3, 5.0f);
    CHECK(done == 3);

    float t[ZEN_GROUP_TIMES_MAX];
    int n = ZenGroupKeyTimes(e, gi, t);
    CHECK(n == 6);
    CHECK_NEAR(t[3], 5.0f);
    CHECK_NEAR(t[4], 5.5f);
    CHECK_NEAR(t[5], 7.0f);
}

// Order of the selection array is the user's click order, not time order: the
// anchor is the earliest TIME either way.
static void TestCloneSetUnorderedAnchor(void)
{
    float sel[] = { 3.0f, 1.0f, 1.5f };
    AnimDoc doc; AnimElem *e = MakeElem(&doc, sel, 3, AP_T_ALPHA);
    int gi = GroupOfProp(e, AP_T_ALPHA);

    CHECK(ZenGroupCloneKeySet(e, gi, sel, 3, 5.0f) == 3);

    float t[ZEN_GROUP_TIMES_MAX];
    int n = ZenGroupKeyTimes(e, gi, t);
    CHECK(n == 6);
    CHECK_NEAR(t[3], 5.0f);
    CHECK_NEAR(t[4], 5.5f);
    CHECK_NEAR(t[5], 7.0f);
}

// A clone is verbatim: values and easing come from the source keys, and the
// sources are read from a snapshot so earlier writes cannot be re-read as
// later sources.
static void TestCloneSetCopiesValuesAndEase(void)
{
    float orig[] = { 1.0f, 2.0f };
    AnimDoc doc; AnimElem *e = MakeElem(&doc, orig, 2, AP_T_ALPHA);
    int gi = GroupOfProp(e, AP_T_ALPHA);

    AnimTrack *tr = AnimElemFindTrack(e, AP_T_ALPHA);
    tr->keys[0].value = 0.25f; tr->keys[0].ease = ANIM_EASE_QUAD_IN;
    tr->keys[1].value = 0.75f; tr->keys[1].ease = ANIM_EASE_QUAD_OUT;

    CHECK(ZenGroupCloneKeySet(e, gi, orig, 2, 6.0f) == 2);

    CHECK(tr->keyCount == 4);
    CHECK_NEAR(tr->keys[2].t, 6.0f);
    CHECK_NEAR(tr->keys[2].value, 0.25f);
    CHECK(tr->keys[2].ease == ANIM_EASE_QUAD_IN);
    CHECK_NEAR(tr->keys[3].t, 7.0f);
    CHECK_NEAR(tr->keys[3].value, 0.75f);
    CHECK(tr->keys[3].ease == ANIM_EASE_QUAD_OUT);
}

// Cloning a set backwards, onto ground it already overlaps: the copies
// overwrite whatever sat at their landing times rather than doubling up.
static void TestCloneSetOverlappingDest(void)
{
    float orig[] = { 3.0f, 4.0f };
    AnimDoc doc; AnimElem *e = MakeElem(&doc, orig, 2, AP_T_ALPHA);
    int gi = GroupOfProp(e, AP_T_ALPHA);
    AnimTrack *tr = AnimElemFindTrack(e, AP_T_ALPHA);
    tr->keys[0].value = 0.5f;

    CHECK(ZenGroupCloneKeySet(e, gi, orig, 2, 4.0f) == 2);

    float t[ZEN_GROUP_TIMES_MAX];
    int n = ZenGroupKeyTimes(e, gi, t);
    CHECK(n == 3);
    CHECK_NEAR(t[0], 3.0f);
    CHECK_NEAR(t[1], 4.0f);
    CHECK_NEAR(t[2], 5.0f);
    CHECK_NEAR(tr->keys[1].value, 0.5f);   // key at 4 replaced by the clone of 3
}

// The anchor already on the destination is a no-op, matching the single-key
// clone's "playhead is already on this key" guard.
static void TestCloneSetAtOwnAnchorIsNoop(void)
{
    float orig[] = { 2.0f, 3.0f };
    AnimDoc doc; AnimElem *e = MakeElem(&doc, orig, 2, AP_T_ALPHA);
    int gi = GroupOfProp(e, AP_T_ALPHA);

    CHECK(ZenGroupCloneKeySet(e, gi, orig, 2, 2.0f) == 0);

    float t[ZEN_GROUP_TIMES_MAX];
    CHECK(ZenGroupKeyTimes(e, gi, t) == 2);
}

// ---------------------------------------------------------------------------
//  Load truncation reporting (anim_io.c).
//
//  The anim capacities are a TWO-TIER build setting: CMake raises them off Web,
//  so a desktop-authored .cfg can exceed what a web build holds. AnimDocLoad
//  still returns true there and drops the overflow, and the editor needs to
//  know in order to say so instead of showing a mangled document.
//
//  This binary IS a desktop build, so it cannot overflow its own caps from a
//  file it wrote. Instead it writes a .cfg with MORE elements than this build
//  allows and checks the report - which exercises the same code path a web
//  build hits on a desktop-authored file.
// ---------------------------------------------------------------------------
static void WriteOverCapDoc(const char *path, int elems)
{
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "doc trunctest 5.0 0.0 5.0 0.300000 1\n");
    for (int i = 0; i < elems; i++)
        fprintf(f, "elem text e%d\n  text hi\n  track alpha 1\n"
                   "  key 0.0 1.0 linear\nend\n", i);
    fclose(f);
}

static void TestLoadTruncationReported(void)
{
    const char *path = "zen_trunc_test.cfg";
    // Two elements past whatever this build allows: the count must be exact,
    // not merely nonzero, or a miscount would still "report truncation".
    WriteOverCapDoc(path, ANIM_ELEMS_MAX + 2);

    AnimDoc doc;
    CHECK(AnimDocLoad(&doc, path));            // over-cap is NOT a load failure
    CHECK(doc.elemCount == ANIM_ELEMS_MAX);    // filled to the brim
    CHECK(AnimDocLoadTruncated());
    CHECK(AnimDocLoadTrunc()->elems == 2);
    CHECK(AnimDocLoadTrunc()->keys == 0);      // the kept elements' keys all fit

    char msg[256];
    int n = AnimDocLoadTruncMessage(msg, (int)sizeof msg);
    CHECK(n > 0);
    CHECK(strstr(msg, "2 elements") != NULL);

    remove(path);
}

// A document that FITS must report nothing - otherwise the editor would raise
// the "too large for this build" notice on every ordinary open.
static void TestFittingLoadReportsNothing(void)
{
    const char *path = "zen_trunc_fit.cfg";
    WriteOverCapDoc(path, 2);

    AnimDoc doc;
    CHECK(AnimDocLoad(&doc, path));
    CHECK(doc.elemCount == 2);
    CHECK(!AnimDocLoadTruncated());

    char msg[256];
    CHECK(AnimDocLoadTruncMessage(msg, (int)sizeof msg) == 0);
    CHECK(msg[0] == '\0');

    remove(path);
}

// The counters describe the LAST load only - the editor reads them right after
// one, and an export's scratch reads must not leave a stale notice behind.
static void TestTruncationResetsPerLoad(void)
{
    const char *big = "zen_trunc_big.cfg", *small = "zen_trunc_small.cfg";
    WriteOverCapDoc(big, ANIM_ELEMS_MAX + 2);
    WriteOverCapDoc(small, 1);

    AnimDoc doc;
    CHECK(AnimDocLoad(&doc, big));
    CHECK(AnimDocLoadTruncated());
    CHECK(AnimDocLoad(&doc, small));
    CHECK(!AnimDocLoadTruncated());            // reset, not accumulated

    remove(big); remove(small);
}

int main(void)
{
    TestSetShiftRight();
    TestSetShiftLeft();
    TestSetShiftPreservesGaps();
    TestClampDeltaAtZero();
    TestClampDeltaAtDuration();
    TestClampedShiftKeepsSpacing();
    TestSetShiftOntoStationaryKey();
    TestSetShiftOntoOwnMember();
    TestSetShiftLeftOntoOwnMember();
    TestDragFramesAcrossStationaryKey();
    TestDragFramesLeftAcrossStationaryKey();
    TestSetShiftMovesEveryMember();
    TestSetShiftKeepsValues();
    TestSingleKeySetIsPlainMove();
    TestDeleteSetRemovesOnlySelected();
    TestDeleteSetUnorderedTimes();
    TestCloneSetAnchorsEarliestAtDest();
    TestCloneSetUnorderedAnchor();
    TestCloneSetCopiesValuesAndEase();
    TestCloneSetOverlappingDest();
    TestCloneSetAtOwnAnchorIsNoop();
    TestLoadTruncationReported();
    TestFittingLoadReportsNothing();
    TestTruncationResetsPerLoad();

    printf("zen_tests: %d checks, %d failed\n", s_checks, s_fails);
    return s_fails ? 1 : 0;
}
