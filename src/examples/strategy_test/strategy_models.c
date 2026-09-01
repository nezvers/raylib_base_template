// ============================================================================
//  strategy_models.c  -  the part tables (see strategy_models.h for the why)
//
//  Every table below is a straight port of the matching switch case in
//  strategy_world.c's DrawNode / DrawBuilding / DrawUnit, with one mechanical
//  change: absolute coordinates become LOCAL offsets. Where the original said
//
//      Vector3 head = (Vector3){ u->pos.x, 0.95f, u->pos.z };
//
//  the part here is simply { .offset = { 0.0f, 0.95f, 0.0f } } - the matrix in
//  StrategyModelDraw supplies pos and the yaw.
//
//  NOT INCLUDED, on purpose: the selection ring, the carried-resource cube and
//  the tending hat. Those are per-instance STATE, not the asset - the ring
//  depends on u->selected, the hat reads world.buildings[]. A gallery showing
//  them would be showing a moment, not a model.
//
//  height/radius are hand-measured from the parts and used to frame a preview
//  camera. They do not need to be exact, only honest enough that nothing
//  clips out of its tile.
// ============================================================================

#include "strategy_models.h"
#include "strategy_world.h"     // strategyFactionColor
#include "rlgl.h"
#include <stddef.h>     // NULL

// Colors the originals used inline, named once here.
#define C_LEAF      (Color){  60, 140,  60, 255 }
#define C_WHEAT     (Color){ 220, 190,  90, 255 }
#define C_CORPSE    (Color){ 140,  60,  50, 255 }
#define C_TILLED    (Color){ 150, 110,  60, 255 }

// partCount is derived, never typed by hand: a table and a hand-counted length
// drift the moment a part is added, and the failure is silent (a missing limb,
// or reading past the array). Let the compiler count.
#define COUNTOF(a) ((int)(sizeof(a)/sizeof((a)[0])))

// ----------------------------------------------------------------------------
//  Resource nodes  (ported from DrawNode)
// ----------------------------------------------------------------------------
static const ModelPart treeParts[] = {
    { PART_CYLINDER, .offset = { 0.0f, 0.0f, 0.0f },
      .r0 = 0.12f, .r1 = 0.16f, .h = 0.8f, .sides = 6,
      .role = ROLE_FIXED, .color = BROWN },
    { PART_CYLINDER, .offset = { 0.0f, 0.8f, 0.0f },
      .r0 = 0.0f, .r1 = 0.55f, .h = 1.2f, .sides = 6,
      .role = ROLE_FIXED, .color = C_LEAF },
};

static const ModelPart rockParts[] = {
    { PART_CUBE,       .offset = { 0.0f, 0.3f, 0.0f }, .size = { 0.9f, 0.6f, 0.8f },
      .role = ROLE_FIXED, .color = GRAY },
    { PART_CUBE_WIRES, .offset = { 0.0f, 0.3f, 0.0f }, .size = { 0.9f, 0.6f, 0.8f },
      .role = ROLE_FIXED, .color = DARKGRAY },
};

// The original loops i = 0..3 over a 2x2 grid: x += (i%2 - 0.5)*0.5,
// z += (i/2 - 0.5)*0.5. Unrolled here so the table stays declarative.
static const ModelPart wheatParts[] = {
    { PART_CYLINDER, .offset = { -0.25f, 0.0f, -0.25f },
      .r0 = 0.02f, .r1 = 0.08f, .h = 0.7f, .sides = 5, .role = ROLE_FIXED, .color = C_WHEAT },
    { PART_CYLINDER, .offset = {  0.25f, 0.0f, -0.25f },
      .r0 = 0.02f, .r1 = 0.08f, .h = 0.7f, .sides = 5, .role = ROLE_FIXED, .color = C_WHEAT },
    { PART_CYLINDER, .offset = { -0.25f, 0.0f,  0.25f },
      .r0 = 0.02f, .r1 = 0.08f, .h = 0.7f, .sides = 5, .role = ROLE_FIXED, .color = C_WHEAT },
    { PART_CYLINDER, .offset = {  0.25f, 0.0f,  0.25f },
      .r0 = 0.02f, .r1 = 0.08f, .h = 0.7f, .sides = 5, .role = ROLE_FIXED, .color = C_WHEAT },
};

