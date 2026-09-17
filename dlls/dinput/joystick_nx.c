/* Copyright 2026 Wine-NX contributors. LGPL-2.1-or-later.
 *
 * The Switch controller as a DirectInput joystick. Horizon has no Windows HID
 * gamepad, so 2006 titles that EnumDevices for a joystick (Barnyard) otherwise
 * see only the keyboard and mouse. One unix poll of libnx fills DIJOYSTATE;
 * there is no 256-key GetAsyncKeyState scan. */
#include <stdarg.h>
#include <string.h>

#include "windef.h"
#include "winbase.h"
#include "winerror.h"
#include "winternl.h"
#include "dinput.h"
#include "hidusage.h"

#include "dinput_private.h"
#include "device_private.h"
#include "nx_joystick.h"

#include "wine/debug.h"
#include "wine/unixlib.h"

WINE_DEFAULT_DEBUG_CHANNEL(dinput);

#define NX_VID 0x057e
#define NX_PID 0x2009
#define NX_PAD_A      (1u << 0)
#define NX_PAD_B      (1u << 1)
#define NX_PAD_X      (1u << 2)
#define NX_PAD_Y      (1u << 3)
#define NX_PAD_STICKL (1u << 4)
#define NX_PAD_STICKR (1u << 5)
#define NX_PAD_L      (1u << 6)
#define NX_PAD_R      (1u << 7)
#define NX_PAD_ZL     (1u << 8)
#define NX_PAD_ZR     (1u << 9)
#define NX_PAD_PLUS   (1u << 10)
#define NX_PAD_MINUS  (1u << 11)
#define NX_PAD_LEFT   (1u << 12)
#define NX_PAD_UP     (1u << 13)
#define NX_PAD_RIGHT  (1u << 14)
#define NX_PAD_DOWN   (1u << 15)

static const GUID nx_joystick_instance_guid =
{
    0x6e696e78, 0x6a6f, 0x7973,
    { 0x74, 0x69, 0x63, 0x6b, 0x00, 0x00, 0x00, 0x01 }
};

static const struct dinput_device_vtbl nx_joystick_vtbl;

struct nx_joystick
{
    struct dinput_device base;
};

static inline struct nx_joystick *impl_from_IDirectInputDevice8W( IDirectInputDevice8W *iface )
{
    return CONTAINING_RECORD( CONTAINING_RECORD( iface, struct dinput_device, IDirectInputDevice8W_iface ),
                              struct nx_joystick, base );
}

static BOOL nx_joystick_available(void)
{
    static int ready = -1;
    if (ready < 0) ready = !__wine_init_unix_call();
    return ready;
}

static void nx_joystick_fill_instance( DIDEVICEINSTANCEW *instance, DWORD version )
{
    DWORD size = instance->dwSize;

    memset( instance, 0, size );
    instance->dwSize = size;
    instance->guidInstance = nx_joystick_instance_guid;
    instance->guidProduct = dinput_pidvid_guid;
    instance->guidProduct.Data1 = MAKELONG( NX_VID, NX_PID );
    instance->guidFFDriver = GUID_NULL;
    if (version >= 0x0800)
        instance->dwDevType = DI8DEVTYPE_GAMEPAD | (DI8DEVTYPEGAMEPAD_STANDARD << 8) | DIDEVTYPE_HID;
    else
        instance->dwDevType = DIDEVTYPE_JOYSTICK | (DIDEVTYPEJOYSTICK_GAMEPAD << 8) | DIDEVTYPE_HID;
    instance->wUsagePage = HID_USAGE_PAGE_GENERIC;
    instance->wUsage = HID_USAGE_GENERIC_GAMEPAD;
    lstrcpynW( instance->tszInstanceName, L"Nintendo Switch Controller", MAX_PATH );
    lstrcpynW( instance->tszProductName, L"Nintendo Switch Controller", MAX_PATH );
}

