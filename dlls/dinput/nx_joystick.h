/*
 * Wine-NX: the Switch pad as a DirectInput joystick.
 *
 * Shared by dinput's joystick_nx.c (32-bit PE) and the runtime unix call
 * (wine-nx-probe/source/dinput_unix.c). No pointers, so both sides share the
 * layout. Buttons are libnx HidNpadButton bits in the low 32.
 */

#ifndef __WINE_DINPUT_NX_JOYSTICK_H
#define __WINE_DINPUT_NX_JOYSTICK_H

enum nx_dinput_funcs
{
    nx_dinput_get_state,
    nx_dinput_funcs_count
};

struct nx_dinput_state_params
{
    DWORD connected;
    INT lx, ly, rx, ry;
    DWORD buttons;
};

#endif /* __WINE_DINPUT_NX_JOYSTICK_H */
