// ============================================================================
//  anim_stage.c  -  the playback stage (see anim_stage.h)
//
//  One fixed pool of slots. Each slot is the exact trio the anim editor drives
//  by hand in its Update/Draw (anim_editor.c): a document, a timeline player,
//  and a signal player that overrides the timeline for the properties it
//  targets. The only things this module adds on top are a draw LAYER, an owned
//  copy of the document, and the terminal-signal rule that lets a signal end
//  the instance.
// ============================================================================

#include "anim_stage.h"
#include "../anim/anim.h"
#include "../anim/anim_io.h"
#include "../signal/anim_signal.h"
#include "raylib.h"     // TextFormat
#include <stddef.h>

// Animations are read from the SAME writable dir the editor saves to, so an
// edit is live in game without a copy step (see ANIM_DIR in anim_editor.c).
#define ANIM_STAGE_DIR "anims"
#define ANIM_STAGE_EXT ".cfg"

typedef struct {
    AnimDoc          doc;        // owned copy: an AnimDoc is a plain value
    AnimPlayer       player;
    AnimSignalPlayer sigPlayer;  // the running signal, if any
    float            docTime;    // live sample time; AnimSignalRegister holds
                                 // a POINTER to this, so a signal fired later
                                 // captures the pose actually on screen
    float            delay;      // seconds still to wait before starting; 0 =
                                 // running. A waiting slot is active but is
                                 // neither advanced nor drawn (see the header).
    int              layer;
    int              generation; // bumped per start; encoded into the handle so
                                 // a stale handle can never address a new play
    bool             active;
    void (*onDone)(void *user);
    void            *user;
} StageSlot;

static StageSlot s_slots[ANIM_STAGE_SLOTS_MAX];

// --- handles ---------------------------------------------------------------
// A handle packs (generation, slot) so that reusing a slot invalidates every
// handle to what played there before. Slot lives in the low bits.
#define HANDLE_SLOT_BITS 8
#define HANDLE_SLOT_MASK ((1 << HANDLE_SLOT_BITS) - 1)

static AnimHandle MakeHandle(int slot, int generation)
{
    return (AnimHandle)((generation << HANDLE_SLOT_BITS) | slot);
}

// The slot a handle refers to, or NULL if the handle is stale/invalid/idle.
static StageSlot *SlotOf(AnimHandle h)
{
    if (h < 0) return NULL;
    int idx = h & HANDLE_SLOT_MASK;
    if (idx >= ANIM_STAGE_SLOTS_MAX) return NULL;
    StageSlot *s = &s_slots[idx];
    if (!s->active || s->generation != (h >> HANDLE_SLOT_BITS)) return NULL;
    return s;
}

// Tear a slot down and report it. `notify` is false only for AnimStageReset,
// which is a lifecycle wipe rather than an animation ending.
static void Deactivate(StageSlot *s, bool notify)
{
    if (!s->active) return;
    AnimSignalUnregister(&s->doc, &s->sigPlayer);
    s->active           = false;
    s->player.playing   = false;
    s->sigPlayer.playing = false;

    void (*fn)(void *) = s->onDone;
    void *user = s->user;
    s->onDone = NULL;   // cleared FIRST: a callback that starts a new animation
    s->user   = NULL;   // may land on this very slot, and must not be re-fired
    if (notify && fn) fn(user);
}

void AnimStageReset(void)
{
    for (int i = 0; i < ANIM_STAGE_SLOTS_MAX; i++)
    {
        Deactivate(&s_slots[i], false);
        s_slots[i] = (StageSlot){0};
    }
}

AnimHandle AnimStagePlay(const char *name, bool loop, int layer)
{
    return AnimStagePlayEx(name, loop, layer, 0.0f);
}

AnimHandle AnimStagePlayEx(const char *name, bool loop, int layer, float delay)
{
    if (!name || !name[0]) return ANIM_HANDLE_NONE;

    int idx = -1;
    for (int i = 0; i < ANIM_STAGE_SLOTS_MAX; i++)
        if (!s_slots[i].active) { idx = i; break; }
    if (idx < 0) return ANIM_HANDLE_NONE;   // pool full

    StageSlot *s = &s_slots[idx];
    int generation = s->generation + 1;
    *s = (StageSlot){0};
    s->generation = generation;

    const char *path = TextFormat("%s/%s%s", ANIM_STAGE_DIR, name, ANIM_STAGE_EXT);
    if (!AnimDocLoad(&s->doc, path)) return ANIM_HANDLE_NONE;   // slot left idle

    s->layer  = layer;
    s->active = true;
    s->delay  = (delay > 0.0f) ? delay : 0.0f;
    AnimPlayerStartAll(&s->player, &s->doc, ANIM_FWD);
    s->player.loop = loop;
    s->docTime = AnimPlayerSampleTime(&s->player);

    // Bind this slot's OWN signal player: a global SignalEmit then reaches
    // every instance declaring that name, each blending from its own pose. This
    // happens even while the slot is still waiting out its delay, so an emit
    // during the wait is not silently dropped.
    AnimSignalRegister(&s->doc, &s->sigPlayer, &s->docTime);
    return MakeHandle(idx, generation);
}

void AnimStageStop(AnimHandle h)
{
    StageSlot *s = SlotOf(h);
    if (s) Deactivate(s, true);
}

