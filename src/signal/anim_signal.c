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

#define ANIM_SIGNAL_BINDINGS_MAX 64

typedef struct {
    const AnimDoc    *doc;
    AnimSignalPlayer *player;
    const AnimSignal *sig;                // POINTER: the signal's targets/keys
                                          // are edited live, so the binding must
                                          // read them at fire time, not copy them
    const float      *docTime;            // clock the doc is drawn at (may be NULL)
    char              name[ANIM_NAME_MAX];// signal name AT registration time
    bool              used;
} Binding;

static Binding s_bindings[ANIM_SIGNAL_BINDINGS_MAX];

// The one handler all anim-signals share: (re)start the bound player, capturing
// the live pose at the document's current time.
static void StartFromBinding(const char *name, void *user,
                             const SignalParams *params)
{
    (void)name;
    Binding *b = (Binding *)user;
    if (!b || !b->used || !b->player) return;
    float t = b->docTime ? *b->docTime : 0.0f;
    AnimSignalPlayerStart(b->player, b->sig, b->doc, t, params);
}

static Binding *AllocBinding(void)
{
    for (int i = 0; i < ANIM_SIGNAL_BINDINGS_MAX; i++)
        if (!s_bindings[i].used) return &s_bindings[i];
    return NULL;
}

void AnimSignalRegister(const AnimDoc *doc, AnimSignalPlayer *player,
                        const float *docTime)
{
    if (!doc || !player) return;
    for (int i = 0; i < doc->signalCount; i++)
    {
        const AnimSignal *sg = &doc->signals[i];
        Binding *b = AllocBinding();
        if (!b) return;   // pool full
        b->doc     = doc;
        b->player  = player;
        b->sig     = sg;
        b->docTime = docTime;
        TextCopy(b->name, sg->name);
        b->used    = true;
        SignalListen(sg->name, StartFromBinding, b);
    }
}

void AnimSignalUnregister(const AnimDoc *doc, AnimSignalPlayer *player)
{
    for (int i = 0; i < ANIM_SIGNAL_BINDINGS_MAX; i++)
    {
        Binding *b = &s_bindings[i];
        if (b->used && b->doc == doc && b->player == player)
        {
            // The binding remembers the name it registered under, so this works
            // even if the doc's signals were renamed/removed since Register.
            SignalStopListening(b->name, StartFromBinding, b);
            b->used = false;
        }
    }
}
