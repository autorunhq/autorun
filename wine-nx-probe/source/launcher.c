/*
 * The runtime's launcher, shown before Wine starts: the Windows programs on
 * the SD card as a grid of their icons, drawn with SDL2 (launcher_ui.c).
 *
 * - The library lists the programs within two folders of drive_c, plus those
 *   added from the file browser (launcher-library.txt), sorted by title.
 * - A program's menu (Y) changes its title, arguments and settings, which go
 *   next to it (launcher_settings.h) and apply whenever it is started.
 * - Settings (X) holds the launcher's look (launcher.txt) and the global
 *   verbose.txt, profile.txt and framebuffer.txt.
 * - The file browser adds a program from the SD card or a mounted USB volume:
 *   C: is drive_c, Z: the card's root and D: through H: are USB volumes.
 *
 * Icons are read and decoded on a worker thread and shown as they arrive.
 * When SDL cannot start, the text menu of launcher_console.c is shown instead.
 */
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include <png.h>

#ifdef __SWITCH__
#include <switch.h>
#endif

#include "launcher.h"
#ifdef WINE_NX_HAS_EMBEDDED_LOGO
#include "forwarder.h"
#endif
#include "launcher_catalog.h"
#include "launcher_icons.h"
#include "launcher_list.h"
#include "launcher_pe.h"
#include "key_names.h"
#include "launcher_settings.h"
#include "launcher_ui.h"
#include "launcher_update.h"
#include "autorun_install.h"
#include "steamgriddb.h"
#include "dxvk_releases.h"
#include "box64_options.h"

#define ICON_SIDE      128    /* icons are decoded no larger than this */
#define ICON_TEXTURES  48     /* a screenful, the row below it and the backdrop, at 1 MiB each */
#define ICON_JOBS      64
#define MAX_FILES      1024
#define FOOTER_SPACE   38

/* Home and Library's header, laid out after GameHub's at 1280x720. */
enum shell_tab { SHELL_HOME, SHELL_LIBRARY, SHELL_SETTINGS, SHELL_TABS };
#define SHELL_Y        UI_HEADER_CENTRE
#define SHELL_ICON     30
#define SHELL_MARGIN   UI_HEADER_MARGIN   /* which Home's content keeps too */
#define SHELL_GAP      44
/* What the header's pill keeps between its outline and the item inside it. */
#define SHELL_PADDING  22

/* Home: the games that have been played, most recent first, as 2:3 covers. The
 * focused one is larger and never moves; the row slides through it, so choosing
 * the next cover pushes the one before it off the left edge. */
#define HOME_TOP          126
#define HOME_FOCUS_W      240
#define HOME_FOCUS_H      360
#define HOME_CARD_W       200
#define HOME_CARD_H       300
#define HOME_FOCUS_GAP    16
#define HOME_CARD_GAP     18
#define HOME_RADIUS       26
#define HOME_TITLE_Y      440
#define HOME_HINT_Y       (720 - 50)
#define BACKDROP_FADE_MS  280

/* Where the D-pad and stick are: the header or the games. */
enum zone { ZONE_HEADER, ZONE_CONTENT };

/* The icons drawn from assets/ (launcher_icons.h), made once at the size they are shown. */
enum symbol { SYMBOL_HOME, SYMBOL_LIBRARY, SYMBOL_ADD, SYMBOL_SETTINGS, SYMBOL_COUNT };

enum icon_state { ICON_UNKNOWN, ICON_QUEUED, ICON_READY, ICON_MISSING };
enum art_kind { ART_PORTRAIT, ART_SQUARE, ART_HERO };

struct program
{
    char path[512];                    /* sdmc:/... */
    char dos[256];                     /* C:\... or Z:\... */
    char title[128];                   /* shown: its own title, the resources' or the file name */
    char resource_title[128];
    unsigned short machine;
    struct launcher_settings settings;
    int own_files;                     /* settings, arguments, controls or Box64 options beside it */
    int added;                         /* listed in launcher-library.txt */
    unsigned int catalog_id;
    unsigned int added_order;
    unsigned int launched_order;
    int favorite;
    int missing;
    char square_art[512];
    char portrait_art[512];
    char hero_art[512];
    int removed;
    enum icon_state icon_state;
    SDL_Texture *icon;
    int icon_width, icon_height;
    int icon_is_art;
    Uint32 icon_time;
    unsigned int icon_use;
    enum icon_state square_state, hero_state;
    SDL_Texture *square_icon, *hero_icon;
    int square_width, square_height, hero_width, hero_height;
    Uint32 square_time, hero_time;
};

struct icon_job
{
    int index;
    int kind;
    char path[512];
    char artwork[512];
};

struct icon_result
{
    int index;
    int kind;
    int width, height;
    unsigned char *rgba;
    int artwork;
};

struct file_entry
{
    char name[256];
    int is_dir;
    int supported;
    unsigned short machine;
};

struct launcher
{
    struct wine_nx_launcher_options *options;
    struct ui ui;
    struct launcher_update *update;

    struct program programs[LAUNCHER_MAX_ENTRIES];
    int program_count;
    int visible[LAUNCHER_MAX_ENTRIES];
    int visible_count;
    int selection;
    struct launcher_catalog catalog;
    char search[128];
    int favorites_only;
    int sort_order;
    float carousel_position;
    int carousel_started;
    Uint32 carousel_tick;
    SDL_Rect carousel_hits[LAUNCHER_MAX_ENTRIES];
    SDL_Texture *symbols[SYMBOL_COUNT];
    /* The mark in the corner of the shell, from assets/logo.png. */
    SDL_Texture *logo;
    /* Home's own list: the programs that have been started, most recent first. */
    int history[LAUNCHER_MAX_ENTRIES];
    int history_count, history_selection;
    /* What the D-pad moves, and which header item it is on. */
    int zone, header_focus;
    /* Where the header's pill stands and how wide it is, eased toward the item
     * in focus so it slides between them instead of jumping. */
    float pill_x, pill_w;
    /* How much of each header item's name is out, eased toward 1 for the view
     * being shown and 0 for the rest, so one name closes as the other opens. */
    float label_open[SHELL_TABS];
    /* Where the last frame drew what a tap can hit. */
    SDL_Rect shell_hits[SHELL_TABS], add_hit;
    /* The programs whose artwork is behind Home, fading from the previous one; -1 for none. */
    int backdrop, backdrop_previous;
    Uint32 backdrop_since;
    /* The header's clock and battery, read once a second. */
    Uint32 status_read;
    int status, clock_hour, clock_minute, battery, charging;

    struct launcher_kv look;
    int top_row, show_hidden, hide_missing;
    char browse_dir[512];

    SDL_Thread *thread;
    SDL_mutex *mutex;
    SDL_cond *cond;
    int stop;
    struct icon_job jobs[ICON_JOBS];
    int job_count;
    struct icon_result results[ICON_JOBS];
    int result_count;
    unsigned int icon_use;
    /* icon_use as the last frame began to draw: what stands above it was asked
     * for by that frame, so it is on the screen and must not be thrown away. */
    unsigned int icon_frame;
    Uint32 icon_event;
    Uint32 usb_event;
    SDL_atomic_t usb_changed;
};

static struct launcher launcher;
static struct file_entry files[MAX_FILES];
static struct ui_row file_rows[MAX_FILES + 8];

void wine_nx_launcher_usb_changed(void)
{
    SDL_Event event;

    if (!launcher.usb_event) return;
    SDL_AtomicSet( &launcher.usb_changed, 1 );
    memset( &event, 0, sizeof(event) );
    event.type = launcher.usb_event;
    SDL_PushEvent( &event );
}

extern int wine_nx_launcher_console_run( const char *drive_c, const char *runtime_dir, const char *build,
                                         int (*machine_of)( const char *path, unsigned short *machine ),
                                         int *verbose, int *profile, char *target, size_t target_size );

/***********************************************************************
 * Platform
 */

static void launcher_log( const char *format, ... )
{
    char line[600];
    va_list args;

    va_start( args, format );
    vsnprintf( line, sizeof(line), format, args );
    va_end( args );
    wine_nx_runtime_trace( line );
}

#ifdef __SWITCH__
static int font_service;

int launcher_platform_font( const void **data, size_t *size )
{
    PlFontData font;

    if (R_FAILED( plInitialize( PlServiceType_User ) )) return 0;
    font_service = 1;
    if (R_FAILED( plGetSharedFontByType( &font, PlSharedFontType_Standard ) ) || !font.address) return 0;
    *data = font.address;
    *size = font.size;
    return 1;
}

static int battery_service;    /* 1 open, -1 refused */

static void launcher_platform_font_release(void)
{
    /* The fonts read the shared memory until they are closed, so this comes after ui_quit. */
    if (font_service) plExit();
    font_service = 0;
    if (battery_service > 0) psmExit();
    battery_service = 0;
}

int launcher_platform_status( int *hour, int *minute, int *battery, int *charging )
{
    TimeCalendarTime calendar;
    TimeCalendarAdditionalInfo info;
    PsmChargerType charger;
    u64 now;
    u32 percent;
    int found = 0;

    /* The clock the user set, in the time zone they chose. */
    if (R_SUCCEEDED( timeGetCurrentTime( TimeType_LocalSystemClock, &now ) ) &&
        R_SUCCEEDED( timeToCalendarTimeWithMyRule( now, &calendar, &info ) ))
    {
        *hour = calendar.hour;
        *minute = calendar.minute;
        found |= LAUNCHER_STATUS_CLOCK;
    }
    if (!battery_service) battery_service = R_SUCCEEDED( psmInitialize() ) ? 1 : -1;
    if (battery_service > 0 && R_SUCCEEDED( psmGetBatteryChargePercentage( &percent ) ))
    {
        *battery = percent;
        *charging = R_SUCCEEDED( psmGetChargerType( &charger ) ) && charger != PsmChargerType_Unconnected;
        found |= LAUNCHER_STATUS_BATTERY;
    }
    return found;
}

int launcher_platform_prompt( const char *header, const char *initial, char *out, size_t size )
{
    SwkbdConfig keyboard;
    Result rc;

    if (R_FAILED( swkbdCreate( &keyboard, 0 ) )) return 0;
    swkbdConfigMakePresetDefault( &keyboard );
    swkbdConfigSetHeaderText( &keyboard, header );
    swkbdConfigSetGuideText( &keyboard, header );
    swkbdConfigSetInitialText( &keyboard, initial );
    swkbdConfigSetStringLenMax( &keyboard, size - 1 < 500 ? size - 1 : 500 );
    rc = swkbdShow( &keyboard, out, size );
    swkbdClose( &keyboard );
    return R_SUCCEEDED( rc );
}
#else
static void launcher_platform_font_release(void) {}
#endif

/***********************************************************************
 * Files
 */

static int file_exists( const char *path )
{
    struct stat st;

    return !stat( path, &st ) && S_ISREG( st.st_mode );
}

static void runtime_file( const struct launcher *l, const char *name, char *out, size_t size )
{
    snprintf( out, size, "%s/%s", l->options->runtime_dir, name );
}

/* The key map everything shares. It lives in the config folder; a card written
 * by an earlier build has it beside the launcher, and the runtime reads that
 * one too. Returns whether either is there, with the path of the one to show. */
static void controls_screen( struct launcher *l, const char *path, const char *under_path,
                             const char *title );

static int shared_keys( const struct launcher *l, char *out, size_t size )
{
    runtime_file( l, "config/keys.txt", out, size );
    if (file_exists( out )) return 1;
    runtime_file( l, "keys.txt", out, size );
    return file_exists( out );
}

/* Nonzero when the line reached the card, which the handoff in start_program
 * has to know before it gives the console away. */
static int write_line( const char *path, const char *text )
{
    FILE *file = fopen( path, "w" );

    if (!file) return 0;
    return fprintf( file, "%s\n", text ) > 0 && !fclose( file );
}

static int read_line( const char *path, char *out, size_t size )
{
    FILE *file = fopen( path, "r" );
    int ok;

    out[0] = 0;
    if (!file) return 0;
    ok = fgets( out, size, file ) != NULL;
    fclose( file );
    out[strcspn( out, "\r\n" )] = 0;
    return ok;
}

static const char *file_name( const char *path )
{
    const char *slash = strrchr( path, '/' );

    return slash ? slash + 1 : path;
}

static void parent_dir( char *dir )
{
    char *slash = strrchr( dir, '/' );

    if (!slash) return;
    if (slash == strchr( dir, '/' )) slash[1] = 0;  /* sdmc:/ stays */
    else *slash = 0;
}

static int is_root( const char *dir )
{
    const char *slash = strchr( dir, '/' );

    return !slash || !slash[1];
}

/***********************************************************************
 * Programs
 */

/* The folder a program sits in, when that is a name rather than a place to put
 * a binary: FalloutNV.exe says nothing, "Fallout New Vegas" says what it is.
 * Returns 0 for the usual containers, and for the drive itself. */
static int folder_title( const char *path, char *out, size_t size )
{
    static const char *const containers[] =
    {
        "bin", "bin32", "bin64", "binaries", "game", "games", "data", "app", "apps", "system",
        "win32", "win64", "x86", "x64", "release", "debug", "drive_c", "program files",
        "program files (x86)", "steamapps", "common", "exe", "build",
    };
    const char *end, *start;
    size_t length, i;

    if (!(end = strrchr( path, '/' )) && !(end = strrchr( path, '\\' ))) return 0;
    for (start = end; start > path; start--)
        if (start[-1] == '/' || start[-1] == '\\') break;
    if (start >= end) return 0;
    length = end - start;
    if (length >= size) return 0;
    for (i = 0; i < sizeof(containers) / sizeof(containers[0]); i++)
        if (strlen( containers[i] ) == length && !strncasecmp( start, containers[i], length )) return 0;
    memcpy( out, start, length );
    out[length] = 0;
    return 1;
}

static void load_program_settings( struct launcher *l, struct program *p )
{
    char path[768];
    struct launcher_kv kv;
    size_t len;

    p->own_files = 0;
    memset( &p->settings, 0, sizeof(p->settings) );
    p->settings.verbose = p->settings.profile = p->settings.framebuffer = -1;
    if (launcher_program_settings_path( l->options->runtime_dir, p->path, path, sizeof(path) ) &&
        launcher_kv_load( &kv, path ))
    {
        launcher_settings_read( &kv, &p->settings );
        p->own_files |= kv.size && file_exists( path );
    }
    if (launcher_args_path( p->path, path, sizeof(path) )) p->own_files |= file_exists( path );
    if (launcher_keys_path( p->path, path, sizeof(path) )) p->own_files |= file_exists( path );
    if (launcher_sibling_path( p->path, ".box64.txt", path, sizeof(path) )) p->own_files |= file_exists( path );

    if (p->settings.title[0]) snprintf( p->title, sizeof(p->title), "%s", p->settings.title );
    else if (p->resource_title[0]) snprintf( p->title, sizeof(p->title), "%s", p->resource_title );
    else if (!folder_title( p->path, p->title, sizeof(p->title) ))
    {
        snprintf( p->title, sizeof(p->title), "%s", file_name( p->path ) );
        if ((len = strlen( p->title )) > 4 && !strcasecmp( p->title + len - 4, ".exe" )) p->title[len - 4] = 0;
    }
}

/* Fill in a program from its file; returns 0 when this runtime cannot start it. */
static int describe_program( struct launcher *l, struct program *p, const char *path )
{
    memset( p, 0, sizeof(*p) );
    p->settings.verbose = p->settings.profile = p->settings.framebuffer = -1;
    if ((size_t)snprintf( p->path, sizeof(p->path), "%s", path ) >= sizeof(p->path)) return 0;
    if (!launcher_dos_path( path, p->dos, sizeof(p->dos) )) return 0;
    if (l->options->machine_of( path, &p->machine )) return 0;
    launcher_pe_describe( path, 0, NULL, p->resource_title, sizeof(p->resource_title) );
    load_program_settings( l, p );
    return 1;
}

static void describe_catalog_program( struct launcher *l, struct program *p,
                                      const struct launcher_catalog_entry *entry )
{
    memset( p, 0, sizeof(*p) );
    p->settings.verbose = p->settings.profile = p->settings.framebuffer = -1;
    snprintf( p->path, sizeof(p->path), "%s", entry->path );
    launcher_dos_path( p->path, p->dos, sizeof(p->dos) );
    p->catalog_id = entry->id;
    p->added_order = entry->added_order;
    p->launched_order = entry->launched_order;
    p->favorite = entry->favorite;
    snprintf( p->square_art, sizeof(p->square_art), "%s", entry->square_art );
    snprintf( p->portrait_art, sizeof(p->portrait_art), "%s", entry->portrait_art );
    snprintf( p->hero_art, sizeof(p->hero_art), "%s", entry->hero_art );
    p->added = 1;
    if (file_exists( p->path ) && !l->options->machine_of( p->path, &p->machine ))
    {
        launcher_pe_describe( p->path, 0, NULL, p->resource_title, sizeof(p->resource_title) );
        load_program_settings( l, p );
    }
    else p->missing = 1;
    if (!p->title[0] && entry->title[0]) snprintf( p->title, sizeof(p->title), "%s", entry->title );
    if (!p->title[0] && folder_title( p->path, p->title, sizeof(p->title) )) return;
    if (!p->title[0])
    {
        size_t len;
        snprintf( p->title, sizeof(p->title), "%s", file_name( p->path ) );
        len = strlen( p->title );
        if (len > 4 && !strcasecmp( p->title + len - 4, ".exe" )) p->title[len - 4] = 0;
    }
}

static int find_program( const struct launcher *l, const char *path )
{
    int i;

    for (i = 0; i < l->program_count; i++)
        if (!l->programs[i].removed && !strcasecmp( l->programs[i].path, path )) return i;
    return -1;
}

static int add_program( struct launcher *l, const char *path, int added )
{
    int index = find_program( l, path );

    if (index >= 0) return index;
    if (l->program_count >= LAUNCHER_MAX_ENTRIES) return -1;
    if (!describe_program( l, &l->programs[l->program_count], path )) return -1;
    l->programs[l->program_count].added = added;
    return l->program_count++;
}

static void draw_loading( struct launcher *l, int found )
{
    struct ui *ui = &l->ui;
    char text[64];

    ui_background( ui );
    ui_header( ui, "Library", NULL );
    ui_text_centered( ui, ui->large, ui->width / 2, ui->height / 2 - 48, "Looking for programs...", ui->value );
    snprintf( text, sizeof(text), found == 1 ? "%d program found" : "%d programs found", found );
    ui_text_centered( ui, ui->small, ui->width / 2, ui->height / 2 + 20, text, ui->dim );
    ui_present( ui );
}

static int save_library( struct launcher *l )
{
    char path[512];
    int i;
    launcher_catalog_init( &l->catalog );
    for (i = 0; i < l->program_count; i++)
    {
        const struct program *p = &l->programs[i];
        struct launcher_catalog_entry *entry;
        int index;
        if (p->removed || !p->added) continue;
        index = launcher_catalog_add( &l->catalog, p->path, p->title );
        if (index < 0) continue;
        entry = &l->catalog.entries[index];
        if (p->catalog_id) entry->id = p->catalog_id;
        if (p->added_order) entry->added_order = p->added_order;
        entry->launched_order = p->launched_order;
        entry->favorite = p->favorite;
        snprintf( entry->square_art, sizeof(entry->square_art), "%s", p->square_art );
        snprintf( entry->portrait_art, sizeof(entry->portrait_art), "%s", p->portrait_art );
        snprintf( entry->hero_art, sizeof(entry->hero_art), "%s", p->hero_art );
        if (entry->id >= l->catalog.next_id) l->catalog.next_id = entry->id + 1;
        if (entry->added_order >= l->catalog.next_order) l->catalog.next_order = entry->added_order + 1;
        if (entry->launched_order >= l->catalog.next_order) l->catalog.next_order = entry->launched_order + 1;
    }
    runtime_file( l, LAUNCHER_CATALOG_FILE, path, sizeof(path) );
    if (!launcher_catalog_save( &l->catalog, path ))
    {
        ui_toast( &l->ui, "Could not save the game library", 2500 );
        return 0;
    }
    return 1;
}

static void load_library( struct launcher *l )
{
    char path[512], legacy[512];
    enum launcher_catalog_result result;
    int i;

    draw_loading( l, 0 );
    runtime_file( l, LAUNCHER_CATALOG_FILE, path, sizeof(path) );
    result = launcher_catalog_load( &l->catalog, path );
    if (result == LAUNCHER_CATALOG_MISSING)
    {
        launcher_catalog_init( &l->catalog );
        runtime_file( l, "launcher-library.txt", legacy, sizeof(legacy) );
        if (!launcher_catalog_import_legacy( &l->catalog, legacy ) || !launcher_catalog_save( &l->catalog, path ))
            ui_message( &l->ui, "Library", "The saved game library could not be migrated." );
    }
    else if (result != LAUNCHER_CATALOG_OK)
    {
        launcher_catalog_init( &l->catalog );
        ui_message( &l->ui, "Library", "The game library is unreadable. It was preserved and no games were loaded." );
    }
    for (i = 0; i < l->catalog.count && l->program_count < LAUNCHER_MAX_ENTRIES; i++)
        describe_catalog_program( l, &l->programs[l->program_count++], &l->catalog.entries[i] );
}

static struct launcher *sort_launcher;

static int contains_case( const char *text, const char *needle )
{
    size_t len = strlen( needle );
    if (!len) return 1;
    while (*text)
    {
        if (!strncasecmp( text, needle, len )) return 1;
        text++;
    }
    return 0;
}

static int compare_visible( const void *a, const void *b )
{
    const struct program *x = &sort_launcher->programs[*(const int *)a];
    const struct program *y = &sort_launcher->programs[*(const int *)b];
    int order;

    if (sort_launcher->sort_order == 1 && x->added_order != y->added_order)
        return x->added_order < y->added_order ? 1 : -1;
    if (sort_launcher->sort_order == 2 && x->launched_order != y->launched_order)
        return x->launched_order < y->launched_order ? 1 : -1;
    order = strcasecmp( x->title, y->title );

    return order ? order : strcasecmp( x->dos, y->dos );
}

/* The programs shown, sorted by title; keep_index stays selected when it is shown. */
static void rebuild_visible( struct launcher *l, int keep_index )
{
    int i;

    l->visible_count = 0;
    for (i = 0; i < l->program_count; i++)
    {
        const struct program *p = &l->programs[i];

        if (p->removed || (l->hide_missing && p->missing) || (p->settings.hidden && !l->show_hidden) ||
            (l->favorites_only && !p->favorite) || !contains_case( p->title, l->search )) continue;
        l->visible[l->visible_count++] = i;
    }
    sort_launcher = l;
    qsort( l->visible, l->visible_count, sizeof(l->visible[0]), compare_visible );
    for (i = 0; i < l->visible_count; i++)
        if (l->visible[i] == keep_index) l->selection = i;
    if (l->selection >= l->visible_count) l->selection = l->visible_count ? l->visible_count - 1 : 0;
}