void AnimStageStopAll(void)
{
    for (int i = 0; i < ANIM_STAGE_SLOTS_MAX; i++) Deactivate(&s_slots[i], true);
}

bool AnimStageAlive(AnimHandle h)
{
    return SlotOf(h) != NULL;
}

void AnimStageSetDoneCallback(AnimHandle h, void (*fn)(void *user), void *user)
{
    StageSlot *s = SlotOf(h);
    if (!s) return;
    s->onDone = fn;
    s->user   = user;
}

void AnimStageEmit(AnimHandle h, const char *name, const SignalParams *params)
{
    StageSlot *s = SlotOf(h);
    if (!s || !name) return;
    for (int i = 0; i < s->doc.signalCount; i++)
    {
        if (TextIsEqual(s->doc.signals[i].name, name))
        {
            AnimSignalPlayerStart(&s->sigPlayer, &s->doc.signals[i],
                                  &s->doc, s->docTime, params);
            return;
        }
    }
}

bool AnimStageEndsOnCurrentSignal(AnimHandle h)
{
    StageSlot *s = SlotOf(h);
    return s && !AnimSignalPlayerDone(&s->sigPlayer)
             && s->sigPlayer.sig && s->sigPlayer.sig->terminal;
}

void AnimStageUpdate(float dt)
{
    for (int i = 0; i < ANIM_STAGE_SLOTS_MAX; i++)
    {
        StageSlot *s = &s_slots[i];
        if (!s->active) continue;

        // Waiting out a start delay: neither the timeline nor a signal moves,
        // and AnimStageDrawOrder skips the slot, so nothing shows. The leftover
        // of the frame that ends the wait is spent on the animation itself, so
        // two copies staggered 0.1s apart stay 0.1s apart at any frame rate.
        float sdt = dt;
        if (s->delay > 0.0f)
        {
            s->delay -= sdt;
            if (s->delay > 0.0f) continue;
            sdt      = -s->delay;
            s->delay = 0.0f;
        }

        // A signal runs on its OWN clock as an override; it does NOT move the
        // timeline, which keeps looping underneath while the signal blends its
        // targets away from it (same model as the editor preview).
        //
        // `sigPlaying` is read BEFORE the update, so the completion edge below
        // is found entirely within this call. A cross-frame "was playing" flag
        // would miss an INSTANT signal (length 0), which is started by an emit
        // and finishes inside its very first update - never observed as playing
        // by any later frame.
        bool sigPlaying = !AnimSignalPlayerDone(&s->sigPlayer);
        if (sigPlaying) AnimSignalPlayerUpdate(&s->sigPlayer, sdt);

        // Completion EDGE of a terminal signal: this is the "end with the
        // signal's allocated time end" case - the instance stops here, having
        // played the authored transition through, rather than being cut off.
        bool sigJustDone = sigPlaying && AnimSignalPlayerDone(&s->sigPlayer);
        if (sigJustDone && s->sigPlayer.sig && s->sigPlayer.sig->terminal)
        {
            Deactivate(s, true);
            continue;
        }

        AnimPlayerUpdate(&s->player, sdt);
        s->docTime = AnimPlayerSampleTime(&s->player);

        // A non-looping doc that ran out ends the instance - unless a signal is
        // still running, which is allowed to finish over the held last frame.
        if (AnimPlayerDone(&s->player) && AnimSignalPlayerDone(&s->sigPlayer))
            Deactivate(s, true);
    }
}

// Build the draw order: active slots, ascending (layer, slot index), so a
// higher layer draws IN FRONT and equal layers keep their start order. A slot
// still waiting out its start delay is omitted - it is alive but not on screen,
// and AnimStageDraw walks exactly this order.
// Insertion sort over <= 8 entries - no allocation. Exposed (not static) so a
// test can assert the ordering without needing to observe actual drawing.
int AnimStageDrawOrder(int *out, int max)
{
    int n = 0;
    for (int i = 0; i < ANIM_STAGE_SLOTS_MAX && n < max; i++)
    {
        if (!s_slots[i].active || s_slots[i].delay > 0.0f) continue;
        int j = n++;
        // walk the sorted prefix back while it sorts AFTER slot i
        while (j > 0 && s_slots[out[j - 1]].layer > s_slots[i].layer)
        {
            out[j] = out[j - 1];
            j--;
        }
        out[j] = i;
    }
    return n;
}

void AnimStageDraw(void)
{
    int order[ANIM_STAGE_SLOTS_MAX];
    int n = AnimStageDrawOrder(order, ANIM_STAGE_SLOTS_MAX);
    for (int i = 0; i < n; i++)
    {
        StageSlot *s = &s_slots[order[i]];
        // Drawn straight over whatever the scene already rendered: what shows
        // through is the document's own authored AP_G_BG_ALPHA (0 = no fill at
        // all), never anything decided here.
        AnimDocDrawEx(&s->doc, AnimPlayerSampleTime(&s->player), &s->sigPlayer);
    }
}

int AnimStageSlotOf(AnimHandle h)
{
    StageSlot *s = SlotOf(h);
    return s ? (int)(s - s_slots) : -1;
}

int AnimStageActiveCount(void)
{
    int n = 0;
    for (int i = 0; i < ANIM_STAGE_SLOTS_MAX; i++) if (s_slots[i].active) n++;
    return n;
}
