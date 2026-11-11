/* Copyright 2026 Wine-NX contributors. LGPL-2.1-or-later.
 * The Switch controller behind Wine's XInput DLLs: player 1's pad, handheld
 * or docked. */
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <switch.h>
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"
#include "xinput.h"
#include "wine/unixlib.h"
#include "../../dlls/xinput1_3/nx_pad.h"
#include "input_profile.h"

extern struct input_profile wine_nx_input_profile;
extern uint64_t wine_nx_input_swallowed;

_Static_assert( NX_PAD_A == HidNpadButton_A && NX_PAD_B == HidNpadButton_B && NX_PAD_X == HidNpadButton_X &&
                NX_PAD_Y == HidNpadButton_Y && NX_PAD_STICKL == HidNpadButton_StickL &&
                NX_PAD_STICKR == HidNpadButton_StickR && NX_PAD_L == HidNpadButton_L &&
                NX_PAD_R == HidNpadButton_R && NX_PAD_ZL == HidNpadButton_ZL && NX_PAD_ZR == HidNpadButton_ZR &&
                NX_PAD_PLUS == HidNpadButton_Plus && NX_PAD_MINUS == HidNpadButton_Minus &&
                NX_PAD_LEFT == HidNpadButton_Left && NX_PAD_UP == HidNpadButton_Up &&
                NX_PAD_RIGHT == HidNpadButton_Right && NX_PAD_DOWN == HidNpadButton_Down,
                "nx_pad.h button bits match libnx" );

static pthread_mutex_t pad_mutex = PTHREAD_MUTEX_INITIALIZER;
/* The floating keyboard and the controls/tasks panels (osk.c). */
int wine_nx_osk_visible( void );
int wine_nx_osk_map_visible( void );

static PadState pad;
static int pad_ready;
static XINPUT_GAMEPAD last_gamepad;
static DWORD packet;

static void nx_xinput_note( const struct nx_xinput_state_params *params )
{
    extern void wine_nx_runtime_trace( const char *msg );
    static int read_noted, press_noted;
    char message[96];

    if (!__atomic_load_n( &read_noted, __ATOMIC_RELAXED ) &&
        !__atomic_exchange_n( &read_noted, 1, __ATOMIC_RELAXED ))
    {
        snprintf( message, sizeof(message), "[XINPUT] the game reads player 1: %s",
                  params->connected ? "the pad is there" : "no pad" );
        wine_nx_runtime_trace( message );
    }
    if (params->connected && params->state.Gamepad.wButtons && !__atomic_load_n( &press_noted, __ATOMIC_RELAXED ) &&
        !__atomic_exchange_n( &press_noted, 1, __ATOMIC_RELAXED ))
    {
        snprintf( message, sizeof(message), "[XINPUT] the first press reaches the game: buttons %#x",
                  params->state.Gamepad.wButtons );
        wine_nx_runtime_trace( message );
    }
}

static void nx_xinput_read( struct nx_xinput_state_params *params )
{
    XINPUT_GAMEPAD gamepad;

    params->connected = 0;
    memset( &params->state, 0, sizeof(params->state) );
    if (params->index || wine_nx_input_profile.mode == INPUT_KEYBOARD_MOUSE) return;
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

        struct input_pad raw = { padGetButtons( &pad ), { left.x, left.y, right.x, right.y } }, mapped;
        const u64 keyboard = HidNpadButton_Minus | HidNpadButton_B;
        const u64 controls = HidNpadButton_Minus | HidNpadButton_A;
        const u64 tasks = HidNpadButton_Minus | HidNpadButton_Y;

        raw.buttons &= ~__atomic_load_n( &wine_nx_input_swallowed, __ATOMIC_RELAXED );
        if ((raw.buttons & keyboard) == keyboard) raw.buttons &= ~keyboard;
        if ((raw.buttons & controls) == controls) raw.buttons &= ~controls;
        if ((raw.buttons & tasks) == tasks) raw.buttons &= ~tasks;
        input_map_pad( &wine_nx_input_profile, &raw, &mapped );
        nx_xinput_map( mapped.buttons, mapped.axis[0], mapped.axis[1], mapped.axis[2], mapped.axis[3], &gamepad );
        /* The floating keyboard, and the panels, have the controller while
         * any of them is up (osk.c): reading a panel does not play. */
        if (wine_nx_osk_visible() || wine_nx_osk_map_visible())
            memset( &gamepad, 0, sizeof(gamepad) );
        if (memcmp( &gamepad, &last_gamepad, sizeof(gamepad) ))
        {
            last_gamepad = gamepad;
            packet++;
        }
        params->connected = 1;
        params->state.dwPacketNumber = packet;
        params->state.Gamepad = gamepad;
    }
    pthread_mutex_unlock( &pad_mutex );
}

int wine_nx_xinput_presented( void )
{
    return wine_nx_input_profile.mode != INPUT_KEYBOARD_MOUSE;
}

int wine_nx_xinput_player( XINPUT_GAMEPAD *gamepad )
{
    struct nx_xinput_state_params params = { .index = 0 };

    nx_xinput_read( &params );
    *gamepad = params.state.Gamepad;
    return params.connected;
}

static NTSTATUS nx_xinput_get_state_unix( void *args )
{
    struct nx_xinput_state_params *params = args;

    nx_xinput_read( params );
    nx_xinput_note( params );
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
