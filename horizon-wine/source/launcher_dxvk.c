#include <errno.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "dxvk_options.h"
#include "launcher_dxvk.h"
#include "launcher_ui.h"

static int option_rows( struct ui_row *rows, int *ids, int count, const struct launcher_kv *kv,
                        enum dxvk_source source, unsigned int version, int advanced )
{
    struct launcher_settings settings;
    launcher_settings_read( kv, &settings );
    for (unsigned int i = 0; i < dxvk_option_count; i++)
    {
        const struct dxvk_option *option = dxvk_options + i;
        struct ui_row *row;
        int choice;

        if (option->advanced != advanced || !dxvk_option_supported( option, source, version )) continue;
        row = rows + count;
        ids[count++] = i;
        choice = dxvk_option_value( kv, option );
        snprintf( row->label, sizeof(row->label), "%s", option->name );
        if (!choice)
            snprintf( row->value, sizeof(row->value), "Default (%s)",
                      dxvk_option_default( option, source, version, settings.dxvk_hud ) );
        else
            snprintf( row->value, sizeof(row->value), "%s", choice < 0 ? "Invalid (not applied)" :
                      option->choices[choice].label );
        row->help = option->help;
        row->kind = UI_ROW_VALUE;
        row->adjustable = 1;
    }
    return count;
}

void launcher_dxvk_options( struct ui *ui, const char *runtime_dir, const char *program,
                            const char *title, enum dxvk_source source, const char *version )
{
    struct launcher_kv kv;
    struct ui_list list = {0};
    struct ui_row *rows;
    int *ids, expanded = 0;
    unsigned int release = dxvk_options_version( source, version );
    char path[768], context[192];

    if (!release)
    {
        ui_message( ui, "DXVK options", "This DXVK version is not recognized. Select a supported release first. Saved options are kept but not applied." );
        return;
    }
    if (!launcher_program_settings_path( runtime_dir, program, path, sizeof(path) ) ||
        !launcher_kv_load( &kv, path ))
    {
        ui_message( ui, "DXVK options", "The program settings could not be read." );
        return;
    }
    rows = calloc( dxvk_option_count + 1, sizeof(*rows) );
    ids = calloc( dxvk_option_count + 1, sizeof(*ids) );
    if (!rows || !ids) goto done;
    snprintf( context, sizeof(context), "%s / %s %s", title,
              source == DXVK_SOURCE_SAREK ? "Sarek" : source == DXVK_SOURCE_GPLASYNC ? "GPLAsync" : "DXVK", version );
    for (;;)
    {
        const struct dxvk_option *option;
        enum ui_action action;
        int count, id, current, next, ready = 1;

        memset( rows, 0, (dxvk_option_count + 1) * sizeof(*rows) );
        count = option_rows( rows, ids, 0, &kv, source, release, 0 );
        ids[count] = -1;
        snprintf( rows[count].label, sizeof(rows[count].label), "Advanced options" );
        snprintf( rows[count].value, sizeof(rows[count].value), "%s", expanded ? "Hide" : "Show" );
        rows[count].kind = UI_ROW_DROPDOWN;
        rows[count].on = expanded;
        rows[count++].help = "Default shows the release baseline; DXVK's game profile can change it. Auto depends on the driver. A dxvk.conf beside the game takes priority.";
        if (expanded) count = option_rows( rows, ids, count, &kv, source, release, 1 );
        action = ui_list_run( ui, &list, "DXVK options", context, rows, count, 1 );
        if (action == UI_ACTION_BACK || action == UI_ACTION_QUIT) break;
        id = ids[list.selection];
        if (id < 0)
        {
            if (action == UI_ACTION_CHOOSE) expanded = !expanded;
            continue;
        }
        option = dxvk_options + id;
        current = dxvk_option_value( &kv, option );
        if (current < 0) current = 0;
        next = action == UI_ACTION_RESET ? 0 :
               (current + (action == UI_ACTION_LEFT ? option->choice_count - 1 : 1)) % option->choice_count;
        if (launcher_settings_on_usb( program ))
        {
            char folder[768];
            int length = snprintf( folder, sizeof(folder), "%s%sprogram-settings", runtime_dir,
                                   runtime_dir[strlen( runtime_dir ) - 1] == '/' ? "" : "/" );
            ready = length > 0 && (size_t)length < sizeof(folder) && (!mkdir( folder, 0777 ) || errno == EEXIST);
        }
        if (!ready || !launcher_kv_set( &kv, option->name, option->choices[next].value ) ||
            !launcher_kv_save( &kv, path ))
        {
            ui_message( ui, "DXVK options", "The options could not be saved." );
            if (!launcher_kv_load( &kv, path )) break;
        }
    }
done:
    free( ids );
    free( rows );
}
