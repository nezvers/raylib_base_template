// ============================================================================
//  strategy_effects.c  -  tiny procedural 3D effect pool
//
//  Every game action spawns one of four effect kinds (see strategy_world.h).
//  Slots are ring-scanned for a free entry; when the pool is full the spawn
//  is silently dropped (cosmetic only, never gameplay-critical).
//  All drawing happens inside the caller's BeginMode3D block.
// ============================================================================

#include "strategy_world.h"
#include "raymath.h"
#include <stddef.h>

typedef struct {
    bool       active;
    EffectKind kind;
    Vector3    pos;
    Vector3    pos2;        // FX_BEAM endpoint
    Vector3    vel;         // FX_PUFF drift
    float      life;        // counts DOWN to 0
    float      maxLife;
    Color      color;
} Effect;

static Effect effects[STRAT_MAX_EFFECTS];

void EffectsReset(void)
{
    for (int i = 0; i < STRAT_MAX_EFFECTS; i++) effects[i].active = false;
}

static Effect *EffectAlloc(void)
{
    for (int i = 0; i < STRAT_MAX_EFFECTS; i++)
    {
        if (!effects[i].active) return &effects[i];
    }
    return NULL;    // pool full: drop the effect
}

// Per-kind lifetimes: beams are a flash, rings/puffs linger a little.
static float EffectLife(EffectKind kind)
{
    switch (kind)
    {
        case FX_BEAM:  return 0.15f;
        case FX_FLASH: return 0.30f;
        case FX_RING:  return 0.60f;
        case FX_PUFF:  return 0.80f;
        default:       return 0.50f;
    }
}

void EffectSpawn(EffectKind kind, Vector3 pos, Color color)
{
    Effect *e = EffectAlloc();
    if (e == NULL) return;

    e->active  = true;
    e->kind    = kind;
    e->pos     = pos;
    e->pos2    = pos;
    e->vel     = (Vector3){ 0.0f, 0.0f, 0.0f };
    e->maxLife = EffectLife(kind);
    e->life    = e->maxLife;
    e->color   = color;

    if (kind == FX_PUFF)
    {
        // Random upward drift so multiple puffs from one event spread out.
        e->vel = (Vector3){
            (float)GetRandomValue(-100, 100)*0.01f,
            1.5f + (float)GetRandomValue(0, 100)*0.01f,
            (float)GetRandomValue(-100, 100)*0.01f,
        };
    }
}

void EffectSpawnBeam(Vector3 from, Vector3 to, Color color)
{
    Effect *e = EffectAlloc();
    if (e == NULL) return;

    e->active  = true;
    e->kind    = FX_BEAM;
    e->pos     = from;
    e->pos2    = to;
    e->vel     = (Vector3){ 0.0f, 0.0f, 0.0f };
    e->maxLife = EffectLife(FX_BEAM);
    e->life    = e->maxLife;
    e->color   = color;
}

// Composite templar blessing: a gold ring plus a gentle burst of gold puffs
// (each puff already gets its own random upward drift in EffectSpawn).
void EffectSpawnBless(Vector3 pos)
{
    EffectSpawn(FX_RING, pos, GOLD);
    for (int i = 0; i < 5; i++)
    {
        EffectSpawn(FX_PUFF, (Vector3){ pos.x, 0.9f, pos.z }, GOLD);
    }
}

void EffectsUpdate(float dt)
{
    for (int i = 0; i < STRAT_MAX_EFFECTS; i++)
    {
        Effect *e = &effects[i];
        if (!e->active) continue;

        e->life -= dt;
        if (e->life <= 0.0f)
        {
            e->active = false;
            continue;
        }
        if (e->kind == FX_PUFF)
        {
            e->pos = Vector3Add(e->pos, Vector3Scale(e->vel, dt));
        }
    }
}

void EffectsDraw3D(void)
{
    for (int i = 0; i < STRAT_MAX_EFFECTS; i++)
    {
        Effect *e = &effects[i];
        if (!e->active) continue;

        float a = e->life/e->maxLife;       // 1 -> 0 remaining life
        float t = 1.0f - a;                 // 0 -> 1 progress

        switch (e->kind)
        {
            case FX_RING:
            {
                // Expanding circle laid flat on the ground (rotate the XY
                // circle 90 degrees around X), slightly lifted to avoid
                // z-fighting with the plane.
                Vector3 p = (Vector3){ e->pos.x, 0.03f, e->pos.z };
                float radius = 0.2f + 1.2f*t;
                DrawCircle3D(p, radius, (Vector3){ 1.0f, 0.0f, 0.0f }, 90.0f,
                             Fade(e->color, a));
            } break;

            case FX_PUFF:
            {
                float size = 0.28f*a;
                DrawCube(e->pos, size, size, size, Fade(e->color, a));
            } break;

            case FX_FLASH:
            {
                float radius = 0.25f + 0.35f*t;
                DrawSphereWires(e->pos, radius, 6, 8, Fade(e->color, a));
            } break;

            case FX_BEAM:
            {
                DrawLine3D(e->pos, e->pos2, Fade(e->color, a));
            } break;
        }
    }
}
