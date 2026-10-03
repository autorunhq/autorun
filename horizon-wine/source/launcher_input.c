#include <math.h>
#include "launcher_input.h"
#include "launcher_image.h"
#include "launcher_icons.h"

extern const unsigned char wine_nx_joycons[];
extern const size_t wine_nx_joycons_size;

struct input_art
{
    SDL_Texture *joycons, *pad[24], *mouse[4], *arrow;
};

static void glyph( struct ui *ui, SDL_Texture *texture, SDL_Rect rect, SDL_Color color )
{
    if (!texture) return;
    SDL_SetTextureColorMod( texture, color.r, color.g, color.b );
    SDL_SetTextureAlphaMod( texture, color.a );
    SDL_RenderCopy( ui->renderer, texture, NULL, &rect );
}

static int output_icon( unsigned int code )
{
    static const int face[] = {1, 0, 3, 2};
    return code <= 4 ? face[code - 1] : (int)code - 1;
}

struct keycap { unsigned short code; const char *label; float x, y, w, h; };
static const struct keycap keyboard[] =
{
    {0x1b,"Esc",0,0,1,1}, {0x70,"F1",2,0,1,1}, {0x71,"F2",3,0,1,1}, {0x72,"F3",4,0,1,1},
    {0x73,"F4",5,0,1,1}, {0x74,"F5",6.5,0,1,1}, {0x75,"F6",7.5,0,1,1}, {0x76,"F7",8.5,0,1,1},
    {0x77,"F8",9.5,0,1,1}, {0x78,"F9",11,0,1,1}, {0x79,"F10",12,0,1,1}, {0x7a,"F11",13,0,1,1}, {0x7b,"F12",14,0,1,1},
    {0xc0,"`",0,1.3,1,1}, {0x31,"1",1,1.3,1,1}, {0x32,"2",2,1.3,1,1}, {0x33,"3",3,1.3,1,1},
    {0x34,"4",4,1.3,1,1}, {0x35,"5",5,1.3,1,1}, {0x36,"6",6,1.3,1,1}, {0x37,"7",7,1.3,1,1},
    {0x38,"8",8,1.3,1,1}, {0x39,"9",9,1.3,1,1}, {0x30,"0",10,1.3,1,1}, {0xbd,"-",11,1.3,1,1},
    {0xbb,"=",12,1.3,1,1}, {0x08,"Back",13,1.3,2,1},
    {0x09,"Tab",0,2.3,1.5,1}, {0x51,"Q",1.5,2.3,1,1}, {0x57,"W",2.5,2.3,1,1},
    {0x45,"E",3.5,2.3,1,1}, {0x52,"R",4.5,2.3,1,1}, {0x54,"T",5.5,2.3,1,1},
    {0x59,"Y",6.5,2.3,1,1}, {0x55,"U",7.5,2.3,1,1}, {0x49,"I",8.5,2.3,1,1},
    {0x4f,"O",9.5,2.3,1,1}, {0x50,"P",10.5,2.3,1,1}, {0xdb,"[",11.5,2.3,1,1},
    {0xdd,"]",12.5,2.3,1,1}, {0xdc,"\\",13.5,2.3,1.5,1},
    {0x14,"Caps",0,3.3,1.75,1}, {0x41,"A",1.75,3.3,1,1}, {0x53,"S",2.75,3.3,1,1},
    {0x44,"D",3.75,3.3,1,1}, {0x46,"F",4.75,3.3,1,1}, {0x47,"G",5.75,3.3,1,1},
    {0x48,"H",6.75,3.3,1,1}, {0x4a,"J",7.75,3.3,1,1}, {0x4b,"K",8.75,3.3,1,1},
    {0x4c,"L",9.75,3.3,1,1}, {0xba,";",10.75,3.3,1,1}, {0xde,"'",11.75,3.3,1,1}, {0x0d,"Enter",12.75,3.3,2.25,1},
    {0xa0,"Shift",0,4.3,2.25,1}, {0x5a,"Z",2.25,4.3,1,1}, {0x58,"X",3.25,4.3,1,1},
    {0x43,"C",4.25,4.3,1,1}, {0x56,"V",5.25,4.3,1,1}, {0x42,"B",6.25,4.3,1,1},
    {0x4e,"N",7.25,4.3,1,1}, {0x4d,"M",8.25,4.3,1,1}, {0xbc,",",9.25,4.3,1,1},
    {0xbe,".",10.25,4.3,1,1}, {0xbf,"/",11.25,4.3,1,1}, {0xa1,"Shift",12.25,4.3,2.75,1},
    {0xa2,"Ctrl",0,5.3,1.5,1}, {0x5b,"Win",1.5,5.3,1.25,1}, {0xa4,"Alt",2.75,5.3,1.25,1},
    {0x20,"Space",4,5.3,6,1}, {0xa5,"Alt",10,5.3,1.25,1}, {0x5c,"Win",11.25,5.3,1.25,1},
    {0x5d,"Menu",12.5,5.3,1.25,1}, {0xa3,"Ctrl",13.75,5.3,1.25,1},
    {0x2c,"Prt",15.5,0,1,1}, {0x91,"Scr",16.5,0,1,1}, {0x13,"Pse",17.5,0,1,1},
    {0x2d,"Ins",15.5,1.3,1,1}, {0x24,"Home",16.5,1.3,1,1}, {0x21,"PgUp",17.5,1.3,1,1},
    {0x2e,"Del",15.5,2.3,1,1}, {0x23,"End",16.5,2.3,1,1}, {0x22,"PgDn",17.5,2.3,1,1},
    {0x26,"Up",16.5,4.3,1,1}, {0x25,"Left",15.5,5.3,1,1}, {0x28,"Down",16.5,5.3,1,1}, {0x27,"Right",17.5,5.3,1,1},
    {0x90,"Num",19,1.3,1,1}, {0x6f,"/",20,1.3,1,1}, {0x6a,"*",21,1.3,1,1}, {0x6d,"-",22,1.3,1,1},
    {0x67,"7",19,2.3,1,1}, {0x68,"8",20,2.3,1,1}, {0x69,"9",21,2.3,1,1}, {0x6b,"+",22,2.3,1,2},
    {0x64,"4",19,3.3,1,1}, {0x65,"5",20,3.3,1,1}, {0x66,"6",21,3.3,1,1},
    {0x61,"1",19,4.3,1,1}, {0x62,"2",20,4.3,1,1}, {0x63,"3",21,4.3,1,1},
    {0x60,"0",19,5.3,2,1}, {0x6e,".",21,5.3,1,1},
    {INPUT_NUMPAD_ENTER,"Ent",22,4.3,1,2},
};
#define KEYCAP_COUNT ((int)(sizeof(keyboard) / sizeof(keyboard[0])))
static const unsigned short mouse_codes[] =
    { INPUT_MOUSE_LEFT, INPUT_MOUSE_RIGHT, INPUT_MOUSE_MIDDLE,
      INPUT_MOUSE_UP, INPUT_MOUSE_DOWN, INPUT_MOUSE_MOVE_LEFT, INPUT_MOUSE_MOVE_RIGHT,
      INPUT_WHEEL_UP, INPUT_WHEEL_DOWN, INPUT_WHEEL_LEFT, INPUT_WHEEL_RIGHT };
