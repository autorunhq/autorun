/*
 * Settings files of the launcher: lines of key=value, with other lines (comments,
 * keys a later build adds) kept as they are when a value is changed.
 *
 * A program on the SD card keeps its settings next to it: SPEED2.EXE reads
 * SPEED2.wine-nx.txt. A program on USB keeps them under the runtime directory
 * on the SD card.
 * The launcher's own look is in sdmc:/switch/wine/launcher.txt.
 */
#ifndef WINE_NX_LAUNCHER_SETTINGS_H
#define WINE_NX_LAUNCHER_SETTINGS_H

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define LAUNCHER_KV_MAX 8192

struct launcher_kv
{
    char text[LAUNCHER_KV_MAX];
    size_t size;
};

/* A missing file reads as empty. Returns 0 when the file is too large to keep whole. */
static inline int launcher_kv_load( struct launcher_kv *kv, const char *path )
{
    FILE *file = fopen( path, "rb" );
    int ok = 1;

    kv->size = 0;
    kv->text[0] = 0;
    if (!file) return 1;
    kv->size = fread( kv->text, 1, sizeof(kv->text) - 1, file );
    if (kv->size == sizeof(kv->text) - 1 && fgetc( file ) != EOF) ok = 0;
    fclose( file );
    kv->text[kv->size] = 0;
    return ok;
}

/* Find key's line: *line and *end bound it (without its newline). */
static inline int launcher_kv_find( const struct launcher_kv *kv, const char *key, size_t *line, size_t *end )
{
    size_t len = strlen( key ), pos = 0;

    while (pos < kv->size)
    {
        size_t start = pos, p = pos, stop;

        while (pos < kv->size && kv->text[pos] != '\n') pos++;
        stop = pos;
        if (pos < kv->size) pos++;
        while (p < stop && (kv->text[p] == ' ' || kv->text[p] == '\t')) p++;
        if (stop - p < len || strncasecmp( kv->text + p, key, len )) continue;
        p += len;
        while (p < stop && (kv->text[p] == ' ' || kv->text[p] == '\t')) p++;
        if (p < stop && kv->text[p] == '=')
        {
            *line = start;
            *end = stop;
            return 1;
        }
    }
    return 0;
}

/* The value of key with surrounding spaces and a carriage return removed, or NULL. */
static inline const char *launcher_kv_get( const struct launcher_kv *kv, const char *key, char *out, size_t size )
{
    size_t line, end, p, len;

    if (!size || !launcher_kv_find( kv, key, &line, &end )) return NULL;
    p = (char *)memchr( kv->text + line, '=', end - line ) - kv->text + 1;
    while (p < end && (kv->text[p] == ' ' || kv->text[p] == '\t')) p++;
    while (end > p && (kv->text[end - 1] == ' ' || kv->text[end - 1] == '\t' || kv->text[end - 1] == '\r')) end--;
    len = end - p < size - 1 ? end - p : size - 1;
    memcpy( out, kv->text + p, len );
    out[len] = 0;
    return out;
}

/* An integer value, or fallback when the key is missing or not a number. */
static inline int launcher_kv_get_int( const struct launcher_kv *kv, const char *key, int fallback )
{
    char value[32], *end;
    long n;

    if (!launcher_kv_get( kv, key, value, sizeof(value) ) || !value[0]) return fallback;
    n = strtol( value, &end, 10 );
    return *end ? fallback : (int)n;
}

/* Set key to value in place, add it at the end, or remove its line when value is NULL.
 * Returns 0 when the result would not fit. */
