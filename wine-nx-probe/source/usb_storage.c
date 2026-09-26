/* Copyright 2026 Wine-NX contributors. LGPL-2.1-or-later.
 * USB drives through libusbhsfs: FAT and exFAT volumes mount as ums0: to ums4:,
 * which dlls/ntdll/unix/file.c maps to D: to H:. */
#include <stdio.h>
#include <string.h>
#include <switch.h>
#include <usbhsfs.h>

#include "launcher.h"

extern void wine_nx_runtime_trace( const char *msg );

#define USB_WAIT_NS 5000000000ULL

static int usb_started;
static Mutex usb_mutex;
static UsbHsFsDevice usb_devices[LAUNCHER_MAX_USB_VOLUMES];
static u32 usb_device_count;

void usbHsFsRequestTransferError(u32 endpoint, Result rc)
{
    char line[96];
    snprintf(line, sizeof(line), "[USB] endpoint 0x%02x closed after incomplete transfer: 0x%08x",
             (unsigned)endpoint, (unsigned)rc);
    wine_nx_runtime_trace(line);
}

static void usb_status_changed( const UsbHsFsDevice *devices, u32 count, void *user_data )
{
    (void)user_data;
    if (count > LAUNCHER_MAX_USB_VOLUMES) count = LAUNCHER_MAX_USB_VOLUMES;
    mutexLock( &usb_mutex );
    usb_device_count = devices ? count : 0;
    if (usb_device_count) memcpy( usb_devices, devices, usb_device_count * sizeof(*devices) );
    mutexUnlock( &usb_mutex );
    wine_nx_launcher_usb_changed();
}

void wine_nx_usb_start(void)
{
    Result rc;
    char buf[64];

    mutexInit( &usb_mutex );
    usbHsFsSetFileSystemMountFlags( UsbHsFsMountFlags_ReplayJournal | UsbHsFsMountFlags_ShowHiddenFiles );
    usbHsFsSetPopulateCallback( usb_status_changed, NULL );
    rc = usbHsFsInitialize( 0 );
    usb_started = R_SUCCEEDED( rc );
    if (usb_started) return;
    usbHsFsSetPopulateCallback( NULL, NULL );
    snprintf( buf, sizeof(buf), "[USB] mass storage unavailable: 0x%08x", (unsigned)rc );
    wine_nx_runtime_trace( buf );
}

void wine_nx_usb_stop(void)
{
    if (!usb_started) return;
    usbHsFsSetPopulateCallback( NULL, NULL );
    usbHsFsExit();
    usb_started = 0;
    usb_device_count = 0;
}

/* Drives mount on libusbhsfs' own thread. Wait for the attached ones, up to
 * five seconds, so a program on a drive plugged in before starting is found. */
void wine_nx_usb_wait(void)
{
    UsbHsInterfaceFilter filter = { .Flags = UsbHsInterfaceFilterFlags_bInterfaceClass,
                                    .bInterfaceClass = USB_CLASS_MASS_STORAGE };
    UsbHsInterface interfaces[8];
    UsbHsFsDevice devices[LAUNCHER_MAX_USB_VOLUMES];
    u64 start = armGetSystemTick();
    s32 attached = 0;
    u32 i, count;
    char buf[200];

    if (!usb_started) return;
    if (R_FAILED( usbHsQueryAllInterfaces( &filter, interfaces, sizeof(interfaces), &attached ) )) attached = 0;
    while (attached > 0 && usbHsFsGetPhysicalDeviceCount() < (u32)attached &&
           armTicksToNs( armGetSystemTick() - start ) < USB_WAIT_NS)
        svcSleepThread( 50000000 );
    mutexLock( &usb_mutex );
    count = usb_device_count;
    if (count) memcpy( devices, usb_devices, count * sizeof(*devices) );
    mutexUnlock( &usb_mutex );
    snprintf( buf, sizeof(buf), "[USB] %d mass storage interfaces, %u volumes mounted after %llu ms",
              (int)attached, (unsigned)count,
              (unsigned long long)(armTicksToNs( armGetSystemTick() - start ) / 1000000) );
    wine_nx_runtime_trace( buf );
    for (i = 0; i < count; i++)
    {
        snprintf( buf, sizeof(buf), "[USB] %s %s, %llu MB%s: %s %s", devices[i].name,
                  LIBUSBHSFS_FS_TYPE_STR( devices[i].fs_type ), (unsigned long long)(devices[i].capacity >> 20),
                  devices[i].write_protect ? ", read-only" : "", devices[i].manufacturer, devices[i].product_name );
        wine_nx_runtime_trace( buf );
    }
}

int wine_nx_usb_list( struct wine_nx_launcher_usb_volume *volumes, int max )
{
    int i, count = 0;

    if (!usb_started || !volumes || max <= 0) return 0;
    mutexLock( &usb_mutex );
    for (i = 0; i < (int)usb_device_count && count < max; i++)
    {
        const UsbHsFsDevice *device = usb_devices + i;
        int index;

        if (strncmp( device->name, "ums", 3 ) || device->name[3] < '0' || device->name[3] > '4' ||
            device->name[4] != ':' || device->name[5]) continue;
        index = device->name[3] - '0';
        snprintf( volumes[count].path, sizeof(volumes[count].path), "%s/", device->name );
        if (device->product_name[0])
            snprintf( volumes[count].label, sizeof(volumes[count].label), "%s (%s)",
                      device->product_name, LIBUSBHSFS_FS_TYPE_STR( device->fs_type ) );
        else
            snprintf( volumes[count].label, sizeof(volumes[count].label), "USB %d (%s)",
                      index + 1, LIBUSBHSFS_FS_TYPE_STR( device->fs_type ) );
        volumes[count].drive = 'D' + index;
        count++;
    }
    mutexUnlock( &usb_mutex );
    return count;
}
