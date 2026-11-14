#ifndef WINE_NX_LAUNCHER_GRAPHICS_H
#define WINE_NX_LAUNCHER_GRAPHICS_H

#include "dxvk_releases.h"

struct ui;
struct ui_list;
struct launcher_graphics;

struct launcher_graphics *launcher_graphics_create( struct ui *ui, const char *runtime_dir );
void launcher_graphics_destroy( struct launcher_graphics *graphics );
int launcher_graphics_select( struct launcher_graphics *graphics, const struct ui_list *anchor,
                              unsigned short machine, int vkd3d, int combined,
                              enum dxvk_source *source, char version[32] );
int launcher_graphics_ensure( struct launcher_graphics *graphics, unsigned short machine,
                              int combined, enum dxvk_source source,
                              char dxvk_version[32], char vkd3d_version[32] );
/* The FEX a 32-bit program runs on: Autorun's own, from the DLL repository, or
 * a release of the one with Carafe's patches, listed and installed like a DXVK
 * release. 1 when a choice was made, with *carafe and version holding it. */
int launcher_graphics_select_fex( struct launcher_graphics *graphics, const struct ui_list *anchor,
                                  int *carafe, char version[32] );
/* Has a Carafe FEX on the card before a program set to one starts: the version
 * asked for, or the newest installed, or the newest release when none is. */
int launcher_graphics_ensure_fex( struct launcher_graphics *graphics, char version[32] );

#endif