static BOOL nx_joystick_guid_matches( const GUID *guid )
{
    DIDEVICEINSTANCEW instance = {.dwSize = sizeof(instance)};

    if (!guid || !nx_joystick_available()) return FALSE;
    nx_joystick_fill_instance( &instance, 0x0800 );
    return IsEqualGUID( guid, &instance.guidInstance )
        || IsEqualGUID( guid, &instance.guidProduct )
        || IsEqualGUID( guid, &GUID_Joystick );
}

HRESULT nx_joystick_enum_device( DWORD type, DWORD flags, DIDEVICEINSTANCEW *instance, DWORD version, int index )
{
    TRACE( "type %#lx, flags %#lx, instance %p, version %#lx, index %d\n", type, flags, instance, version, index );

    if (index || !nx_joystick_available()) return DIERR_DEVICENOTREG;
    nx_joystick_fill_instance( instance, version );
    return DI_OK;
}

static BOOL try_enum_object( struct dinput_device *impl, const DIPROPHEADER *filter, DWORD flags,
                             enum_object_callback callback, UINT index, DIDEVICEOBJECTINSTANCEW *instance, void *data )
{
    if (flags != DIDFT_ALL && !(flags & DIDFT_GETTYPE( instance->dwType ))) return DIENUM_CONTINUE;

    switch (filter->dwHow)
    {
    case DIPH_DEVICE:
        return callback( impl, index, NULL, instance, data );
    case DIPH_BYOFFSET:
        if (filter->dwObj != instance->dwOfs) return DIENUM_CONTINUE;
        return callback( impl, index, NULL, instance, data );
    case DIPH_BYID:
        if ((filter->dwObj & 0x00ffffff) != (instance->dwType & 0x00ffffff)) return DIENUM_CONTINUE;
        return callback( impl, index, NULL, instance, data );
    }

    return DIENUM_CONTINUE;
}

