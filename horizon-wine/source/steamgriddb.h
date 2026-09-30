#ifndef WINE_NX_STEAMGRIDDB_H
#define WINE_NX_STEAMGRIDDB_H

#include <stddef.h>

#define STEAMGRIDDB_MAX_GAMES 6
#define STEAMGRIDDB_MAX_PICTURES 24

struct steamgriddb_picture
{
    char url[768];
    char author[96];
};

struct steamgriddb_game
{
    long id;
    char name[192];
};

enum steamgriddb_kind { STEAMGRIDDB_ICON, STEAMGRIDDB_COVER, STEAMGRIDDB_BACKGROUND };

struct steamgriddb_request
{
    int (*cancelled)( void *data );
    void *data;
};

enum steamgriddb_result
{
    STEAMGRIDDB_OK,
    STEAMGRIDDB_NO_KEY,
    STEAMGRIDDB_NETWORK_ERROR,
    STEAMGRIDDB_NOT_FOUND,
    STEAMGRIDDB_INVALID_RESPONSE,
    STEAMGRIDDB_IO_ERROR,
    STEAMGRIDDB_CANCELLED
};

enum steamgriddb_result steamgriddb_search_games( const char *key, const char *title,
    struct steamgriddb_game *games, int max, int *count, const struct steamgriddb_request *request );
enum steamgriddb_result steamgriddb_pictures( const char *key, long game_id, enum steamgriddb_kind kind,
    struct steamgriddb_picture *pictures, int max, int *count, const struct steamgriddb_request *request );
enum steamgriddb_result steamgriddb_picture_data( const char *url, unsigned char **data, size_t *size,
    const struct steamgriddb_request *request );
enum steamgriddb_result steamgriddb_save_picture( const unsigned char *data, size_t size, const char *path );
const char *steamgriddb_result_message( enum steamgriddb_result result );

#endif
