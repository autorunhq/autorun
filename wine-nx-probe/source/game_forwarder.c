#include <switch.h>
#include <stdio.h>
#include "forwarder_launch.h"

u32 __nx_applet_type = AppletType_Application;
size_t __nx_heap_size = 16 * 1024 * 1024;

int main(void)
{
    struct autorun_game_launch launch = {0};
    AppletStorage storage;
    Result rc = romfsInit();
    FILE *file;

    if (R_SUCCEEDED( rc ))
    {
        rc = MAKERESULT( Module_Libnx, LibnxError_BadInput );
        if ((file = fopen( "romfs:/game", "rb" )))
        {
            size_t size = fread( &launch, 1, sizeof(launch), file );
            if (autorun_game_launch_valid( &launch, size ) && fgetc( file ) == EOF && !ferror( file )) rc = 0;
            fclose( file );
        }
        romfsExit();
    }
    if (R_SUCCEEDED( rc ))
    {
        rc = appletCreateStorage( &storage, sizeof(launch) );
        if (R_SUCCEEDED( rc ))
        {
            rc = appletStorageWrite( &storage, 0, &launch, sizeof(launch) );
            /* The low-address window belongs to Autorun's title ID. */
            if (R_SUCCEEDED( rc )) rc = appletRequestLaunchApplication( AUTORUN_TITLE_ID, &storage );
            else appletStorageClose( &storage );
        }
    }
    if (R_SUCCEEDED( rc ))
    {
        while (appletMainLoop()) eventWait( appletGetMessageEvent(), UINT64_MAX );
        return 0;
    }
    if (R_FAILED( rc ))
    {
        PadState pad;
        consoleInit( NULL );
        printf( "Could not start Autorun (0x%X).\n\nInstall Autorun's main forwarder from Quick setup, then try again.\n", rc );
        padConfigureInput( 1, HidNpadStyleSet_NpadStandard );
        padInitializeDefault( &pad );
        while (appletMainLoop())
        {
            padUpdate( &pad );
            if (padGetButtonsDown( &pad ) & (HidNpadButton_A | HidNpadButton_B | HidNpadButton_Plus)) break;
            consoleUpdate( NULL );
        }
        consoleExit( NULL );
    }
    return 0;
}