/* Home lists what has been played, most recently started first; nothing else. */
static int compare_history( const void *a, const void *b )
{
    const struct program *x = &sort_launcher->programs[*(const int *)a];
    const struct program *y = &sort_launcher->programs[*(const int *)b];

    if (x->launched_order != y->launched_order) return x->launched_order < y->launched_order ? 1 : -1;
    return strcasecmp( x->title, y->title );
}

static void rebuild_history( struct launcher *l, int keep_index )
{
    int i;

    l->history_count = 0;
    for (i = 0; i < l->program_count; i++)
    {
        const struct program *p = &l->programs[i];

        if (p->removed || !p->launched_order || (l->hide_missing && p->missing) ||
            (p->settings.hidden && !l->show_hidden)) continue;
        l->history[l->history_count++] = i;
    }
    sort_launcher = l;
    qsort( l->history, l->history_count, sizeof(l->history[0]), compare_history );
    for (i = 0; i < l->history_count; i++)
        if (l->history[i] == keep_index) l->history_selection = i;
    if (l->history_selection >= l->history_count) l->history_selection = l->history_count ? l->history_count - 1 : 0;
}

static void save_program_settings( struct launcher *l, struct program *p )
{
    struct launcher_kv kv;
    char path[768], folder[768];
    int ready = 1;

    if (launcher_settings_on_usb( p->path ))
    {
        runtime_file( l, "program-settings", folder, sizeof(folder) );
        ready = !mkdir( folder, 0777 ) || errno == EEXIST;
    }
    if (!ready || !launcher_program_settings_path( l->options->runtime_dir, p->path, path, sizeof(path) ) ||
        !launcher_kv_load( &kv, path ) || !launcher_settings_write( &kv, &p->settings ) ||
        !launcher_kv_save( &kv, path ))
        ui_toast( &l->ui, "Could not save the program's settings", 2500 );
    load_program_settings( l, p );
}

static void enable_program_dxvk( struct launcher *l, struct program *p )
{
    if (!l->options->dxvk_on_add || !launcher_dxvk_directory( p->machine )) return;
    p->settings.dxvk = 1;
    save_program_settings( l, p );
}

/***********************************************************************
 * Icons
 */

static int decode_png( struct launcher_icon *icon )
{
    png_image image;
    unsigned char *rgba;

    memset( &image, 0, sizeof(image) );
    image.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory( &image, icon->data, icon->size )) return 0;
    if (image.width > LAUNCHER_ICON_MAX_SIDE || image.height > LAUNCHER_ICON_MAX_SIDE)
    {
        png_image_free( &image );
        return 0;
    }
    image.format = PNG_FORMAT_RGBA;
    if (!(rgba = malloc( PNG_IMAGE_SIZE( image ) )))
    {
        png_image_free( &image );
        return 0;
    }
    if (!png_image_finish_read( &image, NULL, rgba, 0, NULL ))
    {
        free( rgba );
        return 0;
    }
    free( icon->data );
    icon->kind = LAUNCHER_ICON_RGBA;
    icon->data = rgba;
    icon->width = image.width;
    icon->height = image.height;
    icon->size = PNG_IMAGE_SIZE( image );
    return 1;
}

/* The mark the shell shows in its corner, carried inside the runtime. */
static SDL_Texture *load_logo( struct launcher *l )
{
#ifdef WINE_NX_HAS_EMBEDDED_LOGO
    struct launcher_icon icon;
    SDL_Texture *texture = NULL;
    SDL_Surface *surface;

    memset( &icon, 0, sizeof(icon) );
    icon.size = wine_nx_logo_size;
    if (!icon.size || !(icon.data = malloc( icon.size ))) return NULL;
    memcpy( icon.data, wine_nx_logo, icon.size );
    if (!decode_png( &icon )) { free( icon.data ); return NULL; }
    if ((surface = SDL_CreateRGBSurfaceWithFormatFrom( icon.data, icon.width, icon.height, 32,
                                                       icon.width * 4, SDL_PIXELFORMAT_ABGR8888 )))
    {
        texture = SDL_CreateTextureFromSurface( l->ui.renderer, surface );
        SDL_FreeSurface( surface );
    }
    free( icon.data );
    return texture;
#else
    (void)l;
    return NULL;
#endif
}

/* Cover files are optional. Decode on the existing worker, with bounded input. */
static int read_cover( const char *path, struct launcher_icon *icon )
{
    png_image png = {0};
    struct stat st;
    if (!path[0] || stat( path, &st ) || st.st_size > 16 * 1024 * 1024) return 0;
    png.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_file( &png, path )) return 0;
    if (!png.width || !png.height || png.width > 2048 || png.height > 2048)
    {
        png_image_free( &png );
        return 0;
    }
    png.format = PNG_FORMAT_RGBA;
    memset( icon, 0, sizeof(*icon) );
    icon->data = malloc( PNG_IMAGE_SIZE( png ) );
    if (!icon->data || !png_image_finish_read( &png, NULL, icon->data, 0, NULL ))
    {
        free( icon->data );
        memset( icon, 0, sizeof(*icon) );
        png_image_free( &png );
        return 0;
    }
    icon->kind = LAUNCHER_ICON_RGBA;
    icon->width = png.width;
    icon->height = png.height;
    icon->size = PNG_IMAGE_SIZE( png );
    png_image_free( &png );
    if (!launcher_icon_fit( icon, 512 )) { launcher_icon_free( icon ); return 0; }
    return 1;
}

static int icon_thread( void *arg )
{
    struct launcher *l = arg;

    SDL_LockMutex( l->mutex );
    while (!l->stop)
    {
        struct launcher_icon icon;
        struct icon_result result;
        struct icon_job job;
        SDL_Event event;

        if (!l->job_count || l->result_count == ICON_JOBS)
        {
            SDL_CondWait( l->cond, l->mutex );
            continue;
        }
        job = l->jobs[0];
        memmove( l->jobs, l->jobs + 1, --l->job_count * sizeof(l->jobs[0]) );
        SDL_UnlockMutex( l->mutex );

        memset( &icon, 0, sizeof(icon) );
        result.artwork = read_cover( job.artwork, &icon );
        if (!result.artwork && job.kind == ART_PORTRAIT)
        {
            launcher_pe_describe( job.path, ICON_SIDE, &icon, NULL, 0 );
            if (icon.kind == LAUNCHER_ICON_PNG && !decode_png( &icon )) launcher_icon_free( &icon );
            if (icon.kind == LAUNCHER_ICON_RGBA && !launcher_icon_fit( &icon, ICON_SIDE )) launcher_icon_free( &icon );
        }
        result.index = job.index;
        result.kind = job.kind;
        result.width = icon.width;
        result.height = icon.height;
        result.rgba = icon.kind == LAUNCHER_ICON_RGBA ? icon.data : NULL;
        if (!result.rgba) launcher_icon_free( &icon );

        SDL_LockMutex( l->mutex );
        l->results[l->result_count++] = result;
        memset( &event, 0, sizeof(event) );
        event.type = l->icon_event;
        SDL_PushEvent( &event );
    }
    SDL_UnlockMutex( l->mutex );
    return 0;
}

static void start_icons( struct launcher *l )
{
    l->icon_event = SDL_RegisterEvents( 1 );
    if (l->icon_event == (Uint32)-1) l->icon_event = SDL_USEREVENT;
    l->mutex = SDL_CreateMutex();
    l->cond = SDL_CreateCond();
    if (l->mutex && l->cond) l->thread = SDL_CreateThreadWithStackSize( icon_thread, "launcher icons", 256 * 1024, l );
}

static void stop_icons( struct launcher *l )
{
    int i;

    if (l->thread)
    {
        SDL_LockMutex( l->mutex );
        l->stop = 1;
        SDL_CondSignal( l->cond );
        SDL_UnlockMutex( l->mutex );
        SDL_WaitThread( l->thread, NULL );
    }
    for (i = 0; i < l->result_count; i++) free( l->results[i].rgba );
    l->result_count = l->job_count = 0;
    for (i = 0; i < l->program_count; i++)
    {
        if (l->programs[i].icon) SDL_DestroyTexture( l->programs[i].icon );
        if (l->programs[i].square_icon) SDL_DestroyTexture( l->programs[i].square_icon );
        if (l->programs[i].hero_icon) SDL_DestroyTexture( l->programs[i].hero_icon );
        l->programs[i].icon = NULL;
        l->programs[i].square_icon = l->programs[i].hero_icon = NULL;
    }
    if (l->cond) SDL_DestroyCond( l->cond );
    if (l->mutex) SDL_DestroyMutex( l->mutex );
    l->thread = NULL;
    l->cond = NULL;
    l->mutex = NULL;
}

static void pump_icons( struct launcher *l )
{
    struct icon_result results[ICON_JOBS];
    int count, i, loaded = 0, oldest;

    if (!l->thread) return;
    SDL_LockMutex( l->mutex );
    count = l->result_count;
    memcpy( results, l->results, count * sizeof(results[0]) );
    l->result_count = 0;
    SDL_CondSignal( l->cond );
    SDL_UnlockMutex( l->mutex );

    for (i = 0; i < count; i++)
    {
        struct program *p = &l->programs[results[i].index];
        SDL_Texture *texture = NULL;

        if (results[i].rgba &&
            (texture = SDL_CreateTexture( l->ui.renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC,
                                          results[i].width, results[i].height )))
        {
            SDL_UpdateTexture( texture, NULL, results[i].rgba, results[i].width * 4 );
            SDL_SetTextureBlendMode( texture, SDL_BLENDMODE_BLEND );
        }
        free( results[i].rgba );
        if (results[i].kind == ART_SQUARE)
        {
            p->square_icon = texture; p->square_width = results[i].width; p->square_height = results[i].height;
            p->square_time = SDL_GetTicks(); p->square_state = texture ? ICON_READY : ICON_MISSING;
        }
        else if (results[i].kind == ART_HERO)
        {
            p->hero_icon = texture; p->hero_width = results[i].width; p->hero_height = results[i].height;
            p->hero_time = SDL_GetTicks(); p->hero_state = texture ? ICON_READY : ICON_MISSING;
        }
        else
        {
            p->icon = texture; p->icon_width = results[i].width; p->icon_height = results[i].height;
            p->icon_is_art = results[i].artwork; p->icon_time = SDL_GetTicks();
            p->icon_state = texture ? ICON_READY : ICON_MISSING;
        }
    }

    /* Keep the most recently shown icons. */
    for (i = 0; i < l->program_count; i++)
        loaded += (l->programs[i].icon != NULL) + (l->programs[i].square_icon != NULL) +
                  (l->programs[i].hero_icon != NULL);
    while (loaded > ICON_TEXTURES)
    {
        oldest = -1;
        for (i = 0; i < l->program_count; i++)
            if ((l->programs[i].icon || l->programs[i].square_icon || l->programs[i].hero_icon) &&
                l->programs[i].icon_use <= l->icon_frame &&
                (oldest < 0 || l->programs[i].icon_use < l->programs[oldest].icon_use))
                oldest = i;
        /* Everything held is on the screen now: keep it and let the frame be
         * over the limit rather than throw away a cover about to be drawn. */
        if (oldest < 0) break;
        if (l->programs[oldest].hero_icon)
        {
            SDL_DestroyTexture( l->programs[oldest].hero_icon );
            l->programs[oldest].hero_icon = NULL; l->programs[oldest].hero_state = ICON_UNKNOWN;
        }
        else if (l->programs[oldest].square_icon)
        {
            SDL_DestroyTexture( l->programs[oldest].square_icon );
            l->programs[oldest].square_icon = NULL; l->programs[oldest].square_state = ICON_UNKNOWN;
        }
        else
        {
            SDL_DestroyTexture( l->programs[oldest].icon );
            l->programs[oldest].icon = NULL; l->programs[oldest].icon_state = ICON_UNKNOWN;
        }
        loaded--;
    }
}

static void request_art( struct launcher *l, int index, enum art_kind kind )
{
    struct program *p = &l->programs[index];
    enum icon_state *state = kind == ART_SQUARE ? &p->square_state : kind == ART_HERO ? &p->hero_state : &p->icon_state;
    const char *art = kind == ART_SQUARE ? p->square_art : kind == ART_HERO ? p->hero_art : p->portrait_art;

    p->icon_use = ++l->icon_use;
    if (*state != ICON_UNKNOWN || !l->thread) return;
    SDL_LockMutex( l->mutex );
    if (l->job_count < ICON_JOBS)
    {
        l->jobs[l->job_count].index = index;
        l->jobs[l->job_count].kind = kind;
        memcpy( l->jobs[l->job_count].path, p->path, sizeof(p->path) );
        if (art[0])
            snprintf( l->jobs[l->job_count].artwork, sizeof(l->jobs[0].artwork), "%s",
                      art );
        else if (kind == ART_PORTRAIT)
        {
            char folder[512];
            snprintf( folder, sizeof(folder), "%s", p->path );
            parent_dir( folder );
            if (snprintf( l->jobs[l->job_count].artwork, sizeof(l->jobs[0].artwork), "%s/cover.png", folder ) >=
                (int)sizeof(l->jobs[0].artwork)) l->jobs[l->job_count].artwork[0] = 0;
        }
        else l->jobs[l->job_count].artwork[0] = 0;
        l->job_count++;
        *state = ICON_QUEUED;
        SDL_CondSignal( l->cond );
    }
    SDL_UnlockMutex( l->mutex );
}

static void request_icon( struct launcher *l, int index ) { request_art( l, index, ART_PORTRAIT ); }

/***********************************************************************
 * Library grid
 */

struct grid
{
    int card, gap_x, gap_y, caption, x0, y0, columns, rows, first;
};

static void draw_shell( struct launcher *l, int home );
static void draw_footprint( struct launcher *l, int with_address_space );
static void draw_backdrop( struct launcher *l, int current );
static void draw_cover( struct ui *ui, const struct program *p, SDL_Rect rect, int radius, int brightness,
                        int alpha );

/* The library is one list that scrolls, so the only thing to work out is how
 * many covers stand between the margins the header keeps: as many as fit at
 * about the size the reference gives them, sharing what is left over. */
#define GRID_CARD_TARGET 200

static void grid_layout( const struct launcher *l, struct grid *g )
{
    const struct ui *ui = &l->ui;
    int width = ui->width - 2 * SHELL_MARGIN, available = ui->height - UI_HEADER_HEIGHT - FOOTER_SPACE, h, rows;

    g->gap_x = 22;
    g->gap_y = 16;
    g->caption = 36;
    g->columns = (width + g->gap_x) / (GRID_CARD_TARGET + g->gap_x);
    if (g->columns < 1) g->columns = 1;
    g->card = (width - (g->columns - 1) * g->gap_x) / g->columns;
    if (g->card < 64) g->card = 64;
    g->rows = (available - 24 + g->gap_y) / (g->card + g->caption + g->gap_y);
    if (g->rows < 1) g->rows = 1;
    rows = (l->visible_count + g->columns - 1) / g->columns;
    if (rows < 1) rows = 1;
    g->first = launcher_first_visible( l->top_row, l->selection / g->columns, rows, g->rows ) * g->columns;
    h = g->rows * (g->card + g->caption) + (g->rows - 1) * g->gap_y;
    g->x0 = SHELL_MARGIN;
    g->y0 = UI_HEADER_HEIGHT + (available - h) / 2 + 4;
}

static int grid_hit( const struct launcher *l, int x, int y )
{
    int row, column, hit;
    struct grid g;

    grid_layout( l, &g );
    if (x < g.x0 || y < g.y0) return -1;
    column = (x - g.x0) / (g.card + g.gap_x);
    row = (y - g.y0) / (g.card + g.caption + g.gap_y);
    if (column >= g.columns || row >= g.rows) return -1;
    if ((x - g.x0) % (g.card + g.gap_x) >= g.card || (y - g.y0) % (g.card + g.caption + g.gap_y) >= g.card + g.caption)
        return -1;
    hit = g.first + row * g.columns + column;
    return hit < l->visible_count ? hit : -1;
}

/* A window with a title bar and the program's first letter, for programs without an icon. */
static void draw_placeholder( struct launcher *l, const struct program *p, int cx, int cy, int size, int alpha )
{
    struct ui *ui = &l->ui;
    SDL_Color frame = ui->dim, fill = { 0, 0, 0, 60 * alpha / 255 };
    int w = size, h = size * 3 / 4, x = cx - w / 2, y = cy - h / 2;
    char letter[8] = {0};
    size_t len = 1;

    frame.a = 200 * alpha / 255;
    ui_rounded( ui, x, y, w, h, 8, fill );
    ui_border( ui, x, y, w, h, 2, frame );
    ui_fill( ui, x, y, w, h / 6, frame );
    while ((p->title[len] & 0xc0) == 0x80 && len < 4) len++;
    memcpy( letter, p->title, p->title[0] ? len : 0 );
    if (letter[0] >= 'a' && letter[0] <= 'z') letter[0] -= 'a' - 'A';
    frame.a = alpha;
    ui_text_centered( ui, ui->large, cx, y + h / 6 + (h * 5 / 6 - TTF_FontHeight( ui->large )) / 2, letter, frame );
}

static void draw_card( struct launcher *l, int index, int x, int y, const struct grid *g, int current )
{
    struct ui *ui = &l->ui;
    struct program *p = &l->programs[l->visible[index]];
    int cx = x + g->card / 2, cy = y + g->card / 2, target = g->card * 60 / 100, dim = current ? 255 : 165;
    SDL_Color caption = current ? ui->value : ui->dim;
    int text_w;

    request_art( l, l->visible[index], ART_SQUARE );
    if (!p->square_art[0]) request_icon( l, l->visible[index] );
    if (current)
    {
        if (ui->glow)
        {
            SDL_Rect rect = { x - g->card / 4, y - g->card / 4, g->card * 3 / 2, g->card * 3 / 2 };

            SDL_SetTextureColorMod( ui->glow, 255, 255, 255 );
            SDL_SetTextureAlphaMod( ui->glow, 40 );
            SDL_RenderCopy( ui->renderer, ui->glow, NULL, &rect );
        }
        /* The same light that goes round what has the focus everywhere else,
         * rather than a plate of its own: the two views frame the selection the
         * same way, and the way the rest of the launcher does. */
        ui_animated_border( ui, x - 3, y - 3, g->card + 6, g->card + 6, 17, 3,
                            (SDL_Color){ 150, 160, 176, 90 }, (SDL_Color){ 244, 247, 250, 255 } );
    }
    else
    {
        ui_rounded( ui, x + 4, y + 6, g->card, g->card, 14, (SDL_Color){ 0, 0, 0, 55 } );
        ui_rounded( ui, x + 2, y + 3, g->card, g->card, 14, (SDL_Color){ 0, 0, 0, 70 } );
    }
    ui_rounded( ui, x, y, g->card, g->card, 14, current ? ui->focus : ui->card );
    ui_fill( ui, x + 14, y, g->card - 28, 1, (SDL_Color){ 255, 255, 255, 30 } );

    if (p->square_icon)
    {
        SDL_Rect src = {0, 0, p->square_width, p->square_height};
        ui_rounded_texture( ui, p->square_icon, &src, (SDL_Rect){x, y, g->card, g->card}, 14,
                            (SDL_Color){ current ? 255 : 190, current ? 255 : 190, current ? 255 : 190, 255 } );
    }
    else if (p->icon && p->icon_is_art)
        draw_cover( ui, p, (SDL_Rect){x, y, g->card, g->card}, 14, current ? 255 : 190, 255 );
    else if (p->icon)
    {
        int side = p->icon_width > p->icon_height ? p->icon_width : p->icon_height, scale, w, h;
        Uint32 age = SDL_GetTicks() - p->icon_time;
        SDL_Rect dst;

        /* Small pixel-art icons grow by whole steps so their pixels stay square. */
        if (side * 2 <= target)
        {
            scale = target / side;
            SDL_SetTextureScaleMode( p->icon, SDL_ScaleModeNearest );
        }
        else
        {
            scale = 0;
            SDL_SetTextureScaleMode( p->icon, SDL_ScaleModeLinear );
        }
        w = scale ? p->icon_width * scale : p->icon_width * target / side;
        h = scale ? p->icon_height * scale : p->icon_height * target / side;
        dst = (SDL_Rect){ cx - w / 2, cy - h / 2 - 4, w, h };
        SDL_SetTextureColorMod( p->icon, dim, dim, dim );
        SDL_SetTextureAlphaMod( p->icon, ui->animations && age < 180 ? age * 255 / 180 : 255 );
        SDL_RenderCopy( ui->renderer, p->icon, NULL, &dst );
    }
    else draw_placeholder( l, p, cx, cy - 4, target, p->icon_state == ICON_MISSING ? dim : dim / 3 );

    if (p->settings.hidden)
    {
        text_w = ui_text_width( ui, ui->small, "Hidden" );
        ui_rounded( ui, x + g->card - text_w - 26, y + g->card - 32, text_w + 16, TTF_FontHeight( ui->small ) + 4, 10,
                    (SDL_Color){ 0, 0, 0, 140 } );
        ui_text( ui, ui->small, x + g->card - text_w - 18, y + g->card - 30, "Hidden", ui->dim );
    }

    text_w = ui_text_width( ui, ui->small, p->title );
    if (text_w > g->card + g->gap_x - 6) text_w = g->card + g->gap_x - 6;
    ui_text_fit( ui, ui->small, cx - text_w / 2, y + g->card + 8, g->card + g->gap_x - 6, p->title, caption, current );
}