static HRESULT nx_joystick_enum_objects( IDirectInputDevice8W *iface, const DIPROPHEADER *filter,
                                         DWORD flags, enum_object_callback callback, void *context )
{
    struct nx_joystick *impl = impl_from_IDirectInputDevice8W( iface );
    DIDEVICEOBJECTINSTANCEW instances[] =
    {
        {
            .dwSize = sizeof(DIDEVICEOBJECTINSTANCEW),
            .guidType = GUID_XAxis,
            .dwOfs = DIJOFS_X,
            .dwType = DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE( 0 ),
            .dwFlags = DIDOI_ASPECTPOSITION,
            .tszName = L"X Axis",
            .wUsagePage = HID_USAGE_PAGE_GENERIC,
            .wUsage = HID_USAGE_GENERIC_X,
        },
        {
            .dwSize = sizeof(DIDEVICEOBJECTINSTANCEW),
            .guidType = GUID_YAxis,
            .dwOfs = DIJOFS_Y,
            .dwType = DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE( 1 ),
            .dwFlags = DIDOI_ASPECTPOSITION,
            .tszName = L"Y Axis",
            .wUsagePage = HID_USAGE_PAGE_GENERIC,
            .wUsage = HID_USAGE_GENERIC_Y,
        },
        {
            .dwSize = sizeof(DIDEVICEOBJECTINSTANCEW),
            .guidType = GUID_ZAxis,
            .dwOfs = DIJOFS_Z,
            .dwType = DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE( 2 ),
            .dwFlags = DIDOI_ASPECTPOSITION,
            .tszName = L"Z Axis",
            .wUsagePage = HID_USAGE_PAGE_GENERIC,
            .wUsage = HID_USAGE_GENERIC_Z,
        },
        {
            .dwSize = sizeof(DIDEVICEOBJECTINSTANCEW),
            .guidType = GUID_RxAxis,
            .dwOfs = DIJOFS_RX,
            .dwType = DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE( 3 ),
            .dwFlags = DIDOI_ASPECTPOSITION,
            .tszName = L"X Rotation",
            .wUsagePage = HID_USAGE_PAGE_GENERIC,
            .wUsage = HID_USAGE_GENERIC_RX,
        },
        {
            .dwSize = sizeof(DIDEVICEOBJECTINSTANCEW),
            .guidType = GUID_RyAxis,
            .dwOfs = DIJOFS_RY,
            .dwType = DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE( 4 ),
            .dwFlags = DIDOI_ASPECTPOSITION,
            .tszName = L"Y Rotation",
            .wUsagePage = HID_USAGE_PAGE_GENERIC,
            .wUsage = HID_USAGE_GENERIC_RY,
        },
        {
            .dwSize = sizeof(DIDEVICEOBJECTINSTANCEW),
            .guidType = GUID_RzAxis,
            .dwOfs = DIJOFS_RZ,
            .dwType = DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE( 5 ),
            .dwFlags = DIDOI_ASPECTPOSITION,
            .tszName = L"Z Rotation",
            .wUsagePage = HID_USAGE_PAGE_GENERIC,
            .wUsage = HID_USAGE_GENERIC_RZ,
        },
        {
            .dwSize = sizeof(DIDEVICEOBJECTINSTANCEW),
            .guidType = GUID_POV,
            .dwOfs = DIJOFS_POV( 0 ),
            .dwType = DIDFT_POV | DIDFT_MAKEINSTANCE( 0 ),
            .tszName = L"Hat Switch",
            .wUsagePage = HID_USAGE_PAGE_GENERIC,
            .wUsage = HID_USAGE_GENERIC_HATSWITCH,
        },
        {
            .dwSize = sizeof(DIDEVICEOBJECTINSTANCEW),
            .guidType = GUID_Button,
            .dwOfs = DIJOFS_BUTTON( 0 ),
            .dwType = DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE( 0 ),
            .tszName = L"Button 0",
        },
        {
            .dwSize = sizeof(DIDEVICEOBJECTINSTANCEW),
            .guidType = GUID_Button,
            .dwOfs = DIJOFS_BUTTON( 1 ),
            .dwType = DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE( 1 ),
            .tszName = L"Button 1",
        },
        {
            .dwSize = sizeof(DIDEVICEOBJECTINSTANCEW),
            .guidType = GUID_Button,
            .dwOfs = DIJOFS_BUTTON( 2 ),
            .dwType = DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE( 2 ),
            .tszName = L"Button 2",
        },
        {
            .dwSize = sizeof(DIDEVICEOBJECTINSTANCEW),
            .guidType = GUID_Button,
            .dwOfs = DIJOFS_BUTTON( 3 ),
            .dwType = DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE( 3 ),
            .tszName = L"Button 3",
        },
        {
            .dwSize = sizeof(DIDEVICEOBJECTINSTANCEW),
            .guidType = GUID_Button,
            .dwOfs = DIJOFS_BUTTON( 4 ),
            .dwType = DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE( 4 ),
            .tszName = L"Button 4",
        },
        {
            .dwSize = sizeof(DIDEVICEOBJECTINSTANCEW),
            .guidType = GUID_Button,
            .dwOfs = DIJOFS_BUTTON( 5 ),
            .dwType = DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE( 5 ),
            .tszName = L"Button 5",
        },
        {
            .dwSize = sizeof(DIDEVICEOBJECTINSTANCEW),
            .guidType = GUID_Button,
            .dwOfs = DIJOFS_BUTTON( 6 ),
            .dwType = DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE( 6 ),
            .tszName = L"Button 6",
        },
        {
            .dwSize = sizeof(DIDEVICEOBJECTINSTANCEW),
            .guidType = GUID_Button,
            .dwOfs = DIJOFS_BUTTON( 7 ),
            .dwType = DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE( 7 ),
            .tszName = L"Button 7",
        },
        {
            .dwSize = sizeof(DIDEVICEOBJECTINSTANCEW),
            .guidType = GUID_Button,
            .dwOfs = DIJOFS_BUTTON( 8 ),
            .dwType = DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE( 8 ),
            .tszName = L"Button 8",
        },
        {
            .dwSize = sizeof(DIDEVICEOBJECTINSTANCEW),
            .guidType = GUID_Button,
            .dwOfs = DIJOFS_BUTTON( 9 ),
            .dwType = DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE( 9 ),
            .tszName = L"Button 9",
        },
        {
            .dwSize = sizeof(DIDEVICEOBJECTINSTANCEW),
            .guidType = GUID_Button,
            .dwOfs = DIJOFS_BUTTON( 10 ),
            .dwType = DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE( 10 ),
            .tszName = L"Button 10",
        },
        {
            .dwSize = sizeof(DIDEVICEOBJECTINSTANCEW),
            .guidType = GUID_Button,
            .dwOfs = DIJOFS_BUTTON( 11 ),
            .dwType = DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE( 11 ),
            .tszName = L"Button 11",
        },
    };
    DWORD i;
    BOOL ret;

    for (i = 0; i < ARRAY_SIZE( instances ); ++i)
    {
        ret = try_enum_object( &impl->base, filter, flags, callback, i, instances + i, context );
        if (ret != DIENUM_CONTINUE) return DIENUM_STOP;
    }

    return DIENUM_CONTINUE;
}

