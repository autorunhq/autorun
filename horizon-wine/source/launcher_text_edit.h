#ifndef WINE_NX_LAUNCHER_TEXT_EDIT_H
#define WINE_NX_LAUNCHER_TEXT_EDIT_H

#include "launcher_ui.h"

/* Whether a file's name is one the editor offers: settings and other text a
 * game keeps beside it (.ini, .cfg, .txt, ...). */
int launcher_text_edit_offers( const char *name );

/* The editor, on the file at path (an sdmc:/ or ums path), shown as name:
 * A changes a line, or the value of an .ini setting, through the keyboard; Y
 * adds and removes lines. Leaving asks whether to save what changed. */
void launcher_text_edit( struct ui *ui, const char *path, const char *name );

#endif