static const ModelPart corpseParts[] = {
    { PART_CUBE, .offset = { 0.0f, 0.12f, 0.0f }, .size = { 0.7f, 0.24f, 0.5f },
      .role = ROLE_FIXED, .color = C_CORPSE },
};

static const StrategyModel nodeModels[NODE_KIND_COUNT] = {
    [NODE_TREE]   = { "TREE", treeParts, COUNTOF(treeParts), 2.0f, 0.55f },
    [NODE_ROCK]   = { "ROCK", rockParts, COUNTOF(rockParts), 0.6f, 0.45f },
    [NODE_WHEAT]  = { "WHEAT", wheatParts, COUNTOF(wheatParts), 0.7f, 0.33f },
    [NODE_CORPSE] = { "CORPSE", corpseParts, COUNTOF(corpseParts), 0.24f, 0.35f },
};

// ----------------------------------------------------------------------------
//  Buildings  (ported from DrawBuilding)
//
//  Every building ends with the same faction banner: a post at (+0.8, +0.8)
//  with a flag on top. In the original that is shared tail code after the
//  switch; here it is simply the last two parts of each table.
// ----------------------------------------------------------------------------
#define BANNER_PARTS                                                          \
    { PART_LINE, .offset = { 0.8f, 0.0f, 0.8f }, .size = { 0.0f, 1.6f, 0.0f }, \
      .role = ROLE_FACTION },                                                 \
    { PART_CUBE, .offset = { 0.8f, 1.6f, 0.8f }, .size = { 0.25f, 0.18f, 0.05f }, \
      .role = ROLE_FACTION }

static const ModelPart houseParts[] = {
    { PART_CUBE,     .offset = { 0.0f, 0.6f, 0.0f }, .size = { 1.4f, 1.2f, 1.4f },
      .role = ROLE_FIXED, .color = RAYWHITE },
    { PART_CYLINDER, .offset = { 0.0f, 1.2f, 0.0f },
      .r0 = 0.0f, .r1 = 1.1f, .h = 0.8f, .sides = 4, .role = ROLE_FACTION },
    BANNER_PARTS,
};

static const ModelPart loggingParts[] = {
    { PART_CUBE,        .offset = { 0.0f, 0.3f, 0.0f }, .size = { 1.8f, 0.6f, 1.3f },
      .role = ROLE_FIXED, .color = RAYWHITE },
    { PART_CYLINDER_EX, .offset = { -0.7f, 0.75f, 0.0f }, .size = { 1.4f, 0.0f, 0.0f },
      .r0 = 0.18f, .r1 = 0.18f, .sides = 8, .role = ROLE_FIXED, .color = BROWN },
    BANNER_PARTS,
};

static const ModelPart quarryParts[] = {
    { PART_CUBE, .offset = { 0.0f, 0.25f, 0.0f }, .size = { 1.6f, 0.5f, 1.6f },
      .role = ROLE_FIXED, .color = RAYWHITE },
    { PART_CUBE, .offset = { 0.0f, 0.75f, 0.0f }, .size = { 0.6f, 0.5f, 0.6f },
      .role = ROLE_FIXED, .color = DARKGRAY },
    BANNER_PARTS,
};

static const ModelPart barracksParts[] = {
    { PART_CUBE, .offset = { 0.0f, 0.5f,  0.0f }, .size = { 2.0f, 1.0f, 1.4f },
      .role = ROLE_FIXED, .color = RAYWHITE },
    { PART_CUBE, .offset = { 0.0f, 1.15f, 0.0f }, .size = { 2.2f, 0.3f, 1.6f },
      .role = ROLE_FIXED, .color = DARKGRAY },
    BANNER_PARTS,
};

