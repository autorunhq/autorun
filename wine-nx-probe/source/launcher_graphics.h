#ifndef WINE_NX_LAUNCHER_GRAPHICS_H
#define WINE_NX_LAUNCHER_GRAPHICS_H

struct ui;
struct ui_list;
struct launcher_graphics;

struct launcher_graphics *launcher_graphics_create( struct ui *ui, const char *runtime_dir );
void launcher_graphics_destroy( struct launcher_graphics *graphics );
int launcher_graphics_select( struct launcher_graphics *graphics, const struct ui_list *anchor,
                              unsigned short machine, int vkd3d, char version[32] );
int launcher_graphics_ensure( struct launcher_graphics *graphics, unsigned short machine,
                              char dxvk_version[32], char vkd3d_version[32] );

#endif