static void draw_library( struct launcher *l )
{
    /* A does what is in focus: with the header in focus it is that, not the game
     * the selection is remembered on. */
    struct ui_hint hints[] = { { UI_A, "Play" }, { UI_Y, "Options" }, { UI_PLUS, "Menu" } };
    /* With nothing to act on, only the menu means anything. */
    static const struct ui_hint empty_hints[] = { { UI_PLUS, "Menu" } };
    struct ui *ui = &l->ui;
    struct grid g;
    int shown, i;

    if (l->zone == ZONE_HEADER) hints[0].label = "Select";

    draw_backdrop( l, l->visible_count ? l->visible[l->selection] : -1 );
    ui_fill( ui, 0, 0, ui->width, ui->height, (SDL_Color){ 0, 0, 0, 120 } );
    grid_layout( l, &g );
    l->top_row = g.first / g.columns;
    shown = g.columns * g.rows;

    draw_shell( l, 0 );
    for (i = g.first; i < l->visible_count && i < g.first + shown; i++)
    {
        int column = (i - g.first) % g.columns, row = (i - g.first) / g.columns;

        if (i == l->selection) continue;
        draw_card( l, i, g.x0 + column * (g.card + g.gap_x), g.y0 + row * (g.card + g.caption + g.gap_y), &g, 0 );
    }
    if (l->visible_count && l->selection >= g.first && l->selection < g.first + shown)
    {
        int column = (l->selection - g.first) % g.columns, row = (l->selection - g.first) / g.columns;

        /* Framed only while the games themselves have the focus: with the
         * header in focus the selection is remembered, not pointed at. */
        draw_card( l, l->selection, g.x0 + column * (g.card + g.gap_x),
                   g.y0 + row * (g.card + g.caption + g.gap_y), &g, l->zone != ZONE_HEADER );
    }
    /* Decode what is just off the bottom too, so scrolling on shows it at once. */
    for (i = g.first + shown; i < l->visible_count && i < g.first + 2 * shown; i++)
        request_icon( l, l->visible[i] );

    if (!l->visible_count)
    {
        ui_text_centered( ui, ui->large, ui->width / 2, ui->height / 2 - 70,
                          l->program_count ? "No games match this view" : "Your library is empty", ui->value );
        ui_text_wrapped( ui, ui->normal, ui->width / 2, ui->height / 2, 900, 3,
                         l->program_count ? "Change the search or favorite filter, or show hidden games in Settings."
                                          : "Press + and choose Add game to browse for a Windows executable.",
                         ui->dim, 1 );
    }
    if (l->visible_count) ui_hints_right( ui, hints, sizeof(hints) / sizeof(hints[0]), ui->width - SHELL_MARGIN, HOME_HINT_Y );
    else ui_hints_right( ui, empty_hints, 1, ui->width - SHELL_MARGIN, HOME_HINT_Y );
    ui_fade( ui );
}

static int draw_symbol( struct launcher *l, enum symbol symbol, int x, int cy, int alpha )
{
    SDL_Texture *texture = l->symbols[symbol];
    SDL_Rect dst;

    if (!texture || SDL_QueryTexture( texture, NULL, NULL, &dst.w, &dst.h )) return 0;
    dst.x = x;
    dst.y = cy - dst.h / 2;
    SDL_SetTextureAlphaMod( texture, alpha );
    SDL_RenderCopy( l->ui.renderer, texture, NULL, &dst );
    return dst.w;
}

static void draw_battery( struct ui *ui, int x, int cy, int percent, int charging )
{
    const SDL_Color line = { 255, 255, 255, 235 };
    const int w = 26, h = 13, y = cy - h / 2;
    int level = (w - 6) * (percent < 0 ? 0 : percent > 100 ? 100 : percent) / 100;

    ui_fill( ui, x + 1, y, w - 2, 2, line );
    ui_fill( ui, x + 1, y + h - 2, w - 2, 2, line );
    ui_fill( ui, x, y + 1, 2, h - 2, line );
    ui_fill( ui, x + w - 2, y + 1, 2, h - 2, line );
    ui_fill( ui, x + w + 1, y + 4, 2, h - 8, line );
    ui_fill( ui, x + 3, y + 3, level, h - 6, charging ? (SDL_Color){ 124, 222, 146, 255 } : line );
}

static const char *const shell_labels[SHELL_TABS] = { "Home", "Library" };

/* How wide a header item is: its icon, and as much of the name as is out. */
static int shell_width( struct launcher *l, int tab )
{
    int width = 0;

    if (l->symbols[tab]) SDL_QueryTexture( l->symbols[tab], NULL, NULL, &width, NULL );
    if (shell_labels[tab])
        width += (int)((12 + ui_text_width( &l->ui, l->ui.normal, shell_labels[tab] )) * l->label_open[tab] + 0.5f);
    return width;
}

/* The clock and the battery, at the right of whichever header asks for them.
 * Returns where the left of what it drew is. */
static int draw_status( struct launcher *l, int right, int cy )
{
    struct ui *ui = &l->ui;
    Uint32 now = SDL_GetTicks();
    char text[32];

    if (!l->status_read || now - l->status_read >= 1000)
    {
        l->status = launcher_platform_status( &l->clock_hour, &l->clock_minute, &l->battery, &l->charging );
        l->status_read = now ? now : 1;
    }
    if (l->status & LAUNCHER_STATUS_BATTERY)
    {
        snprintf( text, sizeof(text), "%d%%", l->battery );
        right -= ui_text_width( ui, ui->small, text );
        ui_text( ui, ui->small, right, cy - TTF_FontHeight( ui->small ) / 2, text, ui->value );
        right -= 8 + 29;
        draw_battery( ui, right, cy, l->battery, l->charging );
        right -= 24;
    }
    if (l->status & LAUNCHER_STATUS_CLOCK)
    {
        snprintf( text, sizeof(text), "%02d:%02d", l->clock_hour, l->clock_minute );
        right -= ui_text_width( ui, ui->small, text );
        ui_text( ui, ui->small, right, cy - TTF_FontHeight( ui->small ) / 2, text, ui->value );
    }
    return right;
}

/* What a header outside the shell asks for, through the ui. */
static void header_status( void *data, int right, int y )
{
    struct launcher *l = data;

    l->ui.status_left = draw_status( l, right, y );
}

/* The header over Home and Library: the current view's icon and name, the other
 * views and Add Game as icons, and Settings, the clock and the battery at the right. */
static void draw_shell( struct launcher *l, int home )
{
    static const struct { enum symbol symbol; const char *label; } tabs[] =
    {
        [SHELL_HOME] = { SYMBOL_HOME, "Home" },
        [SHELL_LIBRARY] = { SYMBOL_LIBRARY, "Library" },
    };
    struct ui *ui = &l->ui;
    int x = SHELL_MARGIN, right = ui->width - SHELL_MARGIN, i, width;

    ui_gradient( ui, 0, 0, ui->width, 150, (SDL_Color){ 0, 0, 0, 150 }, (SDL_Color){ 0, 0, 0, 0 }, 0 );
    /* One name closes as the other opens, and everything after them moves with
     * it: the widths below are measured from where they have got to. */
    for (i = SHELL_HOME; i < SHELL_TABS; i++)
    {
        float target = i == (home ? SHELL_HOME : SHELL_LIBRARY) ? 1.0f : 0.0f;

        if (!ui->animations) l->label_open[i] = target;
        else l->label_open[i] += (target - l->label_open[i]) * 0.22f;
        if (fabsf( target - l->label_open[i] ) < 0.004f) l->label_open[i] = target;
    }

    /* The clock and the battery first: Settings stands to the left of whatever
     * they take, and the pill has to know where that is before it goes there. */
    ui->status_left = right = draw_status( l, right, SHELL_Y );
    right -= SHELL_GAP;
    width = l->symbols[SYMBOL_SETTINGS] ? SHELL_ICON - 4 : 0;
    right -= width;

    /* Where the pill is going, before anything is drawn over it. */
    {
        int target_x = 0, target_w = 0, at = SHELL_MARGIN;

        for (i = SHELL_HOME; i <= SHELL_LIBRARY; i++)
        {
            int item_w = shell_width( l, i );

            if (l->zone == ZONE_HEADER && l->header_focus == i)
            {
                target_x = at - SHELL_PADDING;
                target_w = item_w + 2 * SHELL_PADDING;
            }
            at += item_w + SHELL_GAP;
        }
        if (l->zone == ZONE_HEADER && l->header_focus == SHELL_SETTINGS)
        {
            target_x = right - SHELL_PADDING;
            target_w = width + 2 * SHELL_PADDING;
        }
        if (!target_w) l->pill_w = 0;      /* focus left the header; it starts again where it returns */
        else
        {
            if (!ui->animations || l->pill_w <= 0)
            {
                l->pill_x = target_x;
                l->pill_w = target_w;
            }
            else
            {
                l->pill_x += (target_x - l->pill_x) * 0.30f;
                l->pill_w += (target_w - l->pill_w) * 0.30f;
            }
            ui_animated_border( ui, (int)(l->pill_x + 0.5f), SHELL_Y - 24, (int)(l->pill_w + 0.5f), 48,
                                14, 2, (SDL_Color){ 150, 160, 176, 90 },
                                (SDL_Color){ 244, 247, 250, 255 } );
        }
    }
    for (i = SHELL_HOME; i <= SHELL_LIBRARY; i++)
    {
        int focused = l->zone == ZONE_HEADER && l->header_focus == i;
        float lit = focused ? 1.0f : l->label_open[i];
        int start = x;

        x += draw_symbol( l, tabs[i].symbol, x, SHELL_Y, 150 + (int)(105 * lit) );
        if (tabs[i].label && l->label_open[i] > 0.01f)
        {
            x += (int)(12 * l->label_open[i] + 0.5f);
            ui_text_opening( ui, ui->normal, x, SHELL_Y - TTF_FontHeight( ui->normal ) / 2, tabs[i].label,
                             ui->value, l->label_open[i] );
            x += (int)(ui_text_width( ui, ui->normal, tabs[i].label ) * l->label_open[i] + 0.5f);
        }
        l->shell_hits[i] = (SDL_Rect){ start - SHELL_GAP / 2, 0, x - start + SHELL_GAP, UI_HEADER_HEIGHT };
        x += SHELL_GAP;
    }

    draw_symbol( l, SYMBOL_SETTINGS, right, SHELL_Y,
                 l->zone == ZONE_HEADER && l->header_focus == SHELL_SETTINGS ? 255 : 190 );
    l->shell_hits[SHELL_SETTINGS] = (SDL_Rect){ right - SHELL_GAP / 2, 0, width + SHELL_GAP, UI_HEADER_HEIGHT };
    if (home) draw_footprint( l, 1 );
}

/* The mark, and beside it the address space when it is the low one -- which is
 * the only one worth saying, because it is the only one that changes what can
 * be started. It belongs on the home screen, where there is room for it and
 * nothing else to read; the settings show the mark alone, and the library is
 * covers, where one more thing in the corner is one too many. */
static void draw_footprint( struct launcher *l, int with_address_space )
{
    struct ui *ui = &l->ui;
    const int line = HOME_HINT_Y - 12;
    int height = 44, x = SHELL_MARGIN, width, h;

    if (l->logo)
    {
        SDL_Rect rect;

        SDL_QueryTexture( l->logo, NULL, NULL, &width, &h );
        rect.h = height;
        rect.w = h ? width * height / h : height;
        rect.x = x;
        rect.y = line - height / 2;
        SDL_RenderCopy( ui->renderer, l->logo, NULL, &rect );
        x += rect.w + 12;
    }
    if (!with_address_space || l->options->address_space_bits != 32) return;
    ui_text( ui, ui->small, x, line - TTF_FontHeight( ui->small ) / 2,
             "Running 32-bit address space", ui->dim );
}

/* What the settings screens ask for through the ui. */
static void footer_mark( void *data )
{
    draw_footprint( data, 0 );
}

static SDL_Rect cover_crop( const struct program *p, int width, int height )
{
    SDL_Rect src = {0, 0, p->icon_width, p->icon_height};
    if (src.w * height > src.h * width)
    {
        src.w = src.h * width / height;
        src.x = (p->icon_width - src.w) / 2;
    }
    else
    {
        src.h = src.w * height / width;
        src.y = (p->icon_height - src.h) / 2;
    }
    return src;
}

static void draw_cover( struct ui *ui, const struct program *p, SDL_Rect rect, int radius, int brightness, int alpha )
{
    SDL_Rect src = cover_crop( p, rect.w, rect.h );

    ui_rounded_texture( ui, p->icon, &src, rect, radius, (SDL_Color){ brightness, brightness, brightness, alpha } );
}

static void draw_backdrop_art( struct launcher *l, int index, int alpha )
{
    struct ui *ui = &l->ui;
    struct program *p;
    Uint32 age;
    SDL_Rect src;

    if (index < 0 || alpha <= 0) return;
    p = &l->programs[index];
    request_art( l, index, ART_HERO );
    if (!p->hero_icon)
    {
        request_icon( l, index );
        if (!p->icon || !p->icon_is_art) return;
    }
    age = SDL_GetTicks() - (p->hero_icon ? p->hero_time : p->icon_time);
    if (ui->animations && age < BACKDROP_FADE_MS) alpha = alpha * (int)age / BACKDROP_FADE_MS;
    if (p->hero_icon)
    {
        int sw = p->hero_width, sh = p->hero_height;
        src = (SDL_Rect){0, 0, sw, sh};
        if ((long long)sw * ui->height > (long long)sh * ui->width)
        { src.w = sh * ui->width / ui->height; src.x = (sw - src.w) / 2; }
        else { src.h = sw * ui->height / ui->width; src.y = (sh - src.h) / 2; }
        SDL_SetTextureColorMod( p->hero_icon, 205, 205, 205 );
        SDL_SetTextureAlphaMod( p->hero_icon, alpha );
        SDL_SetTextureScaleMode( p->hero_icon, SDL_ScaleModeLinear );
        SDL_RenderCopy( ui->renderer, p->hero_icon, &src, NULL );
    }
    else
    {
        src = cover_crop( p, ui->width, ui->height );
        SDL_SetTextureColorMod( p->icon, 205, 205, 205 );
        SDL_SetTextureAlphaMod( p->icon, alpha );
        SDL_SetTextureScaleMode( p->icon, SDL_ScaleModeLinear );
        SDL_RenderCopy( ui->renderer, p->icon, &src, NULL );
    }
}

/* The focused game's artwork behind the whole screen, shaded toward the left and
 * the bottom where the text sits, and crossfaded from the last game's. */
static void draw_backdrop( struct launcher *l, int current )
{
    struct ui *ui = &l->ui;
    Uint32 now = SDL_GetTicks();
    int alpha = 255;

    ui_background( ui );
    if (current != l->backdrop)
    {
        l->backdrop_previous = l->backdrop;
        l->backdrop = current;
        l->backdrop_since = now;
    }
    if (ui->animations && now - l->backdrop_since < BACKDROP_FADE_MS)
        alpha = 255 * (int)(now - l->backdrop_since) / BACKDROP_FADE_MS;
    else l->backdrop_previous = -1;
    draw_backdrop_art( l, l->backdrop_previous, 255 );
    draw_backdrop_art( l, l->backdrop, alpha );

    ui_fill( ui, 0, 0, ui->width, ui->height, (SDL_Color){ 0, 0, 0, 30 } );
    ui_gradient( ui, 0, 0, ui->width * 2 / 3, ui->height, (SDL_Color){ 0, 0, 0, 130 }, (SDL_Color){ 0, 0, 0, 0 }, 1 );
    ui_gradient( ui, 0, ui->height / 2, ui->width, ui->height - ui->height / 2,
                 (SDL_Color){ 0, 0, 0, 0 }, (SDL_Color){ 0, 0, 0, 215 }, 0 );
}

/* Where a cover sits. offset is the row's scroll in covers, focus how focused
 * this one is (0-1): focused covers are larger and push the rest along. */
static SDL_Rect carousel_rect( float slot, float focus )
{
    float expansion = slot < 0 ? 0 : slot > 1 ? 1 : slot;

    return (SDL_Rect){ SHELL_MARGIN + (int)lroundf( slot * (HOME_CARD_W + HOME_CARD_GAP) +
                                                    expansion * (HOME_FOCUS_W + HOME_FOCUS_GAP - HOME_CARD_W - HOME_CARD_GAP) ),
                       HOME_TOP, HOME_CARD_W + (int)lroundf( (HOME_FOCUS_W - HOME_CARD_W) * focus ),
                       HOME_CARD_H + (int)lroundf( (HOME_FOCUS_H - HOME_CARD_H) * focus ) };
}

static void draw_carousel_card( struct launcher *l, int index, SDL_Rect rect, float focus )
{
    struct ui *ui = &l->ui;
    struct program *p = &l->programs[l->history[index]];
    int x = rect.x, y = rect.y, w = rect.w, h = rect.h;
    int target = w / 2, cx = x + w / 2, cy = y + h / 2, shade = 170 + (int)(85 * focus);

    request_icon( l, l->history[index] );
    if (focus > 0 && l->zone != ZONE_HEADER)
    {
        /* A soft light behind the focused cover, and a thin bright edge around
         * it -- while the games have the focus. The header takes it away. */
        int strength = (int)(focus * 255);

        if (ui->glow)
        {
            SDL_Rect glow = { x - w / 3, y - h / 4, w + w * 2 / 3, h + h / 2 };
            SDL_SetTextureColorMod( ui->glow, 255, 255, 255 );
            SDL_SetTextureAlphaMod( ui->glow, strength * 40 / 255 );
            SDL_RenderCopy( ui->renderer, ui->glow, NULL, &glow );
        }
        /* The light that goes round what has the focus, coming up as the cover
         * comes into the middle. */
        ui_animated_border( ui, x - 3, y - 3, w + 6, h + 6, HOME_RADIUS + 3, 3,
                            (SDL_Color){ 150, 160, 176, strength * 90 / 255 },
                            (SDL_Color){ 244, 247, 250, strength } );
    }
    ui_rounded( ui, x, y, w, h, HOME_RADIUS, (SDL_Color){ 30, 33, 36, 255 } );
    if (p->icon && p->icon_is_art) draw_cover( ui, p, rect, HOME_RADIUS, 225 + (int)(30 * focus), 255 );
    else
    {
        if (p->icon)
        {
            int side = p->icon_width > p->icon_height ? p->icon_width : p->icon_height;
            int iw = p->icon_width * target / side, ih = p->icon_height * target / side;
            SDL_Rect dst = { cx - iw / 2, cy - ih / 2 - 5, iw, ih };
            SDL_SetTextureScaleMode( p->icon, SDL_ScaleModeLinear );
            SDL_SetTextureColorMod( p->icon, shade, shade, shade );
            SDL_SetTextureAlphaMod( p->icon, 255 );
            SDL_RenderCopy( ui->renderer, p->icon, NULL, &dst );
        }
        else draw_placeholder( l, p, cx, cy - 5, target, shade );
        ui_text_wrapped( ui, ui->small, cx, y + h - 66, w - 24, 2, p->title, ui->text, 1 );
    }
    /* The gloss over the cover: brightest along its top edge, gone lower down. */
    if (focus > 0)
        ui_rounded_texture( ui, ui_sheen( ui ), NULL, rect, HOME_RADIUS,
                            (SDL_Color){ 255, 255, 255, (int)(38 * focus) } );
    if (p->missing)
    {
        int badge_h = TTF_FontHeight( ui->small ) + 8;
        int badge_w = ui_text_width( ui, ui->small, "Missing" ) + 20;

        ui_rounded( ui, x + 12, y + 12, badge_w, badge_h, badge_h / 2,
                    (SDL_Color){ 120, 28, 32, 230 } );
        ui_text( ui, ui->small, x + 22, y + 12 + (badge_h - TTF_FontHeight( ui->small )) / 2,
                 "Missing", ui->value );
    }
}

static int carousel_hit( const struct launcher *l, int x, int y )
{
    SDL_Point point = {x, y};
    int i;
    if (l->history_count && SDL_PointInRect( &point, &l->carousel_hits[l->history_selection] ))
        return l->history_selection;
    for (i = 0; i < l->history_count; i++)
        if (SDL_PointInRect( &point, &l->carousel_hits[i] )) return i;
    return -1;
}

/* A label in a rounded outline beside the title; returns its width. */
static int draw_tag( struct ui *ui, int x, int cy, const char *text, int warning )
{
    const int h = 30, w = ui_text_width( ui, ui->small, text ) + 26;

    ui_rounded( ui, x, cy - h / 2, w, h, h / 2, warning ? (SDL_Color){ 196, 64, 68, 255 } : (SDL_Color){ 255, 255, 255, 64 } );
    ui_rounded( ui, x + 1, cy - h / 2 + 1, w - 2, h - 2, h / 2 - 1,
                warning ? (SDL_Color){ 120, 28, 32, 235 } : (SDL_Color){ 12, 14, 16, 150 } );
    ui_text( ui, ui->small, x + 13, cy - TTF_FontHeight( ui->small ) / 2, text, ui->value );
    return w;
}

static void draw_home( struct launcher *l )
{
    struct ui_hint hints[] = { { UI_A, "Play" }, { UI_Y, "Options" }, { UI_PLUS, "Menu" } };
    static const struct ui_hint empty_hints[] = { { UI_A, "Open Library" } };
    struct ui *ui = &l->ui;
    int i;
    Uint32 now = SDL_GetTicks();

    if (!l->carousel_started || !ui->animations)
        l->carousel_position = l->history_selection;
    else
    {
        float dt = (now - l->carousel_tick) / 1000.0f;
        if (dt > 0.05f) dt = 0.05f;
        l->carousel_position += (l->history_selection - l->carousel_position) * (1 - expf( -18 * dt ));
        if (fabsf( l->history_selection - l->carousel_position ) < 0.002f) l->carousel_position = l->history_selection;
    }
    l->carousel_started = 1;
    l->carousel_tick = now;
    memset( l->carousel_hits, 0, sizeof(l->carousel_hits) );
    memset( &l->add_hit, 0, sizeof(l->add_hit) );

    draw_backdrop( l, l->history_count ? l->history[l->history_selection] : -1 );
    draw_shell( l, 1 );
    if (!l->history_count)
    {
        const SDL_Rect open = { (ui->width - 260) / 2, 420, 260, 60 };

        ui_text_centered( ui, ui->large, ui->width / 2, 250, "Nothing played yet", ui->value );
        ui_text_wrapped( ui, ui->normal, ui->width / 2, 320, 760, 2,
                         "Games you start appear here, the most recent first. Open your library to choose one.",
                         ui->dim, 1 );
        ui_rounded( ui, open.x, open.y, open.w, open.h, 20, (SDL_Color){ 242, 244, 246, 255 } );
        ui_text_centered( ui, ui->normal, ui->width / 2, open.y + (open.h - TTF_FontHeight( ui->normal )) / 2,
                          "Open Library", (SDL_Color){ 18, 20, 24, 255 } );
        l->add_hit = open;
        ui_hints_right( ui, empty_hints, 1, ui->width - SHELL_MARGIN, HOME_HINT_Y );
    }
    else
    {
        struct program *p = &l->programs[l->history[l->history_selection]];
        const SDL_Rect viewport = { 0, HOME_TOP - 60, ui->width, HOME_FOCUS_H + 120 };
        /* The focus stays where it is and the covers slide through it. */
        float offset = l->carousel_position;
        SDL_Rect focused_rect = {0};
        int title_x, focused_drawn = 0;

        SDL_RenderSetClipRect( ui->renderer, &viewport );
        for (i = 0; i <= l->history_count; i++)
        {
            /* The focused cover last, so its light falls over its neighbours. */
            int index = i == l->history_count ? l->history_selection : i;
            float slot = index - offset, closeness = 1 - fabsf( index - l->carousel_position );
            float focus = closeness < 0 ? 0 : closeness;
            SDL_Rect rect = carousel_rect( slot, focus );

            if (i < l->history_count && index == l->history_selection) continue;
            if (rect.x >= ui->width || rect.x + rect.w <= 0) continue;
            SDL_IntersectRect( &rect, &viewport, &l->carousel_hits[index] );
            if (i == l->history_count)
            {
                focused_rect = rect;
                focused_drawn = 1;
            }
            draw_carousel_card( l, index, rect, focus );
        }
        SDL_RenderSetClipRect( ui->renderer, NULL );

        title_x = focused_drawn ? focused_rect.x + focused_rect.w + HOME_FOCUS_GAP
                                : SHELL_MARGIN + HOME_FOCUS_W + HOME_FOCUS_GAP;
        ui_text_fit( ui, ui->normal, title_x, HOME_TITLE_Y, ui->width - SHELL_MARGIN - title_x - 120, p->title,
                     ui->value, 1 );
        if (p->missing)
            draw_tag( ui, title_x, HOME_TITLE_Y + TTF_FontHeight( ui->normal ) + 20, "Missing", 1 );

        if (l->zone == ZONE_HEADER) hints[0].label = "Select";
        ui_hints_right( ui, hints, 3, ui->width - SHELL_MARGIN, HOME_HINT_Y );
    }
    ui_fade( ui );
}

