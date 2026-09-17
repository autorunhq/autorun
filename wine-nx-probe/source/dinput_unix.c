/* Copyright 2026 Wine-NX contributors. LGPL-2.1-or-later.
 * One libnx pad read for DirectInput's virtual joystick (dlls/dinput/joystick_nx.c). */
#include <stdio.h>
#include <switch.h>
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"
#include "wine/unixlib.h"
#include "../../dlls/dinput/nx_joystick.h"

void wine_nx_horizon_pad_snapshot( unsigned int *connected, unsigned int *buttons,
                                   int *lx, int *ly, int *rx, int *ry );
extern void wine_nx_runtime_trace( const char *msg );

/* While this is recent, the runtime stops turning the d-pad and face buttons
 * into keys (wine_nx_pointer_poll), so the program does not get both. A still
 * clicks for menus that have no pad. */
u64 wine_nx_dinput_last_poll;

static NTSTATUS nx_dinput_get_state_unix( void *args )
{
    struct nx_dinput_state_params *params = args;
    static int logged;
    char line[128];

    wine_nx_horizon_pad_snapshot( &params->connected, &params->buttons,
                                  &params->lx, &params->ly, &params->rx, &params->ry );
    if (params->connected)
    {
        wine_nx_dinput_last_poll = armGetSystemTick();
        if (!logged)
        {
            snprintf( line, sizeof(line),
                      "[NXINPUT] dinput joystick: Switch pad connected stick=%d,%d",
                      params->lx, params->ly );
            wine_nx_runtime_trace( line );
            logged = 1;
        }
    }
    return STATUS_SUCCESS;
}

const unixlib_entry_t wine_nx_dinput_wow64_unix_funcs[] =
{
    nx_dinput_get_state_unix,
};
C_ASSERT( ARRAY_SIZE(wine_nx_dinput_wow64_unix_funcs) == nx_dinput_funcs_count );
const unsigned int wine_nx_dinput_wow64_unix_count = ARRAY_SIZE(wine_nx_dinput_wow64_unix_funcs);