static HRESULT nx_joystick_get_property( IDirectInputDevice8W *iface, DWORD property,
                                         DIPROPHEADER *header, const DIDEVICEOBJECTINSTANCEW *instance )
{
    struct nx_joystick *impl = impl_from_IDirectInputDevice8W( iface );

    switch (property)
    {
    case (DWORD_PTR)DIPROP_PRODUCTNAME:
    {
        DIPROPSTRING *value = (DIPROPSTRING *)header;
        lstrcpynW( value->wsz, impl->base.instance.tszProductName, MAX_PATH );
        return DI_OK;
    }
    case (DWORD_PTR)DIPROP_INSTANCENAME:
    {
        DIPROPSTRING *value = (DIPROPSTRING *)header;
        lstrcpynW( value->wsz, impl->base.instance.tszInstanceName, MAX_PATH );
        return DI_OK;
    }
    case (DWORD_PTR)DIPROP_VIDPID:
    {
        DIPROPDWORD *value = (DIPROPDWORD *)header;
        value->dwData = MAKELONG( NX_VID, NX_PID );
        return DI_OK;
    }
    case (DWORD_PTR)DIPROP_JOYSTICKID:
    {
        DIPROPDWORD *value = (DIPROPDWORD *)header;
        value->dwData = 0;
        return DI_OK;
    }
    }
    return DIERR_UNSUPPORTED;
}

static LONG scale_stick_axis( int value, const struct object_properties *properties, BOOL invert )
{
    LONG min = 0, max = 65535;
    INT64 v = invert ? -(INT64)value : value;

    if (properties && properties->range_min != DIPROPRANGE_NOMIN) min = properties->range_min;
    if (properties && properties->range_max != DIPROPRANGE_NOMAX) max = properties->range_max;
    if (v > 32767) v = 32767;
    if (v < -32767) v = -32767;
    return min + (LONG)(((v + 32767) * (INT64)(max - min)) / 65534);
}

static LONG scale_trigger( BOOL pressed, const struct object_properties *properties )
{
    LONG min = 0, max = 65535;

    if (properties && properties->range_min != DIPROPRANGE_NOMIN) min = properties->range_min;
    if (properties && properties->range_max != DIPROPRANGE_NOMAX) max = properties->range_max;
    return pressed ? max : min;
}

