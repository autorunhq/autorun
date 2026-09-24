#ifndef WINE_NX_STEAMGRIDDB_H
#define WINE_NX_STEAMGRIDDB_H

#include <stddef.h>

#define STEAMGRIDDB_MAX_GAMES 12
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

enum steamgriddb_result
{
    STEAMGRIDDB_OK,
    STEAMGRIDDB_NO_KEY,
    STEAMGRIDDB_NETWORK_ERROR,
    STEAMGRIDDB_NOT_FOUND,
    STEAMGRIDDB_INVALID_RESPONSE,
    STEAMGRIDDB_IO_ERROR
};

/* Finds the closest title and downloads the highest-reputation static artwork
 * for all three launcher surfaces. Each destination is replaced atomically. */
enum steamgriddb_result steamgriddb_download_bundle( const char *api_key, const char *title,
                                                     const char *square_path,
                                                     const char *portrait_path,
                                                     const char *hero_path,
                                                     char *matched, size_t matched_size );
enum steamgriddb_result steamgriddb_search_games( const char *api_key, const char *title,
                                                  struct steamgriddb_game *games, int max_games,
                                                  int *count );
enum steamgriddb_result steamgriddb_download_game_bundle( const char *api_key, long game_id,
                                                          const char *square_path,
                                                          const char *portrait_path,
                                                          const char *hero_path );
const char *steamgriddb_result_message( enum steamgriddb_result result );
enum steamgriddb_result steamgriddb_square_pictures( const char *key, long game_id,
    struct steamgriddb_picture *pictures, int max, int *count );
enum steamgriddb_result steamgriddb_picture_data( const char *url, unsigned char **data, size_t *size );

#endif
