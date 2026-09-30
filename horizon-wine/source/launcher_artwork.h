#ifndef WINE_NX_LAUNCHER_ARTWORK_H
#define WINE_NX_LAUNCHER_ARTWORK_H

#include "steamgriddb.h"

struct ui;
struct launcher_icon;

struct launcher_artwork_entry
{
    char title[128];
    char paths[3][512];
    unsigned int missing, downloaded;
};

long launcher_artwork_search( struct ui *ui, const char *key, const char *title );
int launcher_artwork_pick( struct ui *ui, const char *key, long game_id, enum steamgriddb_kind kind,
                           const char *destination, struct launcher_icon *image );
void launcher_artwork_download_all( struct ui *ui, const char *key,
                                    struct launcher_artwork_entry *entries, int count );

#endif
