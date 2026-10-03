#include <errno.h>
#include <math.h>
#include <sys/stat.h>
#include "input_profile.h"
#include "key_names.h"

const struct input_source input_sources[INPUT_SOURCE_COUNT] =
{
    { "A", "A", 1ull << 0 }, { "B", "B", 1ull << 1 },
    { "X", "X", 1ull << 2 }, { "Y", "Y", 1ull << 3 },
    { "L", "L", 1ull << 6 }, { "R", "R", 1ull << 7 },
    { "ZL", "ZL", 1ull << 8 }, { "ZR", "ZR", 1ull << 9 },
    { "PLUS", "+", 1ull << 10 }, { "MINUS", "-", 1ull << 11 },
    { "STICKL", "Left stick click", 1ull << 4 }, { "STICKR", "Right stick click", 1ull << 5 },
    { "UP", "D-pad up", 1ull << 13 }, { "DOWN", "D-pad down", 1ull << 15 },
    { "LEFT", "D-pad left", 1ull << 12 }, { "RIGHT", "D-pad right", 1ull << 14 },
    { "LUP", "Left stick up", 0 }, { "LDOWN", "Left stick down", 0 },
    { "LLEFT", "Left stick left", 0 }, { "LRIGHT", "Left stick right", 0 },
    { "RUP", "Right stick up", 0 }, { "RDOWN", "Right stick down", 0 },
    { "RLEFT", "Right stick left", 0 }, { "RRIGHT", "Right stick right", 0 },
};

const char *const input_pad_labels[INPUT_PAD_TARGET_COUNT] =
{
    "B / Button 2", "A / Button 1", "Y / Button 4", "X / Button 3",
    "LB / Button 5", "RB / Button 6", "LT / Z axis", "RT / Z axis",
    "Start / Button 8", "Back / Button 7", "LS / Button 9", "RS / Button 10",
    "D-pad up", "D-pad down", "D-pad left", "D-pad right",
    "Left stick up", "Left stick down", "Left stick left", "Left stick right",
    "Right stick up", "Right stick down", "Right stick left", "Right stick right"
};
const char *const input_mode_labels[2] = { "Controller", "Keyboard / mouse" };
static const char *const mouse_labels[] =
    { "Left click", "Right click", "Middle click", "Mouse back", "Mouse forward",
      "Wheel up", "Wheel down", "Wheel left", "Wheel right",
      "Mouse up", "Mouse down", "Mouse left", "Mouse right" };

const char *input_key_label( unsigned int code )
{
    if (code >= INPUT_MOUSE_LEFT && code < INPUT_MOUSE_END) return mouse_labels[code - INPUT_MOUSE_LEFT];
    switch (code)
    {
    case 0x5b: return "Left Windows"; case 0x5c: return "Right Windows"; case 0x5d: return "Menu";
    case 0xa0: return "Left Shift"; case 0xa1: return "Right Shift";
    case 0xa2: return "Left Ctrl"; case 0xa3: return "Right Ctrl";
    case 0xa4: return "Left Alt"; case 0xa5: return "Right Alt";
    case 0x90: return "Num lock"; case 0x91: return "Scroll lock";
    case INPUT_NUMPAD_ENTER: return "Numpad Enter";
    }
    int index = wine_nx_key_index( code );
    return index < 0 ? NULL : wine_nx_key_names[index].name;
}

void input_profile_defaults( struct input_profile *p, int keyboard_auto )
{
    static const unsigned short keys[] =
    {
        INPUT_MOUSE_LEFT, INPUT_MOUSE_RIGHT, 0x20, 0x46, 0x09, 0x10, 0x28, 0x26,
        0x1b, 0x09, 0x11, 0x12, 0x31, 0x32, 0x33, 0x34,
        0x57, 0x53, 0x41, 0x44, INPUT_MOUSE_UP, INPUT_MOUSE_DOWN, INPUT_MOUSE_MOVE_LEFT, INPUT_MOUSE_MOVE_RIGHT,
    };
    memset( p, 0, sizeof(*p) );
    p->mode = INPUT_CONTROLLER;
    p->deadzone[0] = p->deadzone[1] = 5;
    p->keyboard_auto = !!keyboard_auto;
    for (unsigned int i = 0; i < INPUT_SOURCE_COUNT; i++)
    {
        if (i < INPUT_PAD_TARGET_COUNT) p->pad[i] = (struct input_binding){ { i + 1 }, 1 };
        if (i < sizeof(keys) / sizeof(keys[0])) p->keys[i] = (struct input_binding){ { keys[i] }, 1 };
    }
}