static const ModelPart farmParts[] = {
    { PART_CUBE,     .offset = { 0.0f, 0.08f, 0.0f }, .size = { 2.0f, 0.16f, 2.0f },
      .role = ROLE_FIXED, .color = C_TILLED },
    { PART_CYLINDER, .offset = { -0.5f, 0.0f, -0.5f },
      .r0 = 0.02f, .r1 = 0.07f, .h = 0.6f, .sides = 5, .role = ROLE_FIXED, .color = C_WHEAT },
    { PART_CYLINDER, .offset = {  0.5f, 0.0f, -0.5f },
      .r0 = 0.02f, .r1 = 0.07f, .h = 0.6f, .sides = 5, .role = ROLE_FIXED, .color = C_WHEAT },
    { PART_CYLINDER, .offset = { -0.5f, 0.0f,  0.5f },
      .r0 = 0.02f, .r1 = 0.07f, .h = 0.6f, .sides = 5, .role = ROLE_FIXED, .color = C_WHEAT },
    { PART_CYLINDER, .offset = {  0.5f, 0.0f,  0.5f },
      .r0 = 0.02f, .r1 = 0.07f, .h = 0.6f, .sides = 5, .role = ROLE_FIXED, .color = C_WHEAT },
    BANNER_PARTS,
};

static const ModelPart townHallParts[] = {
    { PART_CUBE, .offset = { 0.0f, 0.7f, 0.0f }, .size = { 2.4f, 1.4f, 2.4f },
      .role = ROLE_FIXED, .color = RAYWHITE },
    { PART_CUBE, .offset = { 0.0f, 1.7f, 0.0f }, .size = { 1.2f, 0.6f, 1.2f },
      .role = ROLE_FACTION },
    BANNER_PARTS,
};

static const ModelPart chantryParts[] = {
    { PART_CUBE,     .offset = { 0.0f, 0.8f, 0.0f }, .size = { 1.2f, 1.6f, 1.2f },
      .role = ROLE_FIXED, .color = RAYWHITE },
    { PART_CYLINDER, .offset = { 0.0f, 1.6f, 0.0f },
      .r0 = 0.0f, .r1 = 0.5f, .h = 1.0f, .sides = 6, .role = ROLE_FIXED, .color = GOLD },
    BANNER_PARTS,
};

static const ModelPart forestryParts[] = {
    { PART_CUBE,     .offset = { 0.0f, 0.35f, 0.0f }, .size = { 1.4f, 0.7f, 1.4f },
      .role = ROLE_FIXED, .color = RAYWHITE },
    { PART_CYLINDER, .offset = { -0.4f, 0.75f, 0.0f },
      .r0 = 0.0f, .r1 = 0.28f, .h = 0.7f, .sides = 6, .role = ROLE_FIXED, .color = C_LEAF },
    { PART_CYLINDER, .offset = {  0.4f, 0.75f, 0.0f },
      .r0 = 0.0f, .r1 = 0.28f, .h = 0.7f, .sides = 6, .role = ROLE_FIXED, .color = C_LEAF },
    BANNER_PARTS,
};

static const StrategyModel buildingModels[BLD_COUNT] = {
    [BLD_HOUSE]     = { "HOUSE", houseParts, COUNTOF(houseParts), 2.0f, 1.2f },
    [BLD_LOGGING]   = { "LOGGING", loggingParts, COUNTOF(loggingParts), 1.8f, 1.2f },
    [BLD_QUARRY]    = { "QUARRY", quarryParts, COUNTOF(quarryParts), 1.8f, 1.2f },
    [BLD_BARRACKS]  = { "BARRACKS", barracksParts, COUNTOF(barracksParts), 1.8f, 1.4f },
    [BLD_FARM]      = { "FARM", farmParts, COUNTOF(farmParts), 1.8f, 1.4f },
    [BLD_TOWN_HALL] = { "TOWN HALL", townHallParts, COUNTOF(townHallParts), 2.0f, 1.7f },
    [BLD_CHANTRY]   = { "CHANTRY", chantryParts, COUNTOF(chantryParts), 2.6f, 1.2f },
    [BLD_FORESTRY]  = { "FORESTRY", forestryParts, COUNTOF(forestryParts), 1.8f, 1.2f },
};

// ----------------------------------------------------------------------------
//  Units  (ported from DrawUnit)
//
//  DrawCylinder's position is its BASE, so a unit body at offset y = 0 stands
//  on the ground exactly as it does in the world.
// ----------------------------------------------------------------------------
static const ModelPart workerParts[] = {
    { PART_CYLINDER, .offset = { 0.0f, 0.0f, 0.0f },
      .r0 = 0.28f, .r1 = STRAT_UNIT_RADIUS, .h = 0.8f, .sides = 8,
      .role = ROLE_FACTION },
    { PART_SPHERE,   .offset = { 0.0f, 0.95f, 0.0f }, .r0 = 0.18f,
      .role = ROLE_FACTION_BRIGHT },
};