static DWORD pov_from_buttons( DWORD buttons )
{
    unsigned int hat = 0;

    if (buttons & NX_PAD_UP) hat |= 1;
    if (buttons & NX_PAD_RIGHT) hat |= 2;
    if (buttons & NX_PAD_DOWN) hat |= 4;
    if (buttons & NX_PAD_LEFT) hat |= 8;
    switch (hat)
    {
    case 1:  return 0;
    case 3:  return 4500;
    case 2:  return 9000;
    case 6:  return 13500;
    case 4:  return 18000;
    case 12: return 22500;
    case 8:  return 27000;
    case 9:  return 31500;
    default: return 0xffffffff;
    }
}

static void nx_set_long( IDirectInputDevice8W *iface, DWORD ofs, DWORD id, LONG value, DWORD time, DWORD seq )
{
    struct nx_joystick *impl = impl_from_IDirectInputDevice8W( iface );
    LONG *slot = (LONG *)(impl->base.device_state + ofs);
    LONG old = *slot;
    int index;

    *slot = value;
    if (old != value && (index = dinput_device_object_index_from_id( iface, id )) >= 0)
        queue_event( iface, index, value, time, seq );
}

static void nx_set_button( IDirectInputDevice8W *iface, unsigned int n, BOOL down, DWORD time, DWORD seq )
{
    struct nx_joystick *impl = impl_from_IDirectInputDevice8W( iface );
    BYTE value = down ? 0x80 : 0;
    BYTE *slot = impl->base.device_state + DIJOFS_BUTTON( n );
    BYTE old = *slot;
    int index;

    *slot = value;
    if (old != value && (index = dinput_device_object_index_from_id( iface, DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE( n ) )) >= 0)
        queue_event( iface, index, value, time, seq );
}

static const struct object_properties *property_at( struct nx_joystick *impl, DWORD ofs )
{
    UINT i;

    for (i = 0; i < impl->base.device_format.dwNumObjs; i++)
        if (impl->base.device_format.rgodf[i].dwOfs == ofs) return impl->base.object_properties + i;
    return NULL;
}

static HRESULT nx_joystick_poll( IDirectInputDevice8W *iface )
{
    static const struct { DWORD nx; unsigned int button; } buttons[] =
    {
        { NX_PAD_B, 0 },      /* bottom: Xbox A / PC confirm */
        { NX_PAD_A, 1 },      /* right: Xbox B */
        { NX_PAD_Y, 2 },      /* left: Xbox X */
        { NX_PAD_X, 3 },      /* top: Xbox Y */
        { NX_PAD_L, 4 },
        { NX_PAD_R, 5 },
        { NX_PAD_MINUS, 6 },  /* Back */
        { NX_PAD_PLUS, 7 },   /* Start */
        { NX_PAD_STICKL, 8 },
        { NX_PAD_STICKR, 9 },
        { NX_PAD_ZL, 10 },
        { NX_PAD_ZR, 11 },
    };
    struct nx_joystick *impl = impl_from_IDirectInputDevice8W( iface );
    struct nx_dinput_state_params params = {0};
    DWORD time, seq;
    unsigned int i;

    if (!nx_joystick_available()) return DI_OK;
    if (WINE_UNIX_CALL( nx_dinput_get_state, &params ) || !params.connected) return DI_OK;

    time = GetCurrentTime();
    EnterCriticalSection( &impl->base.crit );
    seq = impl->base.dinput->evsequence++;
    nx_set_long( iface, DIJOFS_X, DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE( 0 ),
                 scale_stick_axis( params.lx, property_at( impl, DIJOFS_X ), FALSE ), time, seq );
    nx_set_long( iface, DIJOFS_Y, DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE( 1 ),
                 scale_stick_axis( params.ly, property_at( impl, DIJOFS_Y ), TRUE ), time, seq );
    nx_set_long( iface, DIJOFS_Z, DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE( 2 ),
                 scale_trigger( params.buttons & NX_PAD_ZL, property_at( impl, DIJOFS_Z ) ), time, seq );
    nx_set_long( iface, DIJOFS_RX, DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE( 3 ),
                 scale_stick_axis( params.rx, property_at( impl, DIJOFS_RX ), FALSE ), time, seq );
    nx_set_long( iface, DIJOFS_RY, DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE( 4 ),
                 scale_stick_axis( params.ry, property_at( impl, DIJOFS_RY ), TRUE ), time, seq );
    nx_set_long( iface, DIJOFS_RZ, DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE( 5 ),
                 scale_trigger( params.buttons & NX_PAD_ZR, property_at( impl, DIJOFS_RZ ) ), time, seq );
    nx_set_long( iface, DIJOFS_POV( 0 ), DIDFT_POV | DIDFT_MAKEINSTANCE( 0 ),
                 pov_from_buttons( params.buttons ), time, seq );
    for (i = 0; i < ARRAY_SIZE( buttons ); i++)
        nx_set_button( iface, buttons[i].button, params.buttons & buttons[i].nx, time, seq );
    if (impl->base.hEvent) SetEvent( impl->base.hEvent );
    LeaveCriticalSection( &impl->base.crit );
    return DI_OK;
}