int input_binding_parse( struct input_binding *binding, const char *text, int controller )
{
    struct input_binding parsed = {0};
    char *end;
    const char *p = text;

    if (!p || !*p) return 0;
    if (!strcasecmp( p, "none" ) || !strcmp( p, "0" )) { *binding = parsed; return 1; }
    for (;;)
    {
        unsigned long value;
        while (isspace( (unsigned char)*p )) p++;
        errno = 0;
        value = strtoul( p, &end, 0 );
        if (p == end || errno || !value || parsed.count == INPUT_CHORD_MAX ||
            (controller ? value > INPUT_PAD_TARGET_COUNT :
                          value >= INPUT_MOUSE_END)) return 0;
        for (unsigned int i = 0; i < parsed.count; i++) if (parsed.code[i] == value) return 0;
        parsed.code[parsed.count++] = value;
        p = end;
        while (isspace( (unsigned char)*p )) p++;
        if (!*p) break;
        if (*p++ != '+') return 0;
    }
    *binding = parsed;
    return 1;
}

static int setting( const struct launcher_kv *kv, const char *key, int base, int low, int high )
{
    char text[32], *end;
    long value;
    if (!launcher_kv_get( kv, key, text, sizeof(text) )) return base;
    errno = 0;
    value = strtol( text, &end, 10 );
    return !errno && end != text && !*end && value >= low && value <= high ? value : base;
}

void input_profile_apply( struct input_profile *p, const struct launcher_kv *kv )
{
    char name[64], value[128];

    for (unsigned int i = 0; i < INPUT_SOURCE_COUNT; i++)
    {
        if (launcher_kv_get( kv, input_sources[i].name, value, sizeof(value) ))
        {
            struct input_binding parsed;
            if (!input_binding_parse( &parsed, value, 0 )) continue;
            if (i >= 16 && i < 20)
            {
                if (!parsed.count) p->left_inherit |= 1u << (i - 16);
                else p->left_inherit &= ~(1u << (i - 16));
            }
            if ((i == 0 || i == 1) && !parsed.count)
                parsed = (struct input_binding){ { INPUT_MOUSE_LEFT + i }, 1 };
            p->keys[i] = parsed;
        }
    }
    for (unsigned int i = 0; i < INPUT_SOURCE_COUNT; i++)
    {
        snprintf( name, sizeof(name), "pad.%s", input_sources[i].name );
        if (launcher_kv_get( kv, name, value, sizeof(value) )) input_binding_parse( &p->pad[i], value, 1 );
        snprintf( name, sizeof(name), "key.%s", input_sources[i].name );
        if (launcher_kv_get( kv, name, value, sizeof(value) ) && input_binding_parse( &p->keys[i], value, 0 ) &&
            i >= 16 && i < 20) p->left_inherit &= ~(1u << (i - 16));
    }
    for (unsigned int i = 0; i < 4; i++) if (p->left_inherit & (1u << i)) p->keys[16 + i] = p->keys[12 + i];
    p->mode = setting( kv, "input-mode", p->mode, INPUT_CONTROLLER, INPUT_KEYBOARD_MOUSE );
    p->deadzone[0] = setting( kv, "left-deadzone", p->deadzone[0], 0, 25 );
    p->deadzone[1] = setting( kv, "right-deadzone", p->deadzone[1], 0, 25 );
    p->keyboard_auto = setting( kv, "keyboard-auto", p->keyboard_auto, 0, 1 );
}

int input_profile_global( const char *root, int keyboard_auto, struct input_profile *profile,
                          struct launcher_kv *editable, char *path, size_t size )
{
    struct launcher_kv legacy;
    input_profile_defaults( profile, keyboard_auto );
    if ((size_t)snprintf( path, size, "%s/keys.txt", root ) >= size || !launcher_kv_load( &legacy, path )) return 0;
    input_profile_apply( profile, &legacy );
    if ((size_t)snprintf( path, size, "%s/config/keys.txt", root ) >= size || !launcher_kv_load( editable, path )) return 0;
    input_profile_apply( profile, editable );
    return 1;
}

