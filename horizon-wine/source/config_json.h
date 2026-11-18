/*
 * The runtime's settings, as one JSON object in switch/wine/config/settings.json.
 *
 * Every setting used to be a file of its own whose name was the question and
 * whose presence was the answer -- framebuffer.txt, no-balance.txt -- which
 * left a dozen of them loose in the card's wine folder with no way to tell a
 * setting from a scratch file. They are one object now, read whole and written
 * whole, with the name of each saying what it turns on rather than off.
 *
 * A key this build does not know is kept and written back, so a card shared
 * with a newer or older build loses nothing. Values are booleans, numbers or
 * strings; anything else is kept as its own text and handed back unread.
 */
#ifndef WINE_NX_CONFIG_JSON_H
#define WINE_NX_CONFIG_JSON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WINE_NX_CONFIG_KEYS   64
#define WINE_NX_CONFIG_KEY    64
#define WINE_NX_CONFIG_VALUE  256

struct wine_nx_config
{
    struct
    {
        char key[WINE_NX_CONFIG_KEY];
        char value[WINE_NX_CONFIG_VALUE];  /* the value's own JSON text */
    } entries[WINE_NX_CONFIG_KEYS];
    unsigned int count;
};

static inline const char *wine_nx_config_space( const char *at )
{
    while (*at == ' ' || *at == '\t' || *at == '\r' || *at == '\n') at++;
    return at;
}

/* A JSON string into out, without the quotes and with the escapes a settings
 * file can hold. Returns where it ended, or NULL. */
static inline const char *wine_nx_config_string( const char *at, char *out, size_t size )
{
    size_t len = 0;

    if (*at++ != '"') return NULL;
    while (*at && *at != '"')
    {
        char c = *at++;

        if (c == '\\')
        {
            /* A file cut short just after a backslash: its end is not escaped,
             * and reading on would go past it. */
            if (!*at) return NULL;
            switch (*at++)
            {
            case '"': c = '"'; break;
            case '\\': c = '\\'; break;
            case '/': c = '/'; break;
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            /* \uXXXX and anything else: the character itself, which keeps a
             * path with a backslash in it readable rather than losing it. */
            default: c = at[-1]; break;
            }
        }
        if (out && len + 1 < size) out[len] = c;
        len++;
    }
    if (*at != '"') return NULL;
    if (out && size) out[len < size ? len : size - 1] = 0;
    return at + 1;
}

/* Where a value ends: a string, or a run of everything that is not a comma or
 * the end of the object. */
static inline const char *wine_nx_config_value_end( const char *at )
{
    int depth = 0;

    if (*at == '"') return wine_nx_config_string( at, NULL, 0 );
    while (*at)
    {
        if (*at == '[' || *at == '{') depth++;
        else if (*at == ']' || *at == '}')
        {
            if (!depth) break;
            depth--;
        }
        else if (*at == ',' && !depth) break;
        at++;
    }
    return at;
}

/* Reads the object in text. Returns 0 when it is not one, leaving what was
 * read so far: a settings file cut short by a flat battery still gives up
 * whatever it holds rather than every setting at once. */
static inline int wine_nx_config_parse( struct wine_nx_config *config, const char *text )
{
    const char *at = wine_nx_config_space( text );

    config->count = 0;
    if (*at++ != '{') return 0;
    for (;;)
    {
        const char *end;
        size_t len;

        at = wine_nx_config_space( at );
        if (*at == '}') return 1;
        if (config->count >= WINE_NX_CONFIG_KEYS) return 0;
        if (!(at = wine_nx_config_string( at, config->entries[config->count].key, WINE_NX_CONFIG_KEY )))
            return 0;
        at = wine_nx_config_space( at );
        if (*at++ != ':') return 0;
        at = wine_nx_config_space( at );
        if (!(end = wine_nx_config_value_end( at )) || end == at) return 0;
        len = (size_t)(end - at);
        while (len && (at[len - 1] == ' ' || at[len - 1] == '\t' ||
                       at[len - 1] == '\r' || at[len - 1] == '\n')) len--;
        if (len >= WINE_NX_CONFIG_VALUE) return 0;
        memcpy( config->entries[config->count].value, at, len );
        config->entries[config->count].value[len] = 0;
        config->count++;
        at = wine_nx_config_space( end );
        if (*at == ',') { at++; continue; }
        if (*at == '}') return 1;
        return 0;
    }
}

