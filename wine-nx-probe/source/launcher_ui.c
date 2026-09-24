/*
 * Drawing and input for the launcher (launcher_ui.h).
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __SWITCH__
#include <switch.h>
#endif

#include "launcher_svg.h"
#include "launcher_ui.h"

#define FADE_MS          160
#define REPEAT_DELAY_MS  360
#define REPEAT_MS        85
#define STICK_PRESS      18000
#define STICK_RELEASE    8000
#define LIST_TOP         124
#define ROW_HEIGHT       58
/* What a row keeps between its outline and its text. */
#define ROW_PADDING      20
/* Everything that has the focus wears the same outline. */
#define UI_FOCUS_DIM     (SDL_Color){ 150, 160, 176, 90 }
#define UI_FOCUS_LIT     (SDL_Color){ 244, 247, 250, 255 }

enum glyph
{
    GLYPH_A, GLYPH_B, GLYPH_X, GLYPH_Y, GLYPH_PLUS, GLYPH_MINUS, GLYPH_L, GLYPH_R, GLYPH_LEFT, GLYPH_RIGHT,
    GLYPH_UP, GLYPH_DOWN, GLYPH_ZL, GLYPH_ZR, GLYPH_COUNT
};

static char last_error[256];
static int window_created;

const char *ui_error(void)
{
    return last_error;
}

int ui_screen_used(void)
{
    return window_created;
}

static float clampf( float v, float lo, float hi )
{
    return v < lo ? lo : v > hi ? hi : v;
}

static int platform_running(void)
{
#ifdef __SWITCH__
    return appletMainLoop();
#else
    return 1;
#endif
}

int ui_animated( const struct ui *ui )
{
    return ui->animations;
}

void ui_fill( struct ui *ui, int x, int y, int w, int h, SDL_Color color )
{
    SDL_Rect rect = { x, y, w, h };

    SDL_SetRenderDrawColor( ui->renderer, color.r, color.g, color.b, color.a );
    SDL_RenderFillRect( ui->renderer, &rect );
}

void ui_border( struct ui *ui, int x, int y, int w, int h, int thickness, SDL_Color color )
{
    int i;

    SDL_SetRenderDrawColor( ui->renderer, color.r, color.g, color.b, color.a );
    for (i = 0; i < thickness; i++)
    {
        SDL_Rect rect = { x - i, y - i, w + 2 * i, h + 2 * i };
        SDL_RenderDrawRect( ui->renderer, &rect );
    }
}

/* A pie of a circle from angle a0 to a1 as triangles. */
static void fill_arc( struct ui *ui, float cx, float cy, float radius, float a0, float a1, SDL_Color color )
{
    SDL_Vertex vertices[3 * 32];
    int i, segments = 32;

    for (i = 0; i < segments; i++)
    {
        float s = a0 + (a1 - a0) * i / segments, e = a0 + (a1 - a0) * (i + 1) / segments;
        SDL_Vertex *v = vertices + 3 * i;

        v[0] = (SDL_Vertex){ { cx, cy }, color, { 0, 0 } };
        v[1] = (SDL_Vertex){ { cx + cos( s ) * radius, cy + sin( s ) * radius }, color, { 0, 0 } };
        v[2] = (SDL_Vertex){ { cx + cos( e ) * radius, cy + sin( e ) * radius }, color, { 0, 0 } };
    }
    SDL_RenderGeometry( ui->renderer, NULL, vertices, 3 * segments, NULL, 0 );
}

/* The border tico-nx draws around what has the focus (src/ui/UIStyle.h,
 * DrawAnimatedGradientBorder): a ribbon extruded along the rounded rectangle's
 * perimeter, with a gradient running along it and travelling as time passes.
 * Theirs samples a cyan-to-magenta strip; the launcher has one light and no
 * colours, so the ribbon keeps its own and the band is what brightens it. */
#define UI_BORDER_SEGMENTS 8
#define UI_BORDER_POINTS   (4 * (UI_BORDER_SEGMENTS + 1))

struct border_point { float x, y, nx, ny, dist; };

static void border_arc( struct border_point *points, int *count, float *dist, float cx, float cy,
                        float radius, float from, float to )
{
    int i;

    for (i = 0; i <= UI_BORDER_SEGMENTS; i++)
    {
        float a = from + (to - from) * i / UI_BORDER_SEGMENTS;
        struct border_point *p = points + *count;

        p->nx = cosf( a );
        p->ny = sinf( a );
        p->x = cx + p->nx * radius;
        p->y = cy + p->ny * radius;
        if (*count) *dist += hypotf( p->x - p[-1].x, p->y - p[-1].y );
        p->dist = *dist;
        (*count)++;
    }
}

/* Where along the band a point is, as a colour: dim over most of the ribbon,
 * rising to lit in a narrow stretch that moves. */
static SDL_Color border_tint( SDL_Color dim, SDL_Color lit, float u )
{
    float t = 0.5f + 0.5f * cosf( 2.0f * (float)M_PI * u );
    SDL_Color c;

    t = t * t * t;
    c.r = (Uint8)(dim.r + (lit.r - dim.r) * t);
    c.g = (Uint8)(dim.g + (lit.g - dim.g) * t);
    c.b = (Uint8)(dim.b + (lit.b - dim.b) * t);
    c.a = (Uint8)(dim.a + (lit.a - dim.a) * t);
    return c;
}

/* The same ribbon with nothing travelling round it. */
void ui_outline( struct ui *ui, int x, int y, int w, int h, int radius, int thickness, SDL_Color color )
{
    ui_animated_border( ui, x, y, w, h, radius, thickness, color, color );
}

void ui_animated_border( struct ui *ui, int x, int y, int w, int h, int radius, int thickness,
                         SDL_Color dim, SDL_Color lit )
{
    struct border_point points[UI_BORDER_POINTS];
    SDL_Vertex vertices[UI_BORDER_POINTS * 6];
    float dist = 0, perimeter, half = thickness / 2.0f, phase = 0;
    int count = 0, i;

    if (w <= 0 || h <= 0) return;
    if (radius * 2 > w) radius = w / 2;
    if (radius * 2 > h) radius = h / 2;
    if (radius < 1) radius = 1;
    if (half < 0.5f) half = 0.5f;

    /* Clockwise from the left of the top-left corner. The straight sides need
     * no points of their own: they are the stretch between one arc's last point
     * and the next arc's first, and both carry the side's own normal. */
    border_arc( points, &count, &dist, x + radius, y + radius, radius, (float)M_PI, 1.5f * (float)M_PI );
    border_arc( points, &count, &dist, x + w - radius, y + radius, radius, 1.5f * (float)M_PI, 2.0f * (float)M_PI );
    border_arc( points, &count, &dist, x + w - radius, y + h - radius, radius, 0, 0.5f * (float)M_PI );
    border_arc( points, &count, &dist, x + radius, y + h - radius, radius, 0.5f * (float)M_PI, (float)M_PI );
    perimeter = dist + hypotf( points[0].x - points[count - 1].x, points[0].y - points[count - 1].y );
    if (perimeter <= 0) return;
    if (ui->animations) phase = (SDL_GetTicks() % 2500) / 2500.0f;

    for (i = 0; i < count; i++)
    {
        const struct border_point *a = points + i, *b = points + (i + 1) % count;
        float d2 = i + 1 == count ? perimeter : b->dist;
        SDL_Color c1 = border_tint( dim, lit, fmodf( a->dist / perimeter + phase, 1.0f ) );
        SDL_Color c2 = border_tint( dim, lit, fmodf( d2 / perimeter + phase, 1.0f ) );
        SDL_FPoint a_out = { a->x + a->nx * half, a->y + a->ny * half };
        SDL_FPoint a_in = { a->x - a->nx * half, a->y - a->ny * half };
        SDL_FPoint b_out = { b->x + b->nx * half, b->y + b->ny * half };
        SDL_FPoint b_in = { b->x - b->nx * half, b->y - b->ny * half };
        SDL_Vertex *v = vertices + 6 * i;

        v[0] = (SDL_Vertex){ a_out, c1, { 0, 0 } };
        v[1] = (SDL_Vertex){ b_out, c2, { 0, 0 } };
        v[2] = (SDL_Vertex){ a_in, c1, { 0, 0 } };
        v[3] = (SDL_Vertex){ a_in, c1, { 0, 0 } };
        v[4] = (SDL_Vertex){ b_out, c2, { 0, 0 } };
        v[5] = (SDL_Vertex){ b_in, c2, { 0, 0 } };
    }
    SDL_RenderGeometry( ui->renderer, NULL, vertices, 6 * count, NULL, 0 );
}

void ui_fill_circle( struct ui *ui, float cx, float cy, float radius, SDL_Color color )
{
    fill_arc( ui, cx, cy, radius, 0, 2 * M_PI, color );
}

void ui_rounded( struct ui *ui, int x, int y, int w, int h, int radius, SDL_Color color )
{
    if (radius * 2 > w) radius = w / 2;
    if (radius * 2 > h) radius = h / 2;
    ui_fill( ui, x + radius, y, w - 2 * radius, h, color );
    ui_fill( ui, x, y + radius, radius, h - 2 * radius, color );
    ui_fill( ui, x + w - radius, y + radius, radius, h - 2 * radius, color );
    fill_arc( ui, x + radius, y + radius, radius, M_PI, 1.5 * M_PI, color );
    fill_arc( ui, x + w - radius, y + radius, radius, 1.5 * M_PI, 2 * M_PI, color );
    fill_arc( ui, x + w - radius, y + h - radius, radius, 0, 0.5 * M_PI, color );
    fill_arc( ui, x + radius, y + h - radius, radius, 0.5 * M_PI, M_PI, color );
}

void ui_panel( struct ui *ui, int x, int y, int w, int h )
{
    ui_fill( ui, x, y, w, h, ui->panel );
    ui_border( ui, x, y, w, h, 1, (SDL_Color){ 255, 255, 255, ui_animated( ui ) ? 28 : 16 } );
}

/* A white disc fading out from its centre, tinted and stretched for glows, light and bubbles. */
static SDL_Texture *make_glow( struct ui *ui )
{
    SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormat( 0, 256, 256, 32, SDL_PIXELFORMAT_RGBA32 );
    SDL_Texture *texture = NULL;
    int x, y;

    if (!surface) return NULL;
    SDL_LockSurface( surface );
    for (y = 0; y < 256; y++)
    {
        Uint8 *row = (Uint8 *)surface->pixels + y * surface->pitch;

        for (x = 0; x < 256; x++)
        {
            float dx = (x - 127.5f) / 128, dy = (y - 127.5f) / 128, d = sqrt( dx * dx + dy * dy );
            float strength = d >= 1 ? 0 : 1 - d;

            row[x * 4] = row[x * 4 + 1] = row[x * 4 + 2] = 255;
            row[x * 4 + 3] = 255 * strength * strength;
        }
    }
    SDL_UnlockSurface( surface );
    if ((texture = SDL_CreateTextureFromSurface( ui->renderer, surface )))
        SDL_SetTextureBlendMode( texture, SDL_BLENDMODE_BLEND );
    SDL_FreeSurface( surface );
    return texture;
}

/* A controller button: a light disc (or pill for L and R) with a dark label, as
 * GameHub draws them, three times larger than shown so its edge stays smooth. */
static SDL_Texture *make_glyph( struct ui *ui, const char *label, int pill )
{
    const int scale = 3, base = TTF_FontHeight( ui->small ) + 6;
    int height = base * scale, width = (pill ? base * 8 / 5 : base) * scale;
    SDL_Texture *texture = SDL_CreateTexture( ui->renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET,
                                              width, height );
    const SDL_Color disc = { 242, 244, 246, 255 };
    SDL_Texture *previous = SDL_GetRenderTarget( ui->renderer );
    SDL_Surface *surface;

    if (!texture) return NULL;
    SDL_SetTextureBlendMode( texture, SDL_BLENDMODE_BLEND );
    SDL_SetRenderTarget( ui->renderer, texture );
    SDL_SetRenderDrawColor( ui->renderer, 0, 0, 0, 0 );
    SDL_RenderClear( ui->renderer );
    if (pill) ui_rounded( ui, 0, 0, width, height, height / 2, disc );
    else ui_fill_circle( ui, width / 2.0f, height / 2.0f, height / 2.0f, disc );
    if ((surface = TTF_RenderUTF8_Blended( ui->large, label, (SDL_Color){ 18, 20, 24, 255 } )))
    {
        SDL_Texture *text = SDL_CreateTextureFromSurface( ui->renderer, surface );
        int h = height * 56 / 100, w = surface->w * h / (surface->h ? surface->h : 1);
        SDL_Rect dst = { (width - w) / 2, (height - h) / 2, w, h };

        if (text)
        {
            SDL_RenderCopy( ui->renderer, text, NULL, &dst );
            SDL_DestroyTexture( text );
        }
        SDL_FreeSurface( surface );
    }
    SDL_SetRenderTarget( ui->renderer, previous );
    return texture;
}

static TTF_Font *open_font( const void *data, size_t size, int points )
{
    SDL_RWops *stream = SDL_RWFromConstMem( data, (int)size );

    return stream ? TTF_OpenFontRW( stream, 1, points ) : NULL;
}