int input_profile_program( const char *root, const char *program, int keyboard_auto,
                           struct input_profile *profile, struct launcher_kv *editable, char *path, size_t size )
{
    struct launcher_kv global;
    size_t len = strlen( program );
    if (!input_profile_global( root, keyboard_auto, profile, &global, path, size )) return 0;
    if (len > 4 && !strcasecmp( program + len - 4, ".exe" ))
    {
        if ((size_t)snprintf( path, size, "%.*s.keys.txt", (int)len - 4, program ) >= size ||
            !launcher_kv_load( editable, path )) return 0;
        input_profile_apply( profile, editable );
    }
    if (!launcher_program_settings_path( root, program, path, size ) || !launcher_kv_load( editable, path )) return 0;
    input_profile_apply( profile, editable );
    return 1;
}

int input_profile_save( const struct launcher_kv *kv, const char *path )
{
    char parent[768], *slash;
    if (strlen( path ) >= sizeof(parent)) return 0;
    strcpy( parent, path );
    if ((slash = strrchr( parent, '/' )))
    {
        *slash = 0;
        if (mkdir( parent, 0777 ) && errno != EEXIST) return 0;
    }
    return launcher_kv_save( kv, path );
}

int input_binding_save( struct launcher_kv *kv, int source, int controller, const struct input_binding *binding )
{
    char key[64], value[80] = "none";
    size_t len = 0;
    if (source < 0 || source >= INPUT_SOURCE_COUNT || (binding && binding->count > INPUT_CHORD_MAX)) return 0;
    snprintf( key, sizeof(key), "%s.%s", controller ? "pad" : "key", input_sources[source].name );
    if (binding)
        for (unsigned int i = 0; i < binding->count; i++)
            len += snprintf( value + len, sizeof(value) - len, "%s0x%x", i ? "+" : "", binding->code[i] );
    return launcher_kv_set( kv, key, binding ? value : NULL );
}

void input_binding_label( const struct input_binding *binding, int controller, char *out, size_t size )
{
    size_t len = 0;
    if (!size) return;
    snprintf( out, size, "Unassigned" );
    for (unsigned int i = 0; i < binding->count && len < size; i++)
    {
        char number[16];
        unsigned int code = binding->code[i];
        const char *label = controller && code && code <= INPUT_PAD_TARGET_COUNT ? input_pad_labels[code - 1] : input_key_label( code );
        snprintf( number, sizeof(number), "0x%02x", code );
        len += snprintf( out + len, size - len, "%s%s", i ? " + " : "", label ? label : number );
    }
}

void input_filter_stick( int *x, int *y, int deadzone )
{
    if (!deadzone) return;
    double length = hypot( *x, *y ), limit = 32767.0 * deadzone / 100;
    if (length <= limit) { *x = *y = 0; return; }
    double scale = (fmin( length, 32767 ) - limit) * 32767 / ((32767 - limit) * length);
    *x = lround( *x * scale );
    *y = lround( *y * scale );
}

static int source_value( const struct input_pad *raw, unsigned int source )
{
    if (input_sources[source].button) return raw->buttons & input_sources[source].button ? 32767 : 0;
    unsigned int direction = (source - 16) % 4;
    int x = raw->axis[((source - 16) / 4) * 2];
    int y = raw->axis[((source - 16) / 4) * 2 + 1];
    int value = direction == 0 ? y : direction == 1 ? -y : direction == 2 ? -x : x;
    return value > 32768 ? 32768 : value > 0 ? value : 0;
}

void input_map_pad( const struct input_profile *profile, const struct input_pad *raw, struct input_pad *mapped )
{
    int values[INPUT_PAD_TARGET_COUNT] = {0};
    struct input_pad filtered = *raw;
    input_filter_stick( &filtered.axis[0], &filtered.axis[1], profile->deadzone[0] );
    input_filter_stick( &filtered.axis[2], &filtered.axis[3], profile->deadzone[1] );
    memset( mapped, 0, sizeof(*mapped) );
    for (unsigned int i = 0; i < INPUT_SOURCE_COUNT; i++)
    {
        int strength = source_value( &filtered, i );
        const struct input_binding *binding = profile->pad + i;
        for (unsigned int j = 0; strength && j < binding->count; j++)
        {
            unsigned int target = binding->code[j] - 1;
            if (target < INPUT_PAD_TARGET_COUNT && strength > values[target]) values[target] = strength;
        }
    }
    for (unsigned int i = 0; i < 16; i++) if (values[i] > 12000) mapped->buttons |= input_sources[i].button;
    for (unsigned int i = 0; i < 2; i++)
    {
        mapped->axis[i * 2] = values[19 + i * 4] - values[18 + i * 4];
        mapped->axis[i * 2 + 1] = values[16 + i * 4] - values[17 + i * 4];
        if (mapped->axis[i * 2] > 32767) mapped->axis[i * 2] = 32767;
        if (mapped->axis[i * 2 + 1] > 32767) mapped->axis[i * 2 + 1] = 32767;
    }
}

