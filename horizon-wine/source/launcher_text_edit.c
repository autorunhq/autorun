/* Copyright 2026 Wine-NX contributors. LGPL-2.1-or-later.
 *
 * The launcher's editor for a game's text files: the settings a game keeps in
 * an .ini or a .cfg beside it, and that its own menu may not show (the
 * resolution of a game without a setup program that still runs, a value a
 * fix's guide gives). One line a row; an .ini setting shows its key and its
 * value. The file is read whole, changed in memory, and written back only when
 * the user says so on leaving, as it was but for the lines changed
 * (launcher_text.h).
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "launcher.h"
#include "launcher_text.h"
#include "launcher_text_edit.h"

#define TEXT_EDIT_MAX_SIZE  (2u << 20)
#define TEXT_EDIT_MAX_LINES 8000
/* What the keyboard takes (launcher_platform_prompt: swkbd's 500 characters). */
#define TEXT_EDIT_PROMPT    500

int launcher_text_edit_offers( const char *name )
{
    static const char *const extensions[] =
    {
        ".ini", ".cfg", ".conf", ".config", ".txt", ".xml", ".json", ".inf", ".lua", ".properties",
        ".yml", ".yaml", ".toml", ".cvars", ".bat", ".cmd",
    };
    size_t len = strlen( name );

    /* A program's Autorun settings beside it are the menu's to change. */
    if (name[0] == '.' || (len > 12 && !strcasecmp( name + len - 12, ".wine-nx.txt" ))) return 0;
    for (size_t i = 0; i < sizeof(extensions) / sizeof(extensions[0]); i++)
    {
        size_t ext = strlen( extensions[i] );
        if (len > ext && !strcasecmp( name + len - ext, extensions[i] )) return 1;
    }
    return 0;
}

static void trace( const char *format, ... ) __attribute__((format(printf, 1, 2)));
static void trace( const char *format, ... )
{
    char line[700];
    va_list args;

    va_start( args, format );
    vsnprintf( line, sizeof(line), format, args );
    va_end( args );
    wine_nx_runtime_trace( line );
}

static unsigned char *read_file( const char *path, size_t *size, int *too_big )
{
    unsigned char *data = NULL;
    struct stat st;
    FILE *file;

    *too_big = 0;
    if (stat( path, &st ) || !(file = fopen( path, "rb" ))) return NULL;
    if ((unsigned long long)st.st_size > TEXT_EDIT_MAX_SIZE) *too_big = 1;
    else if ((data = malloc( st.st_size + 1 )))
    {
        *size = fread( data, 1, st.st_size, file );
        if (ferror( file )) { free( data ); data = NULL; }
    }
    fclose( file );
    return data;
}

/* Written beside the file first, then put in its place, so that a card pulled
 * out or a full one leaves the old file rather than half of the new. The
 * card's file system renames onto no file that exists: the old one steps aside
 * first and comes back if the new one cannot take its name. */
static int write_file( const char *path, const unsigned char *data, size_t size )
{
    char fresh[600], old[600];
    FILE *file;
    int ok;

    if ((size_t)snprintf( fresh, sizeof(fresh), "%s.autorun-new", path ) >= sizeof(fresh) ||
        (size_t)snprintf( old, sizeof(old), "%s.autorun-old", path ) >= sizeof(old))
        return 0;
    if (!(file = fopen( fresh, "wb" ))) return 0;
    ok = fwrite( data, 1, size, file ) == size;
    ok &= !fflush( file );
    ok &= !fclose( file );
    if (!ok)
    {
        remove( fresh );
        return 0;
    }
    remove( old );
    if (rename( path, old ))
    {
        remove( fresh );
        return 0;
    }
    if (rename( fresh, path ))
    {
        rename( old, path );
        remove( fresh );
        return 0;
    }
    remove( old );
    return 1;
}

/* Text for a row: cut to size at the end of a character, with tabs as spaces
 * (the font has no glyph for them). */
static void display( char *out, size_t size, const char *text, size_t len )
{
    size_t n = 0, pos = 0;

    while (pos < len)
    {
        size_t next = pos;

        if (launcher_text_utf8_next( (const unsigned char *)text, len, &next ) < 0) next = pos + 1;
        if (n + (next - pos) >= size) break;
        while (pos < next)
        {
            out[n++] = text[pos] == '\t' ? ' ' : text[pos];
            pos++;
        }
    }
    out[n] = 0;
}

static void make_row( struct ui_row *row, const char *line )
{
    size_t key, key_len, value;

    memset( row, 0, sizeof(*row) );
    if (launcher_text_setting( line, &key, &key_len, &value ))
    {
        display( row->label, sizeof(row->label), line + key, key_len );
        display( row->value, sizeof(row->value), line + value, strlen( line + value ) );
    }
    else display( row->label, sizeof(row->label), line, strlen( line ) );
}

/* The keyboard's answer on one line: a line end typed into it is a space. */
static void one_line( char *text )
{
    for (; *text; text++) if (*text == '\r' || *text == '\n') *text = ' ';
}

static int too_long( struct ui *ui, const char *text )
{
    if (strlen( text ) <= TEXT_EDIT_PROMPT) return 0;
    ui_message( ui, "Text file", "This line is longer than the keyboard takes (500 characters), so it cannot be "
                "changed here. Edit it on a computer." );
    return 1;
}