static const ModelPart soldierParts[] = {
    { PART_CYLINDER, .offset = { 0.0f, 0.0f, 0.0f },
      .r0 = 0.34f, .r1 = 0.45f, .h = 1.1f, .sides = 8, .role = ROLE_FACTION_DARK },
    { PART_SPHERE,   .offset = { 0.0f, 1.28f, 0.0f }, .r0 = 0.2f, .role = ROLE_FACTION },
};

static const ModelPart rangedParts[] = {
    { PART_CYLINDER, .offset = { 0.0f, 0.0f, 0.0f },
      .r0 = 0.30f, .r1 = 0.40f, .h = 1.0f, .sides = 8, .role = ROLE_FACTION_LIGHT },
    { PART_SPHERE,   .offset = { 0.0f, 1.18f, 0.0f }, .r0 = 0.18f, .role = ROLE_FACTION },
    { PART_LINE,     .offset = { 0.35f, 0.25f, 0.0f }, .size = { 0.0f, 0.8f, 0.0f },
      .role = ROLE_FIXED, .color = DARKBROWN },
};

static const ModelPart templarParts[] = {
    { PART_CYLINDER, .offset = { 0.0f, 0.0f, 0.0f },
      .r0 = 0.26f, .r1 = 0.42f, .h = 1.0f, .sides = 8,
      .role = ROLE_FIXED, .color = RAYWHITE },
    { PART_CUBE,     .offset = { 0.0f, 0.55f, 0.0f }, .size = { 0.5f, 0.12f, 0.5f },
      .role = ROLE_FACTION },
    { PART_SPHERE,   .offset = { 0.0f, 1.16f, 0.0f }, .r0 = 0.17f,
      .role = ROLE_FIXED, .color = GOLD },
};

// Same robe, lime head: the healer's only visual difference.
static const ModelPart healerParts[] = {
    { PART_CYLINDER, .offset = { 0.0f, 0.0f, 0.0f },
      .r0 = 0.26f, .r1 = 0.42f, .h = 1.0f, .sides = 8,
      .role = ROLE_FIXED, .color = RAYWHITE },
    { PART_CUBE,     .offset = { 0.0f, 0.55f, 0.0f }, .size = { 0.5f, 0.12f, 0.5f },
      .role = ROLE_FACTION },
    { PART_SPHERE,   .offset = { 0.0f, 1.16f, 0.0f }, .r0 = 0.17f,
      .role = ROLE_FIXED, .color = LIME },
};

static const ModelPart animalWeakParts[] = {
    { PART_CUBE, .offset = { 0.0f, 0.22f, 0.0f }, .size = { 0.55f, 0.4f, 0.35f },
      .role = ROLE_FACTION },
};

static const ModelPart animalStrongParts[] = {
    { PART_CUBE,       .offset = { 0.0f, 0.34f, 0.0f }, .size = { 0.9f, 0.65f, 0.55f },
      .role = ROLE_FACTION_BEAST },
    { PART_CUBE_WIRES, .offset = { 0.0f, 0.34f, 0.0f }, .size = { 0.9f, 0.65f, 0.55f },
      .role = ROLE_FIXED, .color = DARKBROWN },
};

static const StrategyModel unitModels[UNIT_KIND_COUNT] = {
    [KIND_WORKER]         = { "WORKER", workerParts, COUNTOF(workerParts), 1.13f, 0.35f },
    [KIND_SOLDIER]        = { "SOLDIER", soldierParts, COUNTOF(soldierParts), 1.48f, 0.45f },
    [KIND_RANGED]         = { "RANGED", rangedParts, COUNTOF(rangedParts), 1.36f, 0.40f },
    [KIND_TEMPLAR]        = { "TEMPLAR", templarParts, COUNTOF(templarParts), 1.33f, 0.42f },
    [KIND_TEMPLAR_HEALER] = { "HEALER", healerParts, COUNTOF(healerParts), 1.33f, 0.42f },
    [KIND_ANIMAL_WEAK]    = { "DEER", animalWeakParts, COUNTOF(animalWeakParts), 0.42f, 0.33f },
    [KIND_ANIMAL_STRONG]  = { "BOAR", animalStrongParts, COUNTOF(animalStrongParts), 0.67f, 0.53f },
};

