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
#include "../src/strategy_asset/strategy_asset_ease.h"
#include "../src/strategy_asset/strategy_bindings.h"
#include "../src/anim/anim.h"
#include "../src/anim/anim_ease_custom.h"

#include <stdio.h>
#include <math.h>
#include <string.h>

// anim.c pulls this in for AnimDocDraw, which nothing here calls. Same stub
// zen_tests.c uses, for the same reason: the suite links the anim module for
// its EASING tables, not for its drawing.
Vector2 ScreenStateTargetSize(void) { return (Vector2){ 1280, 720 }; }

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
    // Designated, not positional: adding a field to SgaKey once shifted `rot`
    // into the new slot and quietly changed what this sample meant.
    an->keys[0] = (SgaKey){ .t = 0.0f, .u = 0.0f, .offset = { 0, 0, 0 },
                            .rot = { 0, 0, 0 }, .scale = { 1, 1, 1 }, .ease = -1 };
    an->keys[1] = (SgaKey){ .t = 0.5f, .u = 1.0f, .offset = { 0.25f, 0, 0 },
                            .rot = { 0, 90.0f, 0 }, .scale = { 1, 2.0f, 1 }, .ease = 0 };
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
    CHECK_NEAR(an->keys[1].offset.x, 0.25f);
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

    // 608,480 bytes at the pinned capacities (64 parts / 32 keys / 32 eases),
    // as of v2 - which added a per-key offset, growing SgaKeyDisk from 36 to 48
    // bytes. If this number moves, the format changed and SGA_SAVE_VERSION must
    // too, or every previously saved asset is misread rather than refused.
    int len = (int)GetFileLength(TEST_PATH);
    CHECK(len == 608480);

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

// ---------------------------------------------------------------------------
//  Baking (strategy_asset_ease.c)
//
//  These are the Phase 3 tests, and the reason the suite links src/anim/.
// ---------------------------------------------------------------------------

// A builtin baked into an asset must still be the SAME CURVE. This is the test
// that justifies sampling builtins into knots rather than storing their name:
// strategy_asset.c cannot see the builtin table, so a name-only slot would play
// linear in the showcase while looking correct in the forge.
static void TestBakeBuiltinKeepsShape(void)
{
    static SgaAsset a;
    StrategyAssetInit(&a, "bake");

    // The smooth curves should be reproduced tightly by 8 fitted knots.
    const int smooth[] = { ANIM_EASE_SINE_INOUT, ANIM_EASE_QUAD_OUT,
                           ANIM_EASE_CUBIC_IN, ANIM_EASE_CUBIC_INOUT,
                           ANIM_EASE_EXPO_OUT };
    for (int c = 0; c < (int)(sizeof(smooth)/sizeof(smooth[0])); c++)
    {
        int slot = StrategyAssetBakeEase(&a, smooth[c]);
        CHECK(slot >= 0);

        float worst = 0.0f;
        for (int i = 0; i <= 20; i++)
        {
            float p = (float)i/20.0f;
            float d = fabsf(StrategyAssetEase(&a, slot, p) - AnimEaseApply(smooth[c], p));
            if (d > worst) worst = d;
        }
        // 2% of the curve's range: well under a pixel of motion at any size a
        // part is drawn, and the difference is invisible in the viewport.
        CHECK(worst < 0.02f);
    }

    // Endpoints must be exact regardless of fit quality - a curve that does not
    // land on 1.0 leaves the part short of its keyed pose forever.
    int s2 = StrategyAssetBakeEase(&a, ANIM_EASE_BACK_OUT);
    CHECK(s2 >= 0);
    CHECK_NEAR(StrategyAssetEase(&a, s2, 0.0f), 0.0f);
    CHECK_NEAR(StrategyAssetEase(&a, s2, 1.0f), 1.0f);

    // backOut overshoots past 1 on its way; a fit that clamped would flatten
    // the character out of the motion entirely.
    float peak = 0.0f;
    for (int i = 0; i <= 40; i++)
    {
        float v = StrategyAssetEase(&a, s2, (float)i/40.0f);
        if (v > peak) peak = v;
    }
    CHECK(peak > 1.02f);

    // Linear is not a curve and must never consume a slot.
    int before = a.easeCount;
    CHECK(StrategyAssetBakeEase(&a, ANIM_EASE_LINEAR) < 0);
    CHECK(a.easeCount == before);
}

// Baking the same curve twice must reuse the slot. Without this, every click in
// the easing picker burns a slot and a normal session runs out.
static void TestBakeDedupes(void)
{
    static SgaAsset a;
    StrategyAssetInit(&a, "dedup");

    int x = StrategyAssetBakeEase(&a, ANIM_EASE_SINE_OUT);
    int y = StrategyAssetBakeEase(&a, ANIM_EASE_SINE_OUT);
    int z = StrategyAssetBakeEase(&a, ANIM_EASE_QUAD_IN);
    CHECK(x == y);
    CHECK(z != x);
    CHECK(a.easeCount == 2);
}

