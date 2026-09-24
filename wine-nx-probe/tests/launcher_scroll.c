#include <assert.h>
#include <SDL.h>

static Uint32 text_ticks;
static Uint32 test_ticks(void) { return text_ticks; }
#define SDL_GetTicks test_ticks

#include "../source/launcher_ui.c"

static void test_text_clip( const char *font_path )
{
    struct ui ui = { .animations = 1 };
    SDL_Surface *surface;
    SDL_Rect clip = {40, 30, 80, 10}, restored;
    const char *text = "MMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMM";
    Uint32 black;
    int x, y, drawn = 0;

    assert( !SDL_Init( SDL_INIT_VIDEO ) && !TTF_Init() );
    surface = SDL_CreateRGBSurfaceWithFormat( 0, 256, 128, 32, SDL_PIXELFORMAT_ARGB8888 );
    assert( surface );
    ui.renderer = SDL_CreateSoftwareRenderer( surface );
    ui.normal = TTF_OpenFont( font_path, 20 );
    assert( ui.renderer && ui.normal );
    SDL_SetRenderDrawColor( ui.renderer, 0, 0, 0, 255 );
    SDL_RenderClear( ui.renderer );
    SDL_RenderSetClipRect( ui.renderer, &clip );
    ui_text_fit( &ui, ui.normal, 10, 20, 160, text, (SDL_Color){255, 255, 255, 255}, 1 );
    SDL_RenderGetClipRect( ui.renderer, &restored );
    assert( SDL_RenderIsClipEnabled( ui.renderer ) && SDL_RectEquals( &clip, &restored ) );
    SDL_RenderPresent( ui.renderer );
    black = SDL_MapRGBA( surface->format, 0, 0, 0, 255 );
    for (y = 0; y < surface->h; y++)
        for (x = 0; x < surface->w; x++)
        {
            Uint32 pixel = *(Uint32 *)((char *)surface->pixels + y * surface->pitch + x * 4);

            if (x < clip.x || x >= clip.x + clip.w || y < clip.y || y >= clip.y + clip.h)
                assert( pixel == black );
            else drawn += pixel != black;
        }
    assert( drawn );
    ui_text_fit( &ui, ui.normal, 10, 70, 160, text, (SDL_Color){255, 255, 255, 255}, 1 );
    SDL_RenderGetClipRect( ui.renderer, &restored );
    assert( SDL_RenderIsClipEnabled( ui.renderer ) && SDL_RectEquals( &clip, &restored ) );
    SDL_RenderSetClipRect( ui.renderer, NULL );
    ui_text_fit( &ui, ui.normal, 10, 20, 160, text, (SDL_Color){255, 255, 255, 255}, 1 );
    assert( !SDL_RenderIsClipEnabled( ui.renderer ) );
    {
        const char *name = "A runtime name wider than its menu";
        size_t size = surface->h * surface->pitch;
        void *first = malloc( size );
        assert( first && ui_text_width( &ui, ui.normal, name ) > 160 );
        for (int selected = 0; selected <= 1; selected++)
            for (int frame = 0; frame < 2; frame++)
            {
                text_ticks = frame ? 1900 : 0;
                ui.scrolling_text = 0;
                SDL_SetRenderDrawColor( ui.renderer, 0, 0, 0, 255 );
                SDL_RenderClear( ui.renderer );
                ui_text_fit( &ui, ui.normal, 10, 20, 160, name, (SDL_Color){255, 255, 255, 255}, selected );
                SDL_RenderPresent( ui.renderer );
                assert( ui.scrolling_text == selected );
                if (!frame) memcpy( first, surface->pixels, size );
                else assert( !!memcmp( first, surface->pixels, size ) == selected );
            }
        free( first );
    }
    ui_quit( &ui );
    SDL_FreeSurface( surface );
}