// ----------------------------------------------------------------------------
//  Lookups
// ----------------------------------------------------------------------------
const StrategyModel *StrategyUnitModel(UnitKind kind)
{
    if ((kind < 0) || (kind >= UNIT_KIND_COUNT)) return &unitModels[KIND_WORKER];
    return &unitModels[kind];
}

const StrategyModel *StrategyBuildingModel(BuildingKind kind)
{
    if ((kind < 0) || (kind >= BLD_COUNT)) return &buildingModels[BLD_HOUSE];
    return &buildingModels[kind];
}

const StrategyModel *StrategyNodeModel(NodeKind kind)
{
    if ((kind < 0) || (kind >= NODE_KIND_COUNT)) return &nodeModels[NODE_TREE];
    return &nodeModels[kind];
}

// The guard the world's DrawBuilding never had: FACTION_NEUTRAL is 2 and the
// array holds 2 entries, so neutral must resolve to its own color instead of
// running off the end. BEIGE matches what UnitColor() already returns for it.
Color StrategyFactionTint(int faction)
{
    if ((faction < 0) || (faction >= STRAT_FACTIONS)) return BEIGE;
    return strategyFactionColor[faction];
}

// ----------------------------------------------------------------------------
//  Drawing
// ----------------------------------------------------------------------------
static Color PartColor(const ModelPart *p, int faction, float alpha)
{
    Color base = (Color){ 0, 0, 0, 255 };
    switch (p->role)
    {
        case ROLE_FIXED:          base = p->color; break;
        case ROLE_FACTION:        base = StrategyFactionTint(faction); break;
        case ROLE_FACTION_LIGHT:  base = ColorBrightness(StrategyFactionTint(faction),  0.15f); break;
        case ROLE_FACTION_DARK:   base = ColorBrightness(StrategyFactionTint(faction), -0.25f); break;
        case ROLE_FACTION_BRIGHT: base = ColorBrightness(StrategyFactionTint(faction),  0.30f); break;
        case ROLE_FACTION_BEAST:  base = ColorBrightness(StrategyFactionTint(faction), -0.35f); break;
        default: break;
    }
    return Fade(base, alpha*(float)base.a/255.0f);
}

void StrategyModelDraw(const StrategyModel *model, int faction, Vector3 pos,
                       float yawDeg, float alpha)
{
    if ((model == NULL) || (model->parts == NULL)) return;

    // The matrix is the whole point: DrawCube and friends are axis-aligned and
    // take world coordinates, so the ONLY way to spin a model built from them
    // is to rotate the modelview underneath. Parts then draw at local offsets.
    rlPushMatrix();
    rlTranslatef(pos.x, pos.y, pos.z);
    rlRotatef(yawDeg, 0.0f, 1.0f, 0.0f);

    for (int i = 0; i < model->partCount; i++)
    {
        const ModelPart *p = &model->parts[i];
        Color c = PartColor(p, faction, alpha);
        Vector3 o = p->offset;

        switch (p->kind)
        {
            case PART_CUBE:
                DrawCube(o, p->size.x, p->size.y, p->size.z, c);
                break;

            case PART_CUBE_WIRES:
                DrawCubeWires(o, p->size.x, p->size.y, p->size.z, c);
                break;

            case PART_SPHERE:
                DrawSphere(o, p->r0, c);
                break;

            case PART_CYLINDER:
                DrawCylinder(o, p->r0, p->r1, p->h, p->sides, c);
                break;

            case PART_CYLINDER_EX:
            {
                Vector3 end = { o.x + p->size.x, o.y + p->size.y, o.z + p->size.z };
                DrawCylinderEx(o, end, p->r0, p->r1, p->sides, c);
            } break;

            case PART_LINE:
            {
                Vector3 end = { o.x + p->size.x, o.y + p->size.y, o.z + p->size.z };
                DrawLine3D(o, end, c);
            } break;

            default: break;
        }
    }

    rlPopMatrix();
}
