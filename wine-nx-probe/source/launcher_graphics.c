#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "launcher_graphics.h"
#include "launcher_settings.h"
#include "launcher_ui.h"
#include "dxvk_releases.h"

struct graphics_catalog
{
    const char *root;
    int vkd3d, count, pending_count, refreshed, revision;
    struct dxvk_release releases[DXVK_MAX_RELEASES], pending[DXVK_MAX_RELEASES];
    SDL_Thread *thread;
    SDL_atomic_t done, cancel;
    enum dxvk_result result;
};

struct launcher_graphics
{
    struct ui *ui;
    char root[768];
    struct graphics_catalog *catalogs[2];
};

static const char *backend_name( int vkd3d ) { return vkd3d ? "VKD3D" : "DXVK"; }

static void resolve_version( struct launcher_graphics *g, unsigned short machine, int vkd3d,
                             const char *version, struct dxvk_version *selected )
{
    (vkd3d ? vkd3d_resolve_version : dxvk_resolve_version)( g->root, machine, version, selected );
}

static int catalog_progress( void *opaque, enum dxvk_progress_stage stage,
                             unsigned long long current, unsigned long long total )
{
    struct graphics_catalog *c = opaque;
    (void)stage; (void)current; (void)total;
    return SDL_AtomicGet( &c->cancel );
}

static int catalog_worker( void *opaque )
{
    struct graphics_catalog *c = opaque;
    SDL_Event event = {0};

    c->result = (c->vkd3d ? vkd3d_release_catalog : dxvk_release_catalog)(
        c->root, c->pending, DXVK_MAX_RELEASES, &c->pending_count, 0, catalog_progress, c );
    SDL_AtomicSet( &c->done, 1 );
    event.type = SDL_USEREVENT;
    SDL_PushEvent( &event );
    return 0;
}

static void poll_catalog( struct graphics_catalog *c )
{
    if (!c->thread || !SDL_AtomicGet( &c->done )) return;
    SDL_WaitThread( c->thread, NULL );
    c->thread = NULL;
    if (c->result == DXVK_OK)
    {
        c->count = c->pending_count;
        memcpy( c->releases, c->pending, c->count * sizeof(*c->releases) );
        c->refreshed = 1;
    }
    c->revision++;
}

static struct graphics_catalog *open_catalog( struct launcher_graphics *g, int vkd3d )
{
    struct graphics_catalog *c = g->catalogs[vkd3d];

    if (!c)
    {
        if (!(c = calloc( 1, sizeof(*c) ))) return NULL;
        c->root = g->root;
        c->vkd3d = vkd3d;
        c->revision = 1;
        c->result = (vkd3d ? vkd3d_release_catalog : dxvk_release_catalog)(
            g->root, c->releases, DXVK_MAX_RELEASES, &c->count, 1, NULL, NULL );
        g->catalogs[vkd3d] = c;
    }
    poll_catalog( c );
    if (!c->thread && !c->refreshed)
    {
        SDL_AtomicSet( &c->cancel, 0 );
        SDL_AtomicSet( &c->done, 0 );
        c->thread = SDL_CreateThreadWithStackSize( catalog_worker, "graphics-releases", 256 * 1024, c );
        if (!c->thread) c->result = DXVK_IO_ERROR;
    }
    return c;
}

struct launcher_graphics *launcher_graphics_create( struct ui *ui, const char *runtime_dir )
{
    struct launcher_graphics *g;

    if (strlen( runtime_dir ) >= sizeof(g->root) || !(g = calloc( 1, sizeof(*g) ))) return NULL;
    g->ui = ui;
    strcpy( g->root, runtime_dir );
    return g;
}

void launcher_graphics_destroy( struct launcher_graphics *g )
{
    int i;

    if (!g) return;
    for (i = 0; i < 2; i++)
        if (g->catalogs[i]) SDL_AtomicSet( &g->catalogs[i]->cancel, 1 );
    for (i = 0; i < 2; i++)
    {
        if (!g->catalogs[i]) continue;
        if (g->catalogs[i]->thread) SDL_WaitThread( g->catalogs[i]->thread, NULL );
        free( g->catalogs[i] );
    }
    free( g );
}