// THE standalone guarantee, on a real custom curve: bake it, then delete the
// slot it came from, and the asset must still play the shape it was authored
// with. This is the test the whole baking design exists to pass.
static void TestCustomEaseSurvivesSlotDeletion(void)
{
    AnimEasePt pts[3];
    pts[0] = (AnimEasePt){ 0.0f, 0.0f, 0,0, 0.10f,  0.60f };
    pts[1] = (AnimEasePt){ 0.5f, 0.9f, -0.15f, 0.0f, 0.15f, 0.0f };
    pts[2] = (AnimEasePt){ 1.0f, 1.0f, -0.10f, -0.05f, 0,0 };

    int id = AnimCustomEaseAdd("sgaTestCurve", pts, 3);
    CHECK(id >= ANIM_EASE_COUNT);

    static SgaAsset a;
    StrategyAssetInit(&a, "custom");
    int slot = StrategyAssetBakeEase(&a, id);
    CHECK(slot >= 0);
    CHECK(strcmp(StrategyAssetEaseName(&a, slot), "sgaTestCurve") == 0);

    float before[9];
    for (int i = 0; i < 9; i++) before[i] = StrategyAssetEase(&a, slot, (float)i/8.0f);

    // The curve genuinely bends, or "unchanged" would prove nothing.
    CHECK(fabsf(before[2] - 0.25f) > 0.05f);

    // Now pull the ground out: the slot is gone, exactly as if _easings.cfg had
    // been edited or deleted on another machine.
    CHECK(AnimCustomEaseRemove(id));
    CHECK(AnimCustomEaseGet(id) == NULL);

    for (int i = 0; i < 9; i++)
        CHECK_NEAR(StrategyAssetEase(&a, slot, (float)i/8.0f), before[i]);

    // And it survives a file round trip on top of that.
    CHECK(StrategyAssetSave(&a, TEST_PATH));
    static SgaAsset b;
    CHECK(StrategyAssetLoad(&b, TEST_PATH));
    for (int i = 0; i < 9; i++)
        CHECK_NEAR(StrategyAssetEase(&b, slot, (float)i/8.0f), before[i]);

    // With the live slot gone there is no runtime id to point back at, and the
    // picker must say so rather than reporting a wrong curve as selected.
    CHECK(StrategyAssetEaseRuntimeId(&b, slot) < 0);

    SimpleDelete(TEST_PATH);
}

// Compaction drops unreferenced curves AND repoints the keys that survive. A
// remap that forgot the second half would silently move keys onto other curves.
static void TestCompactEases(void)
{
    static SgaAsset a;
    StrategyAssetInit(&a, "compact");

    int e0 = StrategyAssetBakeEase(&a, ANIM_EASE_SINE_IN);
    int e1 = StrategyAssetBakeEase(&a, ANIM_EASE_QUAD_OUT);
    int e2 = StrategyAssetBakeEase(&a, ANIM_EASE_CUBIC_INOUT);
    CHECK(a.easeCount == 3);
    (void)e0;

    // Only the middle and last curves are actually used.
    CHECK(StrategyAssetAddKey(&a, 0, SGA_STATE_IDLE, 0.0f) == 0);
    CHECK(StrategyAssetAddKey(&a, 0, SGA_STATE_IDLE, 1.0f) == 1);
    a.parts[0].anim[SGA_STATE_IDLE].keys[0].ease = e1;
    a.parts[0].anim[SGA_STATE_IDLE].keys[1].ease = e2;

    float shape1[5], shape2[5];
    for (int i = 0; i < 5; i++)
    {
        shape1[i] = StrategyAssetEase(&a, e1, (float)i/4.0f);
        shape2[i] = StrategyAssetEase(&a, e2, (float)i/4.0f);
    }

    StrategyAssetCompactEases(&a);
    CHECK(a.easeCount == 2);        // the unreferenced first curve is gone

    // The keys must still resolve to the SAME SHAPES, at their new indices.
    int n1 = a.parts[0].anim[SGA_STATE_IDLE].keys[0].ease;
    int n2 = a.parts[0].anim[SGA_STATE_IDLE].keys[1].ease;
    CHECK(n1 >= 0 && n1 < a.easeCount);
    CHECK(n2 >= 0 && n2 < a.easeCount);
    for (int i = 0; i < 5; i++)
    {
        CHECK_NEAR(StrategyAssetEase(&a, n1, (float)i/4.0f), shape1[i]);
        CHECK_NEAR(StrategyAssetEase(&a, n2, (float)i/4.0f), shape2[i]);
    }
}

