// ============================================================================
//  signal.c  -  fixed-capacity named-signal dispatch (see signal.h)
// ============================================================================

#include "signal.h"
#include "raylib.h"     // TextIsEqual, TextCopy
#include <stddef.h>

typedef struct {
    char          name[SIGNAL_NAME_MAX];
    SignalHandler fn;
    void         *user;
    bool          used;
} Listener;

static Listener s_listeners[SIGNAL_MAX];
static int      s_count = 0;   // high-water index (slots [0,s_count) may be used)

void SignalReset(void)
{
    for (int i = 0; i < SIGNAL_MAX; i++) s_listeners[i].used = false;
    s_count = 0;
}

static int FindSlot(const char *name, SignalHandler fn, void *user)
{
    for (int i = 0; i < s_count; i++)
        if (s_listeners[i].used && s_listeners[i].fn == fn &&
            s_listeners[i].user == user && TextIsEqual(s_listeners[i].name, name))
            return i;
    return -1;
}

void SignalListen(const char *name, SignalHandler fn, void *user)
{
    if (!name || !fn) return;
    if (FindSlot(name, fn, user) >= 0) return;   // de-dupe identical triple

    // Reuse a freed slot first, else grow the high-water mark.
    int slot = -1;
    for (int i = 0; i < s_count; i++)
        if (!s_listeners[i].used) { slot = i; break; }
    if (slot < 0)
    {
        if (s_count >= SIGNAL_MAX) return;        // full: silently ignore
        slot = s_count++;
    }

    TextCopy(s_listeners[slot].name, name);
    s_listeners[slot].fn   = fn;
    s_listeners[slot].user = user;
    s_listeners[slot].used = true;
}

void SignalStopListening(const char *name, SignalHandler fn, void *user)
{
    int i = FindSlot(name, fn, user);
    if (i >= 0) s_listeners[i].used = false;
}

void SignalEmit(const char *name)
{
    if (!name) return;
    // Snapshot the count so handlers that register/unregister during dispatch
    // don't cause us to skip or double-run existing listeners this pass.
    int n = s_count;
    for (int i = 0; i < n; i++)
        if (s_listeners[i].used && TextIsEqual(s_listeners[i].name, name))
            s_listeners[i].fn(name, s_listeners[i].user);
}

int SignalListenerCount(const char *name)
{
    int c = 0;
    for (int i = 0; i < s_count; i++)
        if (s_listeners[i].used && TextIsEqual(s_listeners[i].name, name)) c++;
    return c;
}