struct install_progress
{
    struct ui *ui;
    const struct dxvk_release *release;
    const char *name;
    enum dxvk_progress_stage stage;
    Uint32 last_draw;
    int started;
};

static int install_progress( void *opaque, enum dxvk_progress_stage stage,
                              unsigned long long current, unsigned long long total )
{
    struct install_progress *p = opaque;
    const char *status;
    char title[96];
    Uint32 now = SDL_GetTicks();

    if (stage == DXVK_PROGRESS_DOWNLOAD && !total) total = p->release->size;
    if (p->started && stage == p->stage && now - p->last_draw < 40 && (!total || current < total))
        return !p->ui->running;
    switch (stage)
    {
    case DXVK_PROGRESS_DOWNLOAD: status = "Downloading from GitHub..."; break;
    case DXVK_PROGRESS_VERIFY: status = "Verifying download..."; current = total = 0; break;
    default: status = "Installing x86 and x64 files..."; current = total = 0; break;
    }
    snprintf( title, sizeof(title), "Installing %s %s", p->name, p->release->version );
    ui_progress_update( p->ui, title, status, current, total );
    p->stage = stage;
    p->last_draw = now;
    p->started = 1;
    return !p->ui->running;
}

static int install_release( struct launcher_graphics *g, int vkd3d, const struct dxvk_release *release )
{
    struct install_progress progress = { .ui = g->ui, .release = release, .name = backend_name( vkd3d ) };
    enum dxvk_result result;

    ui_progress_begin( g->ui );
    result = (vkd3d ? vkd3d_install_release : dxvk_install_release)( g->root, release, install_progress, &progress );
    ui_progress_end( g->ui );
    if (result != DXVK_OK && g->ui->running)
        ui_message( g->ui, progress.name, dxvk_result_message( result ) );
    return result == DXVK_OK;
}

struct release_menu
{
    struct graphics_catalog *catalog;
    unsigned short machine;
    struct dxvk_version selected;
    struct ui_row rows[DXVK_MAX_RELEASES + 1];
    int ids[DXVK_MAX_RELEASES + 1], count, revision;
};

static int update_menu( void *opaque, int *selection )
{
    struct release_menu *m = opaque;
    struct graphics_catalog *c = m->catalog;
    char focused[32], bundled[32] = "";
    int i, latest = -1, latest_row = 0, current = -1, have_selected = 0, root_installed;
    int (*installed)( const char *, unsigned short, const char * ) =
        c->vkd3d ? vkd3d_release_installed : dxvk_release_installed;

    poll_catalog( c );
    if (m->revision == c->revision) return m->count;
    snprintf( focused, sizeof(focused), "%s", m->count && !m->rows[*selection].disabled
              ? m->rows[*selection].label : m->selected.version );
    m->revision = c->revision;
    m->count = 0;
    memset( m->rows, 0, sizeof(m->rows) );
    root_installed = installed( c->root, m->machine, "" );
    if (root_installed)
        (c->vkd3d ? vkd3d_root_version : dxvk_root_version)( c->root, m->machine, bundled, sizeof(bundled) );
    for (i = 0; i < c->count; i++)
    {
        struct ui_row *row;
        int index;

        if (!c->vkd3d && !launcher_dxvk_version_selectable( c->releases[i].version )) continue;
        if (latest < 0 && !c->releases[i].prerelease) { latest = i; latest_row = m->count; }
        index = m->count++;
        row = m->rows + index;
        m->ids[index] = i;
        snprintf( row->label, sizeof(row->label), "%s", c->releases[i].version );
        if (!strcmp( focused, row->label )) current = index;
        if (!strcmp( m->selected.version, row->label )) have_selected = 1;
        row->download = !(root_installed && !strcmp( bundled, row->label )) &&
                        !installed( c->root, m->machine, row->label );
        snprintf( row->value, sizeof(row->value), "%s", !row->download ? "Installed" :
                  i == latest ? "Latest" : c->releases[i].prerelease ? "Pre-release" : "" );
    }
    if (!have_selected && m->selected.installed &&
        (c->vkd3d || !m->selected.version[0] || launcher_dxvk_version_selectable( m->selected.version )))
    {
        int index = m->count++;
        m->ids[index] = -1;
        snprintf( m->rows[index].label, sizeof(m->rows[index].label), "%s",
                  m->selected.version[0] ? m->selected.version : "Bundled" );
        snprintf( m->rows[index].value, sizeof(m->rows[index].value), "Installed" );
        if (!strcmp( focused, m->selected.version ) || !strcmp( focused, m->rows[index].label )) current = index;
    }
    if (!m->count)
    {
        m->count = 1;
        m->rows[0].disabled = 1;
        snprintf( m->rows[0].label, sizeof(m->rows[0].label), "%s",
                  c->thread ? "Loading releases..." : dxvk_result_message( c->result ) );
    }
    *selection = current >= 0 ? current : latest_row;
    return m->count;
}