/***********************************************************************
 * A program's menu
 */

enum program_row
{
    ROW_START, ROW_FAVORITE, ROW_ARTWORK, ROW_LOCATE, ROW_TITLE, ROW_ARGS, ROW_VERBOSE, ROW_PROFILE,
    ROW_WINDOWS, ROW_D3D9, ROW_VKD3D_VERSION, ROW_DXVK_VERSION, ROW_DXVK_HUD, ROW_FRAME_LIMIT, ROW_VSYNC,
    ROW_LSFG, ROW_LSFG_DLL, ROW_LSFG_PERFORMANCE, ROW_LSFG_FLOW,
    ROW_UPSCALING, ROW_UPSCALING_SHARPNESS,
    ROW_ADDRESS, ROW_OWN_CONTROLS, ROW_CONTROLS, ROW_BOX64,
    ROW_HIDE, ROW_LIBRARY, PROGRAM_ROWS
};

static int file_browser_pick( struct launcher *l, char *target, size_t size );
static void save_look( struct launcher *l );

/* What making one costs, in the fewest words that still say it, and this
 * console's own answer to the question it raises. */
static int confirm_forwarder( struct launcher *l, int bits )
{
    struct ui *ui = &l->ui;
    char message[320];

    snprintf( message, sizeof(message),
              "A %d-bit forwarder is installed as an application. Consoles have been banned for homebrew "
              "in that list.\n\nUse emuMMC only. %s",
              bits,
              l->options->emummc > 0 ? "This console is on emuMMC." :
              l->options->emummc == 0 ? "This console is NOT on emuMMC." :
              "Atmosphere did not say which this console is on." );
    return ui_confirm( ui, "Install forwarder", message, "Install" );
}

/* Build it, say where it went wrong if it did, and name it as the 32-bit one. */
static int install_forwarder( struct launcher *l, int bits, unsigned long long *id )
{
    static const char *const names[] = { "Autorun 32-bit", "Autorun" };
    const char *name = names[bits == 32 ? 0 : 1];
    struct ui *ui = &l->ui;
    const char *step = NULL;
    char message[256], value[32];
    unsigned int rc;

    if (!l->options->install_forwarder) return 0;
    if (!confirm_forwarder( l, bits )) { ui_start_screen( ui ); return 0; }

    /* One frame saying what is happening: building the three parts and writing
     * them takes a moment, and nothing is drawn while it does. */
    ui_start_screen( ui );
    ui_background( ui );
    ui_header_back( ui, "Install forwarder", name );
    ui_text_centered( ui, ui->large, ui->width / 2, ui->height / 2 - 30, "Installing...", ui->value );
    ui_present( ui );

    rc = l->options->install_forwarder( bits, name, id, &step );
    ui_start_screen( ui );
    if (rc)
    {
        snprintf( message, sizeof(message), "The console refused while %s.\n\nResult 0x%X.",
                  step ? step : "working", rc );
        ui_message( ui, "Could not install", message );
        ui_start_screen( ui );
        return 0;
    }
    if (bits == 32)
    {
        snprintf( value, sizeof(value), "%016llX", id ? *id : 0ull );
        launcher_kv_set( &l->look, "forwarder-32bit", value );
        launcher_kv_set( &l->look, "forwarder-32bit-name", name );
        save_look( l );
    }
    return 1;
}

/* Settings: one of the two, made on the spot. */
static void make_forwarder( struct launcher *l, int bits )
{
    struct ui *ui = &l->ui;
    unsigned long long id = 0;
    char message[192];

    if (!install_forwarder( l, bits, &id )) return;
    snprintf( message, sizeof(message), "%s is on the home menu.", bits == 32 ? "Autorun 32-bit" : "Autorun" );
    ui_message( ui, "Installed", message );
    ui_start_screen( ui );
}


static void download_artwork( struct launcher *l, struct program *p )
{
    char key[512], folder[512], square[512], portrait[512], hero[512], matched[192], message[512];
    struct steamgriddb_game games[STEAMGRIDDB_MAX_GAMES];
    struct ui_row rows[STEAMGRIDDB_MAX_GAMES];
    struct ui_list list = {0};
    int count = 0, i;
    enum steamgriddb_result result;

    if (!launcher_kv_get( &l->look, "steamgriddb-key", key, sizeof(key) ) || !key[0])
    {
        if (!launcher_platform_prompt( "SteamGridDB API key", "", key, sizeof(key) ) || !key[0]) return;
        launcher_kv_set( &l->look, "steamgriddb-key", key );
        save_look( l );
    }
    snprintf( folder, sizeof(folder), "%s/artwork", l->options->runtime_dir );
    if (mkdir( folder, 0777 ) && errno != EEXIST)
    { ui_message( &l->ui, "Artwork download failed", "The artwork cache directory could not be created." ); return; }
    if (snprintf( square, sizeof(square), "%s/%u-square.png", folder, p->catalog_id ) >= (int)sizeof(square) ||
        snprintf( portrait, sizeof(portrait), "%s/%u-portrait.png", folder, p->catalog_id ) >= (int)sizeof(portrait) ||
        snprintf( hero, sizeof(hero), "%s/%u-hero.png", folder, p->catalog_id ) >= (int)sizeof(hero)) return;

    ui_background( &l->ui );
    ui_header( &l->ui, "Downloading artwork", p->title );
    ui_text_centered( &l->ui, l->ui.normal, l->ui.width / 2, l->ui.height / 2 - 12,
                      "Searching SteamGridDB...", l->ui.text );
    ui_present( &l->ui );
    result = steamgriddb_search_games( key, p->title, games, STEAMGRIDDB_MAX_GAMES, &count );
    if (result == STEAMGRIDDB_NO_KEY)
    {
        launcher_kv_set( &l->look, "steamgriddb-key", NULL );
        save_look( l );
    }
    if (result != STEAMGRIDDB_OK)
    { ui_message( &l->ui, "Artwork download failed", steamgriddb_result_message( result ) ); return; }

    memset( rows, 0, sizeof(rows) );
    for (i = 0; i < count; i++)
    {
        snprintf( rows[i].label, sizeof(rows[i].label), "%s", games[i].name );
        snprintf( rows[i].value, sizeof(rows[i].value), "SteamGridDB" );
        rows[i].help = "Download the highest-rated square, portrait and hero artwork for this match.";
    }
    if (count > 1 && ui_list_run( &l->ui, &list, "Choose SteamGridDB game", p->title, rows, count, 0 ) != UI_ACTION_CHOOSE)
        return;
    snprintf( matched, sizeof(matched), "%s", games[list.selection].name );

    ui_background( &l->ui );
    ui_header( &l->ui, "Downloading artwork", matched );
    ui_text_centered( &l->ui, l->ui.normal, l->ui.width / 2, l->ui.height / 2 - 12,
                      "Square, portrait and hero from SteamGridDB...", l->ui.text );
    ui_present( &l->ui );
    result = steamgriddb_download_game_bundle( key, games[list.selection].id, square, portrait, hero );
    if (result == STEAMGRIDDB_NO_KEY)
    {
        launcher_kv_set( &l->look, "steamgriddb-key", NULL );
        save_look( l );
    }
    if (result != STEAMGRIDDB_OK)
    { ui_message( &l->ui, "Artwork download failed", steamgriddb_result_message( result ) ); return; }

    snprintf( p->square_art, sizeof(p->square_art), "%s", square );
    snprintf( p->portrait_art, sizeof(p->portrait_art), "%s", portrait );
    snprintf( p->hero_art, sizeof(p->hero_art), "%s", hero );
    if (p->icon) SDL_DestroyTexture( p->icon );
    if (p->square_icon) SDL_DestroyTexture( p->square_icon );
    if (p->hero_icon) SDL_DestroyTexture( p->hero_icon );
    p->icon = NULL; p->icon_state = ICON_UNKNOWN; p->icon_is_art = 0;
    p->square_icon = p->hero_icon = NULL;
    p->square_state = p->hero_state = ICON_UNKNOWN;
    save_library( l );
    snprintf( message, sizeof(message), "Downloaded the highest-rated square, portrait and hero artwork for %s.", matched );
    ui_message( &l->ui, "Artwork downloaded", message );
}

/* A setting that follows the global one (-1) or is on (1) or off (0) for this program. */
static const char *state_text( int state, int global, const char *on, const char *off, char *buffer, size_t size )
{
    if (state >= 0) return state ? on : off;
    snprintf( buffer, size, "Global (%s)", global ? on : off );
    return buffer;
}

static int next_state( int state, int direction )
{
    static const int order[3] = { -1, 1, 0 };
    int i;

    for (i = 0; i < 2 && order[i] != state; i++) {}
    return order[(i + (direction < 0 ? 2 : 1)) % 3];
}

/* What the program needs of the address space: what it was told, or what the
 * program itself says when it was told nothing. */
static enum launcher_address_space program_address_space( struct program *p )
{
    if (p->settings.address_space >= 0)
        return p->settings.address_space ? LAUNCHER_ADDRESS_LOW : LAUNCHER_ADDRESS_ANY;
    return launcher_program_address_space( p->path );
}

/* Whether this process can run it at all. Nothing here can widen or narrow the
 * address space: Horizon fixed it when the forwarder started this process. */
static int address_space_fits( struct launcher *l, struct program *p )
{
    return !l->options->address_space_bits || l->options->address_space_bits == 32 ||
           program_address_space( p ) != LAUNCHER_ADDRESS_LOW;
}

/* The forwarder named under Settings, and whether the console still has it.
 * name comes back as what it was called when it was named. */
static unsigned long long chosen_forwarder( struct launcher *l, char *name, size_t size, int *installed )
{
    char value[64] = "";
    unsigned long long id;

    if (name && size) name[0] = 0;
    if (installed) *installed = 0;
    if (!launcher_kv_get( &l->look, "forwarder-32bit", value, sizeof(value) ) || !value[0]) return 0;
    if (!(id = strtoull( value, NULL, 16 ))) return 0;
    if (name && size && (!launcher_kv_get( &l->look, "forwarder-32bit-name", name, size ) || !name[0]))
        snprintf( name, size, "%s", value );
    /* Without the console to ask, take it on trust rather than refuse. */
    if (installed) *installed = !l->options->title_installed || l->options->title_installed( id );
    return id;
}

static int start_program( struct launcher *l, struct program *p, char *target, size_t size )
{
    struct ui *ui = &l->ui;
    char path[512], text[160];

    if (!file_exists( p->path ) || l->options->machine_of( p->path, &p->machine ))
    {
        p->missing = 1;
        ui_message( ui, "Game unavailable", "The executable is missing or is not supported by this build." );
        return 0;
    }
    if (!address_space_fits( l, p ))
    {
        char message[320], name[128] = "";
        int installed = 0;
        unsigned long long id = chosen_forwarder( l, name, sizeof(name), &installed );

        /* Named, still installed, and the console will open it: nothing to ask
         * about -- the game goes there. */
        if (!id || !installed || !l->options->launch_title)
        {
            snprintf( message, sizeof(message),
                      "%s%s needs the low 4 GB of memory. Autorun is running with %d bits, which begins "
                      "above it.\n\nA 32-bit forwarder starts Autorun where the game fits.",
                      id && !installed ? "The 32-bit forwarder is gone. " : "", p->title,
                      l->options->address_space_bits );
            if (!l->options->install_forwarder || !l->options->launch_title)
            {
                ui_message( ui, "32-bit forwarder needed", message );
                return 0;
            }
            if (!ui_confirm( ui, "32-bit forwarder needed", message, "Install now" ))
            {
                ui_start_screen( ui );
                return 0;
            }
            if (!install_forwarder( l, 32, &id )) return 0;
            installed = 1;
        }
        /* The game goes on the card before the forwarder is asked for, because
         * once the console takes the request nothing here runs again. */
        runtime_file( l, "run-next.txt", path, sizeof(path) );
        if (!write_line( path, p->path ))
        {
            ui_toast( ui, "Could not write run-next.txt", 2500 );
            return 0;
        }
        p->launched_order = l->catalog.next_order++;
        save_library( l );
        if (l->options->launch_title( id ))
        {
            ui_toast( ui, "Opening the 32-bit forwarder...", 4000 );
            return 0;
        }
        remove( path );
        snprintf( message, sizeof(message), "The console refused to open %s.", name[0] ? name : "the forwarder" );
        ui_message( ui, "Could not open it", message );
        return 0;
    }
    p->missing = 0;
    p->launched_order = l->catalog.next_order++;
    save_library( l );

    /* The last frame before Wine starts; the screen stays dark until it shows a window. */
    snprintf( text, sizeof(text), "Starting %s", p->title );
    ui_background( ui );
    ui_header( ui, "Library", p->dos );
    ui_text_fit( ui, ui->large, (ui->width - (ui_text_width( ui, ui->large, text ) < ui->width - 120 ?
                                             ui_text_width( ui, ui->large, text ) : ui->width - 120)) / 2,
                 ui->height / 2 - 40, ui->width - 120, text, ui->value, 0 );
    ui_text_centered( ui, ui->small, ui->width / 2, ui->height / 2 + 24, "Wine is getting ready...", ui->dim );
    ui_present( ui );

    snprintf( target, size, "%s", p->path );
    runtime_file( l, "target.txt", path, sizeof(path) );
    write_line( path, target );
    return 1;
}

static void edit_text( const char *header, char *value, size_t size )
{
    char edited[896];

    if (!launcher_platform_prompt( header, value, edited, size < sizeof(edited) ? size : sizeof(edited) )) return;
    snprintf( value, size, "%s", edited );
}

struct dxvk_progress_ui
{
    struct ui *ui;
    const struct dxvk_release *release;
    enum dxvk_progress_stage stage;
    const char *name;
    Uint32 last_draw;
    int started;
};

static void dxvk_install_progress( void *opaque, enum dxvk_progress_stage stage,
                                   unsigned long long current, unsigned long long total )
{
    struct dxvk_progress_ui *progress = opaque;
    const char *status;
    char title[96];
    Uint32 now = SDL_GetTicks();

    if (stage == DXVK_PROGRESS_DOWNLOAD && !total) total = progress->release->size;
    if (progress->started && stage == progress->stage && now - progress->last_draw < 40 &&
        (!total || current < total)) return;
    switch (stage)
    {
    case DXVK_PROGRESS_DOWNLOAD: status = "Downloading from GitHub..."; break;
    case DXVK_PROGRESS_VERIFY: status = "Verifying download..."; current = total = 0; break;
    default: status = "Installing x86 and x64 files..."; current = total = 0; break;
    }
    snprintf( title, sizeof(title), "Installing %s %s", progress->name, progress->release->version );
    ui_progress_update( progress->ui, title, status, current, total );
    progress->stage = stage;
    progress->last_draw = now;
    progress->started = 1;
}

static int graphics_release_menu( struct launcher *l, struct program *p, const struct ui_list *anchor, int vkd3d )
{
    static struct dxvk_release releases[DXVK_MAX_RELEASES];
    static struct ui_row rows[DXVK_MAX_RELEASES + 1];
    int refresh = 0;
    const char *name = vkd3d ? "VKD3D" : "DXVK";
    char *version = vkd3d ? p->settings.vkd3d_version : p->settings.dxvk_version;
    enum dxvk_result (*catalog)( const char *, struct dxvk_release *, int, int *, int, int * ) =
        vkd3d ? vkd3d_release_catalog : dxvk_release_catalog;
    enum dxvk_result (*install)( const char *, const struct dxvk_release *, dxvk_progress_callback, void * ) =
        vkd3d ? vkd3d_install_release : dxvk_install_release;
    int (*installed_release)( const char *, unsigned short, const char * ) =
        vkd3d ? vkd3d_release_installed : dxvk_release_installed;
    int (*root_release)( const char *, unsigned short, char *, size_t ) =
        vkd3d ? vkd3d_root_version : dxvk_root_version;

    for (;;)
    {
        enum dxvk_result result;
        char root_version[32] = "", message[320];
        int ids[DXVK_MAX_RELEASES + 1];
        int release_count = 0, count = 0, cached = 0, latest = -1, current = -1, chosen, i;

        snprintf( message, sizeof(message), "%s %s releases...", refresh ? "Refreshing" : "Loading", name );
        ui_toast( &l->ui, message, 15000 );
        ui_present( &l->ui );
        result = catalog( l->options->runtime_dir, releases, DXVK_MAX_RELEASES,
                                       &release_count, refresh, &cached );
        ui_toast( &l->ui, "", 0 );
        refresh = 0;
        if (result != DXVK_OK)
        {
            ui_message( &l->ui, name, dxvk_result_message( result ) );
            return 0;
        }
        root_release( l->options->runtime_dir, p->machine, root_version, sizeof(root_version) );
        memset( rows, 0, sizeof(rows) );
        for (i = 0; i < release_count; i++)
        {
            int installed = installed_release( l->options->runtime_dir, p->machine, releases[i].version ) ||
                            (root_version[0] && !strcmp( root_version, releases[i].version ) &&
                             installed_release( l->options->runtime_dir, p->machine, "" ));
            int index;

            if (!vkd3d && !launcher_dxvk_version_selectable( releases[i].version )) continue;
            index = count++;
            ids[index] = i;
            if (latest < 0 && !releases[i].prerelease) latest = i;
            if ((version[0] && !strcmp( version, releases[i].version )) ||
                (!version[0] && root_version[0] && !strcmp( root_version, releases[i].version )))
                current = index;
            snprintf( rows[index].label, sizeof(rows[index].label), "%s", releases[i].version );
            if (installed)
                snprintf( rows[index].value, sizeof(rows[index].value), "%s%s",
                          i == latest ? "Latest / " : "", "Installed" );
            else if (i == latest)
                snprintf( rows[index].value, sizeof(rows[index].value), "Latest" );
            else if (releases[i].prerelease)
                snprintf( rows[index].value, sizeof(rows[index].value), "Pre-release" );
            rows[index].download = !installed;
        }
        if (latest >= 0 && current < 0)
            for (i = 0; i < count; i++)
                if (ids[i] == latest) { current = i; break; }
        ids[count] = -1;
        snprintf( rows[count].label, sizeof(rows[count].label), "Refresh releases" );
        snprintf( rows[count].value, sizeof(rows[count].value), "%s", cached ? "Cached" : "Up to date" );
        count++;
        chosen = ui_settings_dropdown( &l->ui, anchor, rows, count, current >= 0 ? current : 0 );
        if (chosen < 0) return 0;
        i = ids[chosen];
        if (i < 0)
        {
            refresh = 1;
            continue;
        }
        if (rows[chosen].download)
        {
            struct dxvk_progress_ui progress;

            if (releases[i].size)
                snprintf( message, sizeof(message),
                          "Download %s %s (%.1f MiB) from the official GitHub release and install its x86 and x64 DLLs?",
                          name, releases[i].version, releases[i].size / 1048576.0 );
            else
                snprintf( message, sizeof(message),
                          "Download %s %s from the official GitHub release and install its x86 and x64 DLLs?",
                          name, releases[i].version );
            if (!ui_confirm( &l->ui, name, message, "Download" )) continue;
            memset( &progress, 0, sizeof(progress) );
            progress.ui = &l->ui;
            progress.name = name;
            progress.release = releases + i;
            ui_progress_begin( &l->ui );
            result = install( l->options->runtime_dir, releases + i,
                                           dxvk_install_progress, &progress );
            ui_progress_end( &l->ui );
            if (result != DXVK_OK)
            {
                ui_message( &l->ui, name, dxvk_result_message( result ) );
                continue;
            }
        }
        if (root_version[0] && !strcmp( root_version, releases[i].version ) &&
            installed_release( l->options->runtime_dir, p->machine, "" ))
            version[0] = 0;
        else memcpy( version, releases[i].version, strlen( releases[i].version ) + 1 );
        p->settings.dxvk = 1;
        save_program_settings( l, p );
        snprintf( message, sizeof(message), "%s %s selected", name, releases[i].version );
        ui_toast( &l->ui, message, 1800 );
        return 1;
    }
}

static int box64_configured_count( const struct launcher_kv *kv, int advanced )
{
    char value[64];
    int count = 0, i;

    for (i = 0; i < NX_BOX64_OPTION_COUNT; i++)
        if (nx_box64_options[i].advanced == advanced &&
            launcher_kv_get( kv, nx_box64_options[i].name, value, sizeof(value) )) count++;
    return count;
}

static void box64_options_status( const char *path, char *value, size_t size )
{
    struct launcher_kv kv;
    int count;

    if (!launcher_kv_load( &kv, path ))
    {
        snprintf( value, size, "File too large" );
        return;
    }
    count = box64_configured_count( &kv, 0 ) + box64_configured_count( &kv, 1 );
    if (count) snprintf( value, size, "%d flag%s set", count, count == 1 ? "" : "s" );
    else if (file_exists( path )) snprintf( value, size, "Custom file" );
    else snprintf( value, size, "Default" );
}