// ---------------------------------------------------------------------------
//  Keyframes
// ---------------------------------------------------------------------------
static void TestKeysStaySorted(void)
{
    static SgaAsset a;
    StrategyAssetInit(&a, "keys");

    // Added out of order on purpose: evaluation walks the list in order and
    // reads the wrong segment if the invariant is only maintained by luck.
    CHECK(StrategyAssetAddKey(&a, 0, SGA_STATE_MOVING, 1.0f) >= 0);
    CHECK(StrategyAssetAddKey(&a, 0, SGA_STATE_MOVING, 0.25f) >= 0);
    CHECK(StrategyAssetAddKey(&a, 0, SGA_STATE_MOVING, 0.75f) >= 0);

    SgaPartAnim *an = &a.parts[0].anim[SGA_STATE_MOVING];
    CHECK(an->keyCount == 3);
    CHECK_NEAR(an->keys[0].t, 0.25f);
    CHECK_NEAR(an->keys[1].t, 0.75f);
    CHECK_NEAR(an->keys[2].t, 1.0f);

    // A key dragged PAST its neighbour is the normal timeline gesture, and the
    // returned index has to follow it or the selection jumps to another key.
    int moved = StrategyAssetMoveKey(&a, 0, SGA_STATE_MOVING, 0, 0.9f);
    CHECK(moved == 1);
    CHECK_NEAR(an->keys[0].t, 0.75f);
    CHECK_NEAR(an->keys[1].t, 0.9f);
    CHECK_NEAR(an->keys[2].t, 1.0f);

    // Dragged past the far end too.
    moved = StrategyAssetMoveKey(&a, 0, SGA_STATE_MOVING, 1, 5.0f);
    CHECK(moved == 2);
    CHECK_NEAR(an->keys[2].t, 5.0f);

    // Negative time is clamped, not stored - it would sit before the head can
    // ever reach it and be undraggable.
    StrategyAssetMoveKey(&a, 0, SGA_STATE_MOVING, 0, -3.0f);
    CHECK(an->keys[0].t >= 0.0f);

    // A second key at an existing time REPLACES it: two keys at one instant is
    // a zero-length segment the author cannot see or separate.
    int n = an->keyCount;
    CHECK(StrategyAssetAddKey(&a, 0, SGA_STATE_MOVING, 5.0f) >= 0);
    CHECK(an->keyCount == n);

    CHECK(StrategyAssetRemoveKey(&a, 0, SGA_STATE_MOVING, 0));
    CHECK(an->keyCount == n - 1);
    CHECK(!StrategyAssetRemoveKey(&a, 0, SGA_STATE_MOVING, 99));

    // Keys live per STATE: authoring a walk cycle must not disturb the idle.
    CHECK(a.parts[0].anim[SGA_STATE_IDLE].keyCount == 0);
    CHECK(!StrategyAssetStateHasKeys(&a, SGA_STATE_IDLE));
    CHECK(StrategyAssetStateHasKeys(&a, SGA_STATE_MOVING));
    CHECK_NEAR(StrategyAssetStateExtent(&a, SGA_STATE_IDLE), 0.0f);
    CHECK_NEAR(StrategyAssetStateExtent(&a, SGA_STATE_MOVING), 5.0f);
}

// A new key must adopt the pose ALREADY SHOWING at that time. Seeding from rest
// would snap the part back to origin the moment a key is dropped, which reads
// as the tool destroying the animation.
static void TestAddKeySamplesCurrentPose(void)
{
    static SgaAsset a;
    StrategyAssetInit(&a, "pose");

    SgaPartAnim *an = &a.parts[0].anim[SGA_STATE_IDLE];
    CHECK(StrategyAssetAddKey(&a, 0, SGA_STATE_IDLE, 0.0f) >= 0);
    CHECK(StrategyAssetAddKey(&a, 0, SGA_STATE_IDLE, 2.0f) >= 0);
    an->keys[0].rot = (Vector3){ 0.0f, 0.0f, 0.0f };
    an->keys[1].rot = (Vector3){ 0.0f, 90.0f, 0.0f };

    // Halfway between them the part is at 45 degrees, so a key dropped there
    // must BE 45 degrees.
    int mid = StrategyAssetAddKey(&a, 0, SGA_STATE_IDLE, 1.0f);
    CHECK(mid == 1);
    CHECK_NEAR(an->keys[1].rot.y, 45.0f);

    // And the pose is genuinely unchanged by the insertion.
    Vector3 off, rot, scl;
    StrategyAssetPartPose(&a, 0, SGA_STATE_IDLE, 1.0f, &off, &rot, &scl);
    CHECK_NEAR(rot.y, 45.0f);

    // Scale defaults to rest, never to zero - a key seeded with a zero scale
    // makes the part vanish.
    CHECK_NEAR(an->keys[1].scale.x, 1.0f);
}


