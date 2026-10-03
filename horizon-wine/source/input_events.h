#ifndef AUTORUN_INPUT_EVENTS_H
#define AUTORUN_INPUT_EVENTS_H

#include <stdint.h>
#define INPUT_KEY_WORDS 4
#define INPUT_KEY_EVENT_MAX 768
#define INPUT_NUMPAD_ENTER 0xff
struct input_key_event { uint16_t code, down; };
struct input_key_edges
{
    uint64_t held[INPUT_KEY_WORDS], pressed[INPUT_KEY_WORDS], released[INPUT_KEY_WORDS];
};
struct input_snapshot
{
    struct input_key_event keys[INPUT_KEY_EVENT_MAX];
    unsigned int key_count, buttons, pressed, released;
    int x, y, dx, dy, moved, placed, wheel, hwheel;
};
unsigned int input_key_events( const struct input_key_edges *edges, uint64_t delivered[INPUT_KEY_WORDS],
                               struct input_key_event events[INPUT_KEY_EVENT_MAX] );

#endif
