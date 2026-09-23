/* Host test for the Switch pad as an XInput gamepad (dlls/xinput1_3/nx_pad.h). */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../source/xinput_unix.c"

static int mock_connected;
static u64 mock_buttons, mock_tick;
static HidAnalogStickState mock_sticks[2];

void padConfigureInput(int count, int style) { assert(count == 1 && style == HidNpadStyleSet_NpadStandard); }
void padInitializeDefault(PadState *pad) { (void)pad; }
void padUpdate(PadState *pad) { (void)pad; }
int padIsConnected(PadState *pad) { (void)pad; return mock_connected; }
HidAnalogStickState padGetStickPos(PadState *pad, int index) { (void)pad; return mock_sticks[index]; }
u64 padGetButtons(PadState *pad) { (void)pad; return mock_buttons; }
u64 armGetSystemTick(void) { return mock_tick; }
static int mock_keyboard;
int wine_nx_osk_visible( void ) { return mock_keyboard; }

int main(void)
{
    XINPUT_GAMEPAD gamepad;

    /* Nothing held, sticks centred. */
    memset( &gamepad, 0xcc, sizeof(gamepad) );
    nx_xinput_map( 0, 0, 0, 0, 0, &gamepad );
    assert( !gamepad.wButtons && !gamepad.bLeftTrigger && !gamepad.bRightTrigger );
    assert( !gamepad.sThumbLX && !gamepad.sThumbLY && !gamepad.sThumbRX && !gamepad.sThumbRY );

    /* Face buttons by position: the Switch's bottom B is Xbox A, and so on. */
    nx_xinput_map( NX_PAD_B, 0, 0, 0, 0, &gamepad );
    assert( gamepad.wButtons == XINPUT_GAMEPAD_A );
    nx_xinput_map( NX_PAD_A, 0, 0, 0, 0, &gamepad );
    assert( gamepad.wButtons == XINPUT_GAMEPAD_B );
    nx_xinput_map( NX_PAD_Y, 0, 0, 0, 0, &gamepad );
    assert( gamepad.wButtons == XINPUT_GAMEPAD_X );
    nx_xinput_map( NX_PAD_X, 0, 0, 0, 0, &gamepad );
    assert( gamepad.wButtons == XINPUT_GAMEPAD_Y );

    /* The rest, together. */
    nx_xinput_map( NX_PAD_UP | NX_PAD_DOWN | NX_PAD_LEFT | NX_PAD_RIGHT | NX_PAD_PLUS | NX_PAD_MINUS |
                   NX_PAD_STICKL | NX_PAD_STICKR | NX_PAD_L | NX_PAD_R, 0, 0, 0, 0, &gamepad );
    assert( gamepad.wButtons == (XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_LEFT |
                                 XINPUT_GAMEPAD_DPAD_RIGHT | XINPUT_GAMEPAD_START | XINPUT_GAMEPAD_BACK |
                                 XINPUT_GAMEPAD_LEFT_THUMB | XINPUT_GAMEPAD_RIGHT_THUMB |
                                 XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_RIGHT_SHOULDER) );
    assert( !gamepad.bLeftTrigger && !gamepad.bRightTrigger );

    /* ZL and ZR are the triggers, fully pressed; a racing game accelerates with RT. */
    nx_xinput_map( NX_PAD_ZL | NX_PAD_ZR, 0, 0, 0, 0, &gamepad );
    assert( !gamepad.wButtons && gamepad.bLeftTrigger == 255 && gamepad.bRightTrigger == 255 );

    /* Sticks pass through, up positive as in XInput, clamped to a SHORT. */
    nx_xinput_map( 0, -32767, 32767, 12000, -40000, &gamepad );
    assert( gamepad.sThumbLX == -32767 && gamepad.sThumbLY == 32767 );
    assert( gamepad.sThumbRX == 12000 && gamepad.sThumbRY == -32768 );
    nx_xinput_map( 0, 40000, 0, 0, 0, &gamepad );
    assert( gamepad.sThumbLX == 32767 );

    /* Unix call parameters have no pointers: 32-bit and 64-bit layouts agree. */
    assert( sizeof(struct nx_xinput_state_params) == 24 && sizeof(struct nx_xinput_vibration_params) == 12 );

    {
        struct nx_xinput_state_params state = {0};
        struct nx_xinput_vibration_params vibration = {0};

        assert((UINT_PTR)&state > 0xffffffffu);
        assert(wine_nx_xinput_unix_count == nx_xinput_funcs_count);
        assert(wine_nx_xinput_wow64_unix_count == nx_xinput_funcs_count);
        mock_connected = 1;
        mock_buttons = NX_PAD_B | NX_PAD_ZR;
        mock_sticks[0] = (HidAnalogStickState){123, -456};
        mock_sticks[1] = (HidAnalogStickState){789, -1234};
        mock_tick = 0x100000001ull;
        wine_nx_xinput_unix_funcs[nx_xinput_get_state](&state);
        assert(state.connected && state.state.Gamepad.wButtons == XINPUT_GAMEPAD_A);
        assert(state.state.Gamepad.bRightTrigger == 255 && state.state.Gamepad.sThumbLX == 123);
        assert(wine_nx_xinput_last_poll == mock_tick);
        /* The floating keyboard has the controller while it is up: still
         * there, nothing held. */
        mock_keyboard = 1;
        wine_nx_xinput_unix_funcs[nx_xinput_get_state](&state);
        assert(state.connected && !state.state.Gamepad.wButtons && !state.state.Gamepad.bRightTrigger &&
               !state.state.Gamepad.sThumbLX);
        mock_keyboard = 0;
        wine_nx_xinput_unix_funcs[nx_xinput_set_state](&vibration);
        assert(vibration.connected);
        state.index = 1;
        state.connected = 1;
        wine_nx_xinput_unix_funcs[nx_xinput_get_state](&state);
        assert(!state.connected);
    }

    puts( "XInput Switch pad: mapping, native and WoW64 tables, 64-bit call pointer, layout and the floating keyboard passed" );
    return 0;
}
