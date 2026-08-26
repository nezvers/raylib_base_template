// ============================================================================
//  sga_tests.c  -  headless checks for authored strategy assets
//
//  Covers the two things that are expensive to get wrong and invisible when
//  they do go wrong: the BINARY FORMAT (a file that loads as garbage looks like
//  a modelling mistake, not an IO bug) and the STANDALONE GUARANTEE (a baked
//  easing must survive the global easing set changing under it).
//
//  Headless: no window, no GL. strategy_asset.c calls into rlgl only from its
//  draw functions, which nothing here invokes, so raylib links without ever
//  being initialised.
//
//  simple_save.h is header-only and its IMPLEMENTATION normally lives in
//  examples/simple_save_example.c, which this binary does not link - so the
//  suite compiles it here instead. This is the ONE place outside that example
//  that may define it.
// ============================================================================

#define SIMPLE_SAVE_IMPLEMENTATION
#include "simple_save.h"

#include "../src/strategy_asset/strategy_asset.h"
#include "../src/strategy_asset/strategy_asset_io.h"

#include <stdio.h>
#include <math.h>
#include <string.h>

static int s_checks = 0, s_fails = 0;

#define CHECK(cond) do { \
    s_checks++; \
    if (!(cond)) { s_fails++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

#define CHECK_NEAR(a, b) CHECK(fabsf((a) - (b)) < 0.0001f)

// Every test writes here so a crashed run leaves nothing behind in the real
// asset directory.
#define TEST_PATH  "assets_strategy/_sgatest.sga"

// ---------------------------------------------------------------------------
//  A non-trivial asset: every field set to something that is NOT its default,
//  so a field the writer forgets shows up as a mismatch rather than passing by
//  coincidence.
// ---------------------------------------------------------------------------
static void BuildSample(SgaAsset *a)
{
    StrategyAssetInit(a, "sample");
    TextCopy(a->subtype, "soldier");
    a->category = SGA_RESOURCE;

    a->partCount = 3;

    SgaPart *body = &a->parts[0];
    body->kind = SGA_CYLINDER;
    body->visible = 1;
    body->offset = (Vector3){ 0.1f, 0.2f, 0.3f };
    body->size = (Vector3){ 1.1f, 1.2f, 1.3f };
    body->r0 = 0.34f; body->r1 = 0.45f; body->h = 1.1f;
    body->sides = 7;
    body->tintMode = SGA_TINT_PARTIAL;
    body->tintAmount = 0.4f;
    body->brightness = -0.25f;
    body->color = (Color){ 11, 22, 33, 200 };
    TextCopy(body->name, "body");

    SgaPart *head = &a->parts[1];
    head->kind = SGA_SPHERE;
    head->visible = 0;                  // deliberately hidden
    head->r0 = 0.2f;
    head->offset = (Vector3){ 0.0f, 1.28f, 0.0f };
    head->sides = 8;
    head->tintMode = SGA_TINT_FULL;
    head->color = (Color){ 1, 2, 3, 255 };
    TextCopy(head->name, "head");

    SgaPart *path = &a->parts[2];
    path->kind = SGA_PATH;
    path->visible = 1;
    path->sides = 8;
    path->path.center = (Vector3){ 0.0f, 1.0f, 0.0f };
    path->path.radiusX = 0.5f;
    path->path.radiusZ = 0.25f;
    path->path.squareness = 0.6f;
    path->path.rotation = (Vector3){ 10.0f, 20.0f, 30.0f };
    TextCopy(path->name, "orbit");

    // One baked custom curve.
    a->easeCount = 1;
    TextCopy(a->eases[0].name, "bop_n_up");
    a->eases[0].ptCount = 3;
    a->eases[0].pts[0] = (SgaEasePt){ 0.0f, 0.0f, 0.0f, 0.0f, 0.07f, 1.5f };
    a->eases[0].pts[1] = (SgaEasePt){ 0.28f, 0.6f, -0.16f, 1.27f, 0.16f, -1.27f };
    a->eases[0].pts[2] = (SgaEasePt){ 1.0f, 1.0f, -0.25f, -0.25f, 0.0f, 0.0f };

    // Body bobs along the path during MOVING.
    SgaPartAnim *an = &body->anim[SGA_STATE_MOVING];
    an->pathPart = 2;
    an->keyCount = 2;
    an->keys[0] = (SgaKey){ 0.0f, 0.0f, { 0, 0, 0 }, { 1, 1, 1 }, -1 };
    an->keys[1] = (SgaKey){ 0.5f, 1.0f, { 0, 90.0f, 0 }, { 1, 2.0f, 1 }, 0 };
    a->duration[SGA_STATE_MOVING] = 0.5f;

    StrategyAssetMeasure(a);
}

// ---------------------------------------------------------------------------
static void TestRoundTrip(void)
{
    SgaAsset src, dst;
    BuildSample(&src);

    CHECK(StrategyAssetSave(&src, TEST_PATH));
    CHECK(StrategyAssetLoad(&dst, TEST_PATH));
    CHECK(!StrategyAssetLoadTruncated());

    CHECK(TextIsEqual(dst.subtype, "soldier"));
    CHECK(dst.category == SGA_RESOURCE);
    CHECK(dst.partCount == 3);
    CHECK(dst.easeCount == 1);

    const SgaPart *b = &dst.parts[0];
    CHECK(TextIsEqual(b->name, "body"));
    CHECK(b->kind == SGA_CYLINDER);
    CHECK(b->visible == 1);
    CHECK_NEAR(b->offset.x, 0.1f);
    CHECK_NEAR(b->offset.y, 0.2f);
    CHECK_NEAR(b->offset.z, 0.3f);
    CHECK_NEAR(b->size.y, 1.2f);
    CHECK_NEAR(b->r0, 0.34f);
    CHECK_NEAR(b->r1, 0.45f);
    CHECK_NEAR(b->h, 1.1f);
    CHECK(b->sides == 7);
    CHECK(b->tintMode == SGA_TINT_PARTIAL);
    CHECK_NEAR(b->tintAmount, 0.4f);
    CHECK_NEAR(b->brightness, -0.25f);
    CHECK(b->color.r == 11 && b->color.g == 22 && b->color.b == 33 && b->color.a == 200);

    // The hidden part must STAY hidden - visibility is authored intent, not a
    // view setting, so it belongs in the file.
    CHECK(dst.parts[1].visible == 0);

    const SgaPath *p = &dst.parts[2].path;
    CHECK_NEAR(p->radiusX, 0.5f);
    CHECK_NEAR(p->radiusZ, 0.25f);
    CHECK_NEAR(p->squareness, 0.6f);
    CHECK_NEAR(p->rotation.z, 30.0f);

    const SgaPartAnim *an = &dst.parts[0].anim[SGA_STATE_MOVING];
    CHECK(an->pathPart == 2);
    CHECK(an->keyCount == 2);
    CHECK_NEAR(an->keys[1].t, 0.5f);
    CHECK_NEAR(an->keys[1].u, 1.0f);
    CHECK_NEAR(an->keys[1].rot.y, 90.0f);
    CHECK_NEAR(an->keys[1].scale.y, 2.0f);
    CHECK(an->keys[1].ease == 0);
    CHECK_NEAR(dst.duration[SGA_STATE_MOVING], 0.5f);

    // Untouched states must come back empty, not filled with stale keys.
    CHECK(dst.parts[0].anim[SGA_STATE_DIE].keyCount == 0);
    CHECK(dst.parts[0].anim[SGA_STATE_DIE].pathPart == -1);

    CHECK(TextIsEqual(dst.eases[0].name, "bop_n_up"));
    CHECK(dst.eases[0].ptCount == 3);
    CHECK_NEAR(dst.eases[0].pts[1].y, 0.6f);
    CHECK_NEAR(dst.eases[0].pts[1].ix, -0.16f);

    SimpleDelete(TEST_PATH);
}

// A record from another version means something else field for field, so the
// load must REFUSE rather than read it as this version's struct.
static void TestVersionMismatch(void)
{
    SgaAsset src, dst;
    BuildSample(&src);
    CHECK(StrategyAssetSave(&src, TEST_PATH));

    // The version is field zero, so it is the first 4 bytes of the file.
    FILE *f = fopen(TEST_PATH, "r+b");
    CHECK(f != NULL);
    if (f)
    {
        int32_t bogus = 9999;
        fseek(f, 0, SEEK_SET);
        fwrite(&bogus, sizeof(bogus), 1, f);
        fclose(f);
    }

    CHECK(!StrategyAssetLoad(&dst, TEST_PATH));
    SimpleDelete(TEST_PATH);
}

// A file is untrusted input: a bad count used raw is an out-of-bounds walk.
static void TestClampsHostileValues(void)
{
    SgaAsset src, dst;
    BuildSample(&src);

    // Values the authoring UI can never produce, but a corrupt or hand-edited
    // file certainly can.
    src.parts[0].sides = 2;                     // not a solid
    src.parts[0].tintAmount = 40.0f;
    src.parts[0].brightness = -12.0f;
    src.parts[0].tintMode = 99;
    src.parts[0].kind = 77;
    src.parts[2].path.squareness = 5.0f;
    src.parts[0].anim[SGA_STATE_MOVING].keys[0].ease = 123;   // no such curve

    CHECK(StrategyAssetSave(&src, TEST_PATH));
    CHECK(StrategyAssetLoad(&dst, TEST_PATH));

    CHECK(dst.parts[0].sides >= 3);
    CHECK(dst.parts[0].tintAmount <= 1.0f);
    CHECK(dst.parts[0].brightness >= -1.0f);
    CHECK(dst.parts[0].tintMode < SGA_TINT_COUNT);
    CHECK(dst.parts[0].kind < SGA_KIND_COUNT);
    CHECK(dst.parts[2].path.squareness <= 1.0f);

    // An ease index past easeCount must become "linear", never an index that
    // reads off the end of the baked table.
    CHECK(dst.parts[0].anim[SGA_STATE_MOVING].keys[0].ease == -1);

    SimpleDelete(TEST_PATH);
}

// A path reference is a part INDEX, so anything that renumbers parts has to
// repoint it - otherwise the motion silently attaches to the wrong part.
static void TestPathRefsSurviveEdits(void)
{
    SgaAsset a;
    BuildSample(&a);
    CHECK(a.parts[0].anim[SGA_STATE_MOVING].pathPart == 2);

    // Removing a part BELOW the path shifts the path down one slot.
    CHECK(StrategyAssetRemovePart(&a, 1));
    CHECK(a.partCount == 2);
    CHECK(a.parts[0].anim[SGA_STATE_MOVING].pathPart == 1);

    // Swapping the two parts moves the path to slot 0.
    CHECK(StrategyAssetMovePart(&a, 1, -1));
    CHECK(a.parts[1].anim[SGA_STATE_MOVING].pathPart == 0);

    // Removing the path itself detaches rather than dangling.
    CHECK(StrategyAssetRemovePart(&a, 0));
    CHECK(a.parts[0].anim[SGA_STATE_MOVING].pathPart == -1);
}

// THE STANDALONE GUARANTEE. The knots travel inside the file, so evaluation
// must not consult the global easing set at all - clearing it changes nothing.
static void TestBakedEaseIsSelfContained(void)
{
    SgaAsset src, dst;
    BuildSample(&src);

    float before[5];
    for (int i = 0; i < 5; i++)
        before[i] = StrategyAssetEase(&src, 0, (float)i/4.0f);

    CHECK(StrategyAssetSave(&src, TEST_PATH));
    CHECK(StrategyAssetLoad(&dst, TEST_PATH));

    for (int i = 0; i < 5; i++)
        CHECK_NEAR(StrategyAssetEase(&dst, 0, (float)i/4.0f), before[i]);

    // A curve must actually bend, or this test would pass on a linear stub.
    CHECK(fabsf(before[1] - 0.25f) > 0.01f);

    // Endpoints are pinned: an easing that does not start at 0 and end at 1
    // makes every animation using it drift.
    CHECK_NEAR(StrategyAssetEase(&dst, 0, 0.0f), 0.0f);
    CHECK_NEAR(StrategyAssetEase(&dst, 0, 1.0f), 1.0f);

    // An index nobody baked is linear, not a read off the end of the table.
    CHECK_NEAR(StrategyAssetEase(&dst, 7, 0.3f), 0.3f);
    CHECK_NEAR(StrategyAssetEase(&dst, -1, 0.3f), 0.3f);

    SimpleDelete(TEST_PATH);
}

// THE FILE SIZE IS THE FORMAT. The record is one struct blit, so its size must
// NOT follow the build's capacity tier: a desktop-written file read by a Web
// build would otherwise land the header by luck and silently corrupt every
// field after it, while still reporting success. The on-disk capacities are
// pinned for exactly this reason.
static void TestFileSizeIsTierIndependent(void)
{
    SgaAsset src;
    BuildSample(&src);
    CHECK(StrategyAssetSave(&src, TEST_PATH));

    // 461,024 bytes at the pinned capacities (64 parts / 32 keys / 32 eases).
    // If this number moves, the format changed and SGA_SAVE_VERSION must too -
    // every previously saved asset is otherwise unreadable.
    int len = (int)GetFileLength(TEST_PATH);
    CHECK(len == 461024);

    // A short file must be REFUSED, not read as a full record with whatever
    // happened to be in the buffer standing in for the missing tail.
    FILE *f = fopen(TEST_PATH, "r+b");
    CHECK(f != NULL);
    if (f) { fclose(f); }

    unsigned char *buf = LoadFileData(TEST_PATH, &len);
    CHECK(buf != NULL);
    if (buf)
    {
        SaveFileData("assets_strategy/_sgashort.sga", buf, len/2);
        UnloadFileData(buf);

        SgaAsset dst;
        CHECK(!StrategyAssetLoad(&dst, "assets_strategy/_sgashort.sga"));
        SimpleDelete("assets_strategy/_sgashort.sga");
    }

    SimpleDelete(TEST_PATH);
}

// squareness sweeps ellipse -> rectangle; a zeroed axis degenerates to a line.
static void TestPathShapes(void)
{
    SgaPath p = { 0 };
    p.radiusX = 1.0f;
    p.radiusZ = 1.0f;

    // squareness 0: a circle, so every sample sits at radius 1.
    for (int i = 0; i < 8; i++)
    {
        Vector3 v = StrategyPathPoint(&p, (float)i/8.0f);
        CHECK_NEAR(sqrtf(v.x*v.x + v.z*v.z), 1.0f);
    }

    // squareness 1: the unit square, so the larger coordinate is always +-1.
    p.squareness = 1.0f;
    for (int i = 0; i < 8; i++)
    {
        Vector3 v = StrategyPathPoint(&p, (float)i/8.0f + 0.01f);
        float m = fmaxf(fabsf(v.x), fabsf(v.z));
        CHECK_NEAR(m, 1.0f);
    }

    // A flattened axis is a line, and must not produce NaN.
    p.squareness = 0.0f;
    p.radiusZ = 0.0f;
    for (int i = 0; i < 8; i++)
    {
        Vector3 v = StrategyPathPoint(&p, (float)i/8.0f);
        CHECK(v.z == v.z && v.x == v.x);        // NaN fails equality with itself
        CHECK_NEAR(v.z, 0.0f);
        CHECK(fabsf(v.x) <= 1.0001f);
    }

    // u wraps, so a loop is seamless: 1.25 is the same point as 0.25.
    p.radiusZ = 1.0f;
    Vector3 a = StrategyPathPoint(&p, 0.25f);
    Vector3 b = StrategyPathPoint(&p, 1.25f);
    CHECK_NEAR(a.x, b.x);
    CHECK_NEAR(a.z, b.z);
}

// The six legacy ColorRole values must reproduce the built-in colours exactly,
// or every remixed asset comes out subtly the wrong shade.
static void TestLegacyRoleImport(void)
{
    int32_t mode; float amt, br;

    StrategyAssetTintFromRole(0, &mode, &amt, &br);      // ROLE_FIXED
    CHECK(mode == SGA_TINT_NONE);
    CHECK_NEAR(br, 0.0f);

    StrategyAssetTintFromRole(1, &mode, &amt, &br);      // ROLE_FACTION
    CHECK(mode == SGA_TINT_FULL);
    CHECK_NEAR(br, 0.0f);

    struct { int role; float br; } expect[] = {
        { 2,  0.15f },      // ROLE_FACTION_LIGHT
        { 3, -0.25f },      // ROLE_FACTION_DARK
        { 4,  0.30f },      // ROLE_FACTION_BRIGHT
        { 5, -0.35f },      // ROLE_FACTION_BEAST
    };
    for (int i = 0; i < 4; i++)
    {
        StrategyAssetTintFromRole(expect[i].role, &mode, &amt, &br);
        CHECK(mode == SGA_TINT_PARTIAL);
        CHECK_NEAR(amt, 1.0f);
        CHECK_NEAR(br, expect[i].br);
    }

    // A role this build has never heard of must not land on a random mode.
    StrategyAssetTintFromRole(999, &mode, &amt, &br);
    CHECK(mode >= 0 && mode < SGA_TINT_COUNT);
}

// height/radius are derived, and the camera that frames a preview depends on
// them being honest about the tallest and widest point.
static void TestMeasure(void)
{
    SgaAsset a;
    StrategyAssetInit(&a, "m");

    a.partCount = 1;
    a.parts[0].kind = SGA_CUBE;
    a.parts[0].offset = (Vector3){ 0.0f, 0.5f, 0.0f };
    a.parts[0].size = (Vector3){ 2.0f, 1.0f, 2.0f };
    StrategyAssetMeasure(&a);
    CHECK_NEAR(a.height, 1.0f);         // centre 0.5 + half of 1.0
    CHECK_NEAR(a.radius, 1.0f);

    // DrawCylinder's position is its BASE, so the top is offset + h, not h/2.
    a.parts[0].kind = SGA_CYLINDER;
    a.parts[0].offset = (Vector3){ 0.0f, 0.0f, 0.0f };
    a.parts[0].r0 = 0.3f; a.parts[0].r1 = 0.5f; a.parts[0].h = 1.2f;
    StrategyAssetMeasure(&a);
    CHECK_NEAR(a.height, 1.2f);
    CHECK_NEAR(a.radius, 0.5f);

    // A path contributes nothing: it is not geometry and must not blow the
    // preview camera out to frame a loop nobody can see.
    a.partCount = 2;
    a.parts[1].kind = SGA_PATH;
    a.parts[1].path.radiusX = 50.0f;
    a.parts[1].path.radiusZ = 50.0f;
    StrategyAssetMeasure(&a);
    CHECK_NEAR(a.radius, 0.5f);
}

// Both taxonomy fields are mandatory, and the UI needs to know WHICH one is
// missing so it can say so instead of leaving a button dead.
static void TestValidation(void)
{
    SgaAsset a;
    const char *why = NULL;

    StrategyAssetInit(&a, "thing");
    CHECK(!StrategyAssetValid(&a, &why));       // subtype starts empty
    CHECK(why != NULL);

    TextCopy(a.subtype, "worker");
    CHECK(StrategyAssetValid(&a, &why));
    CHECK(why == NULL);

    a.name[0] = '\0';
    CHECK(!StrategyAssetValid(&a, &why));
    TextCopy(a.name, "thing");

    // An asset of nothing but paths draws nothing at all.
    a.parts[0].kind = SGA_PATH;
    CHECK(!StrategyAssetValid(&a, &why));

    // ... and so does one whose only geometry is hidden.
    a.parts[0].kind = SGA_CUBE;
    a.parts[0].visible = 0;
    CHECK(!StrategyAssetValid(&a, &why));
}

// Poses interpolate, hold outside the key range, and follow their bound path.
static void TestPose(void)
{
    SgaAsset a;
    BuildSample(&a);

    Vector3 off, rot, scl;

    // Before the first key: hold the first value.
    StrategyAssetPartPose(&a, 0, SGA_STATE_MOVING, -1.0f, &off, &rot, &scl);
    CHECK_NEAR(rot.y, 0.0f);
    CHECK_NEAR(scl.y, 1.0f);

    // After the last: hold the last.
    StrategyAssetPartPose(&a, 0, SGA_STATE_MOVING, 99.0f, &off, &rot, &scl);
    CHECK_NEAR(rot.y, 90.0f);
    CHECK_NEAR(scl.y, 2.0f);

    // A state with no keys is the rest pose, not garbage.
    StrategyAssetPartPose(&a, 0, SGA_STATE_DIE, 0.25f, &off, &rot, &scl);
    CHECK_NEAR(off.x, 0.0f);
    CHECK_NEAR(rot.y, 0.0f);
    CHECK_NEAR(scl.x, 1.0f);

    // Mid-segment lands strictly between the two keys.
    StrategyAssetPartPose(&a, 0, SGA_STATE_MOVING, 0.25f, &off, &rot, &scl);
    CHECK(rot.y > 0.0f && rot.y < 90.0f);

    // The bound path actually moves the part off the origin.
    CHECK(fabsf(off.x) + fabsf(off.y) + fabsf(off.z) > 0.0f);

    // An out-of-range part index must not read out of bounds.
    StrategyAssetPartPose(&a, 999, SGA_STATE_MOVING, 0.25f, &off, &rot, &scl);
    CHECK_NEAR(scl.x, 1.0f);
}

// Partial tint is the whole reason ColorRole was generalised: it has to sit
// strictly between the part's own colour and the faction's.
static Color TestTint(int faction)
{
    return (faction == 0) ? (Color){ 0, 0, 255, 255 } : (Color){ 255, 0, 0, 255 };
}

static void TestPartColor(void)
{
    StrategyAssetSetFactionTint(TestTint);

    SgaPart p = { 0 };
    p.color = (Color){ 0, 0, 0, 255 };
    p.visible = 1;

    p.tintMode = SGA_TINT_NONE;
    CHECK(StrategyAssetPartColor(&p, 0, 1.0f).b == 0);      // faction ignored

    p.tintMode = SGA_TINT_FULL;
    CHECK(StrategyAssetPartColor(&p, 0, 1.0f).b == 255);    // faction wins

    p.tintMode = SGA_TINT_PARTIAL;
    p.tintAmount = 0.5f;
    Color half = StrategyAssetPartColor(&p, 0, 1.0f);
    CHECK(half.b > 100 && half.b < 155);                    // strictly between

    // Brightness applies on top of the blend, so two parts on the same faction
    // can read as light and dark versions of one army.
    p.tintAmount = 1.0f;
    p.brightness = -0.5f;
    Color dark = StrategyAssetPartColor(&p, 0, 1.0f);
    p.brightness = 0.0f;
    Color full = StrategyAssetPartColor(&p, 0, 1.0f);
    CHECK(dark.b < full.b);

    // Alpha composes with the part's own alpha - the ghosted reference model
    // in the forge rides on exactly this.
    CHECK(StrategyAssetPartColor(&p, 0, 0.5f).a < 200);

    StrategyAssetSetFactionTint(NULL);
}

int main(void)
{
    SetTraceLogLevel(LOG_ERROR);    // the version test logs an expected warning

    TestRoundTrip();
    TestVersionMismatch();
    TestClampsHostileValues();
    TestPathRefsSurviveEdits();
    TestBakedEaseIsSelfContained();
    TestFileSizeIsTierIndependent();
    TestPathShapes();
    TestLegacyRoleImport();
    TestMeasure();
    TestValidation();
    TestPose();
    TestPartColor();

    printf("sga_tests: %d checks, %d failures\n", s_checks, s_fails);
    return (s_fails == 0) ? 0 : 1;
}