static inline int launcher_kv_set( struct launcher_kv *kv, const char *key, const char *value )
{
    char line[1024];
    size_t start, end, len = 0;

    if (value)
    {
        int n = snprintf( line, sizeof(line), "%s=%s", key, value );

        if (n < 0 || (size_t)n >= sizeof(line) || strpbrk( value, "\r\n" )) return 0;
        len = n;
    }
    if (launcher_kv_find( kv, key, &start, &end ))
    {
        if (!value && end < kv->size) end++;  /* the newline goes too */
    }
    else
    {
        if (!value) return 1;
        start = end = kv->size;
        if (kv->size && kv->text[kv->size - 1] != '\n')
        {
            if (kv->size + 1 >= sizeof(kv->text)) return 0;
            kv->text[kv->size++] = '\n';
            start = end = kv->size;
        }
        if (kv->size + len + 1 >= sizeof(kv->text)) return 0;
        line[len++] = '\n';
    }
    if (kv->size - (end - start) + len >= sizeof(kv->text)) return 0;
    memmove( kv->text + start + len, kv->text + end, kv->size - end );
    memcpy( kv->text + start, line, len );
    kv->size = kv->size - (end - start) + len;
    kv->text[kv->size] = 0;
    return 1;
}

/* Whether any line holds more than spaces. */
static inline int launcher_kv_empty( const struct launcher_kv *kv )
{
    size_t i;

    for (i = 0; i < kv->size; i++)
        if (!isspace( (unsigned char)kv->text[i] )) return 0;
    return 1;
}

/* Write the file through a temporary one, so a failed write keeps the old file;
 * a file with nothing left in it is removed. */
