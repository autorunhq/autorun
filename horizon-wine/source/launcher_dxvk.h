#ifndef WINE_NX_LAUNCHER_DXVK_H
#define WINE_NX_LAUNCHER_DXVK_H

#include "dxvk_releases.h"

struct ui;

void launcher_dxvk_options( struct ui *ui, const char *runtime_dir, const char *program,
                            const char *title, enum dxvk_source source, const char *version );

#endif