#define MOUSE_TARGET_COUNT ((int)(sizeof(mouse_codes) / sizeof(mouse_codes[0])))

static int has_code( const struct input_binding *binding, unsigned int code )
{
    for (unsigned int i = 0; i < binding->count; i++) if (binding->code[i] == code) return 1;
    return 0;
}

static int inside( SDL_Rect r, int x, int y )
{
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

static void cap( struct ui *ui, SDL_Rect r, const char *label, int active, int selected, int small )
{
    ui_rounded( ui, r.x, r.y, r.w, r.h, 6, active ? ui->selection : ui->card );
    if (selected) ui_outline( ui, r.x - 2, r.y - 2, r.w + 4, r.h + 4, 8, 2, ui->focus );
    TTF_Font *font = small ? ui->small : ui->normal;
    int width = ui_text_width( ui, font, label ), height = TTF_FontHeight( font );
    float scale = fminf( 1, fminf( (r.w - 5.0f) / (width ? width : 1), (r.h - 4.0f) / height ) );
    float sx, sy;
    SDL_RenderGetScale( ui->renderer, &sx, &sy );
    SDL_RenderSetScale( ui->renderer, sx * scale, sy * scale );
    ui_text( ui, font, (r.x + (r.w - width * scale) / 2) / scale,
             (r.y + (r.h - height * scale) / 2) / scale, label, active ? ui->background : ui->text );
    SDL_RenderSetScale( ui->renderer, sx, sy );
}

static SDL_Texture *load_joycons( struct ui *ui )
{
    struct launcher_icon icon = {0};
    SDL_Texture *texture = NULL;
    SDL_Surface *surface;
    if (!launcher_image_decode( wine_nx_joycons, wine_nx_joycons_size, &icon )) return NULL;
    surface = SDL_CreateRGBSurfaceWithFormatFrom( icon.data, icon.width, icon.height, 32,
                                                 icon.width * 4, SDL_PIXELFORMAT_RGBA32 );
    if (surface)
    {
        texture = SDL_CreateTextureFromSurface( ui->renderer, surface );
        SDL_FreeSurface( surface );
    }
    launcher_icon_free( &icon );
    if (texture) SDL_SetTextureScaleMode( texture, SDL_ScaleModeLinear );
    return texture;
}

static void load_art( struct ui *ui, struct input_art *art, int controller )
{
    static const struct launcher_svg_icon *const pad[] =
    {
        &icon_input_a, &icon_input_b, &icon_input_x, &icon_input_y,
        &icon_input_l, &icon_input_r, &icon_input_zl, &icon_input_zr,
        &icon_input_plus, &icon_input_minus, &icon_input_left_stick_click, &icon_input_right_stick_click,
        &icon_input_joycon_dpad_up, &icon_input_joycon_dpad_down, &icon_input_joycon_dpad_left, &icon_input_joycon_dpad_right,
        &icon_input_left_stick_up, &icon_input_left_stick_down, &icon_input_left_stick_left, &icon_input_left_stick_right,
        &icon_input_right_stick_up, &icon_input_right_stick_down, &icon_input_right_stick_left, &icon_input_right_stick_right
    };
    static const struct launcher_svg_icon *const mouse[] =
        { &icon_input_mouse, &icon_input_mouse_left, &icon_input_mouse_right, &icon_input_mouse_middle };
    memset( art, 0, sizeof(*art) );
    if (controller) art->joycons = load_joycons( ui );
    for (unsigned int i = 0; i < 24; i++)
    {
        int inset = i < 4 ? 96 : i < 10 ? 48 : 0;
        art->pad[i] = ui_svg_texture( ui, pad[i]->d, inset, inset, 512 - inset*2, 512 - inset*2, 96 );
    }
    if (!controller)
    {
        for (unsigned int i = 0; i < 4; i++)
            art->mouse[i] = ui_svg_texture( ui, mouse[i]->d, 0, 0, 160, 240, 320 );
        art->arrow = ui_svg_texture( ui, icon_input_wheel_arrow.d, 0, 0, 24, 24, 48 );
    }
}

static void free_art( struct input_art *art )
{
    if (art->joycons) SDL_DestroyTexture( art->joycons );
    for (unsigned int i = 0; i < 24; i++) if (art->pad[i]) SDL_DestroyTexture( art->pad[i] );
    for (unsigned int i = 0; i < 4; i++) if (art->mouse[i]) SDL_DestroyTexture( art->mouse[i] );
    if (art->arrow) SDL_DestroyTexture( art->arrow );
}

static void joycons( struct ui *ui, const struct input_art *art, int x, int y, float scale,
                     int source, SDL_Rect hits[16] )
{
    static const SDL_Rect parts[] = { {28,204,168,457}, {1084,204,168,457} };
    static const SDL_Rect places[] = { {18,67,103,280}, {242,67,103,280} };
    static const SDL_Rect buttons[] =
    {
        {305,131,22,22}, {284,151,22,22}, {284,110,22,22}, {263,131,22,22},
        {18,13,46,42}, {242,13,46,42}, {75,13,46,42}, {299,13,46,42},
        {260,91,18,18}, {86,92,18,14}, {45,121,44,44}, {273,196,44,44},
        {56,189,22,22}, {56,231,22,22}, {35,209,22,22}, {77,209,22,22}
    };
    int selected = source >= 16 && source < 20 ? 10 : source >= 20 && source < 24 ? 11 : source;
    if (!art->joycons) return;
    for (unsigned int i = 0; i < sizeof(parts)/sizeof(parts[0]); i++)
    {
        SDL_Rect dest = {x + places[i].x*scale, y + places[i].y*scale, places[i].w*scale, places[i].h*scale};
        SDL_RenderCopy( ui->renderer, art->joycons, &parts[i], &dest );
    }
    for (int i = 0; i < 16; i++)
    {
        const SDL_Rect *r = buttons + i;
        hits[i] = (SDL_Rect){x + r->x*scale, y + r->y*scale, r->w*scale, r->h*scale};
        if (i >= 4 && i <= 7)
            glyph( ui, art->pad[i], hits[i], i == selected ? ui->value : ui->dim );
        if (i == selected)
            ui_outline( ui, hits[i].x - 4, hits[i].y - 4, hits[i].w + 8, hits[i].h + 8,
                        i >= 4 && i <= 9 ? 6 : hits[i].w, 2, ui->selection );
    }
    if (source >= 16 && source < 24)
    {
        SDL_Rect r = hits[selected];
        int direction = (source - 16) % 4, cx = r.x + r.w/2, cy = r.y + r.h/2;
        cx += direction == 2 ? -r.w/3 : direction == 3 ? r.w/3 : 0;
        cy += direction == 0 ? -r.h/3 : direction == 1 ? r.h/3 : 0;
        ui_fill_circle( ui, cx, cy, 4, ui->selection );
    }
}

static void key_rects( SDL_Rect *rects, int x, int y, float unit )
{
    for (int i = 0; i < KEYCAP_COUNT; i++)
        rects[i] = (SDL_Rect){ x + keyboard[i].x*unit, y + keyboard[i].y*unit,
                              keyboard[i].w*unit - 3, keyboard[i].h*unit - 3 };
}

static void keyboard_draw( struct ui *ui, SDL_Rect *rects, const struct input_binding *binding, int selected )
{
    for (int i = 0; i < KEYCAP_COUNT; i++)
    {
        unsigned int code = keyboard[i].code;
        int active = has_code( binding, code );
        if (code == 0xa0 || code == 0xa2 || code == 0xa4) active |= has_code( binding, 0x10 + (code - 0xa0)/2 );
        cap( ui, rects[i], keyboard[i].label, active, selected == i, 1 );
    }
}

static void mouse_rects( SDL_Rect *r, int x, int y, int width )
{
    static const SDL_Rect parts[] =
        { {22,17,47,94}, {91,17,47,94}, {69,42,22,38},
          {69,132,22,22}, {69,190,22,22}, {40,161,22,22}, {98,161,22,22},
          {34,244,22,22}, {62,244,22,22}, {90,244,22,22}, {118,244,22,22} };
    float scale = width / 160.0f;
    for (int i = 0; i < MOUSE_TARGET_COUNT; i++)
        r[i] = (SDL_Rect){ x + parts[i].x*scale, y + parts[i].y*scale, parts[i].w*scale, parts[i].h*scale };
    r[MOUSE_TARGET_COUNT] = (SDL_Rect){x - width/4, y, width*3/2, width*3/2};
}

static void mouse_draw( struct ui *ui, const struct input_art *art, SDL_Rect *rects,
                        const struct input_binding *binding, int selected )
{
    static const int angles[] = {0, 180, 270, 90};
    glyph( ui, art->mouse[0], rects[MOUSE_TARGET_COUNT], ui->dim );
    for (int i = 0; i < MOUSE_TARGET_COUNT; i++)
    {
        int active = has_code(binding, mouse_codes[i]);
        SDL_Color color = active ? ui->selection : selected == i ? ui->dim : ui->card;
        if (i < 3) glyph( ui, art->mouse[i + 1], rects[MOUSE_TARGET_COUNT], color );
        else if (art->arrow)
        {
            if (active) ui_rounded( ui, rects[i].x - 2, rects[i].y - 2, rects[i].w + 4, rects[i].h + 4, 5, ui->selection );
            if (selected == i) ui_outline( ui, rects[i].x - 3, rects[i].y - 3, rects[i].w + 6, rects[i].h + 6, 5, 2, ui->dim );
            color = active ? ui->background : ui->dim;
            SDL_SetTextureColorMod( art->arrow, color.r, color.g, color.b );
            SDL_RenderCopyEx( ui->renderer, art->arrow, NULL, rects + i, angles[(i - 3)%4], NULL, SDL_FLIP_NONE );
        }
    }
    ui_text( ui, ui->small, rects[7].x - 66, rects[7].y, "Scroll", ui->dim );
}

static int neighbour( const SDL_Rect *rects, int count, int selected, int button )
{
    float x = rects[selected].x + rects[selected].w/2.0f, y = rects[selected].y + rects[selected].h/2.0f;
    float best = 1e30f;
    int next = selected;
    for (int i = 0; i < count; i++)
    {
        float dx = rects[i].x + rects[i].w/2.0f - x, dy = rects[i].y + rects[i].h/2.0f - y;
        float along = button == UI_LEFT ? -dx : button == UI_RIGHT ? dx : button == UI_UP ? -dy : dy;
        float across = button == UI_LEFT || button == UI_RIGHT ? fabsf(dy) : fabsf(dx);
        if (along < 2) continue;
        float score = along + across*4;
        if (score < best) { best = score; next = i; }
    }
    return next;
}

static int binding_dialog( struct ui *ui, const struct input_art *art, int source, int controller, struct input_binding *binding )
{
    struct input_binding draft = *binding;
    struct ui_input input;
    SDL_Rect targets[KEYCAP_COUNT + MOUSE_TARGET_COUNT + 1], hits[16];
    int count = controller ? INPUT_PAD_TARGET_COUNT : KEYCAP_COUNT + MOUSE_TARGET_COUNT, selected = 0, result = 0;
    const SDL_Rect panel = {48,108,1184,512}, apply = {1040,555,156,43};
    static const struct ui_hint hints[] = { {UI_A,"Add / remove"}, {UI_X,"Clear"}, {UI_PLUS,"Apply"}, {UI_B,"Cancel"} };
    if (controller)
        for (int i = 0; i < count; i++) targets[i] = (SDL_Rect){ 465 + (i%6)*121, 250 + (i/6)*69, 112, 60 };
    else
    {
        for (unsigned int i = 0; i < draft.count; i++)
            if (draft.code[i] >= 0x10 && draft.code[i] <= 0x12)
                draft.code[i] = 0xa0 + (draft.code[i] - 0x10)*2;
        key_rects( targets, 76, 264, 39 );
        mouse_rects( targets + KEYCAP_COUNT, 1032, 271, 151 );
    }
    ui_start_screen( ui );
    while (ui_begin_frame( ui ))
    {
        while (ui_poll( ui, &input ))
        {
            int toggle = input.button == UI_A;
            if (input.button == UI_B) goto done;
            if (input.button == UI_PLUS) { result = 1; goto done; }
            if (input.button == UI_X) { draft.count = 0; ui_sound( ui, LAUNCHER_SOUND_BACK ); }
            if (input.button == UI_LEFT || input.button == UI_RIGHT || input.button == UI_UP || input.button == UI_DOWN)
            {
                int next = neighbour( targets, count, selected, input.button );
                if (next != selected) ui_sound( ui, LAUNCHER_SOUND_MOVE );
                selected = next;
            }
            if (input.touch == UI_TOUCH_TAP)
            {
                if (inside( apply, input.x, input.y )) { result = 1; goto done; }
                for (int i = 0; i < count; i++) if (inside( targets[i], input.x, input.y )) { selected = i; toggle = 1; }
            }
            if (toggle)
            {
                unsigned int code = controller ? selected + 1 : selected < KEYCAP_COUNT ? keyboard[selected].code :
                                                     mouse_codes[selected - KEYCAP_COUNT];
                unsigned int i;
                for (i = 0; i < draft.count; i++) if (draft.code[i] == code) break;
                if (i < draft.count)
                {
                    memmove( draft.code + i, draft.code + i + 1, (--draft.count - i)*sizeof(draft.code[0]) );
                    ui_sound( ui, LAUNCHER_SOUND_BACK );
                }
                else if (draft.count < INPUT_CHORD_MAX)
                {
                    draft.code[draft.count++] = code;
                    ui_sound( ui, LAUNCHER_SOUND_ACCEPT );
                }
                else ui_toast( ui, "A combination can contain up to four inputs", 1800 );
            }
        }
        ui_background( ui );
        ui_rounded( ui, panel.x, panel.y, panel.w, panel.h, 24, ui->panel );
        char title[128], label[192];
        snprintf( title, sizeof(title), "Map %s", input_sources[source].label );
        ui_text( ui, ui->large, 76, 127, title, ui->value );
        input_binding_label( &draft, controller, label, sizeof(label) );
        ui_rounded( ui, 76, 205, 1108, 41, 10, ui->background );
        ui_text_fit( ui, ui->normal, 90, 210, 1080, label, ui->value, 1 );
        if (controller)
        {
            joycons( ui, art, 118, 266, .75f, source, hits );
            for (int i = 0; i < count; i++)
            {
                SDL_Rect r = targets[i];
                int active = has_code(&draft, i + 1);
                ui_rounded( ui, r.x, r.y, r.w, r.h, 9, active ? ui->selection : ui->card );
                if (i == selected) ui_outline( ui, r.x - 2, r.y - 2, r.w + 4, r.h + 4, 10, 2, ui->dim );
                r.x += (r.w - 48)/2; r.y += (r.h - 48)/2; r.w = r.h = 48;
                glyph( ui, art->pad[output_icon(i + 1)], r, active ? ui->background : ui->text );
            }
        }
        else
        {
            keyboard_draw( ui, targets, &draft, selected );
            mouse_draw( ui, art, targets + KEYCAP_COUNT, &draft, selected - KEYCAP_COUNT );
        }
        cap( ui, apply, "Apply", 1, 0, 0 );
        ui_footer( ui, hints, sizeof(hints)/sizeof(hints[0]) );
        ui_fade( ui ); ui_present( ui ); ui_wait( ui );
    }
done:
    if (result) *binding = draft;
    ui_start_screen( ui );
    return result;
}

static int load_profile( const char *root, const char *program, int auto_keyboard,
                         struct input_profile *profile, struct launcher_kv *kv, char *path, size_t size )
{
    return program ? input_profile_program( root, program, auto_keyboard, profile, kv, path, size ) :
                     input_profile_global( root, auto_keyboard, profile, kv, path, size );
}

void launcher_input_remap( struct ui *ui, const char *root, const char *program, const char *title,
                           int keyboard_auto, int controller )
{
    struct input_profile profile;
    struct launcher_kv kv;
    struct ui_input input;
    char path[768];
    int selected = 0, top = 0, count = INPUT_SOURCE_COUNT;
    float scroll = 0;
    Uint32 previous = SDL_GetTicks();
    SDL_Rect joy_hits[16] = {{0}}, key_hits[KEYCAP_COUNT], mouse_hits[MOUSE_TARGET_COUNT + 1];
    struct input_art art;
    static const struct ui_hint hints[] = { {UI_A,"Change"}, {UI_X,"Clear"}, {UI_Y,"Reset"}, {UI_B,"Back"} };
    load_art( ui, &art, controller );
    key_rects( key_hits, 69, 207, 20 );
    mouse_rects( mouse_hits, 359, 355, 137 );
    ui_start_screen( ui );
    for (;;)
    {
        if (!load_profile(root, program, keyboard_auto, &profile, &kv, path, sizeof(path)))
        { ui_message(ui, "Controls", "The control settings could not be read."); break; }
        int edit = 0, reset = 0, clear = 0;
        while (ui_begin_frame( ui ))
        {
            while (ui_poll( ui, &input ))
            {
                int old = selected;
                if (input.button == UI_B || input.button == UI_PLUS) goto done;
                if (input.button == UI_UP) selected--;
                if (input.button == UI_DOWN) selected++;
                if (input.button == UI_L) selected -= 8;
                if (input.button == UI_R) selected += 8;
                if (input.touch == UI_TOUCH_SCROLL_UP) selected += input.steps;
                if (input.touch == UI_TOUCH_SCROLL_DOWN) selected -= input.steps;
                selected = selected < 0 ? 0 : selected >= count ? count - 1 : selected;
                if (old != selected) ui_sound(ui, LAUNCHER_SOUND_MOVE);
                if (input.button == UI_A) edit = 1;
                if (input.button == UI_Y) reset = 1;
                if (input.button == UI_X) clear = 1;
                if (input.touch == UI_TOUCH_TAP)
                {
                    if (input.y < UI_HEADER_HEIGHT) goto done;
                    if (input.x >= 554 && input.x < 1212 && input.y >= 142 && input.y < 582)
                    {
                        int row = (input.y - 142 + (int)scroll)/55;
                        if (row >= 0 && row < count) { selected = row; edit = 1; }
                    }
                    if (controller)
                        for (int i = 0; i < 16; i++) if (inside(joy_hits[i],input.x,input.y))
                            { selected = i; break; }
                }
            }
            if (edit || reset || clear || !ui->running) break;
            if (selected < top) top = selected;
            if (selected >= top + 8) top = selected - 7;
            Uint32 now = SDL_GetTicks();
            float dt = fminf(now - previous, 50) / 1000;
            previous = now;
            scroll += (top*55 - scroll)*fminf(1, dt*18);
            if (fabsf(top*55 - scroll) < .5f) scroll = top*55;
            ui->scrolling_text = scroll != top*55;
            ui_background( ui );
            ui_header_back( ui, controller ? "Controller remapping" : "Keyboard & mouse", title );
            ui_rounded( ui, 52, 133, 481, 467, 22, ui->panel );
            ui_rounded( ui, 552, 133, 664, 467, 22, ui->panel );
            int source = selected;
            const struct input_binding *binding = controller ? profile.pad + source : profile.keys + source;
            if (controller)
            {
                ui_text( ui, ui->small, 76, 150, "NINTENDO SWITCH", ui->dim );
                joycons( ui, &art, 117, 193, .98f, source, joy_hits );
                ui_rounded( ui, 76, 551, 433, 32, 9, ui->card );
                ui_text_centered( ui, ui->small, 292, 555, input_sources[source].label, ui->value );
            }
            else
            {
                ui_text( ui, ui->small, 76, 153, "KEYBOARD", ui->dim );
                keyboard_draw( ui, key_hits, binding, -1 );
                ui_text( ui, ui->small, 79, 379, "MOUSE", ui->dim );
                mouse_draw( ui, &art, mouse_hits, binding, -1 );
            }
            SDL_Rect clip = {562,142,644,440};
            SDL_RenderSetClipRect( ui->renderer, &clip );
            for (int i = 0; i < count; i++)
            {
                int y = 143 + i*55 - lroundf(scroll), id = i;
                if (y + 51 < clip.y || y >= clip.y + clip.h) continue;
                if (i == selected)
                {
                    ui_rounded(ui, 563, y, 641, 51, 12, ui->card);
                    ui_outline(ui, 563, y, 641, 51, 12, 2, ui->dim);
                }
                char label[192];
                input_binding_label( controller ? profile.pad + id : profile.keys + id, controller, label, sizeof(label) );
                SDL_Rect r = {590, y + 3, 45, 45};
                glyph( ui, art.pad[id], r, ui->text );
                if (controller)
                {
                    const struct input_binding *b = profile.pad + id;
                    SDL_SetRenderDrawColor( ui->renderer, ui->dim.r, ui->dim.g, ui->dim.b, ui->dim.a );
                    SDL_RenderDrawLine( ui->renderer, 754, y + 26, 774, y + 26 );
                    SDL_RenderDrawLine( ui->renderer, 768, y + 20, 774, y + 26 );
                    SDL_RenderDrawLine( ui->renderer, 768, y + 32, 774, y + 26 );
                    for (unsigned int j = 0; j < b->count; j++)
                    {
                        SDL_Rect r = {837 + j*80, y + 3, 45, 45};
                        if (j) ui_text( ui, ui->small, r.x - 25, y + 15, "+", ui->dim );
                        glyph( ui, art.pad[output_icon(b->code[j])], r, ui->value );
                    }
                    if (!b->count) ui_text( ui, ui->small, 846, y + 15, "-", ui->dim );
                }
                else ui_text_fit( ui, ui->small, 815, y + 15, 367, label, ui->value, i == selected );
            }
            SDL_RenderSetClipRect( ui->renderer, NULL );
            int thumb = 440 * 8 / count;
            ui_rounded( ui, 1227, 143, 4, 440, 2, ui->card );
            ui_rounded( ui, 1227, 143 + (440 - thumb) * scroll / ((count - 8) * 55), 4, thumb, 2, ui->dim );
            ui_footer( ui, hints, sizeof(hints)/sizeof(hints[0]) );
            ui_fade(ui); ui_present(ui); ui_wait(ui);
        }
        if (!ui->running) break;
        int id = selected, changed = -1;
        struct input_binding binding = controller ? profile.pad[id] : profile.keys[id];
        if (reset) changed = input_binding_save(&kv, id, controller, NULL);
        else if (clear) { binding.count = 0; changed = input_binding_save(&kv, id, controller, &binding); }
        else if (edit && binding_dialog(ui, &art, id, controller, &binding)) changed = input_binding_save(&kv, id, controller, &binding);
        if (changed == 0) ui_message(ui, "Controls", "The settings file is full.");
        else if (changed > 0 && !input_profile_save(&kv, path)) ui_message(ui, "Controls", "The control settings could not be saved.");
        ui_start_screen(ui);
    }
done:
    free_art( &art );
    ui_start_screen(ui);
}

static const struct
{
    const char *key, *label, *help;
} options[INPUT_ROW_COUNT] =
{
    { "input-mode", "Input mode", "Use controller input or keyboard and mouse mappings." },
    { NULL, "Controller remapping", "Change controller buttons and stick directions." },
    { NULL, "Keyboard / mouse mapping", "Assign keys and mouse actions to controller inputs." },
    { "keyboard-auto", "On-screen keyboard", "Automatic: open when a text field gains focus." },
    { "left-deadzone", "Left stick deadzone", "Ignore small movements near the stick centre." },
    { "right-deadzone", "Right stick deadzone", "Ignore small movements near the stick centre." },
};

static int option_count( int row )
{
    if (row == INPUT_ROW_PAD || row == INPUT_ROW_KEYS) return 0;
    if (row == INPUT_ROW_MODE || row == INPUT_ROW_KEYBOARD) return 2;
    return 6;
}

static int option_value( int row, const struct input_profile *p )
{
    switch (row)
    {
    case INPUT_ROW_MODE: return p->mode;
    case INPUT_ROW_KEYBOARD: return p->keyboard_auto;
    case INPUT_ROW_LEFT_ZONE: return p->deadzone[0];
    case INPUT_ROW_RIGHT_ZONE: return p->deadzone[1];
    default: return 0;
    }
}

static void option_label( int row, int value, char *out, size_t size )
{
    if (row == INPUT_ROW_MODE) snprintf(out, size, "%s", input_mode_labels[value - INPUT_CONTROLLER]);
    else if (row == INPUT_ROW_KEYBOARD) snprintf(out, size, "%s", value ? "Automatic" : "Manual");
    else if (row == INPUT_ROW_PAD || row == INPUT_ROW_KEYS) snprintf(out, size, "Configure");
    else snprintf(out, size, "%d%%", value);
}

int launcher_input_rows( struct ui_row *rows, const char *root, const char *program, int keyboard_auto )
{
    struct input_profile p;
    struct launcher_kv kv;
    char path[768];
    int ok = load_profile(root, program, keyboard_auto, &p, &kv, path, sizeof(path));
    if (!ok) input_profile_defaults(&p, keyboard_auto);
    memset(rows, 0, INPUT_ROW_COUNT*sizeof(*rows));
    for (int i = 0; i < INPUT_ROW_COUNT; i++)
    {
        snprintf(rows[i].label, sizeof(rows[i].label), "%s", options[i].label);
        rows[i].help = options[i].help;
        rows[i].kind = options[i].key ? UI_ROW_DROPDOWN : UI_ROW_ACTION;
        rows[i].disabled = !ok;
        rows[i].choices = option_count(i) + (program && options[i].key ? 1 : 0);
        option_label(i, option_value(i, &p), rows[i].value, sizeof(rows[i].value));
    }
    return ok;
}

void launcher_input_edit( struct ui *ui, struct ui_list *list, int row, enum ui_action action,
                          const char *root, const char *program, const char *title, int keyboard_auto )
{
    struct input_profile p;
    struct launcher_kv kv;
    struct ui_row choices[16] = {0};
    int values[16], count, offset = program ? 1 : 0, selected = 0;
    char path[768], value[64];
    if (row < 0 || row >= INPUT_ROW_COUNT) return;
    if (!options[row].key)
    {
        if (action == UI_ACTION_CHOOSE) launcher_input_remap(ui, root, program, title, keyboard_auto, row == INPUT_ROW_PAD);
        return;
    }
    if (action != UI_ACTION_CHOOSE && action != UI_ACTION_RESET) return;
    if (!load_profile(root, program, keyboard_auto, &p, &kv, path, sizeof(path)))
    { ui_message(ui, "Controls", "The control settings could not be read."); return; }
    if (action == UI_ACTION_RESET) launcher_kv_set(&kv, options[row].key, NULL);
    else
    {
        int current = option_value(row, &p);
        int custom = launcher_kv_get(&kv, options[row].key, value, sizeof(value)) != NULL;
        count = option_count(row) + offset;
        if (program)
        {
            struct input_profile global;
            struct launcher_kv global_kv;
            char global_path[768];
            if (!input_profile_global(root, keyboard_auto, &global, &global_kv, global_path, sizeof(global_path)))
            { ui_message(ui, "Controls", "The global controls could not be read."); return; }
            option_label(row, option_value(row, &global), value, sizeof(value));
            snprintf(choices[0].label, sizeof(choices[0].label), "Global (%s)", value);
        }
        for (int i = offset; i < count; i++)
        {
            int item = i - offset;
            values[i] = row == INPUT_ROW_MODE ? item + INPUT_CONTROLLER :
                        row >= INPUT_ROW_LEFT_ZONE ? item*5 : item;
            if ((!program || custom) && values[i] <= current) selected = i;
            option_label(row, values[i], choices[i].label, sizeof(choices[i].label));
        }
        selected = ui_settings_dropdown(ui, list, choices, count, selected);
        if (selected < 0) return;
        if (program && !selected) launcher_kv_set(&kv, options[row].key, NULL);
        else
        {
            snprintf(value, sizeof(value), "%d", values[selected]);
            if (!launcher_kv_set(&kv, options[row].key, value))
            { ui_message(ui, "Controls", "The settings file is full."); return; }
        }
    }
    if (!input_profile_save(&kv, path)) ui_message(ui, "Controls", "The control settings could not be saved.");
}