static inline int launcher_kv_save( const struct launcher_kv *kv, const char *path )
{
    char temp[768];
    FILE *file;
    int ok;

    if (launcher_kv_empty( kv ))
    {
        if (!remove( path ) || !(file = fopen( path, "rb" ))) return 1;
        fclose( file );
        return 0;
    }
    if ((size_t)snprintf( temp, sizeof(temp), "%s.new", path ) >= sizeof(temp)) return 0;
    if (!(file = fopen( temp, "wb" ))) return 0;
    ok = fwrite( kv->text, 1, kv->size, file ) == kv->size;
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

/* The settings of one program. -1 means the global setting applies. */
struct launcher_settings
{
    char title[128];  /* empty: the title from the program's resources */
    int hidden;       /* left out of the library */
    int verbose;      /* verbose traces */
    int profile;      /* the sampling profiler */
    int framebuffer;  /* 1: windows go to the framebuffer, 0: through the compositor */
    int dxvk;         /* architecture-specific DXVK payload */
    char vkd3d_version[32];
    char dxvk_version[32]; /* empty: the bundled latest release */
    int dxvk_hud;
    int frame_limit;
    int vsync;
    int lsfg_enabled;
    int lsfg_performance;
    int lsfg_flow;
    int upscaling;
    int upscaling_sharpness;
    /* Whether the program's own keys apply over the shared ones: -1 they do
     * when it has a file of them, which is what a card written before this
     * setting existed means; 0 Autorun's keys alone, the file kept for when it
     * is turned on again. */
    int own_controls;
    /* What the program needs of the address space (launcher_catalog.h):
     * -1 read it from the program itself, 0 any, 1 the low 4 GB. */
    int address_space;
};

enum {
    LAUNCHER_FRAME_LIMIT_COUNT = 8,
    LAUNCHER_HUD_COUNT = 4,
    LAUNCHER_UPSCALING_COUNT = 3,
    LAUNCHER_SHARPNESS_COUNT = 6
};
static const int launcher_frame_limits[] = { 0, 30, 40, 45, 60, 75, 90, 120 };
static const char *const launcher_frame_limit_labels[] =
    { "Off", "30", "40", "45", "60", "75", "90", "120" };
static const char *const launcher_hud_labels[] = { "Disabled", "FPS", "Compact", "Full" };
static const char *const launcher_hud_values[] =
    { "0", "fps", "api,fps,frametimes", "version,api,devinfo,fps,memory,frametimes,compiler" };
static const char *const launcher_lsfg_flow_labels[] = { "12.5%", "25%", "50%" };
static const char *const launcher_lsfg_flow_values[] = { "0.125", "0.25", "0.5" };
static const char *const launcher_upscaling_labels[] = { "Off (Bilinear)", "FSR 1.0", "Integer" };
static const char *const launcher_upscaling_values[] = { "off", "fsr", "integer" };
static const char *const launcher_sharpness_labels[] = { "0%", "20%", "40%", "60%", "80%", "100%" };
static const float launcher_sharpness_values[] = { 0.0f, 0.2f, 0.4f, 0.6f, 0.8f, 1.0f };

static inline int launcher_dxvk_config( const struct launcher_settings *settings, char *out, size_t size )
{
    int length;

    if (settings->frame_limit < 0 || settings->frame_limit >= LAUNCHER_FRAME_LIMIT_COUNT ||
        settings->dxvk_hud < 0 || settings->dxvk_hud >= LAUNCHER_HUD_COUNT) return 0;
    length = snprintf( out, size, "dxgi.syncInterval = %d\nd3d9.presentInterval = %d\n",
                       !!settings->vsync, !!settings->vsync );
    if (length < 0 || (size_t)length >= size) return 0;
    if (settings->frame_limit)
    {
        int extra = snprintf( out + length, size - length,
                              "dxgi.maxFrameRate = %d\nd3d9.maxFrameRate = %d\ndxvk.maxFrameRate = %d\n",
                              launcher_frame_limits[settings->frame_limit],
                              launcher_frame_limits[settings->frame_limit],
                              launcher_frame_limits[settings->frame_limit] );
        if (extra < 0 || (size_t)extra >= size - length) return 0;
    }
    return 1;
}

static inline const char *launcher_dxvk_directory( unsigned short machine )
{
    if (machine == 0x014c) return "dxvk";
    if (machine == 0x8664) return "dxvk64";
    return NULL;
}

static inline int launcher_dxvk_version_valid( const char *version )
{
    const unsigned char *p = (const unsigned char *)version;

    if (!p || !isalnum( *p )) return 0;
    for (; *p; p++)
        if (!isalnum( *p ) && *p != '.' && *p != '-' && *p != '_') return 0;
    return p - (const unsigned char *)version < 32 && isalnum( p[-1] );
}

static inline int launcher_dxvk_version_selectable( const char *version )
{
    char *end;
    unsigned long major;

    if (!launcher_dxvk_version_valid( version )) return 0;
    major = strtoul( version, &end, 10 );
    return end != version && major >= 1;
}

static inline int launcher_dxvk_version_directory( unsigned short machine, const char *version,
                                                   char *out, size_t size )
{
    const char *base = launcher_dxvk_directory( machine );
    int length;

    if (!base || !out || !size || (version && version[0] && !launcher_dxvk_version_valid( version ))) return 0;
    if (version && version[0]) length = snprintf( out, size, "%s\\versions\\%s", base, version );
    else length = snprintf( out, size, "%s", base );
    return length >= 0 && (size_t)length < size;
}

static inline int launcher_vkd3d_version_directory( unsigned short machine, const char *version,
                                                   char *out, size_t size )
{
    const char *base = machine == 0x014c ? "vkd3d" : machine == 0x8664 ? "vkd3d64" : NULL;
    int length;

    if (!base || !out || !size || (version && version[0] && !launcher_dxvk_version_valid( version ))) return 0;
    if (version && version[0]) length = snprintf( out, size, "%s\\versions\\%s", base, version );
    else length = snprintf( out, size, "%s", base );
    return length >= 0 && (size_t)length < size;
}

static inline int launcher_settings_path( const char *exe_path, char *out, size_t size )
{
    size_t len = strlen( exe_path ), suffix_size = sizeof(".wine-nx.txt");

    if (len < 4 || strcasecmp( exe_path + len - 4, ".exe" ) || len - 4 + suffix_size > size) return 0;
    memcpy( out, exe_path, len - 4 );
    memcpy( out + len - 4, ".wine-nx.txt", suffix_size );
    return 1;
}

static inline int launcher_settings_on_usb( const char *exe_path )
{
    return exe_path && !strncasecmp( exe_path, "ums", 3 ) && isdigit( (unsigned char)exe_path[3] ) &&
           exe_path[4] == ':';
}

static inline int launcher_program_settings_path( const char *runtime_dir, const char *exe_path,
                                                  char *out, size_t size )
{
    unsigned long long hash = 1469598103934665603ULL;
    const unsigned char *p;
    int length;

    if (!launcher_settings_on_usb( exe_path )) return launcher_settings_path( exe_path, out, size );
    if (!runtime_dir || !runtime_dir[0]) return 0;
    for (p = (const unsigned char *)exe_path; *p; p++)
    {
        unsigned char c = *p == '\\' ? '/' : (unsigned char)tolower( *p );

        hash ^= c;
        hash *= 1099511628211ULL;
    }
    length = snprintf( out, size, "%s%sprogram-settings/%016llx.wine-nx.txt", runtime_dir,
                       runtime_dir[strlen( runtime_dir ) - 1] == '/' ? "" : "/", hash );
    return length >= 0 && (size_t)length < size;
}

static inline int launcher_setting_state( const struct launcher_kv *kv, const char *key )
{
    char value[16];

    if (!launcher_kv_get( kv, key, value, sizeof(value) )) return -1;
    if (!strcmp( value, "1" ) || !strcasecmp( value, "on" )) return 1;
    if (!strcmp( value, "0" ) || !strcasecmp( value, "off" )) return 0;
    return -1;
}

static inline void launcher_settings_read( const struct launcher_kv *kv, struct launcher_settings *settings )
{
    char value[64];

    if (!launcher_kv_get( kv, "title", settings->title, sizeof(settings->title) )) settings->title[0] = 0;
    settings->hidden = launcher_setting_state( kv, "hidden" ) == 1;
    settings->verbose = launcher_setting_state( kv, "verbose" );
    settings->profile = launcher_setting_state( kv, "profile" );
    settings->framebuffer = -1;
    if (launcher_kv_get( kv, "windows", value, sizeof(value) ))
    {
        if (!strcasecmp( value, "framebuffer" )) settings->framebuffer = 1;
        else if (!strcasecmp( value, "compositor" )) settings->framebuffer = 0;
    }
    if (!launcher_kv_get( kv, "d3d", value, sizeof(value) ) &&
        !launcher_kv_get( kv, "d3d9", value, sizeof(value) )) value[0] = 0;
    settings->dxvk = !strcasecmp( value, "dxvk" );
    settings->vkd3d_version[0] = 0;
    if (launcher_kv_get( kv, "vkd3d-version", value, sizeof(value) ) && launcher_dxvk_version_valid( value ))
        memcpy( settings->vkd3d_version, value, strlen( value ) + 1 );
    settings->dxvk_version[0] = 0;
    if (launcher_kv_get( kv, "dxvk-version", value, sizeof(value) ) && launcher_dxvk_version_valid( value ))
        memcpy( settings->dxvk_version, value, strlen( value ) + 1 );
    settings->own_controls = launcher_setting_state( kv, "own-controls" );
    settings->dxvk_hud = 0;
    if (launcher_kv_get( kv, "dxvk-hud", value, sizeof(value) ))
        for (int i = 1; i < LAUNCHER_HUD_COUNT; i++)
            if (!strcasecmp( value, launcher_hud_values[i] )) settings->dxvk_hud = i;
    settings->frame_limit = 0;
    if (launcher_kv_get( kv, "frame-limit", value, sizeof(value) ))
        for (int i = 1; i < LAUNCHER_FRAME_LIMIT_COUNT; i++)
            if (!strcasecmp( value, launcher_frame_limit_labels[i] )) settings->frame_limit = i;
    settings->vsync = launcher_setting_state( kv, "vsync" ) != 0;
    settings->lsfg_enabled = launcher_setting_state( kv, "lsfg" ) == 1;
    settings->lsfg_performance = launcher_setting_state( kv, "lsfg-performance" ) != 0;
    settings->lsfg_flow = 1;
    if (launcher_kv_get( kv, "lsfg-flow", value, sizeof(value) ))
        for (int i = 0; i < 3; i++)
            if (!strcasecmp( value, launcher_lsfg_flow_values[i] )) settings->lsfg_flow = i;
    settings->upscaling = 0;
    if (launcher_kv_get( kv, "upscaling", value, sizeof(value) ) ||
        launcher_kv_get( kv, "upscale", value, sizeof(value) ))
    {
        for (int i = 0; i < LAUNCHER_UPSCALING_COUNT; i++)
            if (!strcasecmp( value, launcher_upscaling_values[i] )) settings->upscaling = i;
        if (!strcasecmp( value, "1" )) settings->upscaling = 1;
        else if (!strcasecmp( value, "2" )) settings->upscaling = 2;
    }
    settings->upscaling_sharpness = 2;
    if (launcher_kv_get( kv, "upscaling-sharpness", value, sizeof(value) ) ||
        launcher_kv_get( kv, "sharpness", value, sizeof(value) ))
    {
        for (int i = 0; i < LAUNCHER_SHARPNESS_COUNT; i++)
            if (!strcasecmp( value, launcher_sharpness_labels[i] )) settings->upscaling_sharpness = i;
    }
    settings->address_space = -1;
    if (launcher_kv_get( kv, "address-space", value, sizeof(value) ))
    {
        if (!strcasecmp( value, "32-bit" ) || !strcmp( value, "32" )) settings->address_space = 1;
        else if (!strcasecmp( value, "any" )) settings->address_space = 0;
    }
}

/* Store settings, leaving out what matches the global settings. */
static inline int launcher_settings_write( struct launcher_kv *kv, const struct launcher_settings *settings )
{
    static const char *states[] = { NULL, "0", "1" };

    if (settings->dxvk_hud < 0 || settings->dxvk_hud >= LAUNCHER_HUD_COUNT ||
        settings->frame_limit < 0 || settings->frame_limit >= LAUNCHER_FRAME_LIMIT_COUNT ||
        settings->lsfg_flow < 0 || settings->lsfg_flow >= 3 ||
        settings->upscaling < 0 || settings->upscaling >= LAUNCHER_UPSCALING_COUNT ||
        settings->upscaling_sharpness < 0 || settings->upscaling_sharpness >= LAUNCHER_SHARPNESS_COUNT) return 0;
    return launcher_kv_set( kv, "title", settings->title[0] ? settings->title : NULL ) &&
           launcher_kv_set( kv, "hidden", settings->hidden ? "1" : NULL ) &&
           launcher_kv_set( kv, "verbose", states[settings->verbose + 1] ) &&
           launcher_kv_set( kv, "profile", states[settings->profile + 1] ) &&
           launcher_kv_set( kv, "windows", settings->framebuffer < 0 ? NULL :
                                           settings->framebuffer ? "framebuffer" : "compositor" ) &&
           launcher_kv_set( kv, "d3d9", NULL ) &&
           launcher_kv_set( kv, "d3d", settings->dxvk ? "dxvk" : NULL ) &&
           launcher_kv_set( kv, "vkd3d-version", settings->vkd3d_version[0] ? settings->vkd3d_version : NULL ) &&
           launcher_kv_set( kv, "dxvk-version", settings->dxvk_version[0] ? settings->dxvk_version : NULL ) &&
           launcher_kv_set( kv, "dxvk-hud", settings->dxvk_hud ? launcher_hud_values[settings->dxvk_hud] : NULL ) &&
           launcher_kv_set( kv, "frame-limit", settings->frame_limit ?
                            launcher_frame_limit_labels[settings->frame_limit] : NULL ) &&
           launcher_kv_set( kv, "vsync", settings->vsync ? NULL : "0" ) &&
           launcher_kv_set( kv, "lsfg", settings->lsfg_enabled ? "1" : NULL ) &&
           launcher_kv_set( kv, "lsfg-performance", settings->lsfg_performance ? NULL : "0" ) &&
           launcher_kv_set( kv, "lsfg-flow", settings->lsfg_flow == 1 ? NULL :
                            launcher_lsfg_flow_values[settings->lsfg_flow] ) &&
           launcher_kv_set( kv, "upscaling", settings->upscaling ?
                            launcher_upscaling_values[settings->upscaling] : NULL ) &&
           launcher_kv_set( kv, "upscaling-sharpness", settings->upscaling == 1 && settings->upscaling_sharpness != 2 ?
                            launcher_sharpness_labels[settings->upscaling_sharpness] : NULL ) &&
           launcher_kv_set( kv, "own-controls", states[settings->own_controls + 1] ) &&
           launcher_kv_set( kv, "address-space", settings->address_space < 0 ? NULL :
                                                 settings->address_space ? "32-bit" : "any" );
}

#endif