// Per-key offset (v2). Before this existed the only movable value was the
// part's REST offset, which is shared by every key in every state - so
// "animating" a part by moving it changed all of its keys at once.
static void TestPerKeyOffset(void)
{
    static SgaAsset a;
    StrategyAssetInit(&a, "koff");

    SgaPartAnim *an = &a.parts[0].anim[SGA_STATE_IDLE];
    CHECK(StrategyAssetAddKey(&a, 0, SGA_STATE_IDLE, 0.0f) >= 0);
    CHECK(StrategyAssetAddKey(&a, 0, SGA_STATE_IDLE, 1.0f) >= 0);
    an->keys[0].offset = (Vector3){ 0.0f, 0.0f, 0.0f };
    an->keys[1].offset = (Vector3){ 2.0f, 0.0f, 0.0f };

    Vector3 off, rot, scl;
    StrategyAssetPartPose(&a, 0, SGA_STATE_IDLE, 0.5f, &off, &rot, &scl);
    CHECK_NEAR(off.x, 1.0f);            // halfway between the two keys

    // Editing ONE key must not disturb the other - the whole point.
    an->keys[1].offset.x = 4.0f;
    StrategyAssetPartPose(&a, 0, SGA_STATE_IDLE, 0.0f, &off, &rot, &scl);
    CHECK_NEAR(off.x, 0.0f);

    // The rest offset is a SEPARATE value and is not touched by keying.
    CHECK_NEAR(a.parts[0].offset.x, 0.0f);

    // A bound path ADDS to the key offset rather than replacing it, so a part
    // can be positioned by hand and still ride an orbit.
    int pi = StrategyAssetAddPart(&a, SGA_PATH);
    CHECK(pi > 0);
    a.parts[pi].path.radiusX = 1.0f;
    a.parts[pi].path.radiusZ = 1.0f;
    a.parts[pi].path.center = (Vector3){ 0.0f, 0.0f, 0.0f };
    an = &a.parts[0].anim[SGA_STATE_IDLE];      // AddPart may move the array
    an->pathPart = pi;
    an->keys[0].offset = (Vector3){ 10.0f, 0.0f, 0.0f };
    an->keys[0].u = 0.0f;

    StrategyAssetPartPose(&a, 0, SGA_STATE_IDLE, 0.0f, &off, &rot, &scl);
    Vector3 p0 = StrategyPathPoint(&a.parts[pi].path, 0.0f);
    CHECK_NEAR(off.x, 10.0f + p0.x);
    CHECK_NEAR(off.z, p0.z);

    // And a key added mid-animation must not bake the path position into its
    // own offset, or the path would be applied twice from then on.
    an->keys[1].offset = (Vector3){ 10.0f, 0.0f, 0.0f };
    an->keys[1].u = 0.5f;
    int mid = StrategyAssetAddKey(&a, 0, SGA_STATE_IDLE, 0.5f);
    CHECK(mid >= 0);
    CHECK_NEAR(a.parts[0].anim[SGA_STATE_IDLE].keys[mid].offset.x, 10.0f);
}


// A faction-driven built-in part carries NO authored .color - strategy_models.c
// omits it for those roles because its own renderer never reads it - so it
// arrives as {0,0,0,0}. Blending toward that made remixed built-ins invisible
// at every FACTION value below 1, and visible only at FULL where the faction
// colour replaces alpha outright.
static void TestPartialTintKeepsAlpha(void)
{
    StrategyAssetSetFactionTint(TestTint);

    SgaPart p;
    memset(&p, 0, sizeof(p));
    p.color = (Color){ 0, 0, 0, 0 };        // exactly what a builtin import copies
    p.tintMode = SGA_TINT_PARTIAL;
    p.brightness = 0.0f;

    // At every blend the part must stay opaque: the faction colour is opaque,
    // and a transparent .color must not drag the result to invisible.
    for (int i = 0; i <= 10; i++)
    {
        p.tintAmount = (float)i/10.0f;
        CHECK(StrategyAssetPartColor(&p, 0, 1.0f).a == 255);
    }

    // PARTIAL asks "how much of the faction's HUE", so alpha follows the faction
    // rather than the part's own colour. A part being faction-tinted is as
    // opaque as the faction is, whatever junk alpha its .color carries.
    p.color = (Color){ 200, 100, 50, 0 };
    p.tintAmount = 0.5f;
    CHECK(StrategyAssetPartColor(&p, 0, 1.0f).a == 255);

    // NONE is the mode that respects an authored alpha, and must keep doing so.
    p.tintMode = SGA_TINT_NONE;
    CHECK(StrategyAssetPartColor(&p, 0, 1.0f).a == 0);
    p.color.a = 128;
    CHECK(StrategyAssetPartColor(&p, 0, 1.0f).a == 128);

    // The overall alpha argument still composes on top.
    p.tintMode = SGA_TINT_PARTIAL;
    p.color.a = 255;
    p.tintAmount = 1.0f;
    CHECK(StrategyAssetPartColor(&p, 0, 0.5f).a < 200);

    StrategyAssetSetFactionTint(NULL);
}

// ===========================================================================
//  Role bindings
//
//  These write to the REAL bindings path - the module's path is fixed, the way
//  settings.sav's is - so the suite moves any existing file aside first and
//  puts it back at the end. A developer's own bindings surviving a test run is
//  not a nicety: the file is authored work, and losing it to `ctest` would be
//  the same class of bug as an editor that eats a document.
// ===========================================================================
static bool s_bindStashed = false;
#define BIND_STASH  "assets_strategy/_sgbtest_stash.sgb"

static void BindStash(void)
{
    s_bindStashed = false;
    const char *path = StrategyBindingsPath();
    if (!FileExists(path)) return;

    int size = 0;
    unsigned char *data = LoadFileData(path, &size);
    if ((data == NULL) || (size <= 0)) return;

    s_bindStashed = SaveFileData(BIND_STASH, data, size);
    UnloadFileData(data);
}

