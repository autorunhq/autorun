/*
 * The launcher as a text menu on the console, for when SDL cannot draw the
 * graphical one (launcher.c): explicitly registered games, driven by
 * the controller.
 */
#include <switch.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "launcher_catalog.h"
#include "launcher_list.h"

#define LAUNCHER_ROWS  36      /* the console is 80x45 */
#define LAUNCHER_REPEAT_DELAY_NS 400000000ull
#define LAUNCHER_REPEAT_NS       70000000ull

typedef int (*launcher_machine_fn)( const char *path, unsigned short *machine );

static struct launcher_entry launcher_entries[LAUNCHER_MAX_ENTRIES];

static void launcher_load_catalog( const char *runtime_dir, int *count, launcher_machine_fn machine_of )
{
    /* Static: 256 entries of paths and artwork are some 560 KB, more than the
     * main thread's stack has left by the time the launcher falls back here. */
    static struct launcher_catalog catalog;
    char path[512];
    int i;
    snprintf( path, sizeof(path), "%s/%s", runtime_dir, LAUNCHER_CATALOG_FILE );
    if (launcher_catalog_load( &catalog, path ) != LAUNCHER_CATALOG_OK) return;
    for (i = 0; i < catalog.count && *count < LAUNCHER_MAX_ENTRIES; i++)
    {
        struct launcher_entry *item = &launcher_entries[*count];
        if (!machine_of( catalog.entries[i].path, &item->machine ) &&
            launcher_dos_path( catalog.entries[i].path, item->dos, sizeof(item->dos) ))
        {
            snprintf( item->path, sizeof(item->path), "%s", catalog.entries[i].path );
            (*count)++;
        }
    }
}

static void launcher_write_line( const char *runtime_dir, const char *name, const char *text )
{
    char path[256];
    FILE *file;

    snprintf( path, sizeof(path), "%s/%s", runtime_dir, name );
    if (!(file = fopen( path, "w" ))) return;
    fprintf( file, "%s\n", text );
    fclose( file );
}

static void launcher_draw( const char *build, int count, int selected, int first, int verbose,
                           int profile, const char *args )
{
    int i;

    printf( CONSOLE_ESC(2J) CONSOLE_ESC(1;1H) );
    printf( CONSOLE_CYAN "Wine-NX" CONSOLE_RESET "  %s\n", build );
    printf( "Choose a Windows program from sdmc:/switch/wine/drive_c\n\n" );
    if (!count)
    {
        printf( CONSOLE_YELLOW "No registered games are available.\n" CONSOLE_RESET );
        printf( "Start the graphical launcher and use Add Game to choose an executable.\n" );
    }
    for (i = first; i < count && i < first + LAUNCHER_ROWS; i++)
    {
        const char *arch = launcher_machine_name( launcher_entries[i].machine );

        if (i == selected)
            printf( CONSOLE_GREEN " > %-68.68s %5s\n" CONSOLE_RESET, launcher_entries[i].dos, arch );
        else
            printf( "   %-68.68s %5s\n", launcher_entries[i].dos, arch );
    }
    printf( CONSOLE_ESC(41;1H) );
    if (count)
    {
        printf( "%d of %d", selected + 1, count );
        if (args) printf( "   arguments: %.52s", args );
        printf( "\n" );
    }
    printf( CONSOLE_ESC(44;1H) "A start  Up/Down choose  L/R page  Y verbose: %s  X profiler: %s  + quit",
            verbose ? CONSOLE_YELLOW "on " CONSOLE_RESET : "off",
            profile ? CONSOLE_YELLOW "on " CONSOLE_RESET : "off" );
    consoleUpdate( NULL );
}

/* Show the menu. Returns 1 with the chosen program in *target, or 0 when the
 * user quits. The choice is saved to target.txt; Y toggles verbose.txt and X
 * profile.txt. */
int wine_nx_launcher_console_run( const char *drive_c, const char *runtime_dir, const char *build,
                          launcher_machine_fn machine_of, int *verbose, int *profile,
                          char *target, size_t target_size )
{
    const u64 moves = HidNpadButton_AnyUp | HidNpadButton_AnyDown | HidNpadButton_L | HidNpadButton_R;
    char args[896], args_path[256];
    int count = 0, selected, first = 0, redraw = 1, have_args;
    u64 repeat_at = 0;
    PadState pad;

    (void)drive_c;
    launcher_load_catalog( runtime_dir, &count, machine_of );
    qsort( launcher_entries, count, sizeof(launcher_entries[0]), launcher_compare );
    selected = launcher_find( launcher_entries, count, target );
    snprintf( args_path, sizeof(args_path), "%s/args.txt", runtime_dir );
    {
        FILE *file = fopen( args_path, "r" );
        have_args = file && fgets( args, sizeof(args), file );
        if (file) fclose( file );
        if (have_args) args[strcspn( args, "\r\n" )] = 0;
    }

    padConfigureInput( 1, HidNpadStyleSet_NpadStandard );
    padInitializeDefault( &pad );
    while (appletMainLoop())
    {
        u64 now = armGetSystemTick(), down, held, step = 0;

        padUpdate( &pad );
        down = padGetButtonsDown( &pad );
        held = padGetButtons( &pad );

        if (down & HidNpadButton_Plus) break;
        if (down & moves)
        {
            step = down;
            repeat_at = now + armNsToTicks( LAUNCHER_REPEAT_DELAY_NS );
        }
        else if ((held & moves) && now >= repeat_at)
        {
            step = held;
            repeat_at = now + armNsToTicks( LAUNCHER_REPEAT_NS );
        }
        if (count && step)
        {
            int old = selected;

            if (step & HidNpadButton_AnyUp) selected--;
            if (step & HidNpadButton_AnyDown) selected++;
            if (step & HidNpadButton_L) selected -= LAUNCHER_ROWS;
            if (step & HidNpadButton_R) selected += LAUNCHER_ROWS;
            if (selected < 0) selected = 0;
            if (selected >= count) selected = count - 1;
            redraw |= selected != old;
        }
        if (down & HidNpadButton_Y)
        {
            *verbose = !*verbose;
            launcher_write_line( runtime_dir, "verbose.txt", *verbose ? "1" : "0" );
            redraw = 1;
        }
        if (down & HidNpadButton_X)
        {
            *profile = !*profile;
            launcher_write_line( runtime_dir, "profile.txt", *profile ? "1" : "0" );
            redraw = 1;
        }
        if ((down & HidNpadButton_A) && count)
        {
            snprintf( target, target_size, "%s", launcher_entries[selected].path );
            launcher_write_line( runtime_dir, "target.txt", target );
            printf( CONSOLE_ESC(2J) CONSOLE_ESC(1;1H) );
            consoleUpdate( NULL );
            return 1;
        }
        if (redraw)
        {
            const char *shown = NULL;
            char own_path[sizeof(launcher_entries[0].path)], own[256];

            if (count && launcher_args_path( launcher_entries[selected].path, own_path, sizeof(own_path) ))
            {
                FILE *file = fopen( own_path, "r" );
                if (file && fgets( own, sizeof(own), file ) && (own[strcspn( own, "\r\n" )] = 0, own[0]))
                    shown = own;
                if (file) fclose( file );
            }
            if (!shown && count && have_args && launcher_args_match( args, launcher_entries[selected].dos ))
                shown = args;
            first = launcher_first_visible( first, selected, count, LAUNCHER_ROWS );
            launcher_draw( build, count, selected, first, *verbose, *profile, shown );
            redraw = 0;
        }
        svcSleepThread( 16000000 );
    }
    return 0;
}