/* A changes the line: the value of a setting, keeping its key and what stands
 * between them, or the whole of any other line. */
static int edit_line( struct ui *ui, struct launcher_text *text, int index, int whole )
{
    const char *line = text->lines[index];
    char edited[TEXT_EDIT_PROMPT + 1], header[100], *joined;
    size_t key, key_len, value;
    int ok;

    if (!whole && launcher_text_setting( line, &key, &key_len, &value ))
    {
        display( header, sizeof(header), line + key, key_len );
        if (too_long( ui, line + value )) return 0;
        if (!launcher_platform_prompt( header, line + value, edited, sizeof(edited) )) return 0;
        one_line( edited );
        if (!strcmp( edited, line + value )) return 0;
        if (!(joined = malloc( value + strlen( edited ) + 1 ))) return 0;
        memcpy( joined, line, value );
        strcpy( joined + value, edited );
        ok = launcher_text_set( text, index, joined );
        free( joined );
        return ok;
    }
    snprintf( header, sizeof(header), "Line %d", index + 1 );
    if (too_long( ui, line )) return 0;
    if (!launcher_platform_prompt( header, line, edited, sizeof(edited) )) return 0;
    one_line( edited );
    if (!strcmp( edited, line )) return 0;
    return launcher_text_set( text, index, edited );
}

static int line_menu( struct ui *ui, struct launcher_text *text, int *selection )
{
    static const char *const items[] = { "Change the whole line", "Add a line below", "Add a line above",
                                         "Remove this line" };
    char added[TEXT_EDIT_PROMPT + 1];
    int index = *selection, choice = ui_menu( ui, "Line", items, 4, 0 ), at;

    switch (choice)
    {
    case 0:
        return edit_line( ui, text, index, 1 );
    case 1:
    case 2:
        if (text->count >= TEXT_EDIT_MAX_LINES)
        {
            ui_message( ui, "Text file", "The editor takes no more than 8000 lines." );
            return 0;
        }
        if (!launcher_platform_prompt( "New line", "", added, sizeof(added) )) return 0;
        one_line( added );
        at = choice == 1 ? index + 1 : index;
        if (!launcher_text_insert( text, at, added )) return 0;
        *selection = at;
        return 1;
    case 3:
        launcher_text_delete( text, index );
        /* A file keeps one line, empty, rather than none to stand on. */
        if (!text->count) launcher_text_insert( text, 0, "" );
        if (*selection >= text->count) *selection = text->count - 1;
        return 1;
    }
    return 0;
}

void launcher_text_edit( struct ui *ui, const char *path, const char *name )
{
    struct launcher_text text;
    struct ui_list list = {0};
    struct ui_row *rows = NULL;
    enum launcher_text_result result;
    unsigned char *data, *bytes;
    char context[200];
    size_t size = 0;
    int too_big, changed = 0, i;

    if (!(data = read_file( path, &size, &too_big )))
    {
        ui_message( ui, "Text file", too_big ? "This file is too large for the editor (over 2 MB)."
                                             : "The file could not be read." );
        return;
    }
    result = launcher_text_parse( &text, data, size, TEXT_EDIT_MAX_LINES );
    free( data );
    if (result)
    {
        ui_message( ui, "Text file", result == LAUNCHER_TEXT_BINARY ? "This is not a text file."
                    : result == LAUNCHER_TEXT_TOO_LONG ? "This file has too many lines for the editor (over 8000)."
                    : "There is not enough memory to open this file." );
        return;
    }
    trace( "[LAUNCHER] Text editor: %s, %d lines, encoding %d", path, text.count, text.encoding );
    list.choose_label = "Change";
    list.extra_label = "Line";

    for (;;)
    {
        enum ui_action action;
        struct ui_row *grown;

        if (!(grown = realloc( rows, (text.count + 1) * sizeof(*rows) ))) break;
        rows = grown;
        for (i = 0; i < text.count; i++) make_row( rows + i, text.lines[i] );
        snprintf( context, sizeof(context), "%s%s", name, changed ? " (changed)" : "" );
        action = ui_list_run( ui, &list, "Text file", context, rows, text.count, 0 );
        if (action == UI_ACTION_QUIT) break;
        if (action == UI_ACTION_CHOOSE) changed |= edit_line( ui, &text, list.selection, 0 );
        else if (action == UI_ACTION_EXTRA) changed |= line_menu( ui, &text, &list.selection );
        else if (action == UI_ACTION_BACK)
        {
            static const struct ui_hint hints[] = { { UI_A, "Save" }, { UI_X, "Don't save" }, { UI_B, "Keep editing" } };
            char message[400];
            int answer;

            if (!changed) break;
            snprintf( message, sizeof(message), "%s was changed. Save it?", name );
            answer = ui_ask( ui, "Save changes", message, hints, 3 );
            if (answer == UI_X) break;
            if (answer != UI_A) continue;
            if (!(bytes = launcher_text_build( &text, &size )) || !write_file( path, bytes, size ))
            {
                free( bytes );
                trace( "[LAUNCHER] Text editor: %s could not be saved", path );
                ui_message( ui, "Text file", "The file could not be saved. It was left as it was." );
                continue;
            }
            free( bytes );
            trace( "[LAUNCHER] Text editor: saved %s, %u bytes", path, (unsigned int)size );
            ui_toast( ui, "Saved", 1500 );
            break;
        }
    }
    free( rows );
    launcher_text_free( &text );
}