static void BindRestore(void)
{
    SimpleDelete(StrategyBindingsPath());

    if (!s_bindStashed) return;

    int size = 0;
    unsigned char *data = LoadFileData(BIND_STASH, &size);
    if ((data != NULL) && (size > 0)) SaveFileData(StrategyBindingsPath(), data, size);
    if (data != NULL) UnloadFileData(data);

    SimpleDelete(BIND_STASH);
    s_bindStashed = false;
}

static void TestBindingsSetGetClear(void)
{
    StrategyBindingsClear();
    StrategyBindingsSetRoleCounts(8, 8, 4);

    CHECK(StrategyBindingsRoleCount(SGB_ROLE_UNIT) == 8);
    CHECK(StrategyBindingsRoleCount(SGB_ROLE_NODE) == 4);
    CHECK(StrategyBindingsRoleCount(-1) == 0);

    // Unbound reads as the empty string, never NULL - callers print it raw.
    CHECK(StrategyBindingGet(SGB_ROLE_UNIT, 0)[0] == '\0');
    CHECK(!StrategyBindingIsBound(SGB_ROLE_UNIT, 0));

    CHECK(StrategyBindingSet(SGB_ROLE_UNIT, 1, "watchtower"));
    CHECK(StrategyBindingIsBound(SGB_ROLE_UNIT, 1));
    CHECK(TextIsEqual(StrategyBindingGet(SGB_ROLE_UNIT, 1), "watchtower"));

    // The three families are independent: same index, different slot.
    CHECK(!StrategyBindingIsBound(SGB_ROLE_BUILDING, 1));
    CHECK(!StrategyBindingIsBound(SGB_ROLE_NODE, 1));

    // Empty and NULL both mean "back to the built-in".
    CHECK(StrategyBindingSet(SGB_ROLE_UNIT, 1, ""));
    CHECK(!StrategyBindingIsBound(SGB_ROLE_UNIT, 1));
    CHECK(StrategyBindingSet(SGB_ROLE_UNIT, 1, "x"));
    CHECK(StrategyBindingSet(SGB_ROLE_UNIT, 1, NULL));
    CHECK(!StrategyBindingIsBound(SGB_ROLE_UNIT, 1));

    // Out of range is refused rather than clamped into someone else's slot.
    CHECK(!StrategyBindingSet(SGB_ROLE_UNIT, -1, "x"));
    CHECK(!StrategyBindingSet(SGB_ROLE_UNIT, SGB_UNITS_MAX, "x"));
    CHECK(!StrategyBindingSet(SGB_ROLE_FAMILY_COUNT, 0, "x"));
    CHECK(StrategyBindingGet(SGB_ROLE_FAMILY_COUNT, 0)[0] == '\0');

    StrategyBindingsClear();
    CHECK(!StrategyBindingIsBound(SGB_ROLE_UNIT, 1));
}

// The whole point of storing names: the mapping survives the asset list being
// reordered underneath it, which an index could not.
static void TestBindingResolveByName(void)
{
    static SgaAsset assets[3];
    StrategyAssetInit(&assets[0], "alpha");
    StrategyAssetInit(&assets[1], "beta");
    StrategyAssetInit(&assets[2], "gamma");

    StrategyBindingsClear();
    StrategyBindingsSetRoleCounts(8, 8, 4);
    CHECK(StrategyBindingSet(SGB_ROLE_BUILDING, 2, "beta"));

    const SgaAsset *r = StrategyBindingResolve(SGB_ROLE_BUILDING, 2, assets, 3);
    CHECK(r == &assets[1]);

    // Reorder the list - an index-based binding would now point at "alpha".
    SgaAsset tmp = assets[0];
    assets[0] = assets[1];
    assets[1] = tmp;

    r = StrategyBindingResolve(SGB_ROLE_BUILDING, 2, assets, 3);
    CHECK(r == &assets[0]);
    CHECK(TextIsEqual(r->name, "beta"));

    // Unbound resolves to NULL, meaning "draw the built-in".
    CHECK(StrategyBindingResolve(SGB_ROLE_BUILDING, 3, assets, 3) == NULL);
}

// A name with no file behind it must be KEPT, not erased. Copying the bindings
// without the assets is a missing file, not a decision to unbind.
static void TestBindingMissingIsKept(void)
{
    static SgaAsset assets[1];
    StrategyAssetInit(&assets[0], "alpha");

    StrategyBindingsClear();
    StrategyBindingsSetRoleCounts(4, 4, 2);
    CHECK(StrategyBindingSet(SGB_ROLE_UNIT, 0, "alpha"));
    CHECK(StrategyBindingSet(SGB_ROLE_UNIT, 1, "not-here"));

    CHECK(StrategyBindingResolve(SGB_ROLE_UNIT, 1, assets, 1) == NULL);
    CHECK(StrategyBindingIsBound(SGB_ROLE_UNIT, 1));    // still bound
    CHECK(TextIsEqual(StrategyBindingGet(SGB_ROLE_UNIT, 1), "not-here"));

    CHECK(StrategyBindingsMissingCount(assets, 1) == 1);

    // Counting walks only the INSTALLED roles, not the whole capacity - a stale
    // binding past the game's enum must not be reported as a missing asset.
    CHECK(StrategyBindingSet(SGB_ROLE_UNIT, SGB_UNITS_MAX - 1, "ghost"));
    CHECK(StrategyBindingsMissingCount(assets, 1) == 1);
}

