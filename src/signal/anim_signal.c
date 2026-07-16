// ============================================================================
//  anim_signal.c  -  wire AnimDoc signals onto the signal bus (see header)
//
//  A single generic handler (StartFromBinding) is registered once per signal;
//  the per-signal data it needs (which player, which direction, which section)
//  travels through the SignalHandler `user` pointer as a Binding row from a
//  fixed pool here.
// ============================================================================

#include "anim_signal.h"
#include "signal.h"
#include <stddef.h>

#define ANIM_SIGNAL_BINDINGS_MAX 32

typedef struct {
    const AnimDoc *doc;
    AnimPlayer    *player;
    int            dir;
    float          secStart, secEnd;
    bool           used;
} Binding;

static Binding s_bindings[ANIM_SIGNAL_BINDINGS_MAX];

// The one handler all anim-signals share: (re)start the bound player.
static void StartFromBinding(const char *name, void *user)
{
    (void)name;
    Binding *b = (Binding *)user;
    if (!b || !b->used || !b->player) return;
    AnimPlayerStart(b->player, b->doc, b->dir, b->secStart, b->secEnd);
}

static Binding *AllocBinding(void)
{
    for (int i = 0; i < ANIM_SIGNAL_BINDINGS_MAX; i++)
        if (!s_bindings[i].used) return &s_bindings[i];
    return NULL;
}

void AnimSignalRegister(const AnimDoc *doc, AnimPlayer *player)
{
    if (!doc || !player) return;
    for (int i = 0; i < doc->signalCount; i++)
    {
        const AnimSignal *sg = &doc->signals[i];
        Binding *b = AllocBinding();
        if (!b) return;   // pool full
        b->doc      = doc;
        b->player   = player;
        b->dir      = sg->dir;
        b->secStart = sg->sectionStart;
        b->secEnd   = sg->sectionEnd;
        b->used     = true;
        SignalListen(sg->name, StartFromBinding, b);
    }
}

void AnimSignalUnregister(const AnimDoc *doc, AnimPlayer *player)
{
    for (int i = 0; i < ANIM_SIGNAL_BINDINGS_MAX; i++)
    {
        Binding *b = &s_bindings[i];
        if (b->used && b->doc == doc && b->player == player)
        {
            // We don't keep the signal name on the binding, but SignalStop
            // matches on (name, fn, user); the user pointer alone is unique per
            // binding, so stop by scanning the doc's signal names.
            for (int j = 0; j < doc->signalCount; j++)
                SignalStopListening(doc->signals[j].name, StartFromBinding, b);
            b->used = false;
        }
    }
}
