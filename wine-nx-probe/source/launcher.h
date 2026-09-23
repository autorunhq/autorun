/*
 * The runtime's launcher (launcher.c), shown before Wine starts.
 */
#ifndef WINE_NX_LAUNCHER_H
#define WINE_NX_LAUNCHER_H

#include <stddef.h>

#define LAUNCHER_MAX_USB_VOLUMES 5

struct wine_nx_launcher_usb_volume
{
    char path[40];
    char label[128];
    char drive;
};

struct wine_nx_launcher_options
{
    const char *runtime_dir;   /* sdmc:/switch/wine: target.txt, args.txt, config/settings.json... */
    /* Which of the two system memories the console booted from: 1 an emuMMC,
     * 0 the real one, -1 when Atmosphere did not say. */
    int emummc;
    const char *build;
    /* 0 with the program's IMAGE_FILE_MACHINE_* when this runtime can start it. */
    int (*machine_of)( const char *path, unsigned short *machine );
    int (*list_usb)( struct wine_nx_launcher_usb_volume *volumes, int max );
    int vulkan;                /* the runtime has Vulkan for DXVK */
    /* The forwarder's host address-space width, or 0 if unavailable. */
    int address_space_bits;
    int low_window;
    int four_cores_available;
    int (*schedule_restart)(void);
    /* Install the 39-bit forwarder; returns 0 or the failing Result and step. */
    unsigned int (*install_forwarder)( const char **step );
    /* The global settings on entry, as the user left them on return. The
     * runtime keeps them; the launcher only says what they became. */
    int verbose;
    int profile;
    int framebuffer;
    int reopen_launcher;  /* come back here when a program ends, rather than to the menu */
    int dxvk_on_add;      /* a game added to the library starts with DXVK enabled */
};

/* Show the launcher. Returns 1 with the chosen program's path in target, or 0
 * when the user quits. target on entry preselects a program. */
int wine_nx_launcher_run( struct wine_nx_launcher_options *options, char *target, size_t target_size );
void wine_nx_launcher_usb_changed(void);

/* A line in wine-nx-runtime.log (runtime.c). */
void wine_nx_runtime_trace( const char *msg );

/* What launcher_platform_status found. */
#define LAUNCHER_STATUS_CLOCK   1
#define LAUNCHER_STATUS_BATTERY 2

/* The time of day and the battery charge shown in the header. Returns the
 * LAUNCHER_STATUS_* bits for what it could read; the rest is left alone. */
int launcher_platform_status( int *hour, int *minute, int *battery, int *charging );

#ifndef __SWITCH__
/* A host build (tests/launcher_host.c) supplies what the Switch build takes from libnx. */
int launcher_platform_font( const void **data, size_t *size );
int launcher_platform_prompt( const char *header, const char *initial, char *out, size_t size );
#endif

#endif