static void TestBindingsRename(void)
{
    StrategyBindingsClear();
    StrategyBindingsSetRoleCounts(8, 8, 4);
    CHECK(StrategyBindingSet(SGB_ROLE_UNIT, 0, "old"));
    CHECK(StrategyBindingSet(SGB_ROLE_BUILDING, 3, "old"));
    CHECK(StrategyBindingSet(SGB_ROLE_NODE, 1, "other"));

    CHECK(StrategyBindingsRename("old", "new") == 2);
    CHECK(TextIsEqual(StrategyBindingGet(SGB_ROLE_UNIT, 0), "new"));
    CHECK(TextIsEqual(StrategyBindingGet(SGB_ROLE_BUILDING, 3), "new"));
    CHECK(TextIsEqual(StrategyBindingGet(SGB_ROLE_NODE, 1), "other"));

    // A delete clears rather than repoints.
    CHECK(StrategyBindingsRename("new", NULL) == 2);
    CHECK(!StrategyBindingIsBound(SGB_ROLE_UNIT, 0));
    CHECK(!StrategyBindingIsBound(SGB_ROLE_BUILDING, 3));

    CHECK(StrategyBindingsRename(NULL, "x") == 0);
    CHECK(StrategyBindingsRename("nobody", "x") == 0);
}

static void TestBindingsRoundTrip(void)
{
    BindStash();

    StrategyBindingsClear();
    StrategyBindingsSetRoleCounts(8, 8, 4);
    CHECK(StrategyBindingSet(SGB_ROLE_UNIT, 0, "worker-look"));
    CHECK(StrategyBindingSet(SGB_ROLE_BUILDING, 5, "tree-look"));
    CHECK(StrategyBindingSet(SGB_ROLE_NODE, 2, "hall-look"));

    CHECK(StrategyBindingsSave());

    StrategyBindingsClear();
    CHECK(!StrategyBindingIsBound(SGB_ROLE_UNIT, 0));

    CHECK(StrategyBindingsLoad());
    CHECK(TextIsEqual(StrategyBindingGet(SGB_ROLE_UNIT, 0), "worker-look"));
    CHECK(TextIsEqual(StrategyBindingGet(SGB_ROLE_BUILDING, 5), "tree-look"));
    CHECK(TextIsEqual(StrategyBindingGet(SGB_ROLE_NODE, 2), "hall-look"));
    CHECK(!StrategyBindingIsBound(SGB_ROLE_UNIT, 1));

    // A category never restricts a binding, and the file must not start doing
    // so quietly: a RESOURCE asset bound to a building role round-trips.
    CHECK(StrategyBindingSet(SGB_ROLE_BUILDING, 0, "some-tree"));
    CHECK(StrategyBindingsSave());
    StrategyBindingsClear();
    CHECK(StrategyBindingsLoad());
    CHECK(TextIsEqual(StrategyBindingGet(SGB_ROLE_BUILDING, 0), "some-tree"));

    BindRestore();
}

static void TestBindingsVersionMismatch(void)
{
    BindStash();

    StrategyBindingsClear();
    StrategyBindingsSetRoleCounts(8, 8, 4);
    CHECK(StrategyBindingSet(SGB_ROLE_UNIT, 0, "keeper"));
    CHECK(StrategyBindingsSave());

    // Corrupt field zero. A version we do not know must be REFUSED, not read
    // as though its fields meant what this build thinks they mean.
    FILE *f = fopen(StrategyBindingsPath(), "r+b");
    CHECK(f != NULL);
    if (f)
    {
        int32_t bogus = 999;
        fwrite(&bogus, sizeof(bogus), 1, f);
        fclose(f);
    }

    CHECK(!StrategyBindingsLoad());
    // A refused load leaves nothing half-applied.
    CHECK(!StrategyBindingIsBound(SGB_ROLE_UNIT, 0));

    BindRestore();
}

// No file at all is the normal first run: success, everything cleared, and no
// alarming message in front of the user.
static void TestBindingsMissingFileIsFine(void)
{
    BindStash();
    SimpleDelete(StrategyBindingsPath());

    StrategyBindingsSetRoleCounts(8, 8, 4);
    CHECK(StrategyBindingSet(SGB_ROLE_UNIT, 0, "stale"));
    CHECK(StrategyBindingsLoad());
    CHECK(!StrategyBindingIsBound(SGB_ROLE_UNIT, 0));

    BindRestore();
}


// ===========================================================================
//  Phase 5: concurrent per-part states
// ===========================================================================