static int box64_option_value( const struct launcher_kv *kv, const struct nx_box64_option *option,
                               long *value, char *text, size_t size )
{
    int choice;

    if (!launcher_kv_get( kv, option->name, text, size ))
    {
        *value = option->default_value;
        choice = nx_box64_option_choice( option, *value );
        snprintf( text, size, "%s", option->value_names[choice] );
        return choice;
    }
    if (!nx_box64_option_parse_value( text, value ) ||
        (choice = nx_box64_option_choice( option, *value )) < 0)
    {
        char invalid[64];

        snprintf( invalid, sizeof(invalid), "%s", text );
        snprintf( text, size, "Unsupported (%s)", invalid );
        return -1;
    }
    snprintf( text, size, "%s", option->value_names[choice] );
    return choice;
}

static int box64_option_rows( struct ui_row *rows, int *ids, int count,
                              const struct launcher_kv *kv, int advanced )
{
    int i;

    for (i = 0; i < NX_BOX64_OPTION_COUNT; i++)
    {
        long value;

        if (nx_box64_options[i].advanced != advanced) continue;
        ids[count] = i;
        snprintf( rows[count].label, sizeof(rows[count].label), "%s", nx_box64_options[i].name );
        box64_option_value( kv, nx_box64_options + i, &value,
                            rows[count].value, sizeof(rows[count].value) );
        rows[count].help = nx_box64_options[i].help;
        rows[count].kind = UI_ROW_VALUE;
        rows[count].adjustable = 1;
        count++;
    }
    return count;
}

static void box64_options_menu( struct launcher *l, struct program *p )
{
    struct ui_row rows[NX_BOX64_OPTION_COUNT + 1];
    int ids[NX_BOX64_OPTION_COUNT + 1];
    struct ui_list list = {0};
    struct launcher_kv kv;
    char path[520];
    int count, expanded = 0, i;

    if (!launcher_sibling_path( p->path, ".box64.txt", path, sizeof(path) ) ||
        !launcher_kv_load( &kv, path ))
    {
        ui_message( &l->ui, "Box64 options", "The options file is too large to edit." );
        return;
    }
    for (;;)
    {
        enum ui_action action;
        int set;

        memset( rows, 0, sizeof(rows) );
        count = box64_option_rows( rows, ids, 0, &kv, 0 );
        set = box64_configured_count( &kv, 1 );
        ids[count] = -1;
        snprintf( rows[count].label, sizeof(rows[count].label), "Advanced flags" );
        snprintf( rows[count].value, sizeof(rows[count].value), expanded ? "Hide (%d set)" : "Show (%d set)", set );
        rows[count].help = "Compatibility and lower-level DynaRec controls supported by Wine-NX's embedded Box64 backend.";
        rows[count].kind = UI_ROW_DROPDOWN;
        rows[count].on = expanded;
        count++;
        if (expanded) count = box64_option_rows( rows, ids, count, &kv, 1 );
        action = ui_list_run( &l->ui, &list, "Box64 options", p->title, rows, count, 1 );
        if (action == UI_ACTION_BACK || action == UI_ACTION_QUIT) return;
        i = ids[list.selection];
        if (i < 0)
        {
            if (action == UI_ACTION_CHOOSE) expanded = !expanded;
            continue;
        }
        else
        {
            const struct nx_box64_option *option = nx_box64_options + i;
            char current_text[64], number[16];
            long current_value;
            int current = box64_option_value( &kv, option, &current_value,
                                               current_text, sizeof(current_text) );
            int next;

            if (action == UI_ACTION_RESET) next = nx_box64_option_choice( option, option->default_value );
            else
            {
                if (current < 0) current = nx_box64_option_choice( option, option->default_value );
                next = (current + (action == UI_ACTION_LEFT ? option->value_count - 1 : 1)) % option->value_count;
            }
            snprintf( number, sizeof(number), "%d", option->values[next] );
            if (!launcher_kv_set( &kv, option->name,
                                  option->values[next] == option->default_value ? NULL : number ) ||
                !launcher_kv_save( &kv, path ))
            {
                ui_message( &l->ui, "Box64 options", "The options could not be saved." );
                if (!launcher_kv_load( &kv, path )) return;
                continue;
            }
            load_program_settings( l, p );
        }
    }
}

/* Returns 1 when the program is to be started. */
/* The sections of Game Settings, in the order they stand in the list. */
enum program_section
{
    SECTION_GENERAL,
    SECTION_GRAPHICS,
#ifdef WINE_NX_LSFG
    SECTION_FRAME_GENERATION,
#endif
    SECTION_DIAGNOSTICS,
    SECTION_LIBRARY
};

static int program_menu( struct launcher *l, struct program *p, char *target, size_t size )
{
    static const char *const sections[] = { "General", "Graphics",
#ifdef WINE_NX_LSFG
                                            "Frame Generation",
#endif
                                            "Diagnostics", "Library" };
    /* Kept between openings, so a game returns to the section it was left in. */
    static int section;
    struct ui_row rows[PROGRAM_ROWS];
    int ids[PROGRAM_ROWS], count, id, i;
    struct ui_list list = {0};
    struct ui *ui = &l->ui;
    char path[520], dir[512], line[896], global_line[896], name[128], buffer[64];
    struct program copy;

    for (;;)
    {
        const char *base = file_name( p->path );
        int in_library = find_program( l, p->path ) >= 0;
        int x86 = p->machine == 0x014c, x64 = p->machine == 0x8664, dxvk_beside, dxvk_installed;
        int vkd3d_installed = vkd3d_release_installed( l->options->runtime_dir, p->machine,
                                                     p->settings.vkd3d_version );
#ifdef WINE_NX_LSFG
        int lsfg_installed;
#endif
        const char *dxvk_dir = launcher_dxvk_directory( p->machine );
        char dxvk_root[32] = "", vkd3d_root[32] = "";
        enum ui_action action;
        struct ui_row *row;

        snprintf( name, sizeof(name), "%.*s", (int)(strlen( base ) > 4 ? strlen( base ) - 4 : strlen( base )), base );
        snprintf( dir, sizeof(dir), "%s", p->path );
        parent_dir( dir );
        snprintf( path, sizeof(path), "%s/%s", dir, x64 ? "d3d11.dll" : "d3d9.dll" );
        dxvk_beside = file_exists( path );
        if (x64)
        {
            snprintf( path, sizeof(path), "%s/dxgi.dll", dir );
            dxvk_beside |= file_exists( path );
            if (p->settings.dxvk)
            {
                snprintf( path, sizeof(path), "%s/d3d12.dll", dir );
                dxvk_beside |= file_exists( path );
                snprintf( path, sizeof(path), "%s/d3d12core.dll", dir );
                dxvk_beside |= file_exists( path );
            }
        }
        dxvk_installed = dxvk_dir && dxvk_release_installed( l->options->runtime_dir, p->machine,
                                                             p->settings.dxvk_version );
        dxvk_root_version( l->options->runtime_dir, p->machine, dxvk_root, sizeof(dxvk_root) );
        vkd3d_root_version( l->options->runtime_dir, p->machine, vkd3d_root, sizeof(vkd3d_root) );

        count = 0;
#define ADD_ROW(i, section, text, help_text) \
        do { row = rows + count; memset( row, 0, sizeof(*row) ); ids[count++] = (i); \
             row->group = (section); row->kind = UI_ROW_ACTION; \
             snprintf( row->label, sizeof(row->label), "%s", (text) ); row->help = (help_text); } while (0)

        ADD_ROW( ROW_START, SECTION_GENERAL, "Start", "Runs the game." );
        ADD_ROW( ROW_FAVORITE, SECTION_GENERAL, "Favorite", "Keeps the game in the Favorites filter of the library." );
        row->kind = UI_ROW_SWITCH;
        row->on = p->favorite;
        ADD_ROW( ROW_ARTWORK, SECTION_LIBRARY, "Download artwork", "Takes the highest-rated square, portrait and hero pictures for this game from SteamGridDB." );
        if (p->missing) ADD_ROW( ROW_LOCATE, SECTION_LIBRARY, "Locate executable", "Choose the game's executable at its new location." );
        ADD_ROW( ROW_TITLE, SECTION_GENERAL, "Title",
                 "The name shown in the library. Y goes back to the name in the program's own resources." );
        row->kind = UI_ROW_VALUE;
        snprintf( row->value, sizeof(row->value), "%s", p->title );

        ADD_ROW( ROW_ARGS, SECTION_GENERAL, "Arguments",
                 "The command line after the program's name, kept beside it as NAME.args.txt. "
                 "Leave it empty to remove them." );
        line[0] = 0;
        if (launcher_args_path( p->path, path, sizeof(path) )) read_line( path, line, sizeof(line) );
        runtime_file( l, "args.txt", path, sizeof(path) );
        if (line[0]) snprintf( row->value, sizeof(row->value), "%s", line );
        else if (read_line( path, global_line, sizeof(global_line) ) && launcher_args_match( global_line, p->dos ))
            snprintf( row->value, sizeof(row->value), "args.txt: %s", global_line );
        else snprintf( row->value, sizeof(row->value), "None" );

        ADD_ROW( ROW_VERBOSE, SECTION_DIAGNOSTICS, "Verbose traces",
                 "Writes Wine's traces to wine-nx-runtime.log, which slows the program down. "
                 "Global follows the setting in Settings (X on the library)." );
        row->adjustable = 1;
        snprintf( row->value, sizeof(row->value), "%s",
                  state_text( p->settings.verbose, l->options->verbose, "On", "Off", buffer, sizeof(buffer) ) );

        ADD_ROW( ROW_PROFILE, SECTION_DIAGNOSTICS, "Profiler",
                 "Samples where every thread spends its time and writes [PROF] lines to wine-nx-runtime.log." );
        row->adjustable = 1;
        snprintf( row->value, sizeof(row->value), "%s",
                  state_text( p->settings.profile, l->options->profile, "On", "Off", buffer, sizeof(buffer) ) );

        ADD_ROW( ROW_WINDOWS, SECTION_GRAPHICS, "Windows shown by",
                 "The compositor draws every window through OpenGL. The framebuffer copies window pixels "
                 "straight to the screen, for when the compositor misbehaves." );
        row->adjustable = 1;
        snprintf( row->value, sizeof(row->value), "%s",
                  state_text( p->settings.framebuffer, l->options->framebuffer, "Framebuffer", "Compositor",
                              buffer, sizeof(buffer) ) );

        if (l->options->vulkan && (x86 || x64))
        {
            ADD_ROW( ROW_D3D9, SECTION_GRAPHICS, "Direct3D renderer",
                      "Wine uses its built-in renderer. DXVK + VKD3D uses Vulkan for Direct3D 9/10/11/12. "
                     "Graphics DLLs next to the game have priority." );
            row->adjustable = 1;
            row->download = p->settings.dxvk && !dxvk_installed;
            snprintf( row->value, sizeof(row->value), "%s%s%s",
                      p->settings.dxvk ? "DXVK + VKD3D" : "Wine",
                      p->settings.dxvk && !dxvk_installed ? " (download required)" : "",
                      dxvk_beside ? " (app DLL first)" : "" );

            ADD_ROW( ROW_VKD3D_VERSION, SECTION_GRAPHICS, "VKD3D version",
                     "Choose an official VKD3D GitHub release." );
            row->kind = UI_ROW_DROPDOWN;
            row->download = !vkd3d_installed;
            if (p->settings.vkd3d_version[0])
                snprintf( row->value, sizeof(row->value), "%s", p->settings.vkd3d_version );
            else if (vkd3d_root[0])
                snprintf( row->value, sizeof(row->value), "%s", vkd3d_root );
            else snprintf( row->value, sizeof(row->value), "Latest" );

            ADD_ROW( ROW_DXVK_VERSION, SECTION_GRAPHICS, "DXVK version",
                     "Choose an official DXVK GitHub release." );
            row->kind = UI_ROW_DROPDOWN;
            row->download = !dxvk_installed;
            if (p->settings.dxvk_version[0])
                snprintf( row->value, sizeof(row->value), "%s", p->settings.dxvk_version );
            else if (dxvk_root[0])
                snprintf( row->value, sizeof(row->value), "Latest (%s)", dxvk_root );
            else snprintf( row->value, sizeof(row->value), "Latest" );
            ADD_ROW( ROW_DXVK_HUD, SECTION_GRAPHICS, "DXVK HUD",
                     "FPS shows only the frame rate. Compact shows the DirectX version, FPS and frame times. "
                     "Full also shows the DXVK version, GPU, video memory and shader compiler activity. "
                     "The DXVK HUD does not cover VKD3D's D3D12 rendering." );
            row->kind = UI_ROW_DROPDOWN;
            snprintf( row->value, sizeof(row->value), "%s", launcher_hud_labels[p->settings.dxvk_hud] );

            ADD_ROW( ROW_FRAME_LIMIT, SECTION_GRAPHICS, "Frame rate limit",
                     "Limits real game frames in Vulkan, DXVK and VKD3D. Off adds no cap. "
                     "VSync and the game's own limit still apply." );
            row->kind = UI_ROW_DROPDOWN;
            snprintf( row->value, sizeof(row->value), "%s", launcher_frame_limit_labels[p->settings.frame_limit] );

            ADD_ROW( ROW_VSYNC, SECTION_GRAPHICS, "VSync",
                     "Synchronizes Vulkan, DXVK and VKD3D presentation to the display. LSFG-VK always uses synchronized presentation." );
            row->kind = UI_ROW_SWITCH;
            row->on = p->settings.vsync;
            snprintf( row->value, sizeof(row->value), "%s", p->settings.vsync ? "Enabled" : "Disabled" );

            ADD_ROW( ROW_UPSCALING, SECTION_GRAPHICS, "Upscaling",
                     "Spatial upscaling method when running below native display resolution. "
                     "FSR 1.0 reconstructs sharp edges with adaptive sharpening. "
                     "Integer provides crisp nearest-neighbor pixel art scaling." );
            row->kind = UI_ROW_DROPDOWN;
            snprintf( row->value, sizeof(row->value), "%s", launcher_upscaling_labels[p->settings.upscaling] );

            if (p->settings.upscaling == 1)
            {
                ADD_ROW( ROW_UPSCALING_SHARPNESS, SECTION_GRAPHICS, "FSR Sharpness",
                         "Contrast-adaptive sharpening strength applied to upscaled edges." );
                row->kind = UI_ROW_DROPDOWN;
                snprintf( row->value, sizeof(row->value), "%s", launcher_sharpness_labels[p->settings.upscaling_sharpness] );
            }
        }

#ifdef WINE_NX_LSFG
        runtime_file( l, "lsfg/Lossless.dll", path, sizeof(path) );
        lsfg_installed = file_exists( path );
        if (!lsfg_installed) p->settings.lsfg_enabled = 0;
        ADD_ROW( ROW_LSFG, SECTION_FRAME_GENERATION, "LSFG-VK (2x)",
                 "Generates one frame between each pair of game frames for Vulkan, DXVK and VKD3D only. "
                 "Output follows the physical display resolution and uses synchronized presentation regardless of "
                 "the VSync setting." );
        row->kind = UI_ROW_SWITCH;
        row->disabled = !lsfg_installed;
        row->on = p->settings.lsfg_enabled;
        snprintf( row->value, sizeof(row->value), "%s",
                  !lsfg_installed ? "Unavailable" : p->settings.lsfg_enabled ? "Enabled" : "Disabled" );

        ADD_ROW( ROW_LSFG_DLL, SECTION_FRAME_GENERATION, "Lossless.dll",
                 "Copy Lossless.dll to sdmc:/switch/wine/lsfg/Lossless.dll." );
        row->kind = UI_ROW_INFO;
        row->disabled = 1;
        if (lsfg_installed)
        {
            snprintf( row->value, sizeof(row->value), "Installed" );
            row->value_tone = UI_VALUE_SUCCESS;
        }
        else
        {
            snprintf( row->value, sizeof(row->value), "Not found" );
            row->value_tone = UI_VALUE_DANGER;
        }

        ADD_ROW( ROW_LSFG_PERFORMANCE, SECTION_FRAME_GENERATION, "Performance Mode",
                 "Uses LSFG's performance path. Disable it for the quality path, which costs more GPU time." );
        row->kind = UI_ROW_SWITCH;
        row->on = p->settings.lsfg_performance;
        snprintf( row->value, sizeof(row->value), "%s",
                  p->settings.lsfg_performance ? "Enabled" : "Disabled" );

        ADD_ROW( ROW_LSFG_FLOW, SECTION_FRAME_GENERATION, "Motion Resolution",
                 "Resolution used to estimate motion. Lower values reduce GPU work and memory use; generated output "
                 "still follows the physical display resolution." );
        row->kind = UI_ROW_DROPDOWN;
        snprintf( row->value, sizeof(row->value), "%s",
                  launcher_lsfg_flow_labels[p->settings.lsfg_flow] );
#endif

        {
            enum launcher_address_space needs = launcher_program_address_space( p->path );

            ADD_ROW( ROW_ADDRESS, SECTION_GRAPHICS, "Address space",
                     "What the game needs of the address space Horizon gives Autorun. A game linked for a "
                     "fixed address in the low 4 GB runs only under a forwarder made with 32 bits; the "
                     "forwarder decides this, and a game that needs one it was not given is not started." );
            row->adjustable = 1;
            if (p->settings.address_space >= 0)
                snprintf( row->value, sizeof(row->value), "%s",
                          p->settings.address_space ? "32-bit" : "Any" );
            else snprintf( row->value, sizeof(row->value), "Auto (%s)",
                           needs == LAUNCHER_ADDRESS_LOW ? "32-bit" :
                           needs == LAUNCHER_ADDRESS_ANY ? "any" : "unread" );
        }

        {
            int has_own = launcher_keys_path( p->path, path, sizeof(path) ) && file_exists( path );
            /* Not set means its own when it has any, which is what a card
             * written before this setting existed means. */
            int own = p->settings.own_controls < 0 ? has_own : p->settings.own_controls;

            ADD_ROW( ROW_OWN_CONTROLS, SECTION_DIAGNOSTICS, "Controls",
                     "Autorun's controls, or this program's own over them. Turning them off keeps "
                     "the keys that were set, for when they are wanted again." );
            snprintf( row->value, sizeof(row->value), "%s", own ? "Its own" : "Autorun's" );
            row->kind = UI_ROW_SWITCH;
            row->on = own;

            if (own)
            {
                ADD_ROW( ROW_CONTROLS, SECTION_DIAGNOSTICS, "Edit controls",
                         "The keys this program's controls send, over the ones everything else sends." );
                snprintf( row->value, sizeof(row->value), "%s", has_own ? "Set" : "Default" );
            }
        }

        if (x86 || x64)
        {
            ADD_ROW( ROW_BOX64, SECTION_DIAGNOSTICS, "Box64 options",
                     "Per-game performance and compatibility flags for the Box64 translator." );
            launcher_sibling_path( p->path, ".box64.txt", path, sizeof(path) );
            box64_options_status( path, row->value, sizeof(row->value) );
        }

        if (in_library)
        {
            ADD_ROW( ROW_HIDE, SECTION_LIBRARY, "Hidden",
                     "Hidden programs stay out of the library unless Settings shows them." );
            row->kind = UI_ROW_SWITCH;
            row->on = p->settings.hidden;
        }
        if (!in_library || p->added)
        {
            ADD_ROW( ROW_LIBRARY, SECTION_LIBRARY, in_library ? "Remove from the library" : "Add to the library",
                     "The library finds programs within two folders of drive_c by itself; "
                     "others found with the file browser can be added." );
            row->destructive = in_library;
        }
#undef ADD_ROW

        action = ui_settings_run( ui, &list, "Game Settings", p->title, sections,
                                  sizeof(sections) / sizeof(sections[0]), rows, count, 1, &section );
        if (action == UI_ACTION_BACK || action == UI_ACTION_QUIT) return 0;
        /* The screen shows one section at a time, so the row it chose is the
         * selection counted within that section. */
        for (id = 0, i = 0; i < count; i++)
            if (rows[i].group == section && id++ == list.selection) break;
        id = i < count ? ids[i] : ids[0];
        switch (id)
        {
        case ROW_START:
            if (p->missing)
            {
                ui_message( ui, "Game unavailable", "The executable could not be found. Remove this entry and add the game again at its new location." );
                break;
            }
            return start_program( l, p, target, size );

        case ROW_FAVORITE:
            if (action != UI_ACTION_CHOOSE) break;
            p->favorite = !p->favorite;
            if (!save_library( l )) p->favorite = !p->favorite;
            else ui_toast( ui, p->favorite ? "Added to favorites" : "Removed from favorites", 1500 );
            break;

        case ROW_ARTWORK:
            if (action == UI_ACTION_CHOOSE) download_artwork( l, p );
            break;

        case ROW_LOCATE:
            if (action == UI_ACTION_CHOOSE)
            {
                char selected[512];
                struct program replacement;
                int duplicate;
                if (!file_browser_pick( l, selected, sizeof(selected) )) break;
                duplicate = find_program( l, selected );
                if (duplicate >= 0 && &l->programs[duplicate] != p)
                {
                    ui_message( ui, "Locate executable", "That executable already belongs to another library entry." );
                    break;
                }
                if (!describe_program( l, &replacement, selected ))
                {
                    ui_message( ui, "Locate executable", "Autorun cannot run this executable." );
                    break;
                }
                snprintf( p->path, sizeof(p->path), "%s", replacement.path );
                snprintf( p->dos, sizeof(p->dos), "%s", replacement.dos );
                snprintf( p->resource_title, sizeof(p->resource_title), "%s", replacement.resource_title );
                p->machine = replacement.machine;
                p->missing = 0;
                load_program_settings( l, p );
                save_library( l );
                ui_toast( ui, "Executable location updated", 1800 );
            }
            break;

        case ROW_TITLE:
            if (action == UI_ACTION_RESET) p->settings.title[0] = 0;
            else if (action == UI_ACTION_CHOOSE) edit_text( "Title", p->settings.title, sizeof(p->settings.title) );
            else break;
            save_program_settings( l, p );
            save_library( l );
            break;

        case ROW_ARGS:
            if (action != UI_ACTION_CHOOSE || !launcher_args_path( p->path, path, sizeof(path) )) break;
            read_line( path, global_line, sizeof(global_line) );
            if (!launcher_platform_prompt( "Arguments", global_line, line, sizeof(line) )) break;
            if (line[0]) write_line( path, line );
            else remove( path );
            load_program_settings( l, p );
            ui_toast( ui, line[0] ? "Arguments saved" : "Arguments removed", 1500 );
            break;

        case ROW_VERBOSE:
        case ROW_PROFILE:
        case ROW_WINDOWS:
        {
            int *state = id == ROW_VERBOSE ? &p->settings.verbose :
                         id == ROW_PROFILE ? &p->settings.profile : &p->settings.framebuffer;

            *state = action == UI_ACTION_RESET ? -1 : next_state( *state, action == UI_ACTION_LEFT ? -1 : 1 );
            save_program_settings( l, p );
            break;
        }

        case ROW_D3D9:
            if (action != UI_ACTION_RESET && !p->settings.dxvk && !dxvk_installed)
            {
                graphics_release_menu( l, p, &list, 0 );
                break;
            }
            p->settings.dxvk = action == UI_ACTION_RESET ? 0 : !p->settings.dxvk;
            save_program_settings( l, p );
            break;

        case ROW_VKD3D_VERSION:
        case ROW_DXVK_VERSION:
            if (action == UI_ACTION_RESET)
            {
                char *version = id == ROW_VKD3D_VERSION ? p->settings.vkd3d_version : p->settings.dxvk_version;
                version[0] = 0;
                save_program_settings( l, p );
            }
            else if (action == UI_ACTION_CHOOSE)
                graphics_release_menu( l, p, &list, id == ROW_VKD3D_VERSION );
            break;

        case ROW_DXVK_HUD:
        case ROW_FRAME_LIMIT:
        {
            int *value = id == ROW_DXVK_HUD ? &p->settings.dxvk_hud : &p->settings.frame_limit;
            const char *const *labels = id == ROW_DXVK_HUD ? launcher_hud_labels : launcher_frame_limit_labels;
            int choices = id == ROW_DXVK_HUD ? LAUNCHER_HUD_COUNT : LAUNCHER_FRAME_LIMIT_COUNT;
            struct ui_row items[LAUNCHER_FRAME_LIMIT_COUNT] = {0};

            if (action == UI_ACTION_RESET) *value = 0;
            else if (action == UI_ACTION_CHOOSE)
            {
                for (i = 0; i < choices; i++)
                    snprintf( items[i].label, sizeof(items[i].label), "%s", labels[i] );
                int selected = ui_settings_dropdown( ui, &list, items, choices, *value );
                if (selected < 0) break;
                *value = selected;
            }
            else break;
            save_program_settings( l, p );
            break;
        }

        case ROW_VSYNC:
        {
            p->settings.vsync = action == UI_ACTION_RESET ? 1 : !p->settings.vsync;
            save_program_settings( l, p );
            break;
        }

        case ROW_UPSCALING:
            if (action == UI_ACTION_RESET) p->settings.upscaling = 0;
            else if (action == UI_ACTION_CHOOSE)
            {
                struct ui_row items[LAUNCHER_UPSCALING_COUNT] = {0};
                int selected;

                for (i = 0; i < LAUNCHER_UPSCALING_COUNT; i++)
                    snprintf( items[i].label, sizeof(items[i].label), "%s", launcher_upscaling_labels[i] );
                selected = ui_settings_dropdown( ui, &list, items, LAUNCHER_UPSCALING_COUNT, p->settings.upscaling );
                if (selected < 0) break;
                p->settings.upscaling = selected;
            }
            else break;
            save_program_settings( l, p );
            break;

        case ROW_UPSCALING_SHARPNESS:
            if (action == UI_ACTION_RESET) p->settings.upscaling_sharpness = 2;
            else if (action == UI_ACTION_CHOOSE)
            {
                struct ui_row items[LAUNCHER_SHARPNESS_COUNT] = {0};
                int selected;

                for (i = 0; i < LAUNCHER_SHARPNESS_COUNT; i++)
                    snprintf( items[i].label, sizeof(items[i].label), "%s", launcher_sharpness_labels[i] );
                selected = ui_settings_dropdown( ui, &list, items, LAUNCHER_SHARPNESS_COUNT, p->settings.upscaling_sharpness );
                if (selected < 0) break;
                p->settings.upscaling_sharpness = selected;
            }
            else break;
            save_program_settings( l, p );
            break;

#ifdef WINE_NX_LSFG
        case ROW_LSFG:
        case ROW_LSFG_PERFORMANCE:
        {
            int *value = id == ROW_LSFG ? &p->settings.lsfg_enabled : &p->settings.lsfg_performance;

            *value = action == UI_ACTION_RESET ? id == ROW_LSFG_PERFORMANCE : !*value;
            save_program_settings( l, p );
            break;
        }

        case ROW_LSFG_FLOW:
            if (action == UI_ACTION_RESET) p->settings.lsfg_flow = 1;
            else if (action == UI_ACTION_CHOOSE)
            {
                struct ui_row items[3] = {0};
                int selected;

                for (i = 0; i < 3; i++)
                    snprintf( items[i].label, sizeof(items[i].label), "%s", launcher_lsfg_flow_labels[i] );
                selected = ui_settings_dropdown( ui, &list, items, 3, p->settings.lsfg_flow );
                if (selected < 0) break;
                p->settings.lsfg_flow = selected;
            }
            else break;
            save_program_settings( l, p );
            break;
#endif

        case ROW_ADDRESS:
            /* Auto, then what the two answers are, so either can be forced. */
            p->settings.address_space = action == UI_ACTION_RESET ? -1 :
                                        next_state( p->settings.address_space, action == UI_ACTION_LEFT ? -1 : 1 );
            save_program_settings( l, p );
            break;

        case ROW_OWN_CONTROLS:
        {
            int has_own = launcher_keys_path( p->path, path, sizeof(path) ) && file_exists( path );

            if (action == UI_ACTION_RESET) p->settings.own_controls = -1;
            else p->settings.own_controls = !(p->settings.own_controls < 0 ? has_own :
                                              p->settings.own_controls);
            save_program_settings( l, p );
            break;
        }

        case ROW_CONTROLS:
            if (action != UI_ACTION_CHOOSE) break;
            if (launcher_keys_path( p->path, path, sizeof(path) ))
            {
                char under[512];

                /* What this program alone sends, over what everything does. */
                shared_keys( l, under, sizeof(under) );
                controls_screen( l, path, under, "Controls" );
                ui_start_screen( &l->ui );
            }
            break;

        case ROW_BOX64:
            if (action != UI_ACTION_CHOOSE) break;
            box64_options_menu( l, p );
            break;

        case ROW_HIDE:
            if (action == UI_ACTION_RESET) p->settings.hidden = 0;
            else if (action == UI_ACTION_CHOOSE) p->settings.hidden = !p->settings.hidden;
            else break;
            save_program_settings( l, p );
            ui_toast( ui, p->settings.hidden ? "Hidden from the library" : "Shown in the library", 1500 );
            break;

        case ROW_LIBRARY:
            if (action != UI_ACTION_CHOOSE) break;
            if (in_library)
            {
                int index = find_program( l, p->path );

                l->programs[index].removed = 1;
                if (!save_library( l ))
                {
                    l->programs[index].removed = 0;
                    break;
                }
                ui_toast( ui, "Removed from the library", 1500 );
                /* p may be the removed entry itself; the menu goes on with a copy. */
                copy = *p;
                copy.icon = NULL;
                copy.added = 0;
                p = &copy;
            }
            else
            {
                int index = add_program( l, p->path, 1 );

                if (index < 0) break;
                save_library( l );
                ui_toast( ui, "Added to the library", 1500 );
                p = &l->programs[index];
                enable_program_dxvk( l, p );
            }
            break;
        }
    }
}