static HRESULT nx_joystick_acquire( IDirectInputDevice8W *iface )
{
    TRACE( "iface %p.\n", iface );
    return nx_joystick_poll( iface );
}

static HRESULT nx_joystick_unacquire( IDirectInputDevice8W *iface )
{
    struct nx_joystick *impl = impl_from_IDirectInputDevice8W( iface );
    memset( impl->base.device_state, 0, sizeof(impl->base.device_state) );
    *(LONG *)(impl->base.device_state + DIJOFS_POV( 0 )) = -1;
    return DI_OK;
}

HRESULT nx_joystick_create_device( struct dinput *dinput, const GUID *guid, IDirectInputDevice8W **out )
{
    struct nx_joystick *impl;
    HRESULT hr;
    UINT i;

    TRACE( "dinput %p, guid %s, out %p.\n", dinput, debugstr_guid( guid ), out );

    *out = NULL;
    if (!nx_joystick_guid_matches( guid )) return DIERR_DEVICENOTREG;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    dinput_device_init( &impl->base, &nx_joystick_vtbl, guid, dinput );
    impl->base.crit.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": struct nx_joystick*->base.crit");

    nx_joystick_fill_instance( &impl->base.instance, dinput->dwVersion );
    impl->base.caps.dwDevType = impl->base.instance.dwDevType;
    impl->base.caps.dwFirmwareRevision = 100;
    impl->base.caps.dwHardwareRevision = 100;
    impl->base.dwCoopLevel = DISCL_NONEXCLUSIVE | DISCL_BACKGROUND;

    if (FAILED(hr = dinput_device_init_device_format( &impl->base.IDirectInputDevice8W_iface ))) goto failed;

    for (i = 0; i < impl->base.device_format.dwNumObjs; i++)
    {
        struct object_properties *properties = impl->base.object_properties + i;
        DWORD ofs = impl->base.device_format.rgodf[i].dwOfs;

        if (!(impl->base.device_format.rgodf[i].dwType & DIDFT_AXIS)) continue;
        properties->range_min = 0;
        properties->range_max = 65535;
        properties->saturation = 10000;
        if (ofs == DIJOFS_Z || ofs == DIJOFS_RZ)
            *(LONG *)(impl->base.device_state + ofs) = 0;
        else
            *(LONG *)(impl->base.device_state + ofs) = 32767;
    }
    *(LONG *)(impl->base.device_state + DIJOFS_POV( 0 )) = -1;

    *out = &impl->base.IDirectInputDevice8W_iface;
    return DI_OK;

failed:
    IDirectInputDevice_Release( &impl->base.IDirectInputDevice8W_iface );
    return hr;
}

static const struct dinput_device_vtbl nx_joystick_vtbl =
{
    NULL,
    nx_joystick_poll,
    NULL,
    nx_joystick_acquire,
    nx_joystick_unacquire,
    nx_joystick_enum_objects,
    nx_joystick_get_property,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
};
