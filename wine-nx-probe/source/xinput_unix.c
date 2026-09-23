/* Copyright 2026 Wine-NX contributors. LGPL-2.1-or-later.
 * The Switch controller behind Wine's XInput DLLs: player 1's pad, handheld
 * or docked. */
#include <pthread.h>
#include <string.h>
#include <switch.h>
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"
#include "xinput.h"
#include "wine/unixlib.h"
#include "../../dlls/xinput1_3/nx_pad.h"

_Static_assert( NX_PAD_A == HidNpadButton_A && NX_PAD_B == HidNpadButton_B && NX_PAD_X == HidNpadButton_X &&
                NX_PAD_Y == HidNpadButton_Y && NX_PAD_STICKL == HidNpadButton_StickL &&
                NX_PAD_STICKR == HidNpadButton_StickR && NX_PAD_L == HidNpadButton_L &&
                NX_PAD_R == HidNpadButton_R && NX_PAD_ZL == HidNpadButton_ZL && NX_PAD_ZR == HidNpadButton_ZR &&
                NX_PAD_PLUS == HidNpadButton_Plus && NX_PAD_MINUS == HidNpadButton_Minus &&
                NX_PAD_LEFT == HidNpadButton_Left && NX_PAD_UP == HidNpadButton_Up &&
                NX_PAD_RIGHT == HidNpadButton_Right && NX_PAD_DOWN == HidNpadButton_Down,
                "nx_pad.h button bits match libnx" );

/* When a program last read the pad through XInput, in system ticks. While it is
 * recent the runtime stops turning the pad into keys and mouse clicks
 * (wine_nx_pointer_poll in runtime.c), so the program does not get both. */
u64 wine_nx_xinput_last_poll;

static pthread_mutex_t pad_mutex = PTHREAD_MUTEX_INITIALIZER;
/* The floating keyboard (osk.c). */
int wine_nx_osk_visible( void );

static PadState pad;
static int pad_ready;
static XINPUT_GAMEPAD last_gamepad;
static DWORD packet;

static NTSTATUS nx_xinput_get_state_unix( void *args )
{
    struct nx_xinput_state_params *params = args;
    XINPUT_GAMEPAD gamepad;

    params->connected = 0;
    if (params->index) return STATUS_SUCCESS;  /* player 1 only */
    pthread_mutex_lock( &pad_mutex );
    if (!pad_ready)
    {
        padConfigureInput( 1, HidNpadStyleSet_NpadStandard );
        padInitializeDefault( &pad );
        pad_ready = 1;
    }
    padUpdate( &pad );
    if (padIsConnected( &pad ))
    {
        HidAnalogStickState left = padGetStickPos( &pad, 0 ), right = padGetStickPos( &pad, 1 );

        nx_xinput_map( padGetButtons( &pad ), left.x, left.y, right.x, right.y, &gamepad );
        /* The floating keyboard has the controller while it is up (osk.c). */
        if (wine_nx_osk_visible()) memset( &gamepad, 0, sizeof(gamepad) );
        if (memcmp( &gamepad, &last_gamepad, sizeof(gamepad) ))
        {
            last_gamepad = gamepad;
            packet++;
        }
        params->connected = 1;
        params->state.dwPacketNumber = packet;
        params->state.Gamepad = gamepad;
        wine_nx_xinput_last_poll = armGetSystemTick();
    }
    pthread_mutex_unlock( &pad_mutex );
    return STATUS_SUCCESS;
}

/* Rumble is not sent yet; the pad just has to be there. */
static NTSTATUS nx_xinput_set_state_unix( void *args )
{
    struct nx_xinput_vibration_params *params = args;
    struct nx_xinput_state_params state = { .index = params->index };

    nx_xinput_get_state_unix( &state );
    params->connected = state.connected;
    return STATUS_SUCCESS;
}

const unixlib_entry_t wine_nx_xinput_wow64_unix_funcs[] =
{
    nx_xinput_get_state_unix,
    nx_xinput_set_state_unix,
};
C_ASSERT( ARRAY_SIZE(wine_nx_xinput_wow64_unix_funcs) == nx_xinput_funcs_count );
const unsigned int wine_nx_xinput_wow64_unix_count = ARRAY_SIZE(wine_nx_xinput_wow64_unix_funcs);

const unixlib_entry_t wine_nx_xinput_unix_funcs[] =
{
    nx_xinput_get_state_unix,
    nx_xinput_set_state_unix,
};
C_ASSERT( ARRAY_SIZE(wine_nx_xinput_unix_funcs) == nx_xinput_funcs_count );
const unsigned int wine_nx_xinput_unix_count = ARRAY_SIZE(wine_nx_xinput_unix_funcs);