// Gives `part` a two-key animation in `state`, which is all the resolver looks
// at - it asks whether a part has keys, never what they contain.
static void GiveKeys(SgaAsset *a, int part, int state)
{
    SgaPartAnim *an = &a->parts[part].anim[state];
    an->keyCount = 2;
    an->keys[0].t = 0.0f;
    an->keys[1].t = 1.0f;
    an->keys[0].scale = an->keys[1].scale = (Vector3){ 1.0f, 1.0f, 1.0f };
    a->duration[state] = 1.0f;
}

// The ladder has to be a strict total order. Two states sharing a rank would
// make the winner depend on iteration order - stable today, silently different
// the day the enum is reordered.
static void TestStatePriorityIsTotalOrder(void)
{
    for (int i = 0; i < SGA_STATE_COUNT; i++)
        for (int j = 0; j < SGA_STATE_COUNT; j++)
        {
            if (i == j) continue;
            CHECK(StrategyAssetStatePriority(i) != StrategyAssetStatePriority(j));
        }

    // The order the design calls for, spelled out so a reshuffle fails here
    // rather than in someone's playtest.
    CHECK(StrategyAssetStatePriority(SGA_STATE_DIE) >
          StrategyAssetStatePriority(SGA_STATE_DAMAGED));
    CHECK(StrategyAssetStatePriority(SGA_STATE_DAMAGED) >
          StrategyAssetStatePriority(SGA_STATE_HEALED));
    CHECK(StrategyAssetStatePriority(SGA_STATE_HEALED) >
          StrategyAssetStatePriority(SGA_STATE_ATTACKING));
    CHECK(StrategyAssetStatePriority(SGA_STATE_ATTACKING) >
          StrategyAssetStatePriority(SGA_STATE_MOVING));
    CHECK(StrategyAssetStatePriority(SGA_STATE_MOVING) >
          StrategyAssetStatePriority(SGA_STATE_IDLE));
}

// THE point of the phase: two states driving two different parts both play.
static void TestConcurrentStatesDoNotFight(void)
{
    static SgaAsset a;
    StrategyAssetInit(&a, "concurrent");
    a.partCount = 2;

    GiveKeys(&a, 0, SGA_STATE_IDLE);        // the head bobs
    GiveKeys(&a, 1, SGA_STATE_MOVING);      // the legs walk

    SgaStateSet set;
    StrategyAssetStateSetInit(&set);
    StrategyAssetStateSetAdd(&set, SGA_STATE_IDLE, 0.0f);
    StrategyAssetStateSetAdd(&set, SGA_STATE_MOVING, 0.0f);

    // Neither part is contested, so each keeps its own state - even though
    // MOVING outranks IDLE, it never touches the head.
    CHECK(StrategyAssetResolvePartState(&a, 0, &set) == SGA_STATE_IDLE);
    CHECK(StrategyAssetResolvePartState(&a, 1, &set) == SGA_STATE_MOVING);
}

// A part both states animate is the only case priority decides.
static void TestConflictGoesToPriority(void)
{
    static SgaAsset a;
    StrategyAssetInit(&a, "conflict");
    a.partCount = 1;

    GiveKeys(&a, 0, SGA_STATE_IDLE);
    GiveKeys(&a, 0, SGA_STATE_ATTACKING);

    SgaStateSet set;
    StrategyAssetStateSetInit(&set);
    StrategyAssetStateSetAdd(&set, SGA_STATE_IDLE, 0.0f);
    StrategyAssetStateSetAdd(&set, SGA_STATE_ATTACKING, 0.0f);
    CHECK(StrategyAssetResolvePartState(&a, 0, &set) == SGA_STATE_ATTACKING);

    // Drop the louder state and the part falls back rather than freezing.
    StrategyAssetStateSetInit(&set);
    StrategyAssetStateSetAdd(&set, SGA_STATE_IDLE, 0.0f);
    CHECK(StrategyAssetResolvePartState(&a, 0, &set) == SGA_STATE_IDLE);
}

// An active state with no keys on a part must not claim it - otherwise a unit
// that starts walking would freeze every part the walk does not animate.
static void TestUnanimatedPartFallsToRest(void)
{
    static SgaAsset a;
    StrategyAssetInit(&a, "rest");
    a.partCount = 2;
    GiveKeys(&a, 0, SGA_STATE_MOVING);      // part 1 is animated by nothing

    SgaStateSet set;
    StrategyAssetStateSetInit(&set);
    StrategyAssetStateSetAdd(&set, SGA_STATE_MOVING, 0.0f);

    CHECK(StrategyAssetResolvePartState(&a, 0, &set) == SGA_STATE_MOVING);
    CHECK(StrategyAssetResolvePartState(&a, 1, &set) == -1);

    // -1 means rest pose, and rest is identity - the draw path relies on this.
    Vector3 off, rot, scl;
    StrategyAssetPartPose(&a, 1, SGA_STATE_MOVING, 0.5f, &off, &rot, &scl);
    CHECK_NEAR(off.x, 0.0f); CHECK_NEAR(off.y, 0.0f); CHECK_NEAR(off.z, 0.0f);
    CHECK_NEAR(scl.x, 1.0f); CHECK_NEAR(scl.y, 1.0f); CHECK_NEAR(scl.z, 1.0f);
}

