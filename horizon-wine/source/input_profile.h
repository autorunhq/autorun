#ifndef AUTORUN_INPUT_PROFILE_H
#define AUTORUN_INPUT_PROFILE_H

#include <stdint.h>
#include "input_events.h"
#include "launcher_settings.h"

#define INPUT_CHORD_MAX 4
#define INPUT_SOURCE_COUNT 24
#define INPUT_PAD_TARGET_COUNT 24

/* The analog-stick cursor's speed, as a percentage of the built-in one: the
 * stick sets how fast the pointer moves, and this scales that. 100 is the
 * default (see pointer_cursor.h); lower is finer, higher crosses the screen
 * sooner. The picker offers INPUT_MOUSE_SPEED_COUNT stops from
 * input_mouse_speed_values. */
#define INPUT_MOUSE_SPEED_DEFAULT 100
#define INPUT_MOUSE_SPEED_MIN 5
#define INPUT_MOUSE_SPEED_MAX 400
#define INPUT_MOUSE_SPEED_COUNT 10
static const int input_mouse_speed_values[INPUT_MOUSE_SPEED_COUNT] =
    { 5, 10, 15, 25, 50, 75, 100, 150, 200, 400 };

enum input_mode { INPUT_CONTROLLER = 1, INPUT_KEYBOARD_MOUSE };
enum input_mouse_code
{
    INPUT_MOUSE_LEFT = 0x100, INPUT_MOUSE_RIGHT, INPUT_MOUSE_MIDDLE, INPUT_MOUSE_X1, INPUT_MOUSE_X2,
    INPUT_WHEEL_UP, INPUT_WHEEL_DOWN, INPUT_WHEEL_LEFT, INPUT_WHEEL_RIGHT,
    INPUT_MOUSE_UP, INPUT_MOUSE_DOWN, INPUT_MOUSE_MOVE_LEFT, INPUT_MOUSE_MOVE_RIGHT,
    INPUT_MOUSE_END
};

struct input_binding { uint16_t code[INPUT_CHORD_MAX]; unsigned int count; };
struct input_source { const char *name, *label; uint64_t button; };
struct input_profile
{
    struct input_binding pad[INPUT_SOURCE_COUNT], keys[INPUT_SOURCE_COUNT];
    int mode, deadzone[2], keyboard_auto;
    unsigned int left_inherit;
    int mouse_speed;   /* percent of the built-in cursor speed; 100 is the default */
    /* Whether a settings layer named the speed. A game's recommended one
     * (games/controls.h) is only a suggestion: it stands over the player's
     * global controls, but not over a speed they chose themselves. */
    int mouse_speed_explicit;
};
struct input_pad { uint64_t buttons; int axis[4]; };
struct input_output
{
    uint64_t keys[INPUT_KEY_WORDS];
    unsigned int buttons, wheels;
    int mouse_x, mouse_y;
};

extern const struct input_source input_sources[INPUT_SOURCE_COUNT];
extern const char *const input_pad_labels[INPUT_PAD_TARGET_COUNT];
extern const char *const input_mode_labels[2];
void input_profile_defaults( struct input_profile *profile, int keyboard_auto );
void input_profile_apply( struct input_profile *profile, const struct launcher_kv *kv );
int input_profile_global( const char *root, int keyboard_auto, struct input_profile *profile,
                          struct launcher_kv *editable, char *path, size_t size );
int input_profile_program( const char *root, const char *program, int keyboard_auto,
                           struct input_profile *profile, struct launcher_kv *editable, char *path, size_t size );
int input_profile_save( const struct launcher_kv *kv, const char *path );
int input_binding_parse( struct input_binding *binding, const char *text, int controller );
int input_binding_save( struct launcher_kv *kv, int source, int controller, const struct input_binding *binding );
void input_binding_label( const struct input_binding *binding, int controller, char *out, size_t size );
const char *input_key_label( unsigned int code );
void input_filter_stick( int *x, int *y, int deadzone );
void input_map_pad( const struct input_profile *profile, const struct input_pad *raw, struct input_pad *mapped );
void input_map_keys( const struct input_profile *profile, const struct input_pad *raw, struct input_output *output );
void input_key_edges_update( struct input_key_edges *edges, const uint64_t held[INPUT_KEY_WORDS] );
void input_key_edges_take( struct input_key_edges *edges, struct input_key_edges *out );
int input_key_modifier( unsigned int code );

#endif