/***********************************************************************
 * Settings
 */

enum settings_row
{
    SET_HIDDEN, SET_HIDE_MISSING, SET_DXVK_ON_ADD, SET_VERBOSE, SET_PROFILE, SET_WINDOWS,
    SET_CONTROLS, SET_STEAMGRIDDB,
    SET_UPDATE, SET_REOPEN, SET_FORWARDER, SET_MAKE_32BIT, SET_MAKE_MAIN,
    SET_CREDITS, SETTINGS_ROWS
};

/* Which forwarder to send a game to when this one cannot run it. The console
 * lists what is installed; the user picks the one they made with a 32-bit
 * address space, since only they know which that is. */
static void choose_forwarder( struct launcher *l )
{
    struct wine_nx_launcher_title *titles;
    struct ui_row *rows;
    struct ui_list list = {0};
    char value[64];
    int count, i, chosen;

    if (!l->options->list_titles) return;
    titles = calloc( LAUNCHER_MAX_TITLES, sizeof(*titles) );
    rows = calloc( LAUNCHER_MAX_TITLES + 1, sizeof(*rows) );
    if (!titles || !rows) { free( titles ); free( rows ); return; }

    count = l->options->list_titles( titles, LAUNCHER_MAX_TITLES );
    /* The first row clears the choice; this forwarder is not offered, as sending
     * a game to the address space it was refused in would only refuse it again. */
    snprintf( rows[0].label, sizeof(rows[0].label), "None" );
    snprintf( rows[0].value, sizeof(rows[0].value), "%s", "Do not offer another forwarder" );
    for (i = 0; i < count; i++)
    {
        struct ui_row *row = &rows[i + 1];

        snprintf( row->label, sizeof(row->label), "%s", titles[i].name );
        snprintf( row->value, sizeof(row->value), "%016llX", titles[i].id );
        row->disabled = titles[i].id == l->options->title_id;
        if (row->disabled) snprintf( row->value, sizeof(row->value), "%s", "This forwarder" );
    }
    if (ui_list_run( &l->ui, &list, "32-bit forwarder", "Installed applications", rows, count + 1, 1 ) ==
        UI_ACTION_CHOOSE)
    {
        chosen = list.selection;
        if (!chosen || titles[chosen - 1].id == l->options->title_id)
        {
            launcher_kv_set( &l->look, "forwarder-32bit", NULL );
            launcher_kv_set( &l->look, "forwarder-32bit-name", NULL );
        }
        else
        {
            snprintf( value, sizeof(value), "%016llX", titles[chosen - 1].id );
            launcher_kv_set( &l->look, "forwarder-32bit", value );
            launcher_kv_set( &l->look, "forwarder-32bit-name", titles[chosen - 1].name );
        }
    }
    free( titles );
    free( rows );
}

static void save_look( struct launcher *l )
{
    char path[512];

    launcher_kv_set( &l->look, "theme", NULL );
    /* What the library shows is worked out from the screen now, and the
     * animations are simply on, so none of these are kept. */
    launcher_kv_set( &l->look, "animations", NULL );
    launcher_kv_set( &l->look, "columns", NULL );
    launcher_kv_set( &l->look, "rows", NULL );
    launcher_kv_set( &l->look, "show-hidden", l->show_hidden ? "1" : "0" );
    launcher_kv_set( &l->look, "hide-missing", l->hide_missing ? "1" : NULL );
    launcher_kv_set( &l->look, "browse", l->browse_dir );
    runtime_file( l, "launcher.txt", path, sizeof(path) );
    launcher_kv_save( &l->look, path );
}

/* What Wine-NX is built from and on; README.md's Credits section has the same list. */
static const struct { const char *name, *value, *help; } credits[] =
{
    { "Wine", "WineHQ, LGPL-2.1+",
      "https://www.winehq.org\nThe Windows API, the loader, WoW64 and the Direct3D, OpenGL and Vulkan layers." },
    { "Box64", "ptitSeb, MIT",
      "https://github.com/ptitSeb/box64\nRuns x86 and x86-64 code through its interpreter and ARM64 dynarec." },
    { "DXVK", "Philip Rebohle, zlib",
      "https://github.com/doitsujin/dxvk\nDirect3D over Vulkan, for programs set to d3d=dxvk." },
    { "VKD3D-Proton", "VKD3D-Proton contributors, LGPL-2.1",
      "https://github.com/HansKristian-Work/vkd3d-proton\nDirect3D 12 over Vulkan." },
    { "Mesa", "Mesa3D, MIT",
      "https://mesa3d.org\nOpenGL through nvc0 and Vulkan through NVK on the Switch GPU." },
    { "mesa-switch", "danfromtico, NaGaa95 and others",
      "https://github.com/danfromtico/mesa-switch\nThe Switch port of Mesa 26, with nvc0 and NVK, that the runtime links." },
    { "Switch Mesa and libdrm_nouveau", "fincs, Subv, Jules Blok, MIT",
      "devkitPro's Switch ports of Mesa 20.1 and libdrm_nouveau, the earlier OpenGL path." },
    { "libnx", "switchbrew, ISC",
      "https://github.com/switchbrew/libnx\nThe Horizon system library the runtime is written against." },
    { "devkitPro", "devkitA64 and portlibs",
      "https://devkitpro.org\nThe toolchain and the Switch builds of the libraries below." },
    { "SDL2 and SDL2_ttf", "Sam Lantinga, zlib",
      "https://www.libsdl.org\nThe launcher's drawing, input and text." },
    { "FreeType", "FreeType Project, FTL",
      "https://freetype.org\nFont rendering for the launcher." },
    { "HarfBuzz", "HarfBuzz authors, MIT",
      "https://harfbuzz.github.io\nText shaping for the launcher." },
    { "libpng, zlib, bzip2", "libpng, zlib and BSD licenses",
      "https://www.libpng.org  https://zlib.net  https://sourceware.org/bzip2\nProgram icons and compressed data." },
    { "llvm-mingw", "Martin Storsjo, Apache-2.0",
      "https://github.com/mstorsjo/llvm-mingw\nBuilds Wine's and DXVK's Windows DLLs (LLVM, libc++, mingw-w64)." },
    { "7-Zip", "Igor Pavlov, LGPL-2.1",
      "https://www.7-zip.org\n7zr.exe, the benchmark and archive test program on the card." },
    { "dolphin-nx", "NaGaa95, reference",
      "https://github.com/NaGaa95/dolphin-nx\nA Nintendo Switch port used as a platform reference." },
    { "Atmosphere", "Atmosphere-NX, reference",
      "https://github.com/Atmosphere-NX/Atmosphere\nIts kernel source is how Autorun learns what Horizon's memory calls allow." },
    { "tico-dolphin", "ticohq, reference",
      "https://github.com/ticohq/tico-dolphin\nJIT and exception handling on Horizon." },
    { "WineBox64 NX", "Ibnuard, reference",
      "https://github.com/Ibnuard/winebox64_nx\nA proof of concept running x86-64 Wine under Box64 on Horizon; "
      "reference for Autorun's Box64 and libnx integration." },
    { "sphaira", "ITotalJustice, NaGaa95",
      "https://github.com/NaGaa95/sphaira\nForwarders that start Autorun with a 32-bit address space." },
};
#define CREDIT_COUNT (sizeof(credits) / sizeof(credits[0]))

/* The keys a controller sends, as a screen rather than a file to be written by
 * hand. One row for each control, the key it sends beside it; left and right
 * step through the keys, A opens the whole list, and Y puts a control back to
 * what it sends with no line of its own.
 *
 * What is written is the same keys.txt the runtime has always read -- a
 * NAME=code line for each control that differs -- so a file written here can
 * still be edited on a computer, and one edited there opens here. */
static unsigned short control_key( const struct launcher_kv *keys, const struct launcher_kv *under,
                                   int control )
{
    const char *name = wine_nx_controls[control].name;
    char value[64];

    if (launcher_kv_get( keys, name, value, sizeof(value) ) && value[0])
        return (unsigned short)strtoul( value, NULL, 0 );
    /* A program's own keys are applied over the shared ones, so a control the
     * program says nothing about shows what the shared file gives it. */
    if (under && launcher_kv_get( under, name, value, sizeof(value) ) && value[0])
        return (unsigned short)strtoul( value, NULL, 0 );
    return wine_nx_controls[control].sends;
}

static void set_control_key( struct launcher_kv *keys, int control, int code )
{
    char value[32];

    /* Back to the default is the line taken away, not a line saying the
     * default: a later build that changes what a control sends should reach a
     * controller nobody has touched. */
    if (code < 0) launcher_kv_set( keys, wine_nx_controls[control].name, NULL );
    else
    {
        snprintf( value, sizeof(value), "0x%02x", code );
        launcher_kv_set( keys, wine_nx_controls[control].name, value );
    }
}

/* What one of the three that point is set to do. The keys file says it in
 * words -- LSTICK=mouse -- because it is not a key. */
enum device_choice { DEVICE_MOUSE, DEVICE_DPAD, DEVICE_ARROWS, DEVICE_WASD, DEVICE_CUSTOM };

static const char *const device_choice_names[] =
    { "Mouse", "Same as d-pad", "Arrow keys", "W A S D", "Its own keys" };

static int device_points( const struct launcher_kv *keys, const struct launcher_kv *under, int device )
{
    const char *name = wine_nx_devices[device].name;
    char value[32];

    if (launcher_kv_get( keys, name, value, sizeof(value) ) && value[0])
        return strcasecmp( value, "keys" ) == 0 ? 0 : 1;
    if (under && launcher_kv_get( under, name, value, sizeof(value) ) && value[0])
        return strcasecmp( value, "keys" ) == 0 ? 0 : 1;
    return wine_nx_devices[device].points;
}

static enum device_choice device_choice( const struct launcher_kv *keys, const struct launcher_kv *under,
                                         int device )
{
    const struct wine_nx_device *d = &wine_nx_devices[device];
    unsigned short code[4];
    int i, unset = 1;

    if (device_points( keys, under, device )) return DEVICE_MOUSE;
    for (i = 0; i < 4; i++)
    {
        code[i] = control_key( keys, under, d->first + i );
        if (code[i]) unset = 0;
    }
    if (unset && d->follows_dpad) return DEVICE_DPAD;
    if (!memcmp( code, wine_nx_preset_arrows, sizeof(code) )) return DEVICE_ARROWS;
    if (!memcmp( code, wine_nx_preset_wasd, sizeof(code) )) return DEVICE_WASD;
    return DEVICE_CUSTOM;
}

static void set_device_choice( struct launcher_kv *keys, int device, enum device_choice choice )
{
    const struct wine_nx_device *d = &wine_nx_devices[device];
    const unsigned short *preset = choice == DEVICE_WASD ? wine_nx_preset_wasd : wine_nx_preset_arrows;
    int i;

    launcher_kv_set( keys, d->name, choice == DEVICE_MOUSE ? "mouse" : "keys" );
    if (choice == DEVICE_MOUSE || choice == DEVICE_CUSTOM) return;
    for (i = 0; i < 4; i++)
        set_control_key( keys, d->first + i, choice == DEVICE_DPAD ? -1 : preset[i] );
}

/* Round the choices for this one, leaving out those it does not have. */
static enum device_choice next_device_choice( int device, enum device_choice from, int forward )
{
    int at = from == DEVICE_CUSTOM ? DEVICE_CUSTOM : from;

    for (;;)
    {
        at += forward ? 1 : -1;
        if (at < 0) at = DEVICE_CUSTOM;
        if (at > DEVICE_CUSTOM) at = DEVICE_MOUSE;
        /* Its own keys is where a file written by hand puts it, not somewhere
         * to step into; the d-pad is the left stick's alone. */
        if (at == DEVICE_CUSTOM) continue;
        if (at == DEVICE_DPAD && !wine_nx_devices[device].follows_dpad) continue;
        return (enum device_choice)at;
    }
}

/* The whole list of keys, starting on the one the control sends now. Returns
 * the code chosen, or -1 for the way out. */
static int key_screen( struct launcher *l, const char *control_label, unsigned short current )
{
    static struct ui_row rows[WINE_NX_KEY_NAME_COUNT];
    struct ui_list list = {0};
    char title[128];
    int i, at = wine_nx_key_index( current );

    memset( rows, 0, sizeof(rows) );
    for (i = 0; i < WINE_NX_KEY_NAME_COUNT; i++)
    {
        snprintf( rows[i].label, sizeof(rows[i].label), "%s", wine_nx_key_names[i].name );
        if (wine_nx_key_names[i].code)
            snprintf( rows[i].value, sizeof(rows[i].value), "0x%02x", wine_nx_key_names[i].code );
    }
    snprintf( title, sizeof(title), "%s sends", control_label );
    list.selection = at > 0 ? at : 0;
    for (;;)
    {
        enum ui_action action = ui_list_run( &l->ui, &list, title, "Controls", rows,
                                             WINE_NX_KEY_NAME_COUNT, 0 );

        if (action == UI_ACTION_BACK || action == UI_ACTION_QUIT) return -1;
        if (action == UI_ACTION_CHOOSE) return wine_nx_key_names[list.selection].code;
    }
}

