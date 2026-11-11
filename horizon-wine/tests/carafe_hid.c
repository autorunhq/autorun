/* Host ASan/UBSan test for carafe_hid.c + winebus.sys/hid.c + hidparse.sys/main.c. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "winioctl.h"
#include "ddk/wdm.h"
#include "ddk/hidclass.h"
#include "xinput.h"
#include "wine/debug.h"

#include "../source/carafe_hid.h"

static XINPUT_GAMEPAD g_fake_pad;
static int g_fake_presented = 1;
static int g_fake_connected = 1;
static int g_reports_notified = 0;
static int g_reg_keys_created = 0;
static int g_reg_values_set = 0;

int wine_nx_xinput_player( XINPUT_GAMEPAD *gamepad )
{
    *gamepad = g_fake_pad;
    return g_fake_connected;
}

int wine_nx_xinput_presented( void )
{
    return g_fake_presented;
}

void wine_nx_runtime_trace( const char *message )
{
    (void)message;
}

void wine_nx_device_reports( void )
{
    g_reports_notified++;
}

unsigned int wine_nx_registry_create_volatile( int hive, const unsigned short *path, unsigned int path_len )
{
    (void)hive;
    assert( path && path_len >= 2 );
    g_reg_keys_created++;
    return 0;
}

unsigned int wine_nx_registry_set( int hive, const unsigned short *path, unsigned int path_len,
                                   const unsigned short *name, unsigned int name_len,
                                   unsigned int type, const void *data, unsigned int size )
{
    (void)hive; (void)path; (void)path_len; (void)name; (void)name_len;
    (void)type; (void)data; (void)size;
    g_reg_values_set++;
    return 0;
}

int __cdecl __wine_dbg_header( enum __wine_debug_class cls, struct __wine_debug_channel *channel,
                               const char *function )
{
    (void)cls; (void)channel; (void)function;
    return 0;
}

int __cdecl __wine_dbg_output( const char *str )
{
    (void)str;
    return 0;
}

const char * __cdecl __wine_dbg_strdup( const char *str )
{
    return str;
}

static unsigned int utf16_from_ascii( const char *s, unsigned short *out, size_t max_chars )
{
    unsigned int i = 0;
    while (s[i] && i < max_chars)
    {
        out[i] = (unsigned char)s[i];
        i++;
    }
    return i * 2;
}

int main( void )
{
    unsigned short name[128];
    unsigned int len;
    void *ig, *xi;
    HID_COLLECTION_INFORMATION info;
    unsigned int out_size = 0;
    unsigned char desc[4096];
    unsigned char buf[256];
    ULONG nbuf = 0;
    unsigned int status;

    const char *ig_link = "\\??\\HID#VID_045E&PID_028E&IG_00#0000#{4d1e55b2-f16f-11cf-88cb-001111000030}";
    const char *ig_lower = "\\??\\hid#vid_045e&pid_028e&ig_00#0000#{4D1E55B2-F16F-11CF-88CB-001111000030}";
    const char *xi_link = "\\??\\HID#VID_045E&PID_028E&XI_00#0000#{6c53d5fd-6480-440f-b618-476750c5e1a6}";

    /* Not presented -> register skips creating registry entries. */
    g_fake_presented = 0;
    carafeHidRegister();
    assert( g_reg_keys_created == 0 );

    /* Presented -> register creates IG_00 and XI_00 registry entries. */
    g_fake_presented = 1;
    carafeHidRegister();
    assert( g_reg_keys_created >= 6 );
    assert( g_reg_values_set >= 10 );

    len = utf16_from_ascii( ig_lower, name, 128 );
    ig = carafeHidOpen( name, len );
    assert( ig != NULL );
    len = utf16_from_ascii( xi_link, name, 128 );
    xi = carafeHidOpen( name, len );
    assert( xi != NULL );
    len = utf16_from_ascii( "\\??\\HID#VID_045E&PID_028E&IG_99#0000#{4d1e55b2-f16f-11cf-88cb-001111000030}",
                            name, 128 );
    assert( carafeHidOpen( name, len ) == NULL );
    (void)ig_link;

    /* Collection info and descriptor for both devices. */
    status = carafeHidIoctl( ig, IOCTL_HID_GET_COLLECTION_INFORMATION, NULL, 0,
                             (unsigned char *)&info, sizeof(info), &out_size );
    assert( status == STATUS_SUCCESS && out_size == sizeof(info) );
    assert( info.VendorID == 0x045e && info.ProductID == 0x028e && info.DescriptorSize > 0 &&
            info.DescriptorSize <= sizeof(desc) );

    status = carafeHidIoctl( ig, IOCTL_HID_GET_COLLECTION_DESCRIPTOR, NULL, 0,
                             desc, sizeof(desc), &out_size );
    assert( status == STATUS_SUCCESS && out_size == info.DescriptorSize );

    status = carafeHidIoctl( xi, IOCTL_HID_GET_COLLECTION_INFORMATION, NULL, 0,
                             (unsigned char *)&info, sizeof(info), &out_size );
    assert( status == STATUS_SUCCESS && out_size == sizeof(info) && info.DescriptorSize > 0 );

    /* Product string & queue sizing. */
    status = carafeHidIoctl( ig, IOCTL_HID_GET_PRODUCT_STRING, NULL, 0, buf, 16, &out_size );
    assert( status == (unsigned int)STATUS_BUFFER_TOO_SMALL && out_size > 16 );
    status = carafeHidIoctl( ig, IOCTL_HID_GET_PRODUCT_STRING, NULL, 0, buf, sizeof(buf), &out_size );
    assert( status == STATUS_SUCCESS && out_size > 2 );
    nbuf = 4;
    status = carafeHidIoctl( ig, IOCTL_SET_NUM_DEVICE_INPUT_BUFFERS, (const unsigned char *)&nbuf,
                             sizeof(nbuf), NULL, 0, &out_size );
    assert( status == STATUS_SUCCESS );
    nbuf = 0;
    status = carafeHidIoctl( ig, IOCTL_GET_NUM_DEVICE_INPUT_BUFFERS, NULL, 0,
                             (unsigned char *)&nbuf, sizeof(nbuf), &out_size );
    assert( status == STATUS_SUCCESS && nbuf == 4 );

    /* Initial open pushes 1 seed report; second read before input returns STATUS_PENDING. */
    status = carafeHidRead( ig, buf, sizeof(buf), &out_size );
    assert( status == STATUS_SUCCESS && out_size == 15 );
    status = carafeHidRead( ig, buf, sizeof(buf), &out_size );
    assert( status == STATUS_PENDING );
    status = carafeHidRead( xi, buf, sizeof(buf), &out_size );
    assert( status == STATUS_SUCCESS && out_size > 0 );
    status = carafeHidRead( xi, buf, sizeof(buf), &out_size );
    assert( status == STATUS_PENDING );

    /* Simulate gamepad change + poll -> both IG and XI emit reports and notify. */
    g_fake_pad.wButtons = XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_DPAD_UP;
    g_fake_pad.bLeftTrigger = 255;
    g_fake_pad.sThumbLX = 32767;
    g_fake_pad.sThumbLY = -32768;
    carafeHidPoll();
    assert( g_reports_notified >= 1 );

    status = carafeHidRead( ig, buf, sizeof(buf), &out_size );
    assert( status == STATUS_SUCCESS && out_size == 15 );
    status = carafeHidRead( xi, buf, sizeof(buf), &out_size );
    assert( status == STATUS_SUCCESS && out_size > 0 );

    /* Write returns INVALID_PARAMETER / INVALID_USER_BUFFER with 0 written. */
    memset( buf, 0, 8 );
    status = carafeHidWrite( xi, buf, 8, &out_size );
    assert( status == (unsigned int)STATUS_INVALID_PARAMETER && out_size == 0 );
    status = carafeHidWrite( xi, NULL, 0, &out_size );
    assert( status == (unsigned int)STATUS_INVALID_USER_BUFFER && out_size == 0 );

    carafeHidClose( ig );
    carafeHidClose( xi );
    puts( "carafe_hid tests passed" );
    return 0;
}