/* A missing file is an empty object, which is every setting at its default. */
static inline int wine_nx_config_load( struct wine_nx_config *config, const char *path )
{
    char text[WINE_NX_CONFIG_KEYS * (WINE_NX_CONFIG_KEY + WINE_NX_CONFIG_VALUE) + 64];
    FILE *file = fopen( path, "rb" );
    size_t read;

    config->count = 0;
    if (!file) return 0;
    read = fread( text, 1, sizeof(text) - 1, file );
    fclose( file );
    text[read] = 0;
    return wine_nx_config_parse( config, text );
}

static inline int wine_nx_config_find( const struct wine_nx_config *config, const char *key )
{
    unsigned int i;

    for (i = 0; i < config->count; i++)
        if (!strcmp( config->entries[i].key, key )) return (int)i;
    return -1;
}

/* true and false; a number is true when it is not zero, so a setting written
 * by hand as 1 works as well as one written as true. */
static inline int wine_nx_config_bool( const struct wine_nx_config *config, const char *key, int fallback )
{
    int i = wine_nx_config_find( config, key );
    const char *value;

    if (i < 0) return fallback;
    value = config->entries[i].value;
    if (!strcmp( value, "true" )) return 1;
    if (!strcmp( value, "false" )) return 0;
    if (value[0] == '"' || value[0] == 'n') return fallback;  /* a string or null */
    return strtol( value, NULL, 10 ) != 0;
}

static inline int wine_nx_config_string_value( const struct wine_nx_config *config, const char *key,
                                               char *out, size_t size )
{
    int i = wine_nx_config_find( config, key );

    if (i < 0 || config->entries[i].value[0] != '"') return 0;
    return wine_nx_config_string( config->entries[i].value, out, size ) != NULL;
}

/* Sets a key, adding it at the end when it is new. Returns 0 when there is no
 * room, leaving what was there. */
static inline int wine_nx_config_set( struct wine_nx_config *config, const char *key, const char *json )
{
    int i = wine_nx_config_find( config, key );

    if (strlen( json ) >= WINE_NX_CONFIG_VALUE || strlen( key ) >= WINE_NX_CONFIG_KEY) return 0;
    if (i < 0)
    {
        if (config->count >= WINE_NX_CONFIG_KEYS) return 0;
        i = (int)config->count++;
        snprintf( config->entries[i].key, WINE_NX_CONFIG_KEY, "%s", key );
    }
    snprintf( config->entries[i].value, WINE_NX_CONFIG_VALUE, "%s", json );
    return 1;
}

static inline int wine_nx_config_set_bool( struct wine_nx_config *config, const char *key, int value )
{
    return wine_nx_config_set( config, key, value ? "true" : "false" );
}

/* One key to a line, in the order they were read, so a settings file a person
 * edits keeps the shape they left it in. */
static inline int wine_nx_config_write( const struct wine_nx_config *config, FILE *file )
{
    unsigned int i;

    if (fputs( "{\n", file ) < 0) return 0;
    for (i = 0; i < config->count; i++)
    {
        const char *key = config->entries[i].key;

        /* A key is kept as read, its escapes undone: a quote or a backslash in
         * it is escaped again, or the file would end at it the next time. */
        if (fputs( "  \"", file ) < 0) return 0;
        for (; *key; key++)
            if (((*key == '"' || *key == '\\') && fputc( '\\', file ) == EOF) || fputc( *key, file ) == EOF)
                return 0;
        if (fprintf( file, "\": %s%s\n", config->entries[i].value, i + 1 < config->count ? "," : "" ) < 0)
            return 0;
    }
    return fputs( "}\n", file ) >= 0;
}

/* Written through a temporary file, so a write cut short keeps the old one. */
static inline int wine_nx_config_save( const struct wine_nx_config *config, const char *path )
{
    char temp[768];
    FILE *file;
    int ok;

    if ((size_t)snprintf( temp, sizeof(temp), "%s.new", path ) >= sizeof(temp)) return 0;
    if (!(file = fopen( temp, "wb" ))) return 0;
    ok = wine_nx_config_write( config, file );
    ok = !fclose( file ) && ok;
    /* FAT cannot rename over an existing file. */
    if (ok) remove( path );
    if (!ok || rename( temp, path ))
    {
        remove( temp );
        return 0;
    }
    return 1;
}

#endif /* WINE_NX_CONFIG_JSON_H */