static void controls_screen( struct launcher *l, const char *path, const char *under_path,
                             const char *title )
{
    struct ui_row rows[WINE_NX_DEVICE_COUNT_UI + WINE_NX_CONTROL_COUNT];
    struct launcher_kv keys, under;
    struct ui_list list = {0};
    int changed = 0, i;

    if (under_path) launcher_kv_load( &under, under_path );
    if (!launcher_kv_load( &keys, path ))
    {
        ui_message( &l->ui, "Controls", "That file is too large to open here; edit it on a computer." );
        ui_start_screen( &l->ui );
        return;
    }
    for (;;)
    {
        const struct launcher_kv *base = under_path ? &under : NULL;
        enum ui_action action;
        char label[64];
        unsigned short code;
        int device;

        memset( rows, 0, sizeof(rows) );
        for (device = 0; device < WINE_NX_DEVICE_COUNT_UI; device++)
        {
            snprintf( rows[device].label, sizeof(rows[0].label), "%s", wine_nx_devices[device].label );
            snprintf( rows[device].value, sizeof(rows[0].value), "%s",
                      device_choice_names[device_choice( &keys, base, device )] );
            rows[device].adjustable = 1;
            rows[device].kind = UI_ROW_VALUE;
            rows[device].help = "Move the mouse, or send four keys. A game played with the mouse "
                                "wants both sticks on it; one played with the keyboard wants the "
                                "keys it walks with.";
        }
        for (i = 0; i < WINE_NX_CONTROL_COUNT; i++)
        {
            struct ui_row *row = &rows[WINE_NX_DEVICE_COUNT_UI + i];

            snprintf( row->label, sizeof(row->label), "%s", wine_nx_controls[i].label );
            snprintf( row->value, sizeof(row->value), "%s",
                      wine_nx_key_label( i, control_key( &keys, base, i ), label, sizeof(label) ) );
            row->adjustable = 1;
            row->kind = UI_ROW_VALUE;
            /* A stick on the mouse has no keys to give: say so rather than
             * offer four rows that do nothing. */
            for (device = 0; device < WINE_NX_DEVICE_COUNT_UI; device++)
            {
                if (i < wine_nx_devices[device].first || i >= wine_nx_devices[device].first + 4) continue;
                if (!device_points( &keys, base, device )) continue;
                snprintf( row->value, sizeof(row->value), "Moves the mouse" );
                row->adjustable = 0;
                row->disabled = 1;
            }
        }
        rows[WINE_NX_DEVICE_COUNT_UI].help =
            "A and B are the mouse buttons until they are given a key of their own.";
        action = ui_list_run( &l->ui, &list, title, "Controls", rows,
                              WINE_NX_DEVICE_COUNT_UI + WINE_NX_CONTROL_COUNT, 1 );
        if (action == UI_ACTION_BACK || action == UI_ACTION_QUIT) break;
        if (list.selection < WINE_NX_DEVICE_COUNT_UI)
        {
            enum device_choice choice = device_choice( &keys, base, list.selection );

            if (action == UI_ACTION_LEFT || action == UI_ACTION_RIGHT || action == UI_ACTION_CHOOSE)
            {
                set_device_choice( &keys, list.selection,
                                   next_device_choice( list.selection, choice,
                                                       action != UI_ACTION_LEFT ) );
                changed = 1;
            }
            else if (action == UI_ACTION_RESET)
            {
                launcher_kv_set( &keys, wine_nx_devices[list.selection].name, NULL );
                changed = 1;
            }
            continue;
        }
        i = list.selection - WINE_NX_DEVICE_COUNT_UI;
        code = control_key( &keys, base, i );
        switch (action)
        {
        case UI_ACTION_CHOOSE:
        {
            int picked = key_screen( l, wine_nx_controls[i].label, code );

            ui_start_screen( &l->ui );
            if (picked >= 0 && picked != code) { set_control_key( &keys, i, picked ); changed = 1; }
            break;
        }
        case UI_ACTION_LEFT:
        case UI_ACTION_RIGHT:
        {
            int at = wine_nx_key_index( code );

            /* A code the list does not name steps from the start rather than
             * nowhere, so a hand-written file can be changed here too. */
            if (at < 0) at = 0;
            else at += action == UI_ACTION_RIGHT ? 1 : -1;
            if (at < 0) at = WINE_NX_KEY_NAME_COUNT - 1;
            if (at >= WINE_NX_KEY_NAME_COUNT) at = 0;
            set_control_key( &keys, i, wine_nx_key_names[at].code );
            changed = 1;
            break;
        }
        case UI_ACTION_RESET:
            set_control_key( &keys, i, -1 );
            changed = 1;
            break;
        default: break;
        }
    }
    if (changed && !launcher_kv_save( &keys, path ))
    {
        ui_message( &l->ui, "Controls", "The keys could not be saved to the card." );
        ui_start_screen( &l->ui );
    }
}

static void credits_screen( struct launcher *l )
{
    struct ui_row rows[CREDIT_COUNT];
    struct ui_list list = {0};
    size_t i;

    memset( rows, 0, sizeof(rows) );
    for (i = 0; i < CREDIT_COUNT; i++)
    {
        snprintf( rows[i].label, sizeof(rows[i].label), "%s", credits[i].name );
        snprintf( rows[i].value, sizeof(rows[i].value), "%s", credits[i].value );
        rows[i].help = credits[i].help;
    }
    for (;;)
    {
        enum ui_action action = ui_list_run( &l->ui, &list, "Credits", NULL, rows, CREDIT_COUNT, 0 );

        if (action == UI_ACTION_BACK || action == UI_ACTION_QUIT) return;
        if (action == UI_ACTION_CHOOSE)
        {
            ui_message( &l->ui, rows[list.selection].label, rows[list.selection].help );
            ui_start_screen( &l->ui );
        }
    }
}

/* The sections of Settings, in the order they stand in the list. */
enum settings_section { SET_SECTION_LIBRARY, SET_SECTION_DEFAULTS, SET_SECTION_ARTWORK, SET_SECTION_SYSTEM };

static void settings_menu( struct launcher *l )
{
    static const char *const sections[] = { "Library", "Game defaults", "Artwork", "System" };
    static const char *on_off[2] = { "Off", "On" };
    static int section;
    struct ui_row rows[SETTINGS_ROWS];
    struct ui_list list = {0};
    struct ui *ui = &l->ui;
    char path[512];

    for (;;)
    {
        enum ui_action action;
        int i;
        static const unsigned char row_section[SETTINGS_ROWS] =
        {
            [SET_HIDDEN] = SET_SECTION_LIBRARY,
            [SET_HIDE_MISSING] = SET_SECTION_LIBRARY,
            [SET_DXVK_ON_ADD] = SET_SECTION_LIBRARY,
            [SET_VERBOSE] = SET_SECTION_DEFAULTS, [SET_PROFILE] = SET_SECTION_DEFAULTS,
            [SET_WINDOWS] = SET_SECTION_DEFAULTS, [SET_CONTROLS] = SET_SECTION_DEFAULTS,
            [SET_STEAMGRIDDB] = SET_SECTION_ARTWORK,
            [SET_REOPEN] = SET_SECTION_SYSTEM,
            [SET_UPDATE] = SET_SECTION_SYSTEM,
            [SET_FORWARDER] = SET_SECTION_SYSTEM, [SET_MAKE_32BIT] = SET_SECTION_SYSTEM,
            [SET_MAKE_MAIN] = SET_SECTION_SYSTEM,
            [SET_CREDITS] = SET_SECTION_SYSTEM,
        };

        memset( rows, 0, sizeof(rows) );
        for (i = 0; i < SETTINGS_ROWS; i++)
        {
            rows[i].adjustable = 1;
            rows[i].kind = UI_ROW_VALUE;
            rows[i].group = row_section[i];
        }
        snprintf( rows[SET_HIDDEN].label, sizeof(rows[0].label), "Show hidden programs" );
        snprintf( rows[SET_HIDDEN].value, sizeof(rows[0].value), "%s", on_off[l->show_hidden] );
        rows[SET_HIDDEN].help = "Programs hidden from a game's own settings are listed again.";
        rows[SET_HIDDEN].kind = UI_ROW_SWITCH;
        rows[SET_HIDDEN].on = l->show_hidden;
        snprintf( rows[SET_HIDE_MISSING].label, sizeof(rows[0].label), "Hide missing programs" );
        snprintf( rows[SET_HIDE_MISSING].value, sizeof(rows[0].value), "%s", on_off[l->hide_missing] );
        rows[SET_HIDE_MISSING].help = "Hide unavailable games. USB games return when the drive reconnects.";
        rows[SET_HIDE_MISSING].kind = UI_ROW_SWITCH;
        rows[SET_HIDE_MISSING].on = l->hide_missing;
        snprintf( rows[SET_DXVK_ON_ADD].label, sizeof(rows[0].label), "Give a new game DXVK" );
        snprintf( rows[SET_DXVK_ON_ADD].value, sizeof(rows[0].value), "%s", on_off[!!l->options->dxvk_on_add] );
        rows[SET_DXVK_ON_ADD].kind = UI_ROW_SWITCH;
        rows[SET_DXVK_ON_ADD].on = !!l->options->dxvk_on_add;
        rows[SET_DXVK_ON_ADD].help = "New games use the bundled DXVK version until another version is selected.";
        snprintf( rows[SET_VERBOSE].label, sizeof(rows[0].label), "Verbose traces" );
        snprintf( rows[SET_VERBOSE].value, sizeof(rows[0].value), "%s", on_off[!!l->options->verbose] );
        rows[SET_VERBOSE].kind = UI_ROW_SWITCH;
        rows[SET_VERBOSE].on = !!l->options->verbose;
        rows[SET_VERBOSE].help = "Wine's traces go to wine-nx-runtime.log for every program without its own setting.";
        snprintf( rows[SET_PROFILE].label, sizeof(rows[0].label), "Profiler" );
        snprintf( rows[SET_PROFILE].value, sizeof(rows[0].value), "%s", on_off[!!l->options->profile] );
        rows[SET_PROFILE].kind = UI_ROW_SWITCH;
        rows[SET_PROFILE].on = !!l->options->profile;
        rows[SET_PROFILE].help = "[PROF] lines with where each thread spends its time.";
        snprintf( rows[SET_WINDOWS].label, sizeof(rows[0].label), "Windows shown by" );
        snprintf( rows[SET_WINDOWS].value, sizeof(rows[0].value), "%s",
                  l->options->framebuffer ? "Framebuffer" : "Compositor" );
        rows[SET_WINDOWS].help = "The framebuffer copies window pixels straight to the screen, "
                                 "for when the OpenGL compositor misbehaves.";
        snprintf( rows[SET_CONTROLS].label, sizeof(rows[0].label), "Controls" );
        snprintf( rows[SET_CONTROLS].value, sizeof(rows[0].value), "%s",
                  shared_keys( l, path, sizeof(path) ) ? "Set" : "Default" );
        rows[SET_CONTROLS].help = "The keys every program's controls send, unless the program has its own.";
        /* A row that opens a screen, not one that is changed where it stands:
         * left and right have nothing to do here and A must go in. */
        rows[SET_CONTROLS].adjustable = 0;
        rows[SET_CONTROLS].kind = UI_ROW_ACTION;
        snprintf( rows[SET_STEAMGRIDDB].label, sizeof(rows[0].label), "SteamGridDB API key" );
        snprintf( rows[SET_STEAMGRIDDB].value, sizeof(rows[0].value), "%s",
                  launcher_kv_get( &l->look, "steamgriddb-key", path, sizeof(path) ) && path[0] ? "Configured" : "Not set" );
        rows[SET_STEAMGRIDDB].help = "Used to automatically download the community's highest-rated square, portrait and hero artwork.";
        rows[SET_STEAMGRIDDB].adjustable = 0;
        snprintf( rows[SET_UPDATE].label, sizeof(rows[0].label), "Check for update" );
        rows[SET_UPDATE].kind = UI_ROW_ACTION;
        rows[SET_UPDATE].adjustable = 0;
        rows[SET_UPDATE].disabled = !l->update;
        rows[SET_UPDATE].help = "Official Autorun releases, changelog and installation. Games and settings are preserved.";
        snprintf( rows[SET_REOPEN].label, sizeof(rows[0].label), "Return here when a program ends" );
        snprintf( rows[SET_REOPEN].value, sizeof(rows[0].value), "%s", on_off[!!l->options->reopen_launcher] );
        rows[SET_REOPEN].kind = UI_ROW_SWITCH;
        rows[SET_REOPEN].on = !!l->options->reopen_launcher;
        rows[SET_REOPEN].help = "Autorun starts itself again instead of closing to the HOME menu. "
                                "A forwarder made by sphaira cannot do it and stops the console.";
        snprintf( rows[SET_FORWARDER].label, sizeof(rows[0].label), "32-bit forwarder" );
        {
            char name[128];
            int installed;

            if (!chosen_forwarder( l, name, sizeof(name), &installed ))
                snprintf( rows[SET_FORWARDER].value, sizeof(rows[0].value), "Not set" );
            else
                snprintf( rows[SET_FORWARDER].value, sizeof(rows[0].value), "%s%s", name,
                          installed ? "" : " (gone)" );
        }
        rows[SET_FORWARDER].help = "The forwarder made with a 32-bit address space, for games that need the low "
                                   "4 GB. A game that needs it is offered to that forwarder, which the console "
                                   "opens in this one's place and which starts the game by itself.";
        rows[SET_FORWARDER].adjustable = 0;
        rows[SET_FORWARDER].disabled = !l->options->list_titles || !l->options->launch_title;
        snprintf( rows[SET_MAKE_32BIT].label, sizeof(rows[0].label), "Make a 32-bit forwarder" );
        snprintf( rows[SET_MAKE_32BIT].value, sizeof(rows[0].value), "%s",
                  l->options->install_forwarder ? "Autorun 32-bit" : "Unavailable" );
        rows[SET_MAKE_32BIT].help = "For games linked for a fixed address in the low 4 GB, which only run with "
                                    "32 bits. Named above as soon as it is made, and games that need it are "
                                    "sent to it. Only on an emuMMC: an installed entry is what the console "
                                    "reports online.";
        rows[SET_MAKE_32BIT].adjustable = 0;
        rows[SET_MAKE_32BIT].disabled = !l->options->install_forwarder;
        snprintf( rows[SET_MAKE_MAIN].label, sizeof(rows[0].label), "Make an Autorun forwarder" );
        snprintf( rows[SET_MAKE_MAIN].value, sizeof(rows[0].value), "%s",
                  l->options->install_forwarder ? "Autorun" : "Unavailable" );
        rows[SET_MAKE_MAIN].help = "Autorun itself on the home menu with the 39-bit address space required "
                                   "by AMD64 programs. Only on an emuMMC.";
        rows[SET_MAKE_MAIN].adjustable = 0;
        rows[SET_MAKE_MAIN].disabled = !l->options->install_forwarder;
        snprintf( rows[SET_CREDITS].label, sizeof(rows[0].label), "Credits" );
        snprintf( rows[SET_CREDITS].value, sizeof(rows[0].value), "Wine, Box64, DXVK, Mesa..." );
        rows[SET_CREDITS].help = "The projects and platform references used by Autorun.";
        rows[SET_CREDITS].adjustable = 0;

        action = ui_settings_run( ui, &list, "Settings", NULL, sections,
                                  sizeof(sections) / sizeof(sections[0]), rows, SETTINGS_ROWS, 0, &section );
        if (action == UI_ACTION_BACK || action == UI_ACTION_QUIT) return;
        /* One section is shown at a time, so what it chose is counted within
         * that section: find the row it stands for. */
        {
            int shown = 0;

            for (i = 0; i < SETTINGS_ROWS; i++)
                if (row_section[i] == section && shown++ == list.selection) break;
            if (i >= SETTINGS_ROWS) continue;
        }
        switch (i)
        {
        case SET_HIDDEN: l->show_hidden = !l->show_hidden; break;
        case SET_HIDE_MISSING: l->hide_missing = !l->hide_missing; break;
        /* The runtime keeps these: it owns the settings file and writes every
         * one of them at once when the launcher closes. */
        case SET_VERBOSE: l->options->verbose = !l->options->verbose; break;
        case SET_PROFILE: l->options->profile = !l->options->profile; break;
        case SET_WINDOWS: l->options->framebuffer = !l->options->framebuffer; break;
        case SET_DXVK_ON_ADD: l->options->dxvk_on_add = !l->options->dxvk_on_add; break;
        case SET_REOPEN: l->options->reopen_launcher = !l->options->reopen_launcher; break;
        case SET_UPDATE:
            if (action == UI_ACTION_CHOOSE) launcher_update_open( l->update );
            ui_start_screen( ui );
            break;
        case SET_CONTROLS:
            if (action != UI_ACTION_CHOOSE) break;
            /* Written where the runtime looks first, whichever of the two the
             * card has now. */
            runtime_file( l, "config/keys.txt", path, sizeof(path) );
            controls_screen( l, path, NULL, "Controls" );
            ui_start_screen( ui );
            break;
        case SET_STEAMGRIDDB:
        {
            char key[512] = "";
            launcher_kv_get( &l->look, "steamgriddb-key", key, sizeof(key) );
            if (action == UI_ACTION_CHOOSE && launcher_platform_prompt( "SteamGridDB API key (blank removes)", key, key, sizeof(key) ))
                launcher_kv_set( &l->look, "steamgriddb-key", key[0] ? key : NULL );
            break;
        }
        case SET_FORWARDER:
            if (action == UI_ACTION_CHOOSE) choose_forwarder( l );
            break;
        case SET_MAKE_32BIT:
            if (action == UI_ACTION_CHOOSE) make_forwarder( l, 32 );
            break;
        case SET_MAKE_MAIN:
            if (action == UI_ACTION_CHOOSE) make_forwarder( l, 39 );
            break;

        case SET_CREDITS:
            credits_screen( l );
            ui_start_screen( ui );
            continue;
        }
        save_look( l );
    }
}

static void library_menu( struct launcher *l )
{
    struct ui_row rows[4];
    struct ui_list list = {0};
    static const char *sort_names[] = { "Title", "Recently added", "Recently launched" };

    for (;;)
    {
        enum ui_action action;
        memset( rows, 0, sizeof(rows) );
        snprintf( rows[0].label, sizeof(rows[0].label), "Search" );
        snprintf( rows[0].value, sizeof(rows[0].value), "%s", l->search[0] ? l->search : "All titles" );
        snprintf( rows[1].label, sizeof(rows[1].label), "Favorites only" );
        snprintf( rows[1].value, sizeof(rows[1].value), "%s", l->favorites_only ? "On" : "Off" );
        rows[1].adjustable = 1;
        snprintf( rows[2].label, sizeof(rows[2].label), "Sort by" );
        snprintf( rows[2].value, sizeof(rows[2].value), "%s", sort_names[l->sort_order] );
        rows[2].adjustable = 1;
        snprintf( rows[3].label, sizeof(rows[3].label), "Launcher settings" );

        action = ui_list_run( &l->ui, &list, "Library", "Filter and sort", rows, 4, 1 );
        if (action == UI_ACTION_BACK || action == UI_ACTION_QUIT) return;
        switch (list.selection)
        {
        case 0:
            if (action == UI_ACTION_RESET) l->search[0] = 0;
            else if (action == UI_ACTION_CHOOSE) edit_text( "Search games", l->search, sizeof(l->search) );
            break;
        case 1:
            l->favorites_only = action == UI_ACTION_RESET ? 0 : !l->favorites_only;
            break;
        case 2:
            if (action == UI_ACTION_RESET) l->sort_order = 0;
            else l->sort_order = (l->sort_order + (action == UI_ACTION_LEFT ? 2 : 1)) % 3;
            break;
        case 3:
            if (action == UI_ACTION_CHOOSE) settings_menu( l );
            break;
        }
    }
}

/***********************************************************************
 * File browser
 */

static int compare_files( const void *a, const void *b )
{
    const struct file_entry *x = a, *y = b;

    if (x->is_dir != y->is_dir) return y->is_dir - x->is_dir;
    return strcasecmp( x->name, y->name );
}

static int read_dir( struct launcher *l, const char *dir, int *count )
{
    struct dirent *entry;
    DIR *handle;

    *count = 0;
    if (!(handle = opendir( dir ))) return 0;
    while (*count < MAX_FILES && (entry = readdir( handle )))
    {
        struct file_entry *file = files + *count;
        char path[512];
        struct stat st;

        if (entry->d_name[0] == '.') continue;
        if ((size_t)snprintf( path, sizeof(path), "%s%s%s", dir, is_root( dir ) && dir[strlen( dir ) - 1] == '/' ? "" : "/",
                              entry->d_name ) >= sizeof(path)) continue;
        if (entry->d_type == DT_DIR) file->is_dir = 1;
        else if (entry->d_type == DT_REG) file->is_dir = 0;
        else if (stat( path, &st )) continue;
        else file->is_dir = S_ISDIR( st.st_mode );
        if (!file->is_dir && !launcher_is_exe( entry->d_name )) continue;
        snprintf( file->name, sizeof(file->name), "%s", entry->d_name );
        file->supported = file->is_dir || !l->options->machine_of( path, &file->machine );
        (*count)++;
    }
    closedir( handle );
    qsort( files, *count, sizeof(files[0]), compare_files );
    return 1;
}

static void join_path( char *out, size_t size, const char *dir, const char *name )
{
    snprintf( out, size, "%s%s%s", dir, dir[strlen( dir ) - 1] == '/' ? "" : "/", name );
}

static int directory_exists( const char *path )
{
    struct stat st;

    return !stat( path, &st ) && S_ISDIR( st.st_mode );
}

static int path_below( const char *path, const char *root )
{
    size_t length = strlen( root );

    return !strncmp( path, root, length );
}

/* Choose the filesystem before showing any folders. The saved directory is
 * kept within the chosen filesystem, but never skips this screen. */
static int file_browser_storage( struct launcher *l, char *dir, size_t size )
{
    struct ui *ui = &l->ui;
    static const char *const usb_paths[] = { "ums0:/", "ums1:/", "ums2:/", "ums3:/", "ums4:/" };
    static const char *const usb_names[] = { "USB 1", "USB 2", "USB 3", "USB 4", "USB 5" };
    struct wine_nx_launcher_usb_volume mounted[LAUNCHER_MAX_USB_VOLUMES];
    int mounted_count, i;

    for (;;)
    {
        struct ui_list list = {0};
        struct ui_row rows[2] = {0};
        enum ui_action action;

        memset( mounted, 0, sizeof(mounted) );
        if (l->options->list_usb)
            mounted_count = l->options->list_usb( mounted, LAUNCHER_MAX_USB_VOLUMES );
        else
        {
            mounted_count = 0;
            for (i = 0; i < LAUNCHER_USB_DRIVES; i++)
                if (directory_exists( usb_paths[i] ))
                {
                    snprintf( mounted[mounted_count].path, sizeof(mounted[mounted_count].path), "%s", usb_paths[i] );
                    snprintf( mounted[mounted_count].label, sizeof(mounted[mounted_count].label), "%s", usb_names[i] );
                    mounted[mounted_count++].drive = 'D' + i;
                }
        }

        snprintf( rows[0].label, sizeof(rows[0].label), "SD Card" );
        snprintf( rows[0].value, sizeof(rows[0].value), "C: and Z:" );
        snprintf( rows[1].label, sizeof(rows[1].label), "USB" );
        if (mounted_count)
            snprintf( rows[1].value, sizeof(rows[1].value), mounted_count == 1 ? "1 volume" : "%d volumes",
                      mounted_count );
        else
            snprintf( rows[1].value, sizeof(rows[1].value), "Not connected" );

        action = ui_list_run( ui, &list, "Add Game", "Choose storage", rows, 2, 0 );
        if (action == UI_ACTION_BACK || action == UI_ACTION_QUIT) return 0;
        if (action != UI_ACTION_CHOOSE) continue;
        if (!list.selection)
        {
            if (path_below( l->browse_dir, "sdmc:/" ) && directory_exists( l->browse_dir ))
                snprintf( dir, size, "%s", l->browse_dir );
            else if (directory_exists( LAUNCHER_DRIVE_C ))
                snprintf( dir, size, "%s", LAUNCHER_DRIVE_C );
            else snprintf( dir, size, "sdmc:/" );
            return 1;
        }
        if (list.selection == 1 && !mounted_count)
        {
            ui_message( ui, "USB", "No mounted USB volume was found. Connect a drive, then try again." );
            continue;
        }
        if (list.selection == 1 && mounted_count == 1)
        {
            if (path_below( l->browse_dir, mounted[0].path ) && directory_exists( l->browse_dir ))
                snprintf( dir, size, "%s", l->browse_dir );
            else snprintf( dir, size, "%s", mounted[0].path );
            return 1;
        }
        if (list.selection == 1)
        {
            struct ui_list volumes = {0};
            enum ui_action volume_action;

            for (i = 0; i < mounted_count; i++)
            {
                memset( file_rows + i, 0, sizeof(file_rows[0]) );
                snprintf( file_rows[i].label, sizeof(file_rows[i].label), "%s", mounted[i].label );
                snprintf( file_rows[i].value, sizeof(file_rows[i].value), "%c:", mounted[i].drive );
            }
            volume_action = ui_list_run( ui, &volumes, "USB", "Choose a volume", file_rows,
                                         mounted_count, 0 );
            if (volume_action == UI_ACTION_QUIT) return 0;
            if (volume_action == UI_ACTION_BACK) continue;
            if (volume_action != UI_ACTION_CHOOSE) continue;
            if (path_below( l->browse_dir, mounted[volumes.selection].path ) && directory_exists( l->browse_dir ))
                snprintf( dir, size, "%s", l->browse_dir );
            else snprintf( dir, size, "%s", mounted[volumes.selection].path );
            return 1;
        }
    }
}

