#ifndef LAUNCHER_FORWARDER_H
#define LAUNCHER_FORWARDER_H

#include <stddef.h>
struct ui;
struct wine_nx_launcher_options;

struct launcher_forwarder_game
{
    unsigned int id;
    const char *name;
    const char *path;
    const char *artwork;
};

void launcher_forwarder_run( struct ui *ui, const struct wine_nx_launcher_options *options,
    const struct launcher_forwarder_game *game, char *api_key, size_t key_size,
    int (*pick_image)( void *, char *, size_t ), void *picker_data );

#endif