// A one-shot must hand the part back when it ends, not hold it forever.
static void TestOneShotReleasesPart(void)
{
    static SgaAsset a;
    StrategyAssetInit(&a, "oneshot");
    a.partCount = 1;
    GiveKeys(&a, 0, SGA_STATE_IDLE);
    GiveKeys(&a, 0, SGA_STATE_DAMAGED);

    SgaStateSet set;
    StrategyAssetStateSetInit(&set);
    StrategyAssetStateSetAdd(&set, SGA_STATE_IDLE, 0.0f);
    StrategyAssetStateSetAdd(&set, SGA_STATE_DAMAGED, 0.0f);
    CHECK(StrategyAssetResolvePartState(&a, 0, &set) == SGA_STATE_DAMAGED);

    StrategyAssetStateSetInit(&set);
    StrategyAssetStateSetAdd(&set, SGA_STATE_IDLE, 0.0f);
    CHECK(StrategyAssetResolvePartState(&a, 0, &set) == SGA_STATE_IDLE);
}

// An empty set is not a crash and not a random pose - every part rests.
static void TestEmptySetRestsEverything(void)
{
    static SgaAsset a;
    StrategyAssetInit(&a, "empty");
    a.partCount = 2;
    GiveKeys(&a, 0, SGA_STATE_IDLE);

    SgaStateSet set;
    StrategyAssetStateSetInit(&set);
    CHECK(StrategyAssetResolvePartState(&a, 0, &set) == -1);
    CHECK(StrategyAssetResolvePartState(&a, 1, &set) == -1);

    // Out-of-range parts and a NULL asset resolve to rest too, since the draw
    // loop calls this for every part without pre-checking.
    CHECK(StrategyAssetResolvePartState(&a, -1, &set) == -1);
    CHECK(StrategyAssetResolvePartState(&a, 99, &set) == -1);
    CHECK(StrategyAssetResolvePartState(NULL, 0, &set) == -1);
}

// The single-state draw is implemented in terms of the multi-state one, so the
// old entry point has to keep behaving exactly as it did.
static void TestSingleStateSetMatchesLegacy(void)
{
    static SgaAsset a;
    StrategyAssetInit(&a, "legacy");
    a.partCount = 1;
    GiveKeys(&a, 0, SGA_STATE_MOVING);

    SgaStateSet set;
    StrategyAssetStateSetInit(&set);
    StrategyAssetStateSetAdd(&set, SGA_STATE_MOVING, 0.25f);

    CHECK(StrategyAssetResolvePartState(&a, 0, &set) == SGA_STATE_MOVING);
    CHECK_NEAR(set.slot[SGA_STATE_MOVING].time, 0.25f);
    CHECK(set.slot[SGA_STATE_MOVING].active);
    CHECK(!set.slot[SGA_STATE_IDLE].active);
}

// The catalog is the game's only route to an asset, so its miss cases matter as
// much as its hits: every one of them means "draw the built-in".
static void TestCatalogRoleResolution(void)
{
    StrategyBindingsClear();

    static SgaAsset assets[2];
    StrategyAssetInit(&assets[0], "alpha");
    StrategyAssetInit(&assets[1], "beta");

    CHECK(StrategyBindingResolve(SGB_ROLE_UNIT, 0, assets, 2) == NULL);

    CHECK(StrategyBindingSet(SGB_ROLE_UNIT, 0, "beta"));
    CHECK(StrategyBindingResolve(SGB_ROLE_UNIT, 0, assets, 2) == &assets[1]);

    // Bound to something not on this machine: falls back, keeps the binding.
    CHECK(StrategyBindingSet(SGB_ROLE_UNIT, 1, "ghost"));
    CHECK(StrategyBindingResolve(SGB_ROLE_UNIT, 1, assets, 2) == NULL);
    CHECK(StrategyBindingIsBound(SGB_ROLE_UNIT, 1));

    StrategyBindingsClear();
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
    TestPartialTintKeepsAlpha();

    // Phase 3: keyframes and easing baking.
    TestKeysStaySorted();
    TestPerKeyOffset();
    TestAddKeySamplesCurrentPose();
    TestBakeBuiltinKeepsShape();
    TestBakeDedupes();
    TestCustomEaseSurvivesSlotDeletion();
    TestCompactEases();

    // Phase 4: role bindings.
    TestBindingsSetGetClear();
    TestBindingResolveByName();
    TestBindingMissingIsKept();
    TestBindingsRename();
    TestBindingsRoundTrip();
    TestBindingsVersionMismatch();
    TestBindingsMissingFileIsFine();

    // Phase 5: concurrent per-part state resolution.
    TestStatePriorityIsTotalOrder();
    TestConcurrentStatesDoNotFight();
    TestConflictGoesToPriority();
    TestUnanimatedPartFallsToRest();
    TestOneShotReleasesPart();
    TestEmptySetRestsEverything();
    TestSingleStateSetMatchesLegacy();
    TestCatalogRoleResolution();

    printf("sga_tests: %d checks, %d failures\n", s_checks, s_fails);
    return (s_fails == 0) ? 0 : 1;
}
