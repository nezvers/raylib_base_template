// ============================================================================
//  strategy_models.h  -  the strategy art, as DATA instead of code
//
//  The battlefield's units, buildings and resource nodes have always been
//  hand-written stacks of DrawCube/DrawCylinder/DrawSphere calls inside three
//  static functions in strategy_world.c. That works when the only thing that
//  ever draws them is the world itself, and stops working the moment anything
//  else - an asset gallery, a unit portrait, a build preview - wants to draw
//  one somewhere else, at some other angle, in some other faction's colors.
//
//  So each kind is described here as a table of PARTS: a primitive, a LOCAL
//  offset from the model's ground origin, its dimensions, and a COLOR ROLE
//  saying whether the faction tint reaches it. StrategyModelDraw walks that
//  table inside an rlgl matrix, which is what lets a model rotate at all -
//  DrawCube is axis-aligned and cannot turn on its own.
//
//  WHY LOCAL OFFSETS: the originals bake pos.x/pos.z into every vertex
//  ("Vector3 head = { u->pos.x, 0.95f, u->pos.z }"). A model built that way
//  can only ever be drawn at its own position and can never spin. Every offset
//  here is relative to (0,0,0) at the model's feet, and the matrix does the
//  placing.
//
//  SCOPE: this is the gallery's description of the art. strategy_world.c still
//  owns the live rendering and is untouched - the two are deliberately kept
//  separate for now so the showcase cannot regress the game's visuals.
//
//  LATER: these part tables stand in for real 3D model assets. When those
//  arrive, StrategyModelDraw is the single seam that changes; every caller
//  keeps working unchanged.
// ============================================================================

#ifndef STRATEGY_MODELS_H
#define STRATEGY_MODELS_H

#include "raylib.h"
#include "strategy_types.h"

// -- Part primitives ---------------------------------------------------------
typedef enum {
    PART_CUBE = 0,      // size = w/h/l, centered on offset
    PART_CUBE_WIRES,    // wire pass, drawn over a CUBE of the same size
    PART_SPHERE,        // r0 = radius, centered on offset
    PART_CYLINDER,      // r0 -> r1 over h, `sides` segments; a cone when r0 = 0
    PART_CYLINDER_EX,   // bar from offset to offset+size (the LOGGING log)
    PART_LINE,          // line from offset to offset+size (the banner post)
} PartKind;

// -- Color roles -------------------------------------------------------------
// Which parts the faction switcher is allowed to repaint. The brightness
// deltas are the ones already in strategy_world.c's DrawUnit, kept so a
// SOLDIER still reads darker than a WORKER at a glance.
typedef enum {
    ROLE_FIXED = 0,         // .color verbatim: brown log, gold spire, gray stone
    ROLE_FACTION,           // the faction color itself
    ROLE_FACTION_LIGHT,     // faction, +0.15 brightness (RANGED body)
    ROLE_FACTION_DARK,      // faction, -0.25 brightness (SOLDIER body)
    ROLE_FACTION_BRIGHT,    // faction, +0.30 brightness (WORKER head)
    ROLE_FACTION_BEAST,     // faction, -0.35 brightness (ANIMAL_STRONG body)
} ColorRole;

typedef struct {
    PartKind  kind;
    Vector3   offset;       // LOCAL, from the model's ground origin
    Vector3   size;         // CUBE: w/h/l. CYLINDER_EX / LINE: the end offset.
    float     r0, r1, h;    // CYLINDER: radii + height. SPHERE: r0 = radius.
    int       sides;
    ColorRole role;
    Color     color;        // read only when role == ROLE_FIXED
} ModelPart;

typedef struct {
    const char      *name;
    const ModelPart *parts;
    int              partCount;
    float            height;    // tallest point,  for auto-framing a camera
    float            radius;    // widest extent,  likewise
} StrategyModel;

// -- Lookups (mirror StrategyUnitDef / StrategyBuildingDef) ------------------
const StrategyModel *StrategyUnitModel(UnitKind kind);
const StrategyModel *StrategyBuildingModel(BuildingKind kind);
const StrategyModel *StrategyNodeModel(NodeKind kind);

// Faction color, GUARDED, and THE way anything outside strategy_world.c reads
// the palette. strategyFactionColor[] has STRAT_FACTIONS entries and
// FACTION_NEUTRAL is one past the last of them, so indexing it with a neutral
// faction reads out of bounds. Every color in this module resolves through
// here, and so do the map forge and the showcase - which each used to carry
// their own copy of the nine colours.
Color StrategyFactionTint(int faction);

// Draw one model. MUST be called between BeginMode3D/EndMode3D.
// `faction` may be FACTION_NEUTRAL. `alpha` is 0..1 over the whole model.
// THE SEAM: when real 3D assets land, only this function changes.
void StrategyModelDraw(const StrategyModel *model, int faction, Vector3 pos,
                       float yawDeg, float alpha);

#endif // STRATEGY_MODELS_H