static int file_browser_pick( struct launcher *l, char *target, size_t size )
{
    struct ui *ui = &l->ui;
    char dir[512], came_from[256] = "", dos[512], path[512];

    for (;;)
    {
        int choose_storage = 0;

        if (!file_browser_storage( l, dir, sizeof(dir) ))
        {
            save_look( l );
            return 0;
        }
        came_from[0] = 0;
        while (!choose_storage)
        {
            struct ui_list list = {0};
            int count, has_up, readable, rows, i, reload = 0;

            while (!(readable = read_dir( l, dir, &count )) && !is_root( dir )) parent_dir( dir );
            if (!readable)
            {
                choose_storage = 1;
                continue;
            }
            snprintf( l->browse_dir, sizeof(l->browse_dir), "%s", dir );
            has_up = !is_root( dir );
            if (!launcher_dos_path( dir, dos, sizeof(dos) )) snprintf( dos, sizeof(dos), "%s", dir );

            rows = 0;
            if (has_up)
            {
                char parent[512], parent_dos[512];

                snprintf( parent, sizeof(parent), "%s", dir );
                parent_dir( parent );
                memset( file_rows, 0, sizeof(file_rows[0]) );
                snprintf( file_rows[0].label, sizeof(file_rows[0].label), "Up one folder" );
                if (launcher_dos_path( parent, parent_dos, sizeof(parent_dos) ))
                    snprintf( file_rows[0].value, sizeof(file_rows[0].value), "%s", parent_dos );
                rows = 1;
            }
            for (i = 0; i < count; i++, rows++)
            {
                struct ui_row *row = file_rows + rows;

                memset( row, 0, sizeof(*row) );
                snprintf( row->label, sizeof(row->label), "%s", files[i].name );
                if (files[i].is_dir) snprintf( row->value, sizeof(row->value), "Folder" );
                else if (!files[i].supported)
                {
                    snprintf( row->value, sizeof(row->value), "Cannot run here" );
                    row->disabled = 1;
                }
                else snprintf( row->value, sizeof(row->value), "%s", launcher_machine_name( files[i].machine ) );
                if (came_from[0] && !strcasecmp( files[i].name, came_from )) list.selection = rows;
            }
            if (!rows)
            {
                memset( file_rows, 0, sizeof(file_rows[0]) );
                snprintf( file_rows[0].label, sizeof(file_rows[0].label), "No folders or programs here" );
                file_rows[0].disabled = 1;
                rows = 1;
            }
            /* A folder opens on its first entry; going up returns to the folder left. */
            if (!came_from[0] && has_up && rows > 1) list.selection = 1;
            came_from[0] = 0;

            while (!reload)
            {
                enum ui_action action = ui_list_run( ui, &list, "Files", dos, file_rows, rows, 0 );
                int index = list.selection - has_up;

                if (action == UI_ACTION_QUIT) return 0;
                if (action == UI_ACTION_BACK || (action == UI_ACTION_CHOOSE && index < 0))
                {
                    if (!has_up)
                    {
                        choose_storage = 1;
                        break;
                    }
                    snprintf( came_from, sizeof(came_from), "%s", file_name( dir ) );
                    parent_dir( dir );
                    reload = 1;
                    break;
                }
                if (action != UI_ACTION_CHOOSE) continue;
                if (index < 0 || index >= count) continue;
                join_path( path, sizeof(path), dir, files[index].name );
                launcher_log( "[LAUNCHER] Browser chose %s (%s)", path,
                              files[index].is_dir ? "folder" : "program" );
                if (files[index].is_dir)
                {
                    snprintf( dir, sizeof(dir), "%s", path );
                    reload = 1;
                }
                else
                {
                    snprintf( target, size, "%s", path );
                    save_look( l );
                    return 1;
                }
            }
        }
    }
}

static int add_game( struct launcher *l )
{
    struct program program;
    char path[512], message[800];
    int index;

    if (!file_browser_pick( l, path, sizeof(path) )) return -1;
    launcher_log( "[LAUNCHER] Add Game selected %s", path );
    if ((index = find_program( l, path )) >= 0)
    {
        ui_message( &l->ui, "Add Game", "This game is already in your library." );
        return index;
    }
    if (!describe_program( l, &program, path ))
    {
        ui_message( &l->ui, "Add Game", "Autorun cannot run this executable." );
        return -1;
    }
    for (;;)
    {
        static const struct ui_hint hints[] = { { UI_A, "Add" }, { UI_X, "Rename" }, { UI_B, "Cancel" } };
        char name[128];
        int answer;

        snprintf( message, sizeof(message), "%s\n\n%s\n\nAdd this game to your library?",
                  program.title, program.dos );
        answer = ui_ask( &l->ui, "Review Game", message, hints, 3 );
        if (answer == UI_A) break;
        if (answer != UI_X)
        {
            launcher_log( "[LAUNCHER] Add Game cancelled during review" );
            return -1;
        }
        /* Named before it is added, because the name is what the library is
         * sorted and searched by. It is kept in the file beside the program. */
        snprintf( name, sizeof(name), "%s", program.title );
        if (launcher_platform_prompt( "Name", name, name, sizeof(name) ) && name[0])
        {
            snprintf( program.settings.title, sizeof(program.settings.title), "%s", name );
            snprintf( program.title, sizeof(program.title), "%s", name );
            save_program_settings( l, &program );
        }
        ui_start_screen( &l->ui );
    }
    if (l->program_count >= LAUNCHER_MAX_ENTRIES)
    {
        ui_message( &l->ui, "Add Game", "The library is full." );
        return -1;
    }
    index = l->program_count;
    l->programs[index] = program;
    l->programs[index].added = 1;
    l->programs[index].catalog_id = l->catalog.next_id;
    l->programs[index].added_order = l->catalog.next_order;
    l->program_count++;
    if (!save_library( l ))
    {
        l->program_count--;
        return -1;
    }
    enable_program_dxvk( l, &l->programs[index] );
    launcher_log( "[LAUNCHER] Added %s to the library", path );
    ui_toast( &l->ui, "Game added to the library", 1800 );
    return index;
}

/***********************************************************************
 * The library and the entry point
 */

/* The game the D-pad is on: Home's history, or the library's grid. */
static struct program *current_program( struct launcher *l, int home )
{
    if (home) return l->history_count ? &l->programs[l->history[l->history_selection]] : NULL;
    return l->visible_count ? &l->programs[l->visible[l->selection]] : NULL;
}

static int current_index( const struct launcher *l, int home )
{
    if (home) return l->history_count ? l->history[l->history_selection] : -1;
    return l->visible_count ? l->visible[l->selection] : -1;
}

/* A game can be in either list, so both are rebuilt together. */
static void rebuild_lists( struct launcher *l, int keep_index )
{
    rebuild_visible( l, keep_index );
    rebuild_history( l, keep_index );
}

static int usb_program_path( const char *path )
{
    return !strncasecmp( path, "ums", 3 ) && path[3] >= '0' && path[3] <= '4' &&
           path[4] == ':' && (path[5] == '/' || path[5] == '\\');
}

static int refresh_usb_programs( struct launcher *l )
{
    int changed = 0, i;

    for (i = 0; i < l->program_count; i++)
    {
        struct program *p = l->programs + i;
        unsigned short machine;
        int available;

        if (p->removed || !usb_program_path( p->path )) continue;
        available = file_exists( p->path ) && !l->options->machine_of( p->path, &machine );
        if (available == !p->missing) continue;
        p->missing = !available;
        if (available)
        {
            p->machine = machine;
            p->resource_title[0] = 0;
            launcher_pe_describe( p->path, 0, NULL, p->resource_title, sizeof(p->resource_title) );
            load_program_settings( l, p );
            if (!p->icon) p->icon_state = ICON_UNKNOWN;
            if (!p->square_icon) p->square_state = ICON_UNKNOWN;
            if (!p->hero_icon) p->hero_state = ICON_UNKNOWN;
        }
        changed++;
    }
    return changed;
}

static void show_library( struct launcher *l, int *home, struct ui *ui )
{
    if (*home) ui_start_screen( ui );
    *home = 0;
    l->zone = ZONE_CONTENT;
    l->header_focus = SHELL_LIBRARY;
}

static int run_library( struct launcher *l, char *target, size_t size )
{
    struct ui *ui = &l->ui;
    struct ui_input input;
    /* Home lists what has been played, so until something has it opens the library. */
    int home = l->history_count > 0;

    l->zone = ZONE_CONTENT;
    l->header_focus = home ? SHELL_HOME : SHELL_LIBRARY;
    ui_start_screen( ui );
    while (ui_begin_frame( ui ))
    {
        struct grid g;

        grid_layout( l, &g );
        pump_icons( l );
        while (ui_poll( ui, &input ))
        {
            struct program *p = current_program( l, home );
            int keep = current_index( l, home ), hit;

            switch (input.touch)
            {
            case UI_TOUCH_TAP:
            {
                SDL_Point point = { input.x, input.y };
                int tab;

                for (tab = SHELL_HOME; tab < SHELL_TABS; tab++)
                    if (SDL_PointInRect( &point, &l->shell_hits[tab] )) break;
                if (tab < SHELL_TABS)
                {
                    l->zone = ZONE_HEADER;
                    l->header_focus = tab;
                    input.button = UI_A;
                    break;
                }
                if (input.y < UI_HEADER_HEIGHT) break;
                if (home)
                {
                    if (SDL_PointInRect( &point, &l->add_hit )) show_library( l, &home, ui );
                    else if ((hit = carousel_hit( l, input.x, input.y )) >= 0)
                    {
                        l->zone = ZONE_CONTENT;
                        if (hit == l->history_selection) input.button = UI_A;
                        else l->history_selection = hit;
                    }
                }
                else
                {
                    if ((hit = grid_hit( l, input.x, input.y )) < 0) break;
                    l->zone = ZONE_CONTENT;
                    if (hit == l->selection && p) input.button = UI_A;
                    else l->selection = hit;
                }
                break;
            }
            case UI_TOUCH_SWIPE_LEFT:
            case UI_TOUCH_SCROLL_UP:
                if (home)
                {
                    if (l->history_selection + 1 < l->history_count) l->history_selection++;
                }
                else l->selection = launcher_grid_page( l->selection, l->visible_count, g.columns, g.rows, 1 );
                break;
            case UI_TOUCH_SWIPE_RIGHT:
            case UI_TOUCH_SCROLL_DOWN:
                if (home)
                {
                    if (l->history_selection > 0) l->history_selection--;
                }
                else l->selection = launcher_grid_page( l->selection, l->visible_count, g.columns, g.rows, -1 );
                break;
            default:
                break;
            }

            /* A on the header acts on the item the D-pad is on, whatever the view. */
            if (input.button == UI_A && l->zone == ZONE_HEADER)
            {
                switch (l->header_focus)
                {
                case SHELL_HOME:
                case SHELL_LIBRARY:
                {
                    int to_home = l->header_focus == SHELL_HOME;

                    if (to_home != home) ui_start_screen( ui );
                    home = to_home;
                    /* The view changes under it; what the D-pad is on does not,
                     * so the next press still moves along the header. */
                    input.button = UI_NONE;
                    break;
                }
                default:
                    settings_menu( l );
                    rebuild_lists( l, keep );
                    ui_start_screen( ui );
                    input.button = UI_NONE;
                    break;
                }
            }

            switch (input.button)
            {
            case UI_LEFT:
            case UI_RIGHT:
            {
                int step = input.button == UI_LEFT ? -1 : 1;

                if (l->zone == ZONE_HEADER)
                {
                    int next = l->header_focus + step;

                    if (next >= SHELL_HOME && next < SHELL_TABS) l->header_focus = next;
                }
                else if (home)
                {
                    if (step < 0 && l->history_selection > 0) l->history_selection--;
                    if (step > 0 && l->history_selection + 1 < l->history_count) l->history_selection++;
                }
                else l->selection = launcher_grid_move( l->selection, l->visible_count, g.columns, step, 0 );
                break;
            }
            case UI_UP:
            case UI_DOWN:
            {
                int down = input.button == UI_DOWN;

                if (l->zone == ZONE_HEADER)
                {
                    if (down) l->zone = ZONE_CONTENT;
                }
                else if (home)
                {
                    if (!down)
                    {
                        l->zone = ZONE_HEADER;
                        l->header_focus = SHELL_HOME;
                    }
                }
                else
                {
                    int next = launcher_grid_move( l->selection, l->visible_count, g.columns, 0, down ? 1 : -1 );

                    /* The top row has nowhere above it but the header. */
                    if (next == l->selection && !down)
                    {
                        l->zone = ZONE_HEADER;
                        l->header_focus = SHELL_LIBRARY;
                    }
                    l->selection = next;
                }
                break;
            }
            case UI_L:
            case UI_R:
                if (home != (input.button == UI_L)) ui_start_screen( ui );
                home = input.button == UI_L;
                l->zone = ZONE_CONTENT;
                l->header_focus = home ? SHELL_HOME : SHELL_LIBRARY;
                break;
            case UI_A:
                if (!p)
                {
                    /* Home with nothing played opens the library; the library adds a game. */
                    if (home) show_library( l, &home, ui );
                    else
                    {
                        int added_index = add_game( l );

                        if (added_index >= 0) rebuild_lists( l, added_index );
                        ui_start_screen( ui );
                    }
                    break;
                }
                if (l->zone == ZONE_CONTENT)
                {
                    /* A on a game plays it; Y opens its Options menu. */
                    if (start_program( l, p, target, size )) return 1;
                    ui_start_screen( ui );
                }
                else
                {
                    if (program_menu( l, p, target, size )) return 1;
                    rebuild_lists( l, keep );
                    ui_start_screen( ui );
                }
                break;
            case UI_Y:
                if (!p) break;
                if (program_menu( l, p, target, size )) return 1;
                rebuild_lists( l, keep );
                ui_start_screen( ui );
                break;
            case UI_X:
                break;
            case UI_MINUS:
                if (home) settings_menu( l );
                else library_menu( l );
                rebuild_lists( l, keep );
                ui_start_screen( ui );
                break;
            case UI_PLUS:
            {
                static const char *const items[] = { "Add game", "Exit Autorun" };
                int chosen = ui_menu( ui, "Autorun", items, 2, 0 );

                ui_start_screen( ui );
                if (chosen == 1)
                {
                    if (ui_confirm( ui, "Exit Autorun", "Close Autorun and go back to the Homebrew Menu?",
                                    "Exit" )) return 0;
                    ui_start_screen( ui );
                }
                else if (!chosen)
                {
                    int added_index = add_game( l );

                    if (added_index >= 0)
                    {
                        rebuild_lists( l, added_index );
                        show_library( l, &home, ui );
                    }
                    ui_start_screen( ui );
                }
                break;
            }
            case UI_B:
                if (l->zone != ZONE_CONTENT)
                {
                    l->zone = ZONE_CONTENT;
                    break;
                }
                if (ui_confirm( ui, "Quit", "Close Autorun and go back to the Homebrew Menu?", "Quit" )) return 0;
                ui_start_screen( ui );
                break;
            }
            if (!ui->running) return 0;
        }
        if (!ui->running) break;
        if (SDL_AtomicCAS( &l->usb_changed, 1, 0 ))
        {
            int keep = current_index( l, home );
            int changed = refresh_usb_programs( l );

            if (changed)
            {
                char message[80];

                rebuild_lists( l, keep );
                snprintf( message, sizeof(message), changed == 1 ? "%d USB game refreshed" : "%d USB games refreshed",
                          changed );
                launcher_log( "[LAUNCHER] %s after a mount change", message );
                ui_toast( ui, message, 1800 );
            }
        }
        l->icon_frame = l->icon_use;
        if (home) draw_home( l );
        else draw_library( l );
        ui_present( ui );
        ui_wait( ui );
    }
    return 0;
}

int wine_nx_launcher_run( struct wine_nx_launcher_options *options, char *target, size_t target_size )
{
    struct launcher *l = &launcher;
    const void *font = NULL;
    size_t font_size = 0;
    char path[512];
    int ret, i, added, missing;
    Uint32 started;

    memset( l, 0, sizeof(*l) );
    l->options = options;
    runtime_file( l, "launcher.txt", path, sizeof(path) );
    launcher_kv_load( &l->look, path );
    l->show_hidden = launcher_kv_get_int( &l->look, "show-hidden", 0 ) == 1;
    l->hide_missing = launcher_kv_get_int( &l->look, "hide-missing", 0 ) == 1;
    if (!launcher_kv_get( &l->look, "browse", l->browse_dir, sizeof(l->browse_dir) ) || !l->browse_dir[0])
        snprintf( l->browse_dir, sizeof(l->browse_dir), "%s", LAUNCHER_DRIVE_C );

    started = SDL_GetTicks();
    /* The steps of coming up, on the card before each is taken: the launcher
     * that froze a console said nothing between the settings and its first
     * frame, and the log has to name the call that did not return. */
    launcher_log( "[LAUNCHER] bringing the screen up: the shared font" );
    if (!launcher_platform_font( &font, &font_size ) ||
        !ui_init( &l->ui, font, font_size, 1 ))
    {
        launcher_platform_font_release();
        /* libnx's console cannot draw once EGL has had the screen. */
        if (ui_screen_used())
        {
            launcher_log( "[LAUNCHER] SDL could not start (%s) after taking the screen; closing", ui_error() );
            return 0;
        }
        launcher_log( "[LAUNCHER] SDL could not start (%s); showing the text menu",
                      font ? ui_error() : "no shared font from the pl service" );
#ifdef __SWITCH__
        consoleInit( NULL );
        ret = wine_nx_launcher_console_run( LAUNCHER_DRIVE_C, options->runtime_dir, options->build, options->machine_of,
                                            &options->verbose, &options->profile, target, target_size );
        consoleExit( NULL );
        return ret;
#else
        return 0;
#endif
    }
    /* ui_init starts from a cleared screen, so the clock and the battery are
     * handed to it once it stands. */
    l->ui.header_status = header_status;
    l->ui.header_status_data = l;
    l->ui.footer_mark = footer_mark;
    l->usb_event = SDL_RegisterEvents( 1 );
    if (l->usb_event == (Uint32)-1) l->usb_event = SDL_USEREVENT;

    {
        SDL_RendererInfo info;

        if (SDL_GetRendererInfo( l->ui.renderer, &info )) info.name = "unknown";
        launcher_log( "[LAUNCHER] SDL %s video, %s renderer, font %zu bytes, ready in %u ms",
                      SDL_GetCurrentVideoDriver(), info.name, font_size,
                      SDL_GetTicks() - started );
    }
    {
        static const struct { const struct launcher_svg_icon *icon; int size; } symbols[SYMBOL_COUNT] =
        {
            [SYMBOL_HOME] = { &icon_gamepad_modern, SHELL_ICON },
            [SYMBOL_LIBRARY] = { &icon_grid, SHELL_ICON - 4 },
            [SYMBOL_ADD] = { &icon_plus, SHELL_ICON - 4 },
            [SYMBOL_SETTINGS] = { &icon_sliders, SHELL_ICON - 4 },
        };

        for (i = 0; i < SYMBOL_COUNT; i++)
            l->symbols[i] = ui_svg_texture( &l->ui, symbols[i].icon->d, symbols[i].icon->x, symbols[i].icon->y,
                                            symbols[i].icon->width, symbols[i].icon->height, symbols[i].size );
    }
    l->logo = load_logo( l );
    l->backdrop = l->backdrop_previous = -1;
    start_icons( l );
    started = SDL_GetTicks();
    load_library( l );
    rebuild_visible( l, -1 );
    rebuild_history( l, -1 );
    for (i = 0, added = 0; i < l->program_count; i++) added += l->programs[i].added;
    launcher_log( "[LAUNCHER] %d catalog games (%d registered, %d shown) loaded in %u ms; icons %s",
                  l->program_count, added, l->visible_count, SDL_GetTicks() - started,
                  l->thread ? "load on a worker thread" : "off: no worker thread" );
    for (i = 0; i < l->visible_count; i++)
    {
        const struct program *p = &l->programs[l->visible[i]];

        if (!strcasecmp( p->path, target ) || !strcasecmp( p->dos, target )) l->selection = i;
    }
    for (i = 0; i < l->history_count; i++)
    {
        const struct program *p = &l->programs[l->history[i]];

        if (!strcasecmp( p->path, target ) || !strcasecmp( p->dos, target )) l->history_selection = i;
    }
    if (!autorun_install_finish( options->runtime_dir ))
        ui_toast( &l->ui, "The update recovery files could not be cleared.", 5000 );
    l->update = launcher_update_create( &l->ui, options->runtime_dir, options->schedule_restart );
    l->ui.background_tick = launcher_update_tick;
    l->ui.background_data = l->update;
    ret = run_library( l, target, target_size );
    l->ui.background_tick = NULL;
    launcher_update_destroy( l->update );
    l->update = NULL;
    for (i = 0, added = 0, missing = 0; i < l->program_count; i++)
    {
        added += l->programs[i].icon_state == ICON_READY;
        missing += l->programs[i].icon_state == ICON_MISSING;
    }
    launcher_log( "[LAUNCHER] %s; %d icons shown, %d programs without one", ret ? "starting a program" : "closed",
                  added, missing );
    stop_icons( l );
    for (i = 0; i < SYMBOL_COUNT; i++)
        if (l->symbols[i]) SDL_DestroyTexture( l->symbols[i] );
    if (l->logo) SDL_DestroyTexture( l->logo );
    l->usb_event = 0;
    ui_quit( &l->ui );
    launcher_platform_font_release();
    return ret;
}
