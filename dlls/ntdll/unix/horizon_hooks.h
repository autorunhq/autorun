/* Copyright 2026 Wine-NX contributors. LGPL-2.1-or-later. */
/*
 * Window hooks for the Horizon server (server/hook.c): which hook a chain
 * runs next, and how many of them a thread's queue reports in hooks_count.
 *
 * DirectInput watches WH_MOUSE_LL and WH_KEYBOARD_LL through
 * SetWindowsHookEx - the input thread of dinput.dll sets them, and the low-
 * level hooks run there wherever the input is sent from. Nothing answered
 * those requests before, so a DirectInput game (Harbinger) saw no mouse or
 * keyboard at all; win32u sends the events to the chain now.
 *
 * The chain runs the calling thread's own hooks first and the global ones
 * after, each newest first, as server/hook.c walks its two tables. remove_hook
 * only clears the proc of a hook a chain is walking through.
 */
#ifndef WINE_NX_HORIZON_HOOKS_H
#define WINE_NX_HORIZON_HOOKS_H

#define HORIZON_WH_MINHOOK      (-1)
#define HORIZON_WH_CBT          5
#define HORIZON_WH_KEYBOARD_LL  13
#define HORIZON_WH_MOUSE_LL     14
#define HORIZON_WH_WINEVENT     15
#define HORIZON_NB_HOOKS        17   /* HORIZON_WH_WINEVENT - HORIZON_WH_MINHOOK + 1 */

#define HORIZON_EVENT_MIN               0
#define HORIZON_EVENT_MAX               0x7fffffff

#define HORIZON_WINEVENT_INCONTEXT      0x0001
#define HORIZON_WINEVENT_SKIPOWNPROCESS 0x0002
#define HORIZON_WINEVENT_SKIPOWNTHREAD  0x0004

struct horizon_hook_entry
{
    struct horizon_hook_entry *next;
    unsigned int handle;
    int id;                       /* WH_* */
    unsigned int pid;             /* the hook is for this process, 0 for any */
    unsigned int tid;             /* thread hook's thread, 0 for a global one */
    unsigned int owner_pid;       /* who set it: low-level hooks run there */
    unsigned int owner_tid;
    int event_min, event_max;
    int flags, unicode;
    unsigned long long proc;
    unsigned char *module;
    unsigned int module_size;
    int chain_count;              /* start_hook_chain holds it */
};

/* Whether the hook runs in a given thread (server/hook.c's run_hook_in_thread). */
static inline int horizon_hook_runs_in( const struct horizon_hook_entry *hook,
                                        unsigned int pid, unsigned int tid )
{
    if (hook->pid && hook->pid != pid) return 0;
    if ((hook->flags & HORIZON_WINEVENT_SKIPOWNPROCESS) && hook->pid == pid) return 0;
    if (hook->tid && hook->tid != tid) return 0;
    if ((hook->flags & HORIZON_WINEVENT_SKIPOWNTHREAD) && hook->tid == tid) return 0;
    return 1;
}

/* Low-level hooks run on the thread that set them, wherever input is sent
 * from (server/hook.c's run_hook_in_owner_thread). */
static inline int horizon_hook_runs_in_owner( const struct horizon_hook_entry *hook,
                                              unsigned int tid )
{
    return (hook->id == HORIZON_WH_MOUSE_LL || hook->id == HORIZON_WH_KEYBOARD_LL) &&
           hook->owner_tid != tid;
}

/* One hook of the chain, the calling thread's own before the global ones and
 * each newest first. "from" is the hook just run (NULL to start), and a hook
 * whose proc remove_hook cleared is skipped, not unlinked. */
static inline struct horizon_hook_entry *horizon_hook_next( struct horizon_hook_entry *hooks,
                                                            struct horizon_hook_entry *from,
                                                            unsigned int pid, unsigned int tid,
                                                            int index, int event )
{
    struct horizon_hook_entry *hook;
    int pass, reached = !from;

    for (pass = 0; pass < 2; pass++)
    {
        for (hook = hooks; hook; hook = hook->next)
        {
            if (hook->id - HORIZON_WH_MINHOOK != index) continue;
            /* pass 0: the thread's own hooks; pass 1: the global ones */
            if ((hook->tid != 0) != (pass == 0)) continue;
            if (!reached)
            {
                if (hook != from) continue;
                reached = 1;
                continue;
            }
            if (!hook->proc || !horizon_hook_runs_in( hook, pid, tid )) continue;
            if (event < hook->event_min || event > hook->event_max) continue;
            return hook;
        }
    }
    return NULL;
}

/* is_hooked() reads the calling thread's queue's hooks_count: how many hooks
 * of an id it can run (server/hook.c's add_queue_hook_count). */
static inline int horizon_hook_count_for( struct horizon_hook_entry *hooks, int index,
                                          unsigned int tid )
{
    struct horizon_hook_entry *hook;
    int count = 0;

    for (hook = hooks; hook; hook = hook->next)
    {
        if (hook->id - HORIZON_WH_MINHOOK != index || !hook->proc) continue;
        if (!hook->tid || hook->tid == tid) count++;
    }
    return count;
}

#endif /* WINE_NX_HORIZON_HOOKS_H */