int main( int argc, char **argv )
{
    struct ui ui = { .animations = 1 };
    struct ui_list list = {0}, other;
    float before;

    assert( scroll_list( &ui, &list, 20, 5, 100, 1000 ) );
    list.selection = 3;
    assert( scroll_list( &ui, &list, 20, 5, 100, 1100 ) && !list.top );
    list.selection = 4;
    assert( !scroll_list( &ui, &list, 20, 5, 100, 1200 ) );
    assert( list.top == 1 && list.scroll == 0 );
    other = list;
    scroll_list( &ui, &list, 20, 5, 100, 1216 );
    scroll_list( &ui, &list, 20, 5, 100, 1232 );
    scroll_list( &ui, &other, 20, 5, 100, 1232 );
    assert( fabsf( list.scroll - other.scroll ) < 0.01f );
    assert( list.scroll > 0 && list.scroll < 100 && ui.busy_until >= 1264 );
    assert( (450 - LIST_TOP + (int)lroundf( list.scroll )) / 100 == 3 );
    assert( scroll_list( &ui, &list, 20, 5, 100, 1200 + FADE_MS ) && list.scroll == 100 );

    list.selection = 5;
    assert( !scroll_list( &ui, &list, 20, 5, 100, 100000 ) && list.scroll == 100 );
    scroll_list( &ui, &list, 20, 5, 100, 100064 );
    before = list.scroll;
    list.selection = 7;
    assert( !scroll_list( &ui, &list, 20, 5, 100, 100064 ) );
    assert( list.scroll == before && list.top == 4 );
    scroll_list( &ui, &list, 20, 5, 100, 100128 );
    assert( list.scroll > before && list.scroll < 400 );
    list.selection = 0;
    before = list.scroll;
    assert( !scroll_list( &ui, &list, 20, 5, 100, 100128 ) && list.scroll == before );
    assert( scroll_list( &ui, &list, 20, 5, 100, 100128 + FADE_MS ) && list.scroll == 0 );

    list.selection = 8;
    scroll_list( &ui, &list, 9, 5, 100, 101000 );
    assert( scroll_list( &ui, &list, 9, 5, 100, 101000 + FADE_MS ) );
    assert( list.top == 4 && list.scroll == 400 );
    list.selection = 3;
    assert( scroll_list( &ui, &list, 4, 5, 100, 102000 ) );
    assert( !list.top && list.scroll == 0 );

    ui.animations = 0;
    list.selection = 10;
    assert( scroll_list( &ui, &list, 20, 8, 58, 102000 ) );
    assert( list.top == 4 && list.scroll == 4 * 58 );
    ui.animations = 1;
    list = (struct ui_list){0};
    scroll_list( &ui, &list, 20, 5, 100, 0xffffff00u );
    list.selection = 4;
    assert( !scroll_list( &ui, &list, 20, 5, 100, 0xfffffff0u ) );
    assert( scroll_list( &ui, &list, 20, 5, 100, 0x90 ) && list.scroll == 100 );

    list = (struct ui_list){ .selection = 3, .top = 1 };
    assert( scroll_list( &ui, &list, 8, 5, 100, 2000 ) );
    assert( list.top == 1 && list.scroll == 100 );

    list = (struct ui_list){ .selection = 4, .top = 2 };
    assert( scroll_list( &ui, &list, 7, 5, 100, 2100 ) );
    assert( list.top == 2 && list.scroll == 200 );
    assert( LIST_TOP + (list.selection + 1) * SET_ROW_H - list.scroll - 6 + 3 * 48 + 16 <= 720 - 58 );
    assert( scroll_list( &ui, &list, 5, 5, 100, 2200 ) && list.scroll == 0 );

    list = (struct ui_list){ .selection = 7, .top = 4 };
    assert( scroll_list( &ui, &list, 20, 5, 100, 3000 ) );
    list.selection = 5;
    assert( scroll_list( &ui, &list, 20, 5, 100, 3100 ) && list.top == 4 );
    list.selection = 4;
    assert( !scroll_list( &ui, &list, 20, 5, 100, 3200 ) );
    assert( list.top == 3 && list.scroll == 400 );
    scroll_list( &ui, &list, 20, 5, 100, 3232 );
    assert( list.scroll > 300 && list.scroll < 400 );
    assert( scroll_list( &ui, &list, 20, 5, 100, 3200 + FADE_MS ) && list.scroll == 300 );
    list.selection = 1;
    scroll_list( &ui, &list, 20, 5, 100, 3400 );
    assert( scroll_list( &ui, &list, 20, 5, 100, 3400 + FADE_MS ) && !list.top );
    list.selection = 0;
    assert( scroll_list( &ui, &list, 20, 5, 100, 3600 ) && list.scroll == 0 );

    list = (struct ui_list){ .selection = 4, .top = 4 };
    assert( scroll_list( &ui, &list, 20, 2, 48, 4000 ) && list.top == 4 );
    assert( scroll_list( &ui, &list, 20, 2, 48, 4016 ) && list.top == 4 );
    list.selection = 5;
    assert( scroll_list( &ui, &list, 20, 2, 48, 4032 ) && list.top == 4 );
    assert( argc == 2 );
    test_text_clip( argv[1] );
    puts( "launcher scrolling: timing, bounds, lookahead, retargeting, hit positions, clipping and selected runtime names passed" );
    return 0;
}
