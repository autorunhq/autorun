/* The Voodoo (dgVoodoo2) menu: a settings screen per program, saved into the
 * program's own settings file like everything else. When the switch is on,
 * the runtime writes dgVoodoo.conf beside the program before every start;
 * the wrapper's DLLs the user copies into the game folder pick it up. */
#include <errno.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "launcher_settings.h"
#include "launcher_ui.h"
#include "launcher_voodoo.h"

enum voodoo_row
{
    VOODOO_ENABLED, VOODOO_API, VOODOO_SCALING, VOODOO_RESOLUTION, VOODOO_MSAA, VOODOO_ROWS
};

void launcher_voodoo_options( struct ui *ui, const char *runtime_dir, const char *program,
                              const char *title )
{
    struct launcher_kv kv;
    struct launcher_settings settings;
    struct ui_list list = {0};
    struct ui_row rows[VOODOO_ROWS];
    char path[768];

    if (!launcher_program_settings_path( runtime_dir, program, path, sizeof(path) ) ||
        !launcher_kv_load( &kv, path ))
    {
        ui_message( ui, "Voodoo (dgVoodoo)", "The program settings could not be read." );
        return;
    }
    for (;;)
    {
        struct ui_row items[LAUNCHER_VOODOO_API_COUNT] = {0};
        enum ui_action action;
        const char *const *labels = NULL;
        int *value = NULL, choices = 0, selected, i, ready = 1;

        launcher_settings_read( &kv, &settings );
        memset( rows, 0, sizeof(rows) );

        rows[VOODOO_ENABLED].kind = UI_ROW_SWITCH;
        rows[VOODOO_ENABLED].on = settings.voodoo;
        snprintf( rows[VOODOO_ENABLED].label, sizeof(rows[VOODOO_ENABLED].label), "Write dgVoodoo.conf" );
        snprintf( rows[VOODOO_ENABLED].value, sizeof(rows[VOODOO_ENABLED].value), "%s",
                  settings.voodoo ? "On" : "Off" );
        rows[VOODOO_ENABLED].help = "On writes dgVoodoo.conf before every start, from the settings below, and "
                                    "uses the dgVoodoo2 wrappers dropped at C:\\dgVoodoo (its MS\\x86 for 32-bit "
                                    "games); DLLs beside the program still win, and a DirectDraw of your own in the "
                                    "system folders (D7VK and such) is set aside while this is on. Off brings it "
                                    "back and leaves everything alone: a conf of your own survives.";

        rows[VOODOO_API].kind = UI_ROW_DROPDOWN;
        rows[VOODOO_API].choices = LAUNCHER_VOODOO_API_COUNT;
        snprintf( rows[VOODOO_API].label, sizeof(rows[VOODOO_API].label), "Output API" );
        snprintf( rows[VOODOO_API].value, sizeof(rows[VOODOO_API].value), "%s",
                  launcher_voodoo_api_labels[settings.voodoo_api] );
        rows[VOODOO_API].help = "The Direct3D the wrapper draws into. Best available picks the newest the "
                                "chain supports: the wrapper into DXVK, DXVK into Vulkan, Vulkan to the panel.";

        rows[VOODOO_SCALING].kind = UI_ROW_DROPDOWN;
        rows[VOODOO_SCALING].choices = LAUNCHER_VOODOO_SCALING_COUNT;
        snprintf( rows[VOODOO_SCALING].label, sizeof(rows[VOODOO_SCALING].label), "Scaling" );
        snprintf( rows[VOODOO_SCALING].value, sizeof(rows[VOODOO_SCALING].value), "%s",
                  launcher_voodoo_scaling_labels[settings.voodoo_scaling] );
        rows[VOODOO_SCALING].help = "How the game's picture fills the screen. Stretched keep AR keeps the "
                                    "proportions with bars where needed; Stretched fills the screen without bars.";

        rows[VOODOO_RESOLUTION].kind = UI_ROW_DROPDOWN;
        rows[VOODOO_RESOLUTION].choices = LAUNCHER_VOODOO_RESOLUTION_COUNT;
        snprintf( rows[VOODOO_RESOLUTION].label, sizeof(rows[VOODOO_RESOLUTION].label), "Resolution" );
        snprintf( rows[VOODOO_RESOLUTION].value, sizeof(rows[VOODOO_RESOLUTION].value), "%s",
                  launcher_voodoo_resolution_labels[settings.voodoo_resolution] );
        rows[VOODOO_RESOLUTION].help = "The screen the wrapper puts in front of the game. Unforced lets the "
                                       "game pick its own; the rest force one, scaled onto the panel.";

        rows[VOODOO_MSAA].kind = UI_ROW_DROPDOWN;
        rows[VOODOO_MSAA].choices = LAUNCHER_VOODOO_MSAA_COUNT;
        snprintf( rows[VOODOO_MSAA].label, sizeof(rows[VOODOO_MSAA].label), "Antialiasing" );
        snprintf( rows[VOODOO_MSAA].value, sizeof(rows[VOODOO_MSAA].value), "%s",
                  launcher_voodoo_msaa_labels[settings.voodoo_msaa] );
        rows[VOODOO_MSAA].help = "Smooths the edges in the wrapper. Off is the safest; App driven follows "
                                 "what the game asks for.";

        action = ui_list_run( ui, &list, "Voodoo (dgVoodoo)", title, rows, VOODOO_ROWS, 1 );
        if (action == UI_ACTION_BACK || action == UI_ACTION_QUIT) break;
        switch (list.selection)
        {
        case VOODOO_ENABLED:
            if (action == UI_ACTION_RESET) settings.voodoo = 0;
            else if (action == UI_ACTION_CHOOSE) settings.voodoo = !settings.voodoo;
            else if (action == UI_ACTION_LEFT || action == UI_ACTION_RIGHT)
                settings.voodoo = action == UI_ACTION_RIGHT;
            else continue;
            break;
        case VOODOO_API:
            value = &settings.voodoo_api; choices = LAUNCHER_VOODOO_API_COUNT;
            labels = launcher_voodoo_api_labels;
            break;
        case VOODOO_SCALING:
            value = &settings.voodoo_scaling; choices = LAUNCHER_VOODOO_SCALING_COUNT;
            labels = launcher_voodoo_scaling_labels;
            break;
        case VOODOO_RESOLUTION:
            value = &settings.voodoo_resolution; choices = LAUNCHER_VOODOO_RESOLUTION_COUNT;
            labels = launcher_voodoo_resolution_labels;
            break;
        case VOODOO_MSAA:
            value = &settings.voodoo_msaa; choices = LAUNCHER_VOODOO_MSAA_COUNT;
            labels = launcher_voodoo_msaa_labels;
            break;
        default:
            continue;
        }
        if (value)
        {
            if (action == UI_ACTION_RESET) *value = 0;
            else if (action == UI_ACTION_CHOOSE)
            {
                for (i = 0; i < choices; i++)
                    snprintf( items[i].label, sizeof(items[i].label), "%s", labels[i] );
                selected = ui_settings_dropdown( ui, &list, items, choices, *value );
                if (selected < 0) continue;
                *value = selected;
            }
            else if (action == UI_ACTION_LEFT || action == UI_ACTION_RIGHT)
                *value = (*value + (action == UI_ACTION_LEFT ? choices - 1 : 1)) % choices;
            else continue;
        }
        if (launcher_settings_on_usb( program ))
        {
            char folder[768];
            int length = snprintf( folder, sizeof(folder), "%s%sprogram-settings", runtime_dir,
                                   runtime_dir[strlen( runtime_dir ) - 1] == '/' ? "" : "/" );
            ready = length > 0 && (size_t)length < sizeof(folder) && (!mkdir( folder, 0777 ) || errno == EEXIST);
        }
        if (!ready || !launcher_settings_write( &kv, &settings ) || !launcher_kv_save( &kv, path ))
        {
            ui_message( ui, "Voodoo (dgVoodoo)", "The settings could not be saved." );
            if (!launcher_kv_load( &kv, path )) break;
        }
    }
}