int launcher_graphics_select( struct launcher_graphics *g, const struct ui_list *anchor,
                              unsigned short machine, int vkd3d, char version[32] )
{
    struct release_menu *m;
    int chosen, ok = 0;

    if (!g || !(m = calloc( 1, sizeof(*m) ))) return 0;
    m->machine = machine;
    if (!(m->catalog = open_catalog( g, vkd3d ))) { free( m ); return 0; }
    resolve_version( g, machine, vkd3d, version, &m->selected );
    chosen = ui_settings_dropdown_live( g->ui, anchor, m->rows, 0, update_menu, m );
    if (chosen >= 0)
    {
        int id = m->ids[chosen];
        const struct dxvk_release *release = id >= 0 ? m->catalog->releases + id : NULL;
        if (!m->rows[chosen].download || (release && install_release( g, vkd3d, release )))
        {
            strcpy( version, release ? release->version : m->selected.version );
            ok = 1;
        }
    }
    free( m );
    return ok;
}

static const struct dxvk_release *find_release( struct graphics_catalog *c, const char *version )
{
    int i;

    for (i = 0; i < c->count; i++)
    {
        if (!c->vkd3d && !launcher_dxvk_version_selectable( c->releases[i].version )) continue;
        if (version[0] ? !strcmp( version, c->releases[i].version ) : !c->releases[i].prerelease)
            return c->releases + i;
    }
    return NULL;
}

static int ensure_backend( struct launcher_graphics *g, unsigned short machine, int vkd3d, char version[32] )
{
    struct graphics_catalog *c;
    struct dxvk_version selected;
    const struct dxvk_release *release;

    resolve_version( g, machine, vkd3d, version, &selected );
    if (!selected.installed)
    {
        if (!(c = open_catalog( g, vkd3d ))) return 0;
        release = find_release( c, version );
        if (c->thread && (!release || !version[0]))
        {
            char title[96];
            snprintf( title, sizeof(title), "Installing %s", backend_name( vkd3d ) );
            ui_progress_begin( g->ui );
            while (c->thread && g->ui->running)
            {
                ui_progress_update( g->ui, title, "Checking GitHub releases...", 0, 0 );
                poll_catalog( c );
                ui_wait( g->ui );
            }
            ui_progress_end( g->ui );
            if (!g->ui->running) { SDL_AtomicSet( &c->cancel, 1 ); return 0; }
            release = find_release( c, version );
        }
        if (!release)
        {
            ui_message( g->ui, backend_name( vkd3d ),
                        dxvk_result_message( c->result == DXVK_OK ? DXVK_NOT_FOUND : c->result ) );
            return 0;
        }
        if (!install_release( g, vkd3d, release )) return 0;
        resolve_version( g, machine, vkd3d, release->version, &selected );
    }
    if (!selected.installed) return 0;
    strcpy( version, selected.version );
    return 1;
}

int launcher_graphics_ensure( struct launcher_graphics *g, unsigned short machine,
                              char dxvk_version[32], char vkd3d_version[32] )
{
    char dxvk[32], vkd3d[32];

    if (!g) return 0;
    strcpy( dxvk, dxvk_version );
    strcpy( vkd3d, vkd3d_version );
    if (!ensure_backend( g, machine, 0, dxvk ) || !ensure_backend( g, machine, 1, vkd3d )) return 0;
    strcpy( dxvk_version, dxvk );
    strcpy( vkd3d_version, vkd3d );
    return 1;
}