void input_map_keys( const struct input_profile *profile, const struct input_pad *raw, struct input_output *output )
{
    struct input_pad filtered = *raw;
    int motion[4] = {0};
    input_filter_stick( &filtered.axis[0], &filtered.axis[1], profile->deadzone[0] );
    input_filter_stick( &filtered.axis[2], &filtered.axis[3], profile->deadzone[1] );
    memset( output, 0, sizeof(*output) );
    for (unsigned int i = 0; i < INPUT_SOURCE_COUNT; i++)
    {
        int strength = source_value( &filtered, i );
        const struct input_binding *binding = profile->keys + i;
        for (unsigned int j = 0; j < binding->count; j++)
        {
            unsigned int code = binding->code[j];
            if (code >= INPUT_MOUSE_UP && code < INPUT_MOUSE_END)
            {
                int value = source_value( raw, i );
                if (value > motion[code - INPUT_MOUSE_UP]) motion[code - INPUT_MOUSE_UP] = value;
                continue;
            }
            if (strength <= 12000) continue;
            if (code && code < 256) output->keys[code / 64] |= 1ull << (code % 64);
            else if (code >= INPUT_MOUSE_LEFT && code < INPUT_WHEEL_UP) output->buttons |= 1u << (code - INPUT_MOUSE_LEFT);
            else if (code >= INPUT_WHEEL_UP && code < INPUT_MOUSE_UP) output->wheels |= 1u << (code - INPUT_WHEEL_UP);
        }
    }
    output->mouse_x = motion[3] - motion[2];
    output->mouse_y = motion[0] - motion[1];
    output->mouse_x = output->mouse_x < -32768 ? -32768 : output->mouse_x > 32767 ? 32767 : output->mouse_x;
    output->mouse_y = output->mouse_y < -32768 ? -32768 : output->mouse_y > 32767 ? 32767 : output->mouse_y;
}

void input_key_edges_update( struct input_key_edges *edges, const uint64_t held[INPUT_KEY_WORDS] )
{
    for (unsigned int i = 0; i < INPUT_KEY_WORDS; i++)
    {
        edges->pressed[i] |= held[i] & ~edges->held[i];
        edges->released[i] |= edges->held[i] & ~held[i];
        edges->held[i] = held[i];
    }
}

void input_key_edges_take( struct input_key_edges *edges, struct input_key_edges *out )
{
    *out = *edges;
    memset( edges->pressed, 0, sizeof(edges->pressed) );
    memset( edges->released, 0, sizeof(edges->released) );
}

int input_key_modifier( unsigned int code )
{
    return (code >= 0x10 && code <= 0x12) || (code >= 0xa0 && code <= 0xa5) || code == 0x5b || code == 0x5c;
}

unsigned int input_key_events( const struct input_key_edges *edges, uint64_t delivered[INPUT_KEY_WORDS],
                               struct input_key_event events[INPUT_KEY_EVENT_MAX] )
{
    unsigned int count = 0;
    for (unsigned int phase = 0; phase < 6; phase++)
        for (unsigned int word = 0; word < INPUT_KEY_WORDS; word++)
        {
            uint64_t release = delivered[word] & (~edges->held[word] | edges->released[word]);
            uint64_t press = (edges->held[word] | edges->pressed[word]) & (~delivered[word] | release);
            uint64_t pending = phase < 2 ? release : phase < 4 ? press : press & ~edges->held[word];
            while (pending)
            {
                unsigned int bit = __builtin_ctzll( pending ), code = word * 64 + bit;
                int modifier = input_key_modifier( code );
                pending &= pending - 1;
                if (modifier != (phase == 1 || phase == 2 || phase == 5)) continue;
                events[count++] = (struct input_key_event){ code, phase == 2 || phase == 3 };
            }
        }
    memcpy( delivered, edges->held, sizeof(edges->held) );
    return count;
}