/* Each step of bringing the screen up, on the card before it is taken: a console
 * that came back to the launcher four times froze on the fourth inside here, and
 * a log that stops between two of these says which call did not return. */
static void ui_step( const char *what )
{
    extern void wine_nx_runtime_trace( const char *msg ) __attribute__((weak));
    char line[96];

    if (!&wine_nx_runtime_trace) return;
    snprintf( line, sizeof(line), "[LAUNCHER] bringing the screen up: %s", what );
    wine_nx_runtime_trace( line );
}

int ui_init( struct ui *ui, const void *font_data, size_t font_size, int animations )
{
    static const struct { const char *label; int pill; } glyphs[GLYPH_COUNT] =
    {
        [GLYPH_A] = { "A", 0 }, [GLYPH_B] = { "B", 0 }, [GLYPH_X] = { "X", 0 }, [GLYPH_Y] = { "Y", 0 },
        [GLYPH_PLUS] = { "+", 0 }, [GLYPH_MINUS] = { "-", 0 }, [GLYPH_L] = { "L", 1 }, [GLYPH_R] = { "R", 1 },
        [GLYPH_LEFT] = { "<", 0 }, [GLYPH_RIGHT] = { ">", 0 }, [GLYPH_UP] = { "^", 0 }, [GLYPH_DOWN] = { "v", 0 },
        [GLYPH_ZL] = { "ZL", 1 }, [GLYPH_ZR] = { "ZR", 1 },
    };
    Uint32 flags = 0;
    int i;

    memset( ui, 0, sizeof(*ui) );
    ui->width = 1280;
    ui->height = 720;
    ui->animations = animations;
    ui->running = 1;
    ui->highlight = -1;
    ui->background = (SDL_Color){ 9, 12, 13, 255 };
    ui->text = (SDL_Color){ 239, 242, 239, 255 };
    ui->dim = (SDL_Color){ 169, 176, 171, 255 };
    ui->value = (SDL_Color){ 255, 255, 255, 255 };
    ui->selection = (SDL_Color){ 226, 232, 226, 255 };
    ui->panel = (SDL_Color){ 12, 16, 17, 242 };
    ui->card = (SDL_Color){ 27, 33, 32, 245 };
    ui->focus = (SDL_Color){ 42, 49, 46, 252 };
    ui->success = (SDL_Color){ 112, 218, 146, 255 };
    ui->danger = (SDL_Color){ 255, 120, 120, 255 };

    SDL_SetMainReady();
    SDL_SetHint( SDL_HINT_RENDER_SCALE_QUALITY, "linear" );
    SDL_SetHint( SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1" );
    ui_step( "SDL_Init" );
    if (SDL_Init( SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS ))
    {
        snprintf( last_error, sizeof(last_error), "%s", SDL_GetError() );
        return 0;
    }
    ui_step( "TTF_Init" );
    if (TTF_Init()) goto fail;
#ifdef __SWITCH__
    flags = SDL_WINDOW_FULLSCREEN;
#endif
    ui_step( "SDL_CreateWindow" );
    if (!(ui->window = SDL_CreateWindow( "Autorun", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                         ui->width, ui->height, flags ))) goto fail;
    window_created = 1;
    /* SDL's software renderer draws the same, only slower, if the GPU one cannot start. */
    ui_step( "SDL_CreateRenderer" );
    if (!(ui->renderer = SDL_CreateRenderer( ui->window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC )) &&
        !(ui->renderer = SDL_CreateRenderer( ui->window, -1, SDL_RENDERER_SOFTWARE )))
        goto fail;
    SDL_RenderSetLogicalSize( ui->renderer, ui->width, ui->height );
    SDL_SetRenderDrawBlendMode( ui->renderer, SDL_BLENDMODE_BLEND );
    /* The frame is drawn into a texture and copied out at the end of it, so a
     * modal can keep what it opened over and stand on a dimmed copy of it. A
     * renderer that cannot do this simply leaves both NULL, and a modal has a
     * plain background instead. */
    ui->screen = SDL_CreateTexture( ui->renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET,
                                    ui->width, ui->height );
    ui->snapshot = SDL_CreateTexture( ui->renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET,
                                      ui->width, ui->height );
    if (!ui->screen || !ui->snapshot)
    {
        if (ui->screen) SDL_DestroyTexture( ui->screen );
        if (ui->snapshot) SDL_DestroyTexture( ui->snapshot );
        ui->screen = ui->snapshot = NULL;
    }
    ui_step( "fonts" );
    if (!(ui->small = open_font( font_data, font_size, 18 )) || !(ui->normal = open_font( font_data, font_size, 23 )) ||
        !(ui->large = open_font( font_data, font_size, 32 ))) goto fail;
    ui->glow = make_glow( ui );
    for (i = 0; i < GLYPH_COUNT; i++) ui->glyphs[i] = make_glyph( ui, glyphs[i].label, glyphs[i].pill );
    ui_step( "controllers" );
    for (i = 0; i < SDL_NumJoysticks() && !ui->controller; i++)
        if (SDL_IsGameController( i )) ui->controller = SDL_GameControllerOpen( i );
    return 1;

fail:
    snprintf( last_error, sizeof(last_error), "%s", SDL_GetError() );
    ui_quit( ui );
    return 0;
}

void ui_quit( struct ui *ui )
{
    int i;

    for (i = 0; i < UI_TEXT_CACHE; i++)
        if (ui->cache[i].texture) SDL_DestroyTexture( ui->cache[i].texture );
    for (i = 0; i < GLYPH_COUNT; i++)
        if (ui->glyphs[i]) SDL_DestroyTexture( ui->glyphs[i] );
    if (ui->glow) SDL_DestroyTexture( ui->glow );
    if (ui->sheen) SDL_DestroyTexture( ui->sheen );
    if (ui->screen) SDL_DestroyTexture( ui->screen );
    if (ui->snapshot) SDL_DestroyTexture( ui->snapshot );
    if (ui->small) TTF_CloseFont( ui->small );
    if (ui->normal) TTF_CloseFont( ui->normal );
    if (ui->large) TTF_CloseFont( ui->large );
    if (ui->controller) SDL_GameControllerClose( ui->controller );
    if (ui->renderer) SDL_DestroyRenderer( ui->renderer );
    if (ui->window) SDL_DestroyWindow( ui->window );
    if (TTF_WasInit()) TTF_Quit();
    SDL_Quit();
    memset( ui, 0, sizeof(*ui) );
}

/***********************************************************************
 * Backgrounds
 */


void ui_background( struct ui *ui )
{
    SDL_RenderSetClipRect( ui->renderer, NULL );
    SDL_SetRenderDrawColor( ui->renderer, ui->background.r, ui->background.g, ui->background.b, 255 );
    SDL_RenderClear( ui->renderer );
}

void ui_gradient( struct ui *ui, int x, int y, int w, int h, SDL_Color from, SDL_Color to, int horizontal )
{
    SDL_Vertex vertices[4] =
    {
        { { x, y }, from, { 0, 0 } },
        { { x + w, y }, horizontal ? to : from, { 0, 0 } },
        { { x + w, y + h }, to, { 0, 0 } },
        { { x, y + h }, horizontal ? from : to, { 0, 0 } },
    };
    static const int indices[6] = { 0, 1, 2, 0, 2, 3 };

    SDL_RenderGeometry( ui->renderer, NULL, vertices, 4, indices, 6 );
}

/* Draw a texture inside rounded corners by clipping it to rows, which costs no
 * render target and, unlike a fan of translucent triangles, leaves no seams. */
void ui_rounded_texture( struct ui *ui, SDL_Texture *texture, const SDL_Rect *src, SDL_Rect rect, int radius,
                         SDL_Color mod )
{
    int clipped = SDL_RenderIsClipEnabled( ui->renderer ), y;
    SDL_Rect clip;

    if (!texture || rect.w <= 0 || rect.h <= 0) return;
    if (radius * 2 > rect.w) radius = rect.w / 2;
    if (radius * 2 > rect.h) radius = rect.h / 2;
    SDL_RenderGetClipRect( ui->renderer, &clip );
    SDL_SetTextureColorMod( texture, mod.r, mod.g, mod.b );
    SDL_SetTextureAlphaMod( texture, mod.a );
    SDL_SetTextureBlendMode( texture, SDL_BLENDMODE_BLEND );
    SDL_SetTextureScaleMode( texture, SDL_ScaleModeLinear );
    for (y = 0; y < rect.h; )
    {
        int edge = y < radius ? radius - y - 1 : y >= rect.h - radius ? y - (rect.h - radius) : 0;
        int inset = edge ? radius - (int)sqrt( radius * radius - edge * edge ) : 0;
        int rows = y == radius ? rect.h - 2 * radius : 1;
        SDL_Rect band = { rect.x + inset, rect.y + y, rect.w - 2 * inset, rows }, visible;

        if (!clipped || SDL_IntersectRect( &clip, &band, &visible ))
        {
            SDL_RenderSetClipRect( ui->renderer, clipped ? &visible : &band );
            SDL_RenderCopy( ui->renderer, texture, src, &rect );
        }
        y += rows;
    }
    SDL_RenderSetClipRect( ui->renderer, clipped ? &clip : NULL );
}

SDL_Texture *ui_sheen( struct ui *ui )
{
    SDL_Surface *surface;
    int y;

    if (ui->sheen) return ui->sheen;
    if (!(surface = SDL_CreateRGBSurfaceWithFormat( 0, 1, 256, 32, SDL_PIXELFORMAT_RGBA32 ))) return NULL;
    SDL_LockSurface( surface );
    for (y = 0; y < 256; y++)
    {
        Uint8 *row = (Uint8 *)surface->pixels + y * surface->pitch;

        row[0] = row[1] = row[2] = 255;
        row[3] = 255 - y;
    }
    SDL_UnlockSurface( surface );
    if ((ui->sheen = SDL_CreateTextureFromSurface( ui->renderer, surface )))
        SDL_SetTextureBlendMode( ui->sheen, SDL_BLENDMODE_BLEND );
    SDL_FreeSurface( surface );
    return ui->sheen;
}

SDL_Texture *ui_svg_texture( struct ui *ui, const char *d, float view_x, float view_y, float view_w, float view_h,
                             int size )
{
    unsigned char *mask = malloc( (size_t)size * size );
    SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormat( 0, size, size, 32, SDL_PIXELFORMAT_RGBA32 );
    SDL_Texture *texture = NULL;
    int x, y;

    if (mask && surface && svg_path_mask( d, view_x, view_y, view_w, view_h, size, size, mask ))
    {
        SDL_LockSurface( surface );
        for (y = 0; y < size; y++)
        {
            Uint8 *row = (Uint8 *)surface->pixels + y * surface->pitch;

            for (x = 0; x < size; x++)
            {
                row[x * 4] = row[x * 4 + 1] = row[x * 4 + 2] = 255;
                row[x * 4 + 3] = mask[y * size + x];
            }
        }
        SDL_UnlockSurface( surface );
        if ((texture = SDL_CreateTextureFromSurface( ui->renderer, surface )))
        {
            SDL_SetTextureBlendMode( texture, SDL_BLENDMODE_BLEND );
            SDL_SetTextureScaleMode( texture, SDL_ScaleModeLinear );
        }
    }
    if (surface) SDL_FreeSurface( surface );
    free( mask );
    return texture;
}

/***********************************************************************
 * Text
 */

static struct ui_text_entry *text_entry( struct ui *ui, TTF_Font *font, const char *text, SDL_Color color, int any_color )
{
    struct ui_text_entry *entry, *oldest = ui->cache;
    SDL_Surface *surface;
    int i;

    if (strlen( text ) >= UI_TEXT_KEY) return NULL;
    for (i = 0; i < UI_TEXT_CACHE; i++)
    {
        entry = ui->cache + i;
        if (entry->texture && entry->font == font && (any_color || !memcmp( &entry->color, &color, sizeof(color) )) &&
            !strcmp( entry->text, text ))
        {
            entry->use = ++ui->cache_use;
            return entry;
        }
        if (entry->use < oldest->use) oldest = entry;
    }
    if (any_color) return NULL;
    if (!(surface = TTF_RenderUTF8_Blended( font, text, color ))) return NULL;
    if (oldest->texture) SDL_DestroyTexture( oldest->texture );
    oldest->texture = SDL_CreateTextureFromSurface( ui->renderer, surface );
    oldest->width = surface->w;
    oldest->height = surface->h;
    SDL_FreeSurface( surface );
    if (!oldest->texture) return NULL;
    oldest->font = font;
    oldest->color = color;
    strcpy( oldest->text, text );
    oldest->use = ++ui->cache_use;
    return oldest;
}

int ui_text_width( struct ui *ui, TTF_Font *font, const char *text )
{
    struct ui_text_entry *entry;
    int w = 0, h;

    if (!text[0]) return 0;
    if ((entry = text_entry( ui, font, text, ui->text, 1 ))) return entry->width;
    TTF_SizeUTF8( font, text, &w, &h );
    return w;
}

void ui_text( struct ui *ui, TTF_Font *font, int x, int y, const char *text, SDL_Color color )
{
    struct ui_text_entry *entry;
    SDL_Surface *surface;

    if (!text[0]) return;
    if ((entry = text_entry( ui, font, text, color, 0 )))
    {
        SDL_Rect dst = { x, y, entry->width, entry->height };
        SDL_RenderCopy( ui->renderer, entry->texture, NULL, &dst );
        return;
    }
    /* Too long to cache. */
    if ((surface = TTF_RenderUTF8_Blended( font, text, color )))
    {
        SDL_Texture *texture = SDL_CreateTextureFromSurface( ui->renderer, surface );
        SDL_Rect dst = { x, y, surface->w, surface->h };

        if (texture)
        {
            SDL_RenderCopy( ui->renderer, texture, NULL, &dst );
            SDL_DestroyTexture( texture );
        }
        SDL_FreeSurface( surface );
    }
}

void ui_text_opening( struct ui *ui, TTF_Font *font, int x, int y, const char *text, SDL_Color color, float open )
{
    struct ui_text_entry *entry;
    SDL_Rect clip, dst, previous;
    SDL_bool clipped;

    if (open <= 0.01f || !text[0]) return;
    if (open >= 0.99f || !(entry = text_entry( ui, font, text, color, 0 )))
    {
        if (open >= 0.99f) ui_text( ui, font, x, y, text, color );
        return;
    }
    /* Revealed from the left, out of the icon it belongs to, and faded with it:
     * the text stays where it will end up, so what shows is always its start. */
    clipped = SDL_RenderIsClipEnabled( ui->renderer );
    SDL_RenderGetClipRect( ui->renderer, &previous );
    clip = (SDL_Rect){ x, 0, (int)(entry->width * open + 0.5f), ui->height };
    if (clipped && !SDL_IntersectRect( &clip, &previous, &clip )) return;
    dst = (SDL_Rect){ x, y, entry->width, entry->height };
    SDL_RenderSetClipRect( ui->renderer, &clip );
    SDL_SetTextureAlphaMod( entry->texture, (Uint8)(255 * open) );
    SDL_RenderCopy( ui->renderer, entry->texture, NULL, &dst );
    SDL_SetTextureAlphaMod( entry->texture, 255 );
    SDL_RenderSetClipRect( ui->renderer, clipped ? &previous : NULL );
}

void ui_text_centered( struct ui *ui, TTF_Font *font, int cx, int y, const char *text, SDL_Color color )
{
    ui_text( ui, font, cx - ui_text_width( ui, font, text ) / 2, y, text, color );
}

void ui_text_right( struct ui *ui, TTF_Font *font, int right, int y, const char *text, SDL_Color color )
{
    ui_text( ui, font, right - ui_text_width( ui, font, text ), y, text, color );
}

/* Bytes of text that fit in max_width pixels, on a character boundary. */
static size_t fitting_bytes( TTF_Font *font, const char *text, int max_width )
{
    int extent, count;
    size_t bytes = 0;

    if (max_width <= 0 || TTF_MeasureUTF8( font, text, max_width, &extent, &count )) return 0;
    while (text[bytes] && count > 0)
    {
        bytes++;
        while ((text[bytes] & 0xc0) == 0x80) bytes++;
        count--;
    }
    return bytes;
}

static const char *ellipsis( TTF_Font *font )
{
    return TTF_GlyphIsProvided32( font, 0x2026 ) ? "\xe2\x80\xa6" : "...";
}

void ui_text_fit( struct ui *ui, TTF_Font *font, int x, int y, int max_width, const char *text,
                  SDL_Color color, int scroll )
{
    int width = ui_text_width( ui, font, text );
    char cut[UI_TEXT_KEY];
    size_t bytes;

    if (width <= max_width)
    {
        ui_text( ui, font, x, y, text, color );
        return;
    }
    if (scroll)
    {
        /* Out and back over 5 seconds, resting at both ends. */
        float phase = (SDL_GetTicks() % 5000) / 5000.0f, pos = clampf( (phase < 0.5f ? phase : 1 - phase) * 2.6f - 0.15f, 0, 1 );
        SDL_Rect clip = { x, y - 2, max_width, TTF_FontHeight( font ) + 8 };
        SDL_Rect previous, intersection;
        SDL_bool clipped = SDL_RenderIsClipEnabled( ui->renderer );

        if (clipped)
        {
            SDL_RenderGetClipRect( ui->renderer, &previous );
            if (!SDL_IntersectRect( &clip, &previous, &intersection )) return;
            clip = intersection;
        }
        SDL_RenderSetClipRect( ui->renderer, &clip );
        ui_text( ui, font, x - (int)(pos * (width - max_width)), y, text, color );
        SDL_RenderSetClipRect( ui->renderer, clipped ? &previous : NULL );
        ui->scrolling_text = 1;
        return;
    }
    bytes = fitting_bytes( font, text, max_width - ui_text_width( ui, font, ellipsis( font ) ) );
    if (bytes > sizeof(cut) - 4) bytes = sizeof(cut) - 4;
    while (bytes && text[bytes - 1] == ' ') bytes--;
    memcpy( cut, text, bytes );
    strcpy( cut + bytes, ellipsis( font ) );
    ui_text( ui, font, x, y, cut, color );
}

/* Break text into lines at spaces and newlines; the last line that fits is cut
 * with an ellipsis. Returns the number of lines, drawn only when draw is set. */
static int wrap_text( struct ui *ui, TTF_Font *font, int x, int y, int max_width, int max_lines,
                      const char *text, SDL_Color color, int centered, int draw )
{
    int lines = 0, line_height = TTF_FontHeight( font ) + 4;
    char line[UI_TEXT_KEY];

    while (*text && lines < max_lines)
    {
        const char *newline = strchr( text, '\n' );
        size_t len = newline ? (size_t)(newline - text) : strlen( text ), bytes;

        if (len >= sizeof(line)) len = sizeof(line) - 1;
        memcpy( line, text, len );
        line[len] = 0;
        bytes = fitting_bytes( font, line, max_width );
        if (bytes < len && lines + 1 < max_lines)
        {
            size_t space = bytes;

            while (space > 0 && line[space] != ' ') space--;
            if (space > 0) bytes = space;
            if (!bytes)
                do bytes++; while ((line[bytes] & 0xc0) == 0x80);
            line[bytes] = 0;
            text += bytes;
            while (*text == ' ') text++;
        }
        else
        {
            text += len;
            if (*text == '\n') text++;
        }
        if (draw)
        {
            int w = ui_text_width( ui, font, line );

            if (w > max_width) w = max_width;
            ui_text_fit( ui, font, centered ? x - w / 2 : x, y, max_width, line, color, 0 );
        }
        y += line_height;
        lines++;
    }
    return lines;
}

int ui_text_wrapped( struct ui *ui, TTF_Font *font, int x, int y, int max_width, int max_lines,
                     const char *text, SDL_Color color, int centered )
{
    return wrap_text( ui, font, x, y, max_width, max_lines, text, color, centered, 1 );
}

/***********************************************************************
 * Header, footer and overlays
 */

void ui_header( struct ui *ui, const char *title, const char *context )
{
    const int band = UI_HEADER_HEIGHT - 4;
    int title_right;

    ui_fill( ui, 0, 0, ui->width, band, ui->panel );
    if (!ui_animated( ui )) ui_fill( ui, 0, band, ui->width, 2, ui->selection );
    ui_text( ui, ui->normal, 28, (band - TTF_FontHeight( ui->normal )) / 2, "Autorun", ui->value );
    ui_text_centered( ui, ui->large, ui->width / 2, (band - TTF_FontHeight( ui->large )) / 2, title, ui->value );
    title_right = ui->width / 2 + ui_text_width( ui, ui->large, title ) / 2;
    if (context && context[0])
    {
        int max_width = ui->width - 28 - title_right - 30, width = ui_text_width( ui, ui->small, context );

        if (max_width > 40)
            ui_text_fit( ui, ui->small, ui->width - 28 - (width < max_width ? width : max_width),
                         (band - TTF_FontHeight( ui->small )) / 2, max_width, context, ui->dim, 1 );
    }
}

/* The header of a screen one goes back from, as the reference has it: the arrow
 * and the screen's name at the left, the clock and the battery at the right, and
 * nothing in the middle. */
void ui_header_back( struct ui *ui, const char *title, const char *context )
{
    const int cy = UI_HEADER_CENTRE;
    int x = UI_HEADER_MARGIN, i;

    /* The same shading over the top as the shell's, rather than a band of its
     * own: one screen should not look like it has a bar the others do not. */
    ui_gradient( ui, 0, 0, ui->width, 150, (SDL_Color){ 0, 0, 0, 150 }, (SDL_Color){ 0, 0, 0, 0 }, 0 );
    /* The arrow, drawn rather than written, so it needs no glyph of its own. */
    for (i = 0; i < 9; i++)
    {
        ui_rounded( ui, x + i, cy - i, 2, 2, 1, ui->value );
        ui_rounded( ui, x + i, cy + i, 2, 2, 1, ui->value );
    }
    ui_fill( ui, x + 2, cy - 1, 18, 2, ui->value );
    /* Room around it for the light to go round, when the cursor is on it. */
    if (ui->back_focused)
        ui_animated_border( ui, x - 14, cy - 22, 48, 44, 14, 2, UI_FOCUS_DIM, UI_FOCUS_LIT );
    ui_text( ui, ui->normal, x + 50, cy - TTF_FontHeight( ui->normal ) / 2, title, ui->value );
    x += 50 + ui_text_width( ui, ui->normal, title ) + 20;
    if (context && context[0])
    {
        int room = ui->width - 300 - x;

        if (room > 60)
            ui_text_fit( ui, ui->small, x, cy - TTF_FontHeight( ui->small ) / 2, room, context, ui->dim, 0 );
    }
    ui->status_left = ui->width;
    if (ui->header_status) ui->header_status( ui->header_status_data, ui->width - UI_HEADER_MARGIN, cy );
}

static SDL_Texture *button_glyph( struct ui *ui, int button )
{
    switch (button)
    {
    case UI_A: return ui->glyphs[GLYPH_A];
    case UI_B: return ui->glyphs[GLYPH_B];
    case UI_X: return ui->glyphs[GLYPH_X];
    case UI_Y: return ui->glyphs[GLYPH_Y];
    case UI_PLUS: return ui->glyphs[GLYPH_PLUS];
    case UI_MINUS: return ui->glyphs[GLYPH_MINUS];
    case UI_L: return ui->glyphs[GLYPH_L];
    case UI_R: return ui->glyphs[GLYPH_R];
    case UI_ZL: return ui->glyphs[GLYPH_ZL];
    case UI_ZR: return ui->glyphs[GLYPH_ZR];
    case UI_LEFT: return ui->glyphs[GLYPH_LEFT];
    case UI_RIGHT: return ui->glyphs[GLYPH_RIGHT];
    case UI_UP: return ui->glyphs[GLYPH_UP];
    case UI_DOWN: return ui->glyphs[GLYPH_DOWN];
    }
    return NULL;
}

enum hint_align { HINTS_CENTRED, HINTS_RIGHT };

/* Hints on the line through y, centred on x or ending there. */
static void draw_hints( struct ui *ui, const struct ui_hint *hints, int count, int x, int y, enum hint_align align,
                        SDL_Color label_color )
{
    const int glyph_gap = 10, label_gap = 8, pair_gap = 26;
    int i, total = 0, widths[UI_FOOTER_HINTS], heights[UI_FOOTER_HINTS];

    if (count > UI_FOOTER_HINTS) count = UI_FOOTER_HINTS;
    for (i = 0; i < count; i++)
    {
        SDL_Texture *glyph = button_glyph( ui, hints[i].button );

        widths[i] = heights[i] = 0;
        if (glyph)
        {
            SDL_QueryTexture( glyph, NULL, NULL, &widths[i], &heights[i] );
            widths[i] /= 3;
            heights[i] /= 3;
        }
        total += widths[i];
        if (hints[i].label && hints[i].label[0])
            total += (glyph ? label_gap : 0) + ui_text_width( ui, ui->small, hints[i].label ) + pair_gap;
        else total += glyph_gap;
    }
    if (count) total -= hints[count - 1].label && hints[count - 1].label[0] ? pair_gap : glyph_gap;

    x -= align == HINTS_RIGHT ? total : total / 2;
    ui->footer_count = 0;
    for (i = 0; i < count; i++)
    {
        SDL_Texture *glyph = button_glyph( ui, hints[i].button );
        int start = x, h = heights[i] ? heights[i] : TTF_FontHeight( ui->small );

        if (glyph)
        {
            SDL_Rect dst = { x, y - heights[i] / 2, widths[i], heights[i] };
            SDL_RenderCopy( ui->renderer, glyph, NULL, &dst );
            x += widths[i];
        }
        if (hints[i].label && hints[i].label[0])
        {
            if (glyph) x += label_gap;
            ui_text( ui, ui->small, x, y - TTF_FontHeight( ui->small ) / 2, hints[i].label, label_color );
            x += ui_text_width( ui, ui->small, hints[i].label );
        }
        if (hints[i].button != UI_NONE)
        {
            ui->footer_hits[ui->footer_count] = (SDL_Rect){ start - 6, y - h / 2 - 8, x - start + 12, h + 16 };
            ui->footer_buttons[ui->footer_count++] = hints[i].button;
        }
        x += hints[i].label && hints[i].label[0] ? pair_gap : glyph_gap;
    }
}

void ui_footer( struct ui *ui, const struct ui_hint *hints, int count )
{
    draw_hints( ui, hints, count, ui->width / 2, ui->height - 26, HINTS_CENTRED, ui->dim );
}

void ui_hints_right( struct ui *ui, const struct ui_hint *hints, int count, int right, int y )
{
    draw_hints( ui, hints, count, right, y, HINTS_RIGHT, ui->text );
}

void ui_start_screen( struct ui *ui )
{
    ui->fx_start = SDL_GetTicks();
    ui->highlight = -1;
}

void ui_fade( struct ui *ui )
{
    Uint32 elapsed = SDL_GetTicks() - ui->fx_start;

    if (ui->animations && elapsed < FADE_MS)
        ui_fill( ui, 0, 0, ui->width, ui->height, (SDL_Color){ 0, 0, 0, 200 * (FADE_MS - elapsed) / FADE_MS } );
}

float ui_highlight( struct ui *ui, float target_y )
{
    if (!ui->animations || ui->highlight < 0) ui->highlight = target_y;
    else ui->highlight += (target_y - ui->highlight) * 0.30f;
    if (fabs( ui->highlight - target_y ) < 0.5f) ui->highlight = target_y;
    return ui->highlight;
}

void ui_toast( struct ui *ui, const char *text, int milliseconds )
{
    ui->toast_notice = 0;
    snprintf( ui->toast, sizeof(ui->toast), "%s", text );
    ui->toast_since = SDL_GetTicks();
    ui->toast_until = ui->toast_since + milliseconds;
}

void ui_notice( struct ui *ui, const char *text )
{
    ui_toast( ui, text, 4000 );
    ui->toast_notice = 1;
}

static void ui_download_icon( struct ui *ui, int x, int y, SDL_Color color );

void ui_draw_toast( struct ui *ui )
{
    Uint32 now = SDL_GetTicks();
    int w, h, alpha, y;
    float open = 1.0f;
    SDL_Color card, text;

    if (!ui->toast[0] || now >= ui->toast_until) return;
    if (ui->animations)
    {
        open = clampf( (now - ui->toast_since) / 240.0f, 0, 1 );
        if (ui->toast_until - now < 240) open = (ui->toast_until - now) / 240.0f;
        open = open * open * (3 - 2 * open);
    }
    alpha = (int)(255 * open);
    if (ui->toast_notice)
    {
        w = ui_text_width( ui, ui->small, ui->toast ) + 68;
        h = 48;
        int x = ui->width - w - 32;
        y = UI_HEADER_HEIGHT + 14 - (int)(12 * (1 - open));
        ui_rounded( ui, x + 2, y + 4, w, h, 14, (SDL_Color){0,0,0,80 * alpha / 255} );
        ui_rounded( ui, x, y, w, h, 14, (SDL_Color){28,33,37,248 * alpha / 255} );
        ui_outline( ui, x, y, w, h, 14, 1, (SDL_Color){218,228,235,50 * alpha / 255} );
        ui_download_icon( ui, x + 16, y + 14, (SDL_Color){235,240,243,alpha} );
        ui_text( ui, ui->small, x + 46, y + (h - TTF_FontHeight( ui->small )) / 2,
                 ui->toast, (SDL_Color){235,240,243,alpha} );
        return;
    }
    y = ui->height - 78 + (int)(18 * (1 - open));
    /* Between the list panel and the footer. */
    w = ui_text_width( ui, ui->small, ui->toast ) + 40;
    if (w > ui->width - 80) w = ui->width - 80;
    h = TTF_FontHeight( ui->small ) + 12;
    card = ui->selection;
    card.a = 235 * alpha / 255;
    if (ui->selection.r * 3 + ui->selection.g * 6 + ui->selection.b < 1600)
        text = (SDL_Color){ 255, 255, 255, alpha };
    else text = (SDL_Color){ 10, 14, 20, alpha };
    ui_rounded( ui, (ui->width - w) / 2, y, w, h, h / 2, card );
    ui_text_fit( ui, ui->small, (ui->width - w) / 2 + 20, y + 6, w - 40, ui->toast, text, 0 );
}

/***********************************************************************
 * Input and frames
 */

static void queue_event( struct ui *ui, const SDL_Event *event )
{
    if (ui->queued_count < (int)(sizeof(ui->queued) / sizeof(ui->queued[0]))) ui->queued[ui->queued_count++] = *event;
}

static int next_event( struct ui *ui, SDL_Event *event )
{
    if (ui->queued_count)
    {
        *event = ui->queued[0];
        memmove( ui->queued, ui->queued + 1, --ui->queued_count * sizeof(ui->queued[0]) );
        return 1;
    }
    return SDL_PollEvent( event );
}

static void push_button( struct ui *ui, int button )
{
    SDL_Event event;

    memset( &event, 0, sizeof(event) );
    event.type = SDL_CONTROLLERBUTTONDOWN;
    event.cbutton.button = button;
    queue_event( ui, &event );
}

/* Held directions repeat after a delay, from the D-pad or the left stick. */
static void repeat_held( struct ui *ui )
{
    SDL_GameController *c = ui->controller;
    Uint32 now = SDL_GetTicks();
    int direction = 0;

    if (!c) return;
    if (SDL_GameControllerGetButton( c, SDL_CONTROLLER_BUTTON_DPAD_UP ) ||
        SDL_GameControllerGetAxis( c, SDL_CONTROLLER_AXIS_LEFTY ) < -STICK_PRESS) direction = UI_UP;
    else if (SDL_GameControllerGetButton( c, SDL_CONTROLLER_BUTTON_DPAD_DOWN ) ||
             SDL_GameControllerGetAxis( c, SDL_CONTROLLER_AXIS_LEFTY ) > STICK_PRESS) direction = UI_DOWN;
    else if (SDL_GameControllerGetButton( c, SDL_CONTROLLER_BUTTON_DPAD_LEFT ) ||
             SDL_GameControllerGetAxis( c, SDL_CONTROLLER_AXIS_LEFTX ) < -STICK_PRESS) direction = UI_LEFT;
    else if (SDL_GameControllerGetButton( c, SDL_CONTROLLER_BUTTON_DPAD_RIGHT ) ||
             SDL_GameControllerGetAxis( c, SDL_CONTROLLER_AXIS_LEFTX ) > STICK_PRESS) direction = UI_RIGHT;
    if (direction != ui->held)
    {
        ui->held = direction;
        ui->held_since = ui->held_last = now;
        return;
    }
    if (!direction || now - ui->held_since < REPEAT_DELAY_MS || now - ui->held_last < REPEAT_MS) return;
    ui->held_last = now;
    push_button( ui, direction );
}

int ui_begin_frame( struct ui *ui )
{
    if (!ui->running || !platform_running()) return ui->running = 0;
    if (ui->background_tick) ui->background_tick( ui->background_data );
    if (ui->controller && !SDL_GameControllerGetAttached( ui->controller ))
    {
        SDL_GameControllerClose( ui->controller );
        ui->controller = NULL;
        ui->held = ui->stick_x = ui->stick_y = 0;
        ui->trigger_left = ui->trigger_right = 0;
    }
    ui->scrolling_text = 0;
    repeat_held( ui );
    if (ui->screen) SDL_SetRenderTarget( ui->renderer, ui->screen );
    return 1;
}

/* Keep what is on the screen now, for a modal to dim and stand over. */
static void ui_keep_screen( struct ui *ui )
{
    if (!ui->screen || !ui->snapshot) return;
    SDL_SetRenderTarget( ui->renderer, ui->snapshot );
    SDL_RenderCopy( ui->renderer, ui->screen, NULL, NULL );
    SDL_SetRenderTarget( ui->renderer, ui->screen );
}

static enum ui_touch feed_touch( struct ui *ui, int type, float x, float y, int *steps )
{
    Uint32 now = SDL_GetTicks();
    float dx = x - ui->touch.start_x, dy = y - ui->touch.start_y;

    switch (type)
    {
    case SDL_FINGERDOWN:
        ui->touch.active = 1;
        ui->touch.vertical = 0;
        ui->touch.start_x = x;
        ui->touch.start_y = ui->touch.last_y = y;
        ui->touch.started = now;
        break;
    case SDL_FINGERMOTION:
        if (!ui->touch.active) break;
        if (!ui->touch.vertical && fabs( dy ) > 26 && fabs( dy ) > fabs( dx ) * 1.15f) ui->touch.vertical = 1;
        if (ui->touch.vertical && fabs( y - ui->touch.last_y ) >= 30)
        {
            float step = y - ui->touch.last_y;

            *steps = clampf( fabs( step ) / 30, 1, 6 );
            ui->touch.last_y = y;
            return step < 0 ? UI_TOUCH_SCROLL_UP : UI_TOUCH_SCROLL_DOWN;
        }
        break;
    case SDL_FINGERUP:
        if (!ui->touch.active) break;
        ui->touch.active = 0;
        if (ui->touch.vertical)
        {
            float rest = y - ui->touch.last_y;

            if (fabs( rest ) < 18) break;
            *steps = clampf( fabs( rest ) / 30, 1, 6 );
            return rest < 0 ? UI_TOUCH_SCROLL_UP : UI_TOUCH_SCROLL_DOWN;
        }
        if (fabs( dy ) >= 55 && fabs( dy ) > fabs( dx ) * 1.15f)
        {
            *steps = clampf( fabs( dy ) / 30, 1, 6 );
            return dy < 0 ? UI_TOUCH_SCROLL_UP : UI_TOUCH_SCROLL_DOWN;
        }
        if (fabs( dx ) >= 90 && fabs( dx ) > fabs( dy ) * 1.5f) return dx < 0 ? UI_TOUCH_SWIPE_LEFT : UI_TOUCH_SWIPE_RIGHT;
        if (fabs( dx ) <= 26 && fabs( dy ) <= 26 && now - ui->touch.started <= 400) return UI_TOUCH_TAP;
        break;
    }
    return UI_TOUCH_NONE;
}

static int key_button( SDL_Keycode key )
{
    switch (key)
    {
    case SDLK_UP: return UI_UP;
    case SDLK_DOWN: return UI_DOWN;
    case SDLK_LEFT: return UI_LEFT;
    case SDLK_RIGHT: return UI_RIGHT;
    case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_a: return UI_A;
    case SDLK_ESCAPE: case SDLK_BACKSPACE: case SDLK_b: return UI_B;
    case SDLK_x: return UI_X;
    case SDLK_y: return UI_Y;
    case SDLK_PLUS: case SDLK_EQUALS: case SDLK_KP_PLUS: return UI_PLUS;
    case SDLK_MINUS: case SDLK_KP_MINUS: return UI_MINUS;
    case SDLK_PAGEUP: case SDLK_l: return UI_L;
    case SDLK_PAGEDOWN: case SDLK_r: return UI_R;
    case SDLK_LEFTBRACKET: return UI_ZL;
    case SDLK_RIGHTBRACKET: return UI_ZR;
    }
    return UI_NONE;
}

int ui_poll( struct ui *ui, struct ui_input *input )
{
    SDL_Event event;

    while (next_event( ui, &event ))
    {
        int type = event.type, i;
        float x = 0, y = 0;

        memset( input, 0, sizeof(*input) );
        input->button = UI_NONE;
        switch (event.type)
        {
        case SDL_QUIT:
            ui->running = 0;
            continue;
        case SDL_CONTROLLERDEVICEADDED:
            if (!ui->controller && SDL_IsGameController( event.cdevice.which ))
                ui->controller = SDL_GameControllerOpen( event.cdevice.which );
            continue;
        case SDL_CONTROLLERBUTTONDOWN:
            input->button = event.cbutton.button;
            break;
        case SDL_CONTROLLERAXISMOTION:
        {
            int trigger = event.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT ||
                          event.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT;
            int *latch = event.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX ? &ui->stick_x :
                         event.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY ? &ui->stick_y :
                         event.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT ? &ui->trigger_left :
                         event.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT ? &ui->trigger_right : NULL;
            int value = event.caxis.value;

            if (!latch) continue;
            if (value > -STICK_RELEASE && value < STICK_RELEASE) *latch = 0;
            if (*latch || (value > -STICK_PRESS && value < STICK_PRESS)) continue;
            *latch = 1;
            if (trigger) input->button = latch == &ui->trigger_left ? UI_ZL : UI_ZR;
            else if (latch == &ui->stick_x) input->button = value < 0 ? UI_LEFT : UI_RIGHT;
            else input->button = value < 0 ? UI_UP : UI_DOWN;
            break;
        }
        case SDL_KEYDOWN:
            if ((input->button = key_button( event.key.keysym.sym )) == UI_NONE) continue;
            if (event.key.repeat && input->button != UI_UP && input->button != UI_DOWN &&
                input->button != UI_LEFT && input->button != UI_RIGHT) continue;
            break;
        case SDL_FINGERDOWN:
        case SDL_FINGERMOTION:
        case SDL_FINGERUP:
            x = event.tfinger.x * ui->width;
            y = event.tfinger.y * ui->height;
            break;
        /* A mouse stands in for the touch screen; SDL's own copies of touches are skipped. */
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
            if (event.button.which == SDL_TOUCH_MOUSEID || event.button.button != SDL_BUTTON_LEFT) continue;
            type = event.type == SDL_MOUSEBUTTONDOWN ? SDL_FINGERDOWN : SDL_FINGERUP;
            x = event.button.x;
            y = event.button.y;
            break;
        case SDL_MOUSEMOTION:
            if (event.motion.which == SDL_TOUCH_MOUSEID || !(event.motion.state & SDL_BUTTON_LMASK)) continue;
            type = SDL_FINGERMOTION;
            x = event.motion.x;
            y = event.motion.y;
            break;
        default:
            /* A worker has news, which the frame picks up on its own: take the
             * event and go on reading. Stopping here would spend a whole frame
             * on one of them, and a library whose covers are all being decoded
             * posts one an icon: the buttons behind them would wait a second
             * each, which reads as a screen that has stopped listening. */
            continue;
        }
        ui->busy_until = SDL_GetTicks() + 220;
        if (input->button != UI_NONE) return 1;

        input->touch = feed_touch( ui, type, x, y, &input->steps );
        if (input->touch == UI_TOUCH_NONE) continue;
        input->x = x;
        input->y = y;
        if (input->touch == UI_TOUCH_TAP)
            for (i = 0; i < ui->footer_count; i++)
            {
                const SDL_Rect *r = &ui->footer_hits[i];

                if (x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h)
                {
                    input->touch = UI_TOUCH_NONE;
                    input->button = ui->footer_buttons[i];
                    break;
                }
            }
        return 1;
    }
    return 0;
}

void (*ui_present_hook)( SDL_Renderer *renderer );

void ui_present( struct ui *ui )
{
    if (ui->screen)
    {
        SDL_SetRenderTarget( ui->renderer, NULL );
        SDL_RenderCopy( ui->renderer, ui->screen, NULL, NULL );
    }
    if (!ui->hide_overlays) ui_draw_toast( ui );
    if (ui_present_hook) ui_present_hook( ui->renderer );
    SDL_RenderPresent( ui->renderer );
}

static int needs_animation( struct ui *ui )
{
    Uint32 now = SDL_GetTicks();
    int moving = ui->highlight >= 0 && ui->last_highlight >= 0 && fabs( ui->highlight - ui->last_highlight ) > 0.2f;

    ui->last_highlight = ui->highlight;
    return ui_animated( ui ) || (ui->animations && now - ui->fx_start < FADE_MS) || moving ||
           ui->scrolling_text || now < ui->busy_until || ui->held || ui->touch.active ||
           (ui->toast[0] && now < ui->toast_until + 50);
}

/* Wait for the next frame: about 60 per second while something moves, else
 * until an event, checking every 250 ms whether the system wants the launcher closed. */
void ui_wait( struct ui *ui )
{
    SDL_Event event;
    Uint32 now = SDL_GetTicks();

    if (ui->queued_count) return;
    if (!needs_animation( ui ))
    {
        ui->deadline = 0;
        for (;;)
        {
            if (SDL_WaitEventTimeout( &event, 250 ))
            {
                queue_event( ui, &event );
                return;
            }
            if (!platform_running())
            {
                ui->running = 0;
                return;
            }
        }
    }
    if (!ui->deadline || SDL_TICKS_PASSED( now, ui->deadline + 16 )) ui->deadline = now;
    ui->deadline += 16;
    if (!SDL_TICKS_PASSED( now, ui->deadline ) && SDL_WaitEventTimeout( &event, ui->deadline - now ))
        queue_event( ui, &event );
}

/***********************************************************************
 * Dialogs and lists
 */

/* A modal: the screen it opened over, dimmed, with a panel standing on it.
 * Without a copy of that screen there is nothing to dim, and it falls back to
 * the launcher's own background. */
static void draw_card( struct ui *ui, const char *title, const char *heading, const char *text,
                       const struct ui_hint *hints, int hint_count )
{
    const int margin = 34, hints_h = 48;
    int w = ui->width - 2 * UI_HEADER_MARGIN, x, y, h, lines, title_h, text_w;

    if (w > 700) w = 700;
    text_w = w - 2 * margin;
    title_h = TTF_FontHeight( ui->normal );
    lines = wrap_text( ui, ui->small, 0, 0, text_w, 14, text, ui->text, 0, 0 );
    h = margin + title_h + 16 + lines * (TTF_FontHeight( ui->small ) + 4) + hints_h + margin / 2;
    if (heading && strcmp( heading, title )) h += TTF_FontHeight( ui->normal ) + 8;
    x = (ui->width - w) / 2;
    y = (ui->height - h) / 2;
    if (y < 40) y = 40;

    if (ui->snapshot)
    {
        /* The snapshot of a modal already carries the veil that was drawn over
         * the screen behind it, so only the first one dims. */
        SDL_RenderCopy( ui->renderer, ui->snapshot, NULL, NULL );
        if (ui->modal_depth < 2) ui_fill( ui, 0, 0, ui->width, ui->height, (SDL_Color){ 4, 7, 11, 205 } );
    }
    else
    {
        ui_background( ui );
        ui_gradient( ui, 0, 0, ui->width, ui->height, (SDL_Color){ 3, 6, 10, 156 },
                     (SDL_Color){ 3, 6, 10, 20 }, 1 );
    }

    /* The panel: a shadow under it so it reads as standing over the screen, the
     * launcher's own card colour, and the light that goes round what has the
     * focus -- which, while a modal is up, is the modal. */
    ui_rounded( ui, x + 6, y + 10, w, h, 22, (SDL_Color){ 0, 0, 0, 120 } );
    ui_rounded( ui, x, y, w, h, 22, (SDL_Color){ 22, 27, 30, 250 } );
    ui_rounded_texture( ui, ui_sheen( ui ), NULL, (SDL_Rect){ x, y, w, h / 3 }, 22,
                        (SDL_Color){ 255, 255, 255, 14 } );
    /* A modal is not what has the focus in the sense the light means: it is the
     * only thing there is, so its edge stands still. */
    ui_outline( ui, x, y, w, h, 22, 1, (SDL_Color){ 236, 240, 246, 120 } );

    y += margin;
    ui_text_fit( ui, ui->normal, x + margin, y, text_w, title, ui->value, 0 );
    y += title_h + 16;
    if (heading && strcmp( heading, title ))
    {
        ui_text_fit( ui, ui->normal, x + margin, y, text_w, heading, ui->value, 0 );
        y += TTF_FontHeight( ui->normal ) + 8;
    }
    ui_text_wrapped( ui, ui->small, x + margin, y, text_w, 14, text, ui->text, 0 );
    ui_hints_right( ui, hints, hint_count, x + w - margin, y + lines * (TTF_FontHeight( ui->small ) + 4) + 24 );
    ui_fade( ui );
}

/* Which button answered, so a card can offer more than yes and no. */
static int ask_card( struct ui *ui, const char *title, const char *heading, const char *text,
                     const struct ui_hint *hints, int hint_count )
{
    struct ui_input input;
    int answer = UI_B, i;

    ui_keep_screen( ui );
    ui->modal_depth++;
    ui_start_screen( ui );
    while (ui_begin_frame( ui ))
    {
        while (ui_poll( ui, &input ))
        {
            if (input.button == UI_B) goto done;
            for (i = 0; i < hint_count; i++)
                if (hints[i].button == input.button) { answer = input.button; goto done; }
            if (input.touch == UI_TOUCH_TAP && hint_count == 1) { answer = UI_A; goto done; }
        }
        draw_card( ui, title, heading, text, hints, hint_count );
        ui_present( ui );
        ui_wait( ui );
    }
done:
    if (ui->screen && ui->snapshot)
    {
        SDL_SetRenderTarget( ui->renderer, ui->screen );
        SDL_RenderCopy( ui->renderer, ui->snapshot, NULL, NULL );
    }
    ui->modal_depth--;
    return answer;
}

static int run_card( struct ui *ui, const char *title, const char *heading, const char *text,
                     const struct ui_hint *hints, int hint_count )
{
    return ask_card( ui, title, heading, text, hints, hint_count ) == UI_A;
}

int ui_ask( struct ui *ui, const char *title, const char *text, const struct ui_hint *hints, int count )
{
    return ask_card( ui, title, title, text, hints, count );
}

/* A short list inside a modal: the panel of a card, with the items in it and
 * the light round the one in focus. Returns the item chosen, or -1. */
int ui_menu( struct ui *ui, const char *title, const char *const *items, int count, int selection )
{
    static const struct ui_hint hints[] = { { UI_A, "Choose" }, { UI_B, "Close" } };
    const int margin = 28, item_h = 52, hints_h = 34;
    struct ui_input input;
    int w = 420, h, x, y, top, i, chosen = -1;

    if (count <= 0) return -1;
    if (selection < 0 || selection >= count) selection = 0;
    /* Its own height, so the hints stand under the last item rather than on it. */
    h = margin + TTF_FontHeight( ui->normal ) + 14 + count * item_h + 14 + hints_h + margin / 2;
    x = (ui->width - w) / 2;
    y = (ui->height - h) / 2;

    ui_keep_screen( ui );
    ui->modal_depth++;
    ui_start_screen( ui );
    while (ui_begin_frame( ui ))
    {
        while (ui_poll( ui, &input ))
        {
            if (input.button == UI_B || input.button == UI_PLUS) goto done;
            if (input.button == UI_A) { chosen = selection; goto done; }
            if (input.button == UI_UP && selection > 0) selection--;
            if (input.button == UI_DOWN && selection + 1 < count) selection++;
            if (input.touch == UI_TOUCH_TAP)
            {
                int hit = (input.y - (y + margin + TTF_FontHeight( ui->normal ) + 14)) / item_h;

                if (input.x < x || input.x >= x + w || hit < 0 || hit >= count) continue;
                chosen = hit;
                goto done;
            }
        }
        if (ui->snapshot)
        {
            SDL_RenderCopy( ui->renderer, ui->snapshot, NULL, NULL );
            if (ui->modal_depth < 2) ui_fill( ui, 0, 0, ui->width, ui->height, (SDL_Color){ 4, 7, 11, 205 } );
        }
        else ui_background( ui );
        ui_rounded( ui, x + 6, y + 10, w, h, 22, (SDL_Color){ 0, 0, 0, 120 } );
        ui_rounded( ui, x, y, w, h, 22, (SDL_Color){ 22, 27, 30, 250 } );
        ui_rounded_texture( ui, ui_sheen( ui ), NULL, (SDL_Rect){ x, y, w, h / 3 }, 22,
                            (SDL_Color){ 255, 255, 255, 14 } );
        ui_outline( ui, x, y, w, h, 22, 1, (SDL_Color){ 236, 240, 246, 120 } );
        ui_text_fit( ui, ui->normal, x + margin, y + margin, w - 2 * margin, title, ui->value, 0 );
        top = y + margin + TTF_FontHeight( ui->normal ) + 14;
        for (i = 0; i < count; i++)
        {
            int row = top + i * item_h;

            if (i == selection)
                ui_animated_border( ui, x + 14, row + 2, w - 28, item_h - 6, 12, 2, UI_FOCUS_DIM, UI_FOCUS_LIT );
            ui_text_fit( ui, ui->normal, x + 14 + ROW_PADDING, row + (item_h - TTF_FontHeight( ui->normal )) / 2,
                         w - 28 - 2 * ROW_PADDING, items[i], i == selection ? ui->value : ui->text, 0 );
        }
        ui_hints_right( ui, hints, 2, x + w - margin, y + h - margin / 2 - hints_h / 2 );
        ui_fade( ui );
        ui_present( ui );
        ui_wait( ui );
    }
done:
    ui->modal_depth--;
    return chosen;
}

void ui_message( struct ui *ui, const char *title, const char *text )
{
    static const struct ui_hint hints[] = { { UI_A, "OK" } };

    run_card( ui, title, title, text, hints, 1 );
}

int ui_confirm( struct ui *ui, const char *title, const char *text, const char *yes )
{
    const struct ui_hint hints[] = { { UI_A, yes }, { UI_B, "Cancel" } };

    return run_card( ui, title, title, text, hints, 2 );
}

void ui_progress_begin( struct ui *ui )
{
    ui_keep_screen( ui );
    ui->modal_depth++;
    ui_start_screen( ui );
}

void ui_progress_update( struct ui *ui, const char *title, const char *status,
                         unsigned long long current, unsigned long long total )
{
    struct ui_input input;
    char amount[96];
    const int w = 560, h = 204, margin = 34;
    const int x = (ui->width - w) / 2, y = (ui->height - h) / 2;
    const int track_x = x + margin, track_y = y + 126, track_w = w - 2 * margin, track_h = 12;
    int fill = 0;

    if (!ui_begin_frame( ui )) return;
    while (ui_poll( ui, &input ));
    if (ui->snapshot)
    {
        SDL_RenderCopy( ui->renderer, ui->snapshot, NULL, NULL );
        if (ui->modal_depth < 2) ui_fill( ui, 0, 0, ui->width, ui->height, (SDL_Color){ 4, 7, 11, 205 } );
    }
    else ui_background( ui );

    ui_rounded( ui, x + 6, y + 10, w, h, 22, (SDL_Color){ 0, 0, 0, 120 } );
    ui_rounded( ui, x, y, w, h, 22, (SDL_Color){ 22, 27, 30, 250 } );
    ui_rounded_texture( ui, ui_sheen( ui ), NULL, (SDL_Rect){ x, y, w, h / 3 }, 22,
                        (SDL_Color){ 255, 255, 255, 14 } );
    ui_outline( ui, x, y, w, h, 22, 1, (SDL_Color){ 236, 240, 246, 120 } );
    ui_text_fit( ui, ui->normal, x + margin, y + 30, w - 2 * margin, title, ui->value, 0 );
    ui_text_fit( ui, ui->small, x + margin, y + 78, w - 2 * margin, status, ui->text, 0 );

    ui_rounded( ui, track_x, track_y, track_w, track_h, track_h / 2, (SDL_Color){ 55, 62, 70, 255 } );
    if (total)
    {
        double ratio = current < total ? (double)current / total : 1.0;

        fill = (int)(track_w * ratio);
        if (fill > 0 && fill < track_h) fill = track_h;
        if (fill > track_w) fill = track_w;
        if (fill) ui_rounded( ui, track_x, track_y, fill, track_h, track_h / 2, ui->selection );
        snprintf( amount, sizeof(amount), "%llu%%   %.1f / %.1f MiB",
                  (unsigned long long)(ratio * 100.0), current / 1048576.0, total / 1048576.0 );
    }
    else
    {
        const int segment = 96;
        int position = (SDL_GetTicks() / 6) % (track_w + segment) - segment;
        int start = position < 0 ? 0 : position;
        int end = position + segment > track_w ? track_w : position + segment;

        if (end > start) ui_rounded( ui, track_x + start, track_y, end - start, track_h,
                                     track_h / 2, ui->selection );
        snprintf( amount, sizeof(amount), "Please wait..." );
    }
    ui_text_right( ui, ui->small, x + w - margin, y + 154, amount, ui->dim );
    ui_fade( ui );
    ui_present( ui );
}

void ui_progress_end( struct ui *ui )
{
    if (ui->screen && ui->snapshot)
    {
        SDL_SetRenderTarget( ui->renderer, ui->screen );
        SDL_RenderCopy( ui->renderer, ui->snapshot, NULL, NULL );
    }
    if (ui->modal_depth) ui->modal_depth--;
}

static void ui_download_icon( struct ui *ui, int x, int y, SDL_Color color )
{
    int i;

    ui_rounded( ui, x + 8, y, 3, 11, 1, color );
    for (i = 0; i < 6; i++)
    {
        ui_rounded( ui, x + 3 + i, y + 7 + i, 2, 2, 1, color );
        ui_rounded( ui, x + 14 - i, y + 7 + i, 2, 2, 1, color );
    }
    ui_rounded( ui, x + 1, y + 16, 18, 3, 1, color );
}

static void ui_chevron_down( struct ui *ui, int x, int y, SDL_Color color );
static void ui_chevron_up( struct ui *ui, int x, int y, SDL_Color color );

static SDL_Color ui_value_color( const struct ui *ui, const struct ui_row *row, int current )
{
    if (row->value_tone == UI_VALUE_SUCCESS) return ui->success;
    if (row->value_tone == UI_VALUE_DANGER) return ui->danger;
    return current ? ui->value : ui->dim;
}

static int scroll_list( struct ui *ui, struct ui_list *list, int count, int visible, int row_height, Uint32 now )
{
    int margin = visible > 2, max_top = count > visible ? count - visible : 0;
    int target;
    float remaining;

    if (list->selection < list->top + margin) list->top = list->selection - margin;
    if (list->selection >= list->top + visible - margin)
        list->top = list->selection - visible + margin + 1;
    if (list->top > max_top) list->top = max_top;
    if (list->top < 0) list->top = 0;
    target = list->top * row_height;
    list->scroll = clampf( list->scroll, 0, max_top * row_height );
    list->scroll_from = clampf( list->scroll_from, 0, max_top * row_height );
    if (!ui->animations || !list->started_scroll)
    {
        list->scroll = list->scroll_from = list->scroll_target = target;
        list->scroll_since = now;
        list->started_scroll = 1;
        return 1;
    }
    if (list->scroll_target != target)
    {
        list->scroll_from = list->scroll;
        list->scroll_target = target;
        list->scroll_since = now;
    }
    if (now - list->scroll_since >= FADE_MS || fabsf( target - list->scroll_from ) < 0.5f)
    {
        list->scroll = target;
        return 1;
    }
    remaining = 1.0f - (now - list->scroll_since) / (float)FADE_MS;
    list->scroll = target + (list->scroll_from - target) * remaining * remaining * remaining;
    if ((Sint32)(ui->busy_until - now) < 32) ui->busy_until = now + 32;
    return 0;
}

enum ui_action ui_list_run( struct ui *ui, struct ui_list *list, const char *title, const char *context,
                            const struct ui_row *rows, int count, int can_reset )
{
    /* The same width and the same flat surfaces as the settings screens: one
     * language for every list the launcher shows. */
    const int column_x = UI_HEADER_MARGIN;
    const int visible = (ui->height - LIST_TOP - 86) / ROW_HEIGHT;
    struct ui_input input;
    SDL_Rect clip;
    int column_w = 1280 - 2 * UI_HEADER_MARGIN, value_right, first, end, scroll, i;

    if (!list->started)
    {
        ui_start_screen( ui );
        list->started = 1;
    }
    while (ui_begin_frame( ui ))
    {
        const struct ui_row *row;
        struct ui_hint hints[6];
        int hint_count = 0, any_adjustable = 0;
        float bar;

        if (count <= 0) return UI_ACTION_BACK;
        if (list->selection >= count) list->selection = count - 1;
        if (list->selection < 0) list->selection = 0;
        while (ui_poll( ui, &input ))
        {
            int direction = 0;

            row = rows + list->selection;
            switch (input.touch)
            {
            case UI_TOUCH_SCROLL_UP:
            case UI_TOUCH_SCROLL_DOWN:
                list->selection += (input.touch == UI_TOUCH_SCROLL_UP ? 1 : -1) * input.steps;
                if (list->selection >= count) list->selection = count - 1;
                if (list->selection < 0) list->selection = 0;
                continue;
            case UI_TOUCH_SWIPE_LEFT:
            case UI_TOUCH_SWIPE_RIGHT:
                if (!row->disabled && row->adjustable)
                    return input.touch == UI_TOUCH_SWIPE_LEFT ? UI_ACTION_LEFT : UI_ACTION_RIGHT;
                continue;
            case UI_TOUCH_TAP:
            {
                int index = (input.y - LIST_TOP + (int)lroundf( list->scroll )) / ROW_HEIGHT;

                if (input.y < UI_HEADER_HEIGHT) return UI_ACTION_BACK;
                if (input.x < column_x || input.x >= column_x + column_w || input.y < LIST_TOP ||
                    input.y >= LIST_TOP + visible * ROW_HEIGHT || index < 0 || index >= count) continue;
                list->selection = index;
                if (rows[index].disabled) continue;
                if (rows[index].adjustable) return input.x >= column_x + column_w / 2 ? UI_ACTION_RIGHT : UI_ACTION_LEFT;
                return UI_ACTION_CHOOSE;
            }
            default:
                break;
            }
            switch (input.button)
            {
            case UI_UP: direction = -1; break;
            case UI_DOWN: direction = 1; break;
            case UI_L: list->selection = list->selection - visible < 0 ? 0 : list->selection - visible; break;
            case UI_R: list->selection = list->selection + visible >= count ? count - 1 : list->selection + visible; break;
            case UI_LEFT: if (!list->in_header && !row->disabled && row->adjustable) return UI_ACTION_LEFT; break;
            case UI_RIGHT: if (!list->in_header && !row->disabled && row->adjustable) return UI_ACTION_RIGHT; break;
            case UI_A:
                if (list->in_header) return UI_ACTION_BACK;
                if (!row->disabled) return UI_ACTION_CHOOSE;
                break;
            case UI_B: return UI_ACTION_BACK;
            case UI_Y: if (can_reset && !row->disabled && row->adjustable) return UI_ACTION_RESET; break;
            case UI_X:
                if (row->help)
                {
                    ui_message( ui, row->label, row->help );
                    ui_start_screen( ui );
                }
                break;
            }
            /* The ends of the list stop: rolling round to the far end, and
             * scrolling the whole way to get there, is not what a list does.
             * Above the first row is the arrow back. */
            if (direction)
            {
                int next = list->selection + direction;

                if (list->in_header) { if (direction > 0) list->in_header = 0; }
                else
                {
                    while (next >= 0 && next < count && rows[next].disabled) next += direction;
                    if (next >= 0 && next < count) list->selection = next;
                    else if (direction < 0) list->in_header = 1;
                }
            }
        }
        if (!ui->running) break;

        scroll_list( ui, list, count, visible, ROW_HEIGHT, SDL_GetTicks() );
        scroll = (int)lroundf( list->scroll );
        row = rows + list->selection;

        /* Options use the same calm, layered surface as Home: a dark sheet
         * anchored to the left, spacious cards, and one obvious focus target. */
        ui_background( ui );
        if (ui->glow)
        {
            SDL_Rect glow = { column_x + column_w - 80, 54, 360, 360 };
            SDL_SetTextureColorMod( ui->glow, 136, 158, 190 );
            SDL_SetTextureAlphaMod( ui->glow, 46 );
            SDL_RenderCopy( ui->renderer, ui->glow, NULL, &glow );
        }
        ui_gradient( ui, 0, 0, ui->width, ui->height, (SDL_Color){ 3, 6, 10, 156 },
                     (SDL_Color){ 3, 6, 10, 20 }, 1 );
        ui->back_focused = list->in_header;
        ui_header_back( ui, title, context );
        ui->back_focused = 0;
        column_w = ui->width - UI_HEADER_MARGIN - column_x;
        /* A list long enough to scroll gives the bar its room out of its own
         * width, so the bar stays inside the margin rather than past it. */
        if (count > visible) column_w -= 22;
        value_right = column_x + column_w - ROW_PADDING;
        bar = ui_highlight( ui, LIST_TOP + list->selection * ROW_HEIGHT + 2 ) - list->scroll;
        /* A row half in and half out while the list slides is cut, not drawn
         * over the header or the hints. */
        clip = (SDL_Rect){ 0, LIST_TOP - 2, ui->width, visible * ROW_HEIGHT + 4 };
        SDL_RenderSetClipRect( ui->renderer, &clip );
        first = scroll / ROW_HEIGHT;
        end = (scroll + visible * ROW_HEIGHT + ROW_HEIGHT - 1) / ROW_HEIGHT;
        for (i = first; i < count && i < end; i++)
        {
            int y = LIST_TOP + i * ROW_HEIGHT - scroll, current = i == list->selection;
            int text_y = y + (ROW_HEIGHT - TTF_FontHeight( ui->normal )) / 2;
            int icon_w = rows[i].download ? 30 : 0;
            int disclosure_w = rows[i].kind == UI_ROW_DROPDOWN ? 28 : 0;
            int value_w = rows[i].value[0] ? ui_text_width( ui, ui->small, rows[i].value ) : 0;
            int value_max = value_w < column_w / 3 ? value_w : column_w / 3;
            int label_w = value_right - ROW_PADDING - column_x - icon_w - disclosure_w -
                          (value_w ? value_max + 28 : 0);
            SDL_Color color = rows[i].disabled ? ui->dim : rows[i].destructive ? ui->danger : current ? ui->value : ui->text;

            any_adjustable |= rows[i].adjustable && !rows[i].disabled;
            /* Nothing is filled: what has the focus is outlined, so the colour
             * on the screen is the artwork's and the text's. */
            if (current && !list->in_header)
                ui_animated_border( ui, column_x, (int)bar - 1, column_w, ROW_HEIGHT - 6, 12, 2,
                                    UI_FOCUS_DIM, UI_FOCUS_LIT );
            ui_text_fit( ui, ui->normal, column_x + ROW_PADDING, text_y, label_w, rows[i].label, color, current );
            if (value_w)
                ui_text_fit( ui, ui->small, value_right - icon_w - disclosure_w - value_max,
                             text_y + (TTF_FontHeight( ui->normal ) - TTF_FontHeight( ui->small )) / 2,
                             column_w / 3, rows[i].value, ui_value_color( ui, rows + i, current ), current );
            if (rows[i].download)
                ui_download_icon( ui, value_right - disclosure_w - 20, y + (ROW_HEIGHT - 19) / 2,
                                  current ? ui->value : ui->dim );
            if (rows[i].kind == UI_ROW_DROPDOWN)
            {
                SDL_Color arrow = current ? ui->value : ui->dim;

                if (rows[i].on) ui_chevron_up( ui, value_right - 18, y + (ROW_HEIGHT - 9) / 2, arrow );
                else ui_chevron_down( ui, value_right - 18, y + (ROW_HEIGHT - 9) / 2, arrow );
            }
        }
        SDL_RenderSetClipRect( ui->renderer, NULL );
        if (count > visible)
        {
            int track_h = visible * ROW_HEIGHT - 12, thumb = track_h * visible / count;

            if (thumb < 16) thumb = 16;
            ui_rounded( ui, column_x + column_w + 12, LIST_TOP + 4, 5, track_h, 3, (SDL_Color){ 66, 73, 85, 160 } );
            ui_rounded( ui, column_x + column_w + 12,
                        LIST_TOP + 4 + (int)((track_h - thumb) * list->scroll /
                                             ((count - visible) * ROW_HEIGHT)),
                        5, thumb, 3, ui->selection );
        }
        if (list->in_header)
        {
            hints[hint_count++] = (struct ui_hint){ UI_A, "Back" };
            hints[hint_count++] = (struct ui_hint){ UI_DOWN, "List" };
        }
        else
        {
            if (any_adjustable) hints[hint_count++] = (struct ui_hint){ UI_LEFT, NULL };
            if (any_adjustable) hints[hint_count++] = (struct ui_hint){ UI_RIGHT, "Change" };
            if (!row->disabled) hints[hint_count++] = (struct ui_hint){ UI_A, row->adjustable ? "Next" : "Choose" };
            if (row->help) hints[hint_count++] = (struct ui_hint){ UI_X, "Info" };
            if (can_reset && row->adjustable && !row->disabled)
                hints[hint_count++] = (struct ui_hint){ UI_Y, "Default" };
            hints[hint_count++] = (struct ui_hint){ UI_B, "Back" };
        }
        ui_hints_right( ui, hints, hint_count, ui->width - 34, ui->height - 34 );
        ui_fade( ui );
        ui_present( ui );
        ui_wait( ui );
    }
    return UI_ACTION_QUIT;
}

/***********************************************************************
 * Settings
 *
 * The sections stand at the left and the rows of the section in focus fill the
 * rest, each with its name, the line that says what it does, and what it is set
 * to on the right: a switch, a value, or an arrow into a screen of its own.
 */
#define SET_SIDEBAR_X    UI_HEADER_MARGIN
#define SET_SIDEBAR_W    286
#define SET_GROUP_H      56
/* A section stands further in from its outline than a row does: the names are
 * short, and the outline around them is the widest thing on the screen. */
#define SET_SIDEBAR_PAD  26
#define SET_ROW_X        (SET_SIDEBAR_X + SET_SIDEBAR_W + 32)
#define SET_ROW_H        100

static void ui_switch( struct ui *ui, int x, int y, int on, int current, int disabled )
{
    const int w = 56, h = 30;
    SDL_Color track = disabled ? (SDL_Color){ 38, 42, 48, 200 } :
                      on ? ui->selection : (SDL_Color){ 52, 57, 64, 240 };
    SDL_Color knob = on && !disabled ? (SDL_Color){ 18, 22, 28, 255 } :
                     disabled ? (SDL_Color){ 92, 98, 106, 255 } : (SDL_Color){ 226, 230, 236, 255 };

    ui_rounded( ui, x, y, w, h, h / 2, track );
    ui_rounded( ui, on ? x + w - h + 3 : x + 3, y + 3, h - 6, h - 6, (h - 6) / 2, knob );
    (void)current;
}

/* The arrow that says a row opens something of its own. */
/* A chevron, pointing right for direction 1 and left for -1. */
static void ui_chevron_dir( struct ui *ui, int x, int y, int direction, SDL_Color color )
{
    int i;

    for (i = 0; i < 8; i++)
    {
        int cx = direction > 0 ? x + 8 - i : x + i;

        ui_rounded( ui, cx, y + 8 - i, 2, 2, 1, color );
        ui_rounded( ui, cx, y + 8 + i, 2, 2, 1, color );
    }
}

static void ui_chevron( struct ui *ui, int x, int y, SDL_Color color )
{
    ui_chevron_dir( ui, x, y, 1, color );
}

static void ui_chevron_down( struct ui *ui, int x, int y, SDL_Color color )
{
    int i;

    for (i = 0; i < 8; i++)
    {
        ui_rounded( ui, x + i, y + i, 2, 2, 1, color );
        ui_rounded( ui, x + 14 - i, y + i, 2, 2, 1, color );
    }
}

static void ui_chevron_up( struct ui *ui, int x, int y, SDL_Color color )
{
    int i;

    for (i = 0; i < 8; i++)
    {
        ui_rounded( ui, x + i, y + 7 - i, 2, 2, 1, color );
        ui_rounded( ui, x + 14 - i, y + 7 - i, 2, 2, 1, color );
    }
}

static int settings_dropdown( struct ui *ui, const struct ui_list *anchor,
                              const struct ui_row *rows, int count, int selection,
                              int (*update)( void *data, int *selection ), void *data )
{
    const int panel_w = 430, row_h = 48, padding = 8, max_visible = 5;
    struct ui_input input;
    struct ui_list motion = {0};
    SDL_Rect clip;
    int x, y, h, visible, available, scroll, chosen = -1;

    if (update) count = update( data, &selection );
    if (count <= 0) return -1;
    if (selection < 0 || selection >= count) selection = 0;
    x = ui->width - UI_HEADER_MARGIN - ROW_PADDING - panel_w;

    ui_keep_screen( ui );
    ui->modal_depth++;
    while (ui_begin_frame( ui ))
    {
        if (update)
        {
            count = update( data, &selection );
            if (count <= 0) break;
            if (selection < 0) selection = 0;
            if (selection >= count) selection = count - 1;
        }
        visible = count < max_visible ? count : max_visible;
        y = LIST_TOP + (anchor->selection + 1) * SET_ROW_H - (int)lroundf( anchor->scroll ) - 6;
        available = (ui->height - 58 - y - 2 * padding) / row_h;
        if (available > 0 && visible > available) visible = available;
        h = visible * row_h + 2 * padding;
        if (y + h > ui->height - 58) y = ui->height - 58 - h;
        if (y < UI_HEADER_HEIGHT + 8) y = UI_HEADER_HEIGHT + 8;
        if (!motion.started_scroll)
        {
            motion.top = selection - visible / 2;
            motion.selection = selection;
            scroll_list( ui, &motion, count, visible, row_h, SDL_GetTicks() );
        }
        while (ui_poll( ui, &input ))
        {
            int direction = 0;

            if (input.button == UI_B || input.button == UI_PLUS) goto done;
            if (input.button == UI_A && !rows[selection].disabled) { chosen = selection; goto done; }
            if (input.button == UI_UP) direction = -1;
            if (input.button == UI_DOWN) direction = 1;
            if (input.button == UI_L) direction = -visible;
            if (input.button == UI_R) direction = visible;
            if (input.touch == UI_TOUCH_SCROLL_UP) direction = input.steps;
            if (input.touch == UI_TOUCH_SCROLL_DOWN) direction = -input.steps;
            if (input.touch == UI_TOUCH_TAP)
            {
                int hit = (input.y - y - padding + (int)lroundf( motion.scroll )) / row_h;

                if (input.x < x || input.x >= x + panel_w || input.y < y + padding ||
                    input.y >= y + h - padding || hit < 0 || hit >= count)
                    goto done;
                if (rows[hit].disabled) continue;
                chosen = hit;
                goto done;
            }
            if (direction)
            {
                selection += direction;
                if (selection < 0) selection = 0;
                if (selection >= count) selection = count - 1;
            }
        }
        if (!ui->running) break;
        motion.selection = selection;
        scroll_list( ui, &motion, count, visible, row_h, SDL_GetTicks() );
        scroll = (int)lroundf( motion.scroll );

        if (ui->snapshot) SDL_RenderCopy( ui->renderer, ui->snapshot, NULL, NULL );
        else ui_background( ui );
        ui_rounded( ui, x + 6, y + 8, panel_w, h, 14, (SDL_Color){ 0, 0, 0, 135 } );
        ui_rounded( ui, x, y, panel_w, h, 14, (SDL_Color){ 22, 27, 30, 252 } );
        ui_outline( ui, x, y, panel_w, h, 14, 1, (SDL_Color){ 236, 240, 246, 105 } );
        clip = (SDL_Rect){ x + 4, y + padding, panel_w - 8, visible * row_h };
        SDL_RenderSetClipRect( ui->renderer, &clip );
        {
            int i, first = scroll / row_h, end = (scroll + visible * row_h + row_h - 1) / row_h;

            for (i = first; i < count && i < end; i++)
            {
                int row_y = y + padding + i * row_h - scroll;
                int current = i == selection;
                int icon_w = rows[i].download ? 28 : 0;
                int value_w = rows[i].value[0] ? ui_text_width( ui, ui->small, rows[i].value ) : 0;

                if (value_w > panel_w / 3) value_w = panel_w / 3;
                if (current)
                    ui_animated_border( ui, x + 8, row_y + 2, panel_w - 16, row_h - 4, 10, 2,
                                        UI_FOCUS_DIM, UI_FOCUS_LIT );
                ui_text_fit( ui, ui->normal, x + 8 + ROW_PADDING,
                             row_y + (row_h - TTF_FontHeight( ui->normal )) / 2,
                             panel_w - 48 - 2 * ROW_PADDING - icon_w - value_w,
                             rows[i].label, rows[i].disabled ? ui->dim : current ? ui->value : ui->text, current );
                if (value_w)
                    ui_text_fit( ui, ui->small, x + panel_w - 22 - icon_w - value_w,
                                 row_y + (row_h - TTF_FontHeight( ui->small )) / 2,
                                 value_w, rows[i].value, ui_value_color( ui, rows + i, current ), current );
                if (rows[i].download)
                    ui_download_icon( ui, x + panel_w - 38, row_y + (row_h - 19) / 2,
                                      current ? ui->value : ui->dim );
            }
        }
        SDL_RenderSetClipRect( ui->renderer, NULL );
        if (count > visible)
        {
            int track_h = h - 2 * padding - 8;
            int thumb = track_h * visible / count;

            if (thumb < 16) thumb = 16;
            ui_rounded( ui, x + panel_w - 7, y + padding + 4, 3, track_h, 2,
                        (SDL_Color){ 66, 73, 85, 180 } );
            ui_rounded( ui, x + panel_w - 7,
                        y + padding + 4 + (int)((track_h - thumb) * motion.scroll / ((count - visible) * row_h)),
                        3, thumb, 2, ui->selection );
        }
        ui_present( ui );
        ui_wait( ui );
    }
done:
    if (ui->screen && ui->snapshot)
    {
        SDL_SetRenderTarget( ui->renderer, ui->screen );
        SDL_RenderCopy( ui->renderer, ui->snapshot, NULL, NULL );
    }
    ui->modal_depth--;
    return chosen;
}

int ui_settings_dropdown( struct ui *ui, const struct ui_list *anchor,
                          const struct ui_row *rows, int count, int selection )
{
    return settings_dropdown( ui, anchor, rows, count, selection, NULL, NULL );
}

int ui_settings_dropdown_live( struct ui *ui, const struct ui_list *anchor, const struct ui_row *rows,
                               int selection, int (*update)( void *data, int *selection ), void *data )
{
    return settings_dropdown( ui, anchor, rows, 0, selection, update, data );
}

enum ui_action ui_settings_run( struct ui *ui, struct ui_list *list, const char *title, const char *context,
                                const char *const *groups, int group_count,
                                const struct ui_row *rows, int count, int can_reset, int *group )
{
    /* The rows end where the clock and the battery begin, as everything else on
     * the screen does. */
    const int visible = (ui->height - LIST_TOP - 86) / SET_ROW_H;
    int row_w, controls_right, scroll, first, end, settled;
    int index[64], shown, i;
    int deferred = -1;
    struct ui_input input;
    SDL_Rect clip;

    row_w = controls_right = 0;
    if (!list->started)
    {
        ui_start_screen( ui );
        list->started = 1;
    }
    while (ui_begin_frame( ui ))
    {
        const struct ui_row *row;
        struct ui_hint hints[6];
        int hint_count = 0, any_adjustable = 0;

        if (count <= 0) return UI_ACTION_BACK;
        if (!row_w) row_w = ui->width - UI_HEADER_MARGIN - SET_ROW_X;
        if (*group >= group_count) *group = group_count - 1;
        if (*group < 0) *group = 0;
        int current_group = *group;
        /* The rows of this section, in the order they were given. */
        for (i = 0, shown = 0; i < count && shown < (int)(sizeof(index) / sizeof(index[0])); i++)
            if (rows[i].group == *group) index[shown++] = i;
        if (!shown) { *group = (*group + 1) % group_count; continue; }
        if (list->selection >= shown) list->selection = shown - 1;
        if (list->selection < 0) list->selection = 0;

        while (ui_poll( ui, &input ))
        {
            int direction = 0;

            if (deferred >= 0 && input.button == UI_A) continue;
            if (deferred >= 0 && input.button == UI_B) { deferred = -1; continue; }
            deferred = -1;

            row = rows + index[list->selection];
            switch (input.touch)
            {
            case UI_TOUCH_SCROLL_UP:
            case UI_TOUCH_SCROLL_DOWN:
            {
                int direction = input.touch == UI_TOUCH_SCROLL_UP ? 1 : -1;
                int next = list->selection + direction * input.steps;

                if (next >= shown) next = shown - 1;
                if (next < 0) next = 0;
                while (next >= 0 && next < shown && rows[index[next]].disabled) next += direction;
                if (next >= 0 && next < shown) list->selection = next;
                continue;
            }
            case UI_TOUCH_SWIPE_LEFT:
            case UI_TOUCH_SWIPE_RIGHT:
                if (!row->disabled && row->adjustable)
                    return input.touch == UI_TOUCH_SWIPE_LEFT ? UI_ACTION_LEFT : UI_ACTION_RIGHT;
                continue;
            case UI_TOUCH_TAP:
            {
                int in_list = (input.y - LIST_TOP + (int)lroundf( list->scroll )) / SET_ROW_H;

                if (input.y < UI_HEADER_HEIGHT) return UI_ACTION_BACK;
                /* A section under the finger, or a row of the one in focus. */
                if (input.x >= SET_SIDEBAR_X && input.x < SET_SIDEBAR_X + SET_SIDEBAR_W && input.y >= LIST_TOP)
                {
                    int picked = (input.y - LIST_TOP) / SET_GROUP_H;

                    if (picked >= 0 && picked < group_count && picked != *group)
                    {
                        *group = picked;
                        break;
                    }
                    continue;
                }
                if (input.x < SET_ROW_X || input.x >= SET_ROW_X + row_w || input.y < LIST_TOP ||
                    input.y >= LIST_TOP + visible * SET_ROW_H || in_list < 0 || in_list >= shown) continue;
                list->selection = in_list;
                row = rows + index[in_list];
                if (row->disabled) continue;
                if (row->adjustable) return input.x >= SET_ROW_X + row_w / 2 ? UI_ACTION_RIGHT : UI_ACTION_LEFT;
                if (row->kind == UI_ROW_DROPDOWN)
                {
                    list->top = list->selection > 2 ? list->selection - 2 : 0;
                    deferred = UI_ACTION_CHOOSE;
                    break;
                }
                return UI_ACTION_CHOOSE;
            }
            default:
                break;
            }
            if (*group != current_group) break;
            switch (input.button)
            {
            case UI_UP: direction = -1; break;
            case UI_DOWN: direction = 1; break;
            case UI_LEFT:
                /* Holding a value, left changes it; otherwise it is the way back
                 * to the sections. */
                if (list->editing || list->in_header) { if (list->editing) return UI_ACTION_LEFT; break; }
                if (list->in_rows) list->in_rows = 0;
                break;
            case UI_RIGHT:
                if (list->editing) return UI_ACTION_RIGHT;
                if (list->in_header) break;
                if (!list->in_rows && shown) list->in_rows = 1;
                break;
            case UI_A:
                if (list->in_header) return UI_ACTION_BACK;
                if (!list->in_rows) { if (shown) list->in_rows = 1; break; }
                if (row->disabled) break;
                /* A switch turns over where it stands. A row with a value of its
                 * own is taken hold of, and let go of the same way; everything
                 * else simply happens. */
                if (row->kind == UI_ROW_SWITCH) return UI_ACTION_CHOOSE;
                if (row->adjustable) { list->editing = !list->editing; break; }
                if (row->kind == UI_ROW_DROPDOWN)
                {
                    list->top = list->selection > 2 ? list->selection - 2 : 0;
                    deferred = UI_ACTION_CHOOSE;
                    break;
                }
                return UI_ACTION_CHOOSE;
            case UI_B:
                if (list->editing) { list->editing = 0; break; }
                if (!list->in_header && list->in_rows) { list->in_rows = 0; break; }
                return UI_ACTION_BACK;
            case UI_Y:
                if (can_reset && list->in_rows && !row->disabled && row->adjustable) return UI_ACTION_RESET;
                break;
            case UI_X:
                if (list->in_rows && row->help)
                {
                    ui_message( ui, row->label, row->help );
                    ui_start_screen( ui );
                }
                break;
            }
            if (deferred >= 0) break;
            if (direction && !list->editing)
            {
                /* Up out of the top of whichever pane has the focus lands on the
                 * arrow back, and down comes off it again. */
                if (list->in_header)
                {
                    if (direction > 0) list->in_header = 0;
                }
                else if (!list->in_rows)
                {
                    if (direction < 0 && !*group) list->in_header = 1;
                    else if (*group + direction >= 0 && *group + direction < group_count)
                    {
                        *group += direction;
                        break;  /* the section's rows are gathered again next frame */
                    }
                }
                else
                {
                    int next = list->selection + direction;

                    while (next >= 0 && next < shown && rows[index[next]].disabled) next += direction;
                    if (next >= 0 && next < shown) list->selection = next;
                    else if (direction < 0) list->in_header = 1;
                }
            }
        }
        if (!ui->running) break;
        if (*group != current_group)
        {
            list->selection = list->top = list->started_scroll = 0;
            list->scroll = 0;
            deferred = -1;
            continue;
        }
        settled = scroll_list( ui, list, deferred >= 0 && shown < list->top + visible ?
                               list->top + visible : shown, visible, SET_ROW_H, SDL_GetTicks() );
        scroll = (int)lroundf( list->scroll );
        row = rows + index[list->selection];

        ui_background( ui );
        ui_gradient( ui, 0, 0, ui->width, ui->height, (SDL_Color){ 3, 6, 10, 156 },
                     (SDL_Color){ 3, 6, 10, 20 }, 1 );
        ui->back_focused = list->in_header;
        ui_header_back( ui, title, context );
        ui->back_focused = 0;
        /* The rows reach the margin the battery's reading ends at, as the
         * header's own content does. */
        controls_right = ui->width - UI_HEADER_MARGIN;
        row_w = controls_right - SET_ROW_X;
        controls_right -= ROW_PADDING;

        /* The sections. The one in focus is framed; the rest are their name and
         * nothing else. */
        for (i = 0; i < group_count; i++)
        {
            int y = LIST_TOP + i * SET_GROUP_H;
            int text_y = y + (SET_GROUP_H - TTF_FontHeight( ui->normal )) / 2;

            /* Only what has the focus is framed, so there is one highlight on
             * the screen and it is plain which arrows do what. */
            if (i == *group && !list->in_rows && !list->in_header)
                ui_animated_border( ui, SET_SIDEBAR_X, y + 2, SET_SIDEBAR_W, SET_GROUP_H - 8, 12, 2,
                                    UI_FOCUS_DIM, UI_FOCUS_LIT );
            ui_text_fit( ui, ui->normal, SET_SIDEBAR_X + SET_SIDEBAR_PAD, text_y,
                         SET_SIDEBAR_W - 2 * SET_SIDEBAR_PAD, groups[i],
                         i == *group ? ui->value : ui->dim, i == *group );
        }

        clip = (SDL_Rect){ SET_ROW_X - 2, LIST_TOP - 2, row_w + 4, visible * SET_ROW_H + 4 };
        SDL_RenderSetClipRect( ui->renderer, &clip );
        first = scroll / SET_ROW_H;
        end = (scroll + visible * SET_ROW_H + SET_ROW_H - 1) / SET_ROW_H;
        for (i = first; i < shown && i < end; i++)
        {
            const struct ui_row *r = rows + index[i];
            int box_y = LIST_TOP + i * SET_ROW_H - scroll + 4, box_h = SET_ROW_H - 10;
            int current = i == list->selection, middle = box_y + box_h / 2;
            int control_w = r->kind == UI_ROW_SWITCH ? 92 :
                            r->kind == UI_ROW_INFO ? 0 : 44 + (r->download ? 28 : 0);
            int value_w = r->value[0] ? ui_text_width( ui, ui->small, r->value ) : 0;
            int label_w, right, label_h, help_lines, block_h, text_y;
            SDL_Color color = r->disabled ? ui->dim : r->destructive ? ui->danger : current ? ui->value : ui->text;

            if (value_w > row_w / 3) value_w = row_w / 3;
            control_w += value_w ? value_w + 18 : 0;
            label_w = controls_right - ROW_PADDING - SET_ROW_X - control_w;
            right = controls_right;
            any_adjustable |= r->adjustable && !r->disabled;

            /* Nothing is filled: the row in focus is outlined, and held for
             * changing it is outlined brighter, so it is plain that left and
             * right now change it rather than move away from it. */
            /* Held for changing, the ribbon thickens: the same light, plainly
             * around something the arrows now act on. */
            if (current && list->in_rows && !list->in_header)
                ui_animated_border( ui, SET_ROW_X, box_y, row_w, box_h, 12, list->editing ? 4 : 2,
                                    UI_FOCUS_DIM, UI_FOCUS_LIT );

            /* The name and the line under it are one block, standing in the
             * middle of the row however many lines the second one takes. */
            label_h = TTF_FontHeight( ui->normal );
            help_lines = r->help ? wrap_text( ui, ui->small, 0, 0, label_w, 2, r->help, ui->dim, 0, 0 ) : 0;
            block_h = label_h + (help_lines ? 4 + help_lines * (TTF_FontHeight( ui->small ) + 4) - 4 : 0);
            text_y = box_y + (box_h - block_h) / 2;
            ui_text_fit( ui, ui->normal, SET_ROW_X + ROW_PADDING, text_y, label_w, r->label, color, current );
            if (help_lines)
                ui_text_wrapped( ui, ui->small, SET_ROW_X + ROW_PADDING, text_y + label_h + 4, label_w, 2,
                                 r->help, ui->dim, 0 );
            switch (r->kind)
            {
            case UI_ROW_SWITCH:
                ui_switch( ui, right - 56, middle - 15, r->on, current, r->disabled );
                break;
            case UI_ROW_VALUE:
            case UI_ROW_ACTION:
            case UI_ROW_DROPDOWN:
            default:
                if (r->download && !(current && list->editing && r->adjustable))
                    ui_download_icon( ui, right - 46, middle - 10,
                                      r->disabled ? ui->dim : current ? ui->value : ui->dim );
                if (value_w)
                    ui_text_fit( ui, ui->small, right - 26 - (r->download ? 28 : 0) - value_w,
                                 middle - TTF_FontHeight( ui->small ) / 2,
                                 value_w + 4, r->value, ui_value_color( ui, r, current ), current );
                /* The arrow says the row opens something. A row held for
                 * changing puts one on either side of the value instead, which
                 * is what left and right now do. */
                if (current && list->editing && r->adjustable)
                {
                    ui_chevron_dir( ui, right - 42 - value_w, middle - 8, -1, ui->value );
                    ui_chevron_dir( ui, right - 14, middle - 8, 1, ui->value );
                }
                else if (r->kind == UI_ROW_DROPDOWN)
                {
                    SDL_Color arrow = r->disabled ? ui->dim : current ? ui->value : ui->dim;

                    if (r->on || (deferred >= 0 && current))
                        ui_chevron_up( ui, right - 18, middle - 4, arrow );
                    else ui_chevron_down( ui, right - 18, middle - 4, arrow );
                }
                else if (!r->adjustable && r->kind != UI_ROW_INFO)
                    ui_chevron( ui, right - 14, middle - 8, r->disabled ? ui->dim : current ? ui->value : ui->dim );
                break;
            }
        }
        SDL_RenderSetClipRect( ui->renderer, NULL );
        if (shown > visible)
        {
            int track_h = visible * SET_ROW_H - 12, thumb = track_h * visible / shown;

            if (thumb < 16) thumb = 16;
            ui_rounded( ui, SET_ROW_X + row_w + 14, LIST_TOP + 4, 5, track_h, 3, (SDL_Color){ 66, 73, 85, 160 } );
            ui_rounded( ui, SET_ROW_X + row_w + 14,
                        LIST_TOP + 4 + (int)((track_h - thumb) * list->scroll / ((shown - visible) * SET_ROW_H)),
                        5, thumb, 3, ui->selection );
        }
        (void)any_adjustable;
        if (list->in_header)
        {
            hints[hint_count++] = (struct ui_hint){ UI_A, "Back" };
            hints[hint_count++] = (struct ui_hint){ UI_DOWN, "Settings" };
        }
        else if (list->editing)
        {
            hints[hint_count++] = (struct ui_hint){ UI_LEFT, NULL };
            hints[hint_count++] = (struct ui_hint){ UI_RIGHT, "Change" };
            if (can_reset) hints[hint_count++] = (struct ui_hint){ UI_Y, "Default" };
            hints[hint_count++] = (struct ui_hint){ UI_A, "Done" };
        }
        else if (list->in_rows)
        {
            hints[hint_count++] = (struct ui_hint){ UI_LEFT, "Sections" };
            if (!row->disabled)
                hints[hint_count++] = (struct ui_hint){ UI_A, row->kind == UI_ROW_SWITCH ? "Turn over" :
                                                             row->adjustable ? "Change" : "Choose" };
            hints[hint_count++] = (struct ui_hint){ UI_B, "Back" };
        }
        else
        {
            hints[hint_count++] = (struct ui_hint){ UI_RIGHT, "Settings" };
            hints[hint_count++] = (struct ui_hint){ UI_B, "Back" };
        }
        ui_hints_right( ui, hints, hint_count, ui->width - 34, ui->height - 34 );
        if (ui->footer_mark) ui->footer_mark( ui->header_status_data );
        ui_fade( ui );
        ui_present( ui );
        if (deferred >= 0 && settled) return deferred;
        ui_wait( ui );
    }
    return UI_ACTION_QUIT;
}
