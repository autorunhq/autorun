/* The Sims 2 Ultimate Collection setup for Wine-NX: what the release's
 * "Instalar Registros" batch file writes, written from inside the card.
 *
 * The game is shipped installed -- Base, the expansions and the stuff packs are
 * whole folders of data -- and the only thing left to do is tell it where each
 * one is. The batch file is nothing but reg add lines, and every path in it
 * comes from %CD%, so it cannot be shipped as a registry file either: the card
 * puts the game somewhere else than the computer it was unpacked on. This does
 * the same from wherever it is run: the packs live beside this program's own
 * folder, and their paths are written as the game will read them.
 *
 * - Software\Electronic Arts\The Sims 2 Ultimate Collection 25, with the list
 *   of expansions the game looks through. The list keeps the order and the
 *   empty place the release's own has: the game reads it by position.
 * - A key for each pack that is really there, with Installed and its Path;
 *   one for a pack that is not would send the game to a folder with nothing in
 *   it. The base game and the last expansion also carry Game Registry, which
 *   is where the game looks for the rest.
 * - 1.0\language, the number the batch file asks for, and the locale the same
 *   number stands for under Software\Maxis\The Sims 2 Legacy. The release's
 *   own anadius.cfg sets its language to "invalid" so that the game reads that
 *   second key instead, and asks for it by name: without it the game says
 *   "open: Invalid handle" and stops before it starts. language.txt beside this
 *   program holds the number, and without one it is 1, English (United States);
 *   the numbers are in the readme.
 * - vidc.VP60 and vidc.VP61 under Drivers32, which is what the release's
 *   vp6.reg holds. Not for the game's own movies, which are Maxis' own format
 *   and want no codec, but for the video it writes: it records gameplay
 *   through Video for Windows, which finds a codec by those two names, and
 *   reads a custom video made as a VP6 AVI the same way. The DLL is the
 *   release's to copy, nobody else having it to give away, and registering a
 *   codec that is not there costs nothing.
 * - dxvk.conf, from beside this program, into each pack's TSBin next to its
 *   executable, where DXVK reads it: the game's own DXVK profile reports 2 GB
 *   of video memory, and on the Switch it fills the shared 1.5 GB and
 *   crashes. One already there is the player's and is left alone.
 * - Graphics Rules.sgr, in each pack's TSData\Res\Config, edited where it
 *   is rather than shipped (it is the game's): in its ScreenModeResolution
 *   option every default becomes 1280x720, the Switch's screen, and a maximum
 *   below that is raised to it. Nothing else in the file changes; the file as
 *   it was is kept once as Graphics Rules.sgr.original.
 *
 * Each step is reported to autorun_runtime.log as a [SIMS2 SETUP] line; the
 * exit code is 0 when every step worked. Running it again is harmless. */
#include <windows.h>
#include <winternl.h>

__declspec(dllimport) NTSTATUS NTAPI NtDisplayString( const UNICODE_STRING *str );

/* Built without a C runtime; the compiler still expects these for structures. */
void *memset( void *dst, int c, size_t n )
{
    unsigned char *p = dst;
    while (n--) *p++ = (unsigned char)c;
    return dst;
}

void *memcpy( void *dst, const void *src, size_t n )
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    while (n--) *d++ = *s++;
    return dst;
}

static unsigned int wide_length( const WCHAR *text )
{
    unsigned int n = 0;
    while (text[n]) n++;
    return n;
}

static void wide_append( WCHAR *out, unsigned int *at, unsigned int max, const WCHAR *text )
{
    while (*text && *at + 1 < max) out[(*at)++] = *text++;
    out[*at] = 0;
}

/* One line: "[SIMS2 SETUP] <label> <name>: <result>", with the value in hex. */
static void report( const char *label, const WCHAR *name, const char *result, DWORD value )
{
    static const char hex[] = "0123456789abcdef";
    const char *prefix = "[SIMS2 SETUP] ";
    WCHAR buffer[400];
    UNICODE_STRING str;
    unsigned int n = 0, i;

    while (*prefix) buffer[n++] = *prefix++;
    while (*label && n < 100) buffer[n++] = *label++;
    if (name)
    {
        buffer[n++] = ' ';
        while (*name && n < 320) buffer[n++] = *name++;
    }
    buffer[n++] = ':';
    buffer[n++] = ' ';
    while (*result && n < 380) buffer[n++] = *result++;
    buffer[n++] = ' ';
    buffer[n++] = '0';
    buffer[n++] = 'x';
    for (i = 0; i < 8; i++) buffer[n++] = hex[(value >> (28 - i * 4)) & 15];
    /* No newline: the log makes a line of each call, and anything that is not
     * plain ASCII reaches it as a question mark. */
    str.Buffer = buffer;
    str.Length = (USHORT)(n * sizeof(WCHAR));
    str.MaximumLength = str.Length;
    NtDisplayString( &str );
}

static LONG set_value_in( HKEY root, const WCHAR *path, const WCHAR *name, DWORD type,
                          const BYTE *data, DWORD size )
{
    HKEY key;
    LONG status;

    if ((status = RegCreateKeyExW( root, path, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL ))) return status;
    status = RegSetValueExW( key, name, 0, type, data, size );
    RegCloseKey( key );
    return status;
}

static LONG set_value( const WCHAR *path, const WCHAR *name, DWORD type, const BYTE *data, DWORD size )
{
    return set_value_in( HKEY_CURRENT_USER, path, name, type, data, size );
}

/* A codec is the machine's, not one person's. */
static BOOL set_machine_string( const WCHAR *path, const WCHAR *name, const WCHAR *value )
{
    LONG status = set_value_in( HKEY_LOCAL_MACHINE, path, name, REG_SZ, (const BYTE *)value,
                                (wide_length( value ) + 1) * sizeof(WCHAR) );

    report( "set", name, status ? "failed, error" : "ok", (DWORD)status );
    return !status;
}

static BOOL set_string( const WCHAR *path, const WCHAR *name, const WCHAR *value )
{
    LONG status = set_value( path, name, REG_SZ, (const BYTE *)value,
                             (wide_length( value ) + 1) * sizeof(WCHAR) );

    report( "set", name, status ? "failed, error" : "ok", (DWORD)status );
    return !status;
}

static BOOL set_dword( const WCHAR *path, const WCHAR *name, DWORD value )
{
    LONG status = set_value( path, name, REG_DWORD, (const BYTE *)&value, sizeof(value) );

    report( "set", name, status ? "failed, error" : "ok, value", status ? (DWORD)status : value );
    return !status;
}

static BOOL set_machine_dword( const WCHAR *path, const WCHAR *name, DWORD value )
{
    LONG status = set_value_in( HKEY_LOCAL_MACHINE, path, name, REG_DWORD,
                                (const BYTE *)&value, sizeof(value) );

    report( "set", name, status ? "failed, error" : "ok, value", status ? (DWORD)status : value );
    return !status;
}

/* The folder this program is in, without its name or the trailing slash --
 * except at the root of a drive, where the slash is part of the name. */
static BOOL own_folder( WCHAR *out, unsigned int max )
{
    unsigned int n = GetModuleFileNameW( NULL, out, max );

    if (!n || n >= max) return FALSE;
    while (n && out[n - 1] != '\\') n--;
    if (!n) return FALSE;
    out[n > 3 ? n - 1 : n] = 0;
    return TRUE;
}

/* The folder above that one, where the game sits beside this program rather
 * than inside it: C:\The Sims 2 next to C:\The Sims 2 Setup. */
static BOOL parent_folder( const WCHAR *folder, WCHAR *out, unsigned int max )
{
    unsigned int n = 0;

    while (folder[n] && n + 1 < max) { out[n] = folder[n]; n++; }
    out[n] = 0;
    while (n && out[n - 1] != '\\') n--;
    if (!n) return FALSE;
    out[n > 3 ? n - 1 : n] = 0;
    return TRUE;
}

/* folder\name, without doubling the slash at the root of a drive. */
static void join( WCHAR *out, unsigned int max, const WCHAR *folder, const WCHAR *name )
{
    unsigned int at = 0;

    wide_append( out, &at, max, folder );
    if (at && out[at - 1] != '\\') wide_append( out, &at, max, L"\\" );
    wide_append( out, &at, max, name );
}

static BOOL file_exists( const WCHAR *path )
{
    DWORD attributes = GetFileAttributesW( path );

    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

static void rewrite_digits( char *out, unsigned int *at, unsigned int value )
{
    char digits[12];
    unsigned int n = 0;

    do digits[n++] = '0' + value % 10; while ((value /= 10) && n < sizeof(digits));
    while (n) out[(*at)++] = digits[--n];
}

static BOOL word_is( const char *p, const char *end, const char *word )
{
    while (*word && p < end && *p == *word) { p++; word++; }
    return !*word && (p == end || *p == ' ' || *p == '\t' || *p == '\r' || *p == '\n');
}

/* The part of Graphics Rules.sgr that picks the screen size: the numbers of
 * its ScreenModeResolution option, rewritten where they stand. Returns how
 * many changed, and the new text's length in *out_size. */
static unsigned int resolution_rules( const char *in, unsigned int size, char *out, unsigned int *out_size )
{
    unsigned int i = 0, at = 0, changed = 0;
    BOOL inside = FALSE;

    while (i < size)
    {
        unsigned int start = i, end = i, p;

        while (end < size && in[end] != '\n') end++;
        if (end < size) end++;  /* with its newline */
        for (p = start; p < end && (in[p] == ' ' || in[p] == '\t'); p++) ;
        if (word_is( in + p, in + end, "option" ))
        {
            unsigned int q = p + 6;

            while (q < end && (in[q] == ' ' || in[q] == '\t')) q++;
            inside = word_is( in + q, in + end, "ScreenModeResolution" );
        }
        else if (word_is( in + p, in + end, "end" )) inside = FALSE;
        else if (inside && word_is( in + p, in + end, "uintProp" ))
        {
            static const struct { const char *name; unsigned int value; BOOL at_least; } props[] =
            {
                { "defaultResWidth", 1280, FALSE }, { "defaultResHeight", 720, FALSE },
                { "maxResWidth", 1280, TRUE }, { "maxResHeight", 720, TRUE },
            };
            unsigned int q = p + 8, k, number_start, number_end, value = 0;

            while (q < end && (in[q] == ' ' || in[q] == '\t')) q++;
            for (k = 0; k < sizeof(props) / sizeof(props[0]); k++)
                if (word_is( in + q, in + end, props[k].name )) break;
            if (k < sizeof(props) / sizeof(props[0]))
            {
                number_start = q;
                while (number_start < end && in[number_start] != ' ' && in[number_start] != '\t') number_start++;
                while (number_start < end && (in[number_start] == ' ' || in[number_start] == '\t')) number_start++;
                for (number_end = number_start; number_end < end && in[number_end] >= '0' && in[number_end] <= '9';
                     number_end++)
                    value = value * 10 + (in[number_end] - '0');
                if (number_end > number_start &&
                    (props[k].at_least ? value < props[k].value : value != props[k].value))
                {
                    while (start < number_start) out[at++] = in[start++];
                    rewrite_digits( out, &at, props[k].value );
                    start = number_end;
                    changed++;
                }
            }
        }
        while (start < end) out[at++] = in[start++];
        i = end;
    }
    *out_size = at;
    return changed;
}

static BOOL read_whole( const WCHAR *path, char **data, DWORD *size )
{
    HANDLE file = CreateFileW( path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL );
    DWORD done = 0;

    if (file == INVALID_HANDLE_VALUE) return FALSE;
    *size = GetFileSize( file, NULL );
    *data = HeapAlloc( GetProcessHeap(), 0, *size + 1 );
    if (*size == INVALID_FILE_SIZE || !*data || !ReadFile( file, *data, *size, &done, NULL ) || done != *size)
    {
        CloseHandle( file );
        return FALSE;
    }
    CloseHandle( file );
    return TRUE;
}

static BOOL write_whole( const WCHAR *path, const char *data, DWORD size )
{
    HANDLE file = CreateFileW( path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL );
    DWORD done = 0;
    BOOL ok;

    if (file == INVALID_HANDLE_VALUE) return FALSE;
    ok = WriteFile( file, data, size, &done, NULL ) && done == size;
    return CloseHandle( file ) && ok;
}

/* One pack's Graphics Rules.sgr set for the Switch's screen. A pack without
 * the file is not a failure: the stuff packs have none. */
static BOOL screen_rules( const WCHAR *pack )
{
    static WCHAR path[MAX_PATH * 2], original[MAX_PATH * 2];
    unsigned int at = 0, changed, out_size;
    char *data, *out;
    DWORD size;

    wide_append( path, &at, MAX_PATH * 2, pack );
    wide_append( path, &at, MAX_PATH * 2, L"\\TSData\\Res\\Config\\Graphics Rules.sgr" );
    if (!file_exists( path )) return TRUE;
    if (!read_whole( path, &data, &size ))
    {
        report( "read", path, "failed, error", GetLastError() );
        return FALSE;
    }
    /* A number can only grow by a digit, and there are a few dozen of them. */
    if (!(out = HeapAlloc( GetProcessHeap(), 0, size + 256 ))) return FALSE;
    if (!(changed = resolution_rules( data, size, out, &out_size )))
    {
        report( "keep", path, "already 1280x720, error", 0 );
        return TRUE;
    }
    at = 0;
    wide_append( original, &at, MAX_PATH * 2, path );
    wide_append( original, &at, MAX_PATH * 2, L".original" );
    if (!file_exists( original ) && !CopyFileW( path, original, TRUE ))
    {
        report( "keep the original of", path, "failed, error", GetLastError() );
        return FALSE;
    }
    if (!write_whole( path, out, out_size ))
    {
        report( "write", path, "failed, error", GetLastError() );
        return FALSE;
    }
    report( "set 1280x720 in", path, "ok, values changed", changed );
    return TRUE;
}

/* A pack is known by the executable in its TSBin, not by the name of the
 * folder around it: one release calls them Base and EP1-EP9, another spells
 * out "The Sims 2 Nightlife", and the game itself only ever asks for a path.
 * The executable is the release-independent name, and it is the one already
 * written to the registry beside that path. */
static BOOL pack_here( const WCHAR *folder, const WCHAR *exe )
{
    WCHAR path[MAX_PATH * 2];
    unsigned int at = 0;

    wide_append( path, &at, MAX_PATH * 2, folder );
    if (at && path[at - 1] != '\\') wide_append( path, &at, MAX_PATH * 2, L"\\" );
    wide_append( path, &at, MAX_PATH * 2, L"TSBin\\" );
    wide_append( path, &at, MAX_PATH * 2, exe );
    return file_exists( path );
}

/* The folder under root that holds this pack. The name the release this setup
 * was written for uses is tried first, so that layout costs no search at all;
 * anything else is found by looking through root for the executable. */
static BOOL find_pack( const WCHAR *root, const WCHAR *exe, const WCHAR *known,
                       WCHAR *out, unsigned int max )
{
    WIN32_FIND_DATAW found;
    WCHAR pattern[MAX_PATH];
    HANDLE search;

    join( out, max, root, known );
    if (pack_here( out, exe )) return TRUE;

    join( pattern, MAX_PATH, root, L"*" );
    if ((search = FindFirstFileW( pattern, &found )) == INVALID_HANDLE_VALUE) return FALSE;
    do
    {
        if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (found.cFileName[0] == '.') continue;
        join( out, max, root, found.cFileName );
        if (pack_here( out, exe ))
        {
            FindClose( search );
            return TRUE;
        }
    } while (FindNextFileW( search, &found ));
    FindClose( search );
    return FALSE;
}



/* The number in language.txt beside this program, or 1 for English. */
static DWORD chosen_language( const WCHAR *setup_folder )
{
    WCHAR path[MAX_PATH];
    char text[16];
    DWORD read = 0, value = 0, i;
    HANDLE file;

    join( path, MAX_PATH, setup_folder, L"language.txt" );
    file = CreateFileW( path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL );
    if (file == INVALID_HANDLE_VALUE) return 1;
    if (ReadFile( file, text, sizeof(text) - 1, &read, NULL ))
        for (i = 0; i < read && text[i] >= '0' && text[i] <= '9'; i++) value = value * 10 + (DWORD)(text[i] - '0');
    CloseHandle( file );
    return value ? value : 1;
}

/* Every pack the collection has, in the order the game's own list gives them:
 * the executable it is known by, the folder one release puts it in, and the
 * name EA's own installer registers it under.
 *
 * Two families of release exist and they read different registries. One rebuilt
 * from the collection reads Software\Electronic Arts, below. One rebuilt from
 * the retail discs keeps the executables as they shipped, and those read
 * HKLM\SOFTWARE\EA GAMES\<name>\Install Dir -- Sims2EP9.exe names that key
 * itself, and the repack's own instructions point at it. The names are the
 * pack's with the punctuation dropped, so they cannot be made from the folder;
 * they are an installer's. Both are written: a key a release does not read
 * costs nothing, and which release this is cannot be told from the files. */
static const struct { const WCHAR *exe, *folder, *ea; } packs[] =
{
    { L"Sims2.exe",    L"Base", L"The Sims 2" },
    { L"Sims2EP1.exe", L"EP1", L"The Sims 2 University" },
    { L"Sims2EP2.exe", L"EP2", L"The Sims 2 Nightlife" },
    { L"Sims2EP3.exe", L"EP3", L"The Sims 2 Open For Business" },
    { L"Sims2SP1.exe", L"SP1", L"The Sims 2 Family Fun Stuff" },
    { L"Sims2SP2.exe", L"SP2", L"The Sims 2 Glamour Life Stuff" },
    { L"Sims2EP4.exe", L"EP4", L"The Sims 2 Pets" },
    { L"Sims2EP5.exe", L"EP5", L"The Sims 2 Seasons" },
    { L"Sims2SP4.exe", L"SP4", L"The Sims 2 Celebration Stuff" },
    { L"Sims2SP5.exe", L"SP5", L"The Sims 2 H M Fashion Stuff" },
    { L"Sims2EP6.exe", L"EP6", L"The Sims 2 Bon Voyage" },
    { L"Sims2SP6.exe", L"SP6", L"The Sims 2 Teen Style Stuff" },
    { L"Sims2EP7.exe", L"EP7", L"The Sims 2 FreeTime" },
    { L"Sims2SP7.exe", L"SP7", L"The Sims 2 Kitchen & Bath Interior Design Stuff" },
    { L"Sims2SP8.exe", L"SP8", L"The Sims 2 IKEA Home Stuff" },
    { L"Sims2EP8.exe", L"EP8", L"The Sims 2 Apartment Life" },
    { L"Sims2EP9.exe", L"EP9", L"The Sims 2 Mansion and Garden Stuff" },
};
#define PACK_COUNT (sizeof(packs) / sizeof(packs[0]))
#define INSTALLED_MAX 512        /* the EPsInstalled list, built from what is found */

/* The expansions the base game is told it has, which is read by position: a
 * pack that is not there leaves its place empty rather than shortening the
 * list, and the empty place the release's own list carries is kept. Naming a
 * pack whose key was never written sends the game to a key that is not there,
 * and it reports that required files have been deleted. */
static void installed_list( WCHAR *out, unsigned int max, const BOOL *found )
{
    unsigned int at = 0, i;

    out[0] = 0;
    for (i = 1; i < PACK_COUNT; i++)
    {
        if (at) wide_append( out, &at, max, L"," );
        if (found[i]) wide_append( out, &at, max, packs[i].exe );
        if (i == 11) wide_append( out, &at, max, L"," );
    }
}

/* How many of the packs are under this root. The candidate holding the most is
 * the collection: a folder left behind by an older install answers for one or
 * two packs, and taking the first root that answers for any let those win. */
static unsigned int count_packs( const WCHAR *root )
{
    WCHAR pattern[MAX_PATH], folder[MAX_PATH];
    WIN32_FIND_DATAW found;
    unsigned int i, count = 0;
    HANDLE search;

    join( pattern, MAX_PATH, root, L"*" );
    if ((search = FindFirstFileW( pattern, &found )) == INVALID_HANDLE_VALUE) return 0;
    do
    {
        if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (found.cFileName[0] == '.') continue;
        join( folder, MAX_PATH, root, found.cFileName );
        for (i = 0; i < PACK_COUNT; i++)
            if (pack_here( folder, packs[i].exe )) { count++; break; }
    } while (FindNextFileW( search, &found ));
    FindClose( search );
    return count;
}

/* Keeps the best root seen so far, and how many packs it holds. */
static void consider( const WCHAR *root, WCHAR *best, unsigned int *best_count, unsigned int max )
{
    unsigned int count = count_packs( root ), at = 0;

    if (count <= *best_count) return;
    *best_count = count;
    wide_append( best, &at, max, root );
}

/* The packs may sit one folder further down: a collection copied whole keeps
 * its own folder around them, and the setup is then beside that folder. Every
 * one of them is weighed, or the first folder holding a leftover pack wins. */
static void consider_below( const WCHAR *root, WCHAR *best, unsigned int *best_count, unsigned int max )
{
    WCHAR pattern[MAX_PATH], folder[MAX_PATH];
    WIN32_FIND_DATAW found;
    HANDLE search;

    join( pattern, MAX_PATH, root, L"*" );
    if ((search = FindFirstFileW( pattern, &found )) == INVALID_HANDLE_VALUE) return;
    do
    {
        if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (found.cFileName[0] == '.') continue;
        join( folder, MAX_PATH, root, found.cFileName );
        consider( folder, best, best_count, max );
    } while (FindNextFileW( search, &found ));
    FindClose( search );
}

void __stdcall start(void)
{
    /* The list the game reads by position, empty place and all. */
    static const WCHAR eps_installed[] =
        L"Sims2EP1.exe,Sims2EP2.exe,Sims2EP3.exe,Sims2SP1.exe,Sims2SP2.exe,Sims2EP4.exe,"
        L"Sims2EP5.exe,Sims2SP4.exe,Sims2SP5.exe,Sims2EP6.exe,Sims2SP6.exe,,Sims2EP7.exe,"
        L"Sims2SP7.exe,Sims2SP8.exe,Sims2EP8.exe,Sims2EP9.exe";
    static const WCHAR collection[] = L"Software\\Electronic Arts\\The Sims 2 Ultimate Collection 25";
    static const WCHAR drivers32[] = L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Drivers32";
    static const WCHAR ea_games[] = L"SOFTWARE\\EA GAMES\\";
    static const WCHAR app_paths[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\";
    static WCHAR executable[MAX_PATH], registry[MAX_PATH];
    WCHAR setup_folder[MAX_PATH], game[MAX_PATH], path[MAX_PATH * 2], folder[MAX_PATH];
    /* static: a path for every pack is far past this function's frame. */
    static WCHAR pack_path[PACK_COUNT][MAX_PATH], installed[INSTALLED_MAX];
    static BOOL pack_found[PACK_COUNT];
    unsigned int i, at, found = 0;
    BOOL ok = TRUE;

    report( "start", NULL, "build", 1 );
    if (!own_folder( setup_folder, MAX_PATH ))
    {
        report( "find the game", NULL, "failed: cannot read this program's own path, error", GetLastError() );
        ExitProcess( 1 );
    }
    /* Where the packs are: in here, if they were copied in beside this
     * program; in the folder the readme names, beside this one; in the folder
     * this one is in, for a setup dropped into the game itself; or one further
     * down, when the collection keeps a folder of its own around its packs.
     * Every one of those is weighed, because more than one can answer: a card
     * that held an older install has a folder with a pack or two still in it,
     * and the collection is the one with the most. */
    {
        /* static: -nostdlib has no stack probe, and these on the stack take
         * this function's frame past the 4 KB one would be emitted for. */
        static WCHAR above[MAX_PATH], named[MAX_PATH];
        unsigned int best = 0;

        game[0] = 0;
        consider( setup_folder, game, &best, MAX_PATH );
        if (parent_folder( setup_folder, above, MAX_PATH ))
        {
            join( named, MAX_PATH, above, L"The Sims 2" );
            consider( named, game, &best, MAX_PATH );
            consider( above, game, &best, MAX_PATH );
            consider_below( above, game, &best, MAX_PATH );
        }
        if (!best)
        {
            report( "find the game", setup_folder, "FAILED: no pack found here or beside it, error", 0 );
            ExitProcess( 1 );
        }
        report( "the game is in", game, "packs there", best );
    }
    ok &= set_string( collection, L"DisplayName", L"The Sims 2 Legacy" );
    ok &= set_string( collection, L"EPsInstalled", eps_installed );

    {
        /* The number the batch file asks for, and the locale it stands for:
         * the game takes one and the release's launcher emulation the other,
         * and they have to agree. */
        static const struct { DWORD number; const WCHAR *locale; } locales[] =
        {
            {  1, L"en_US" }, {  2, L"fr_FR" }, {  3, L"de_DE" }, {  4, L"it_IT" },
            {  5, L"es_ES" }, {  6, L"sv_SE" }, {  7, L"fi_FI" }, {  8, L"nl_NL" },
            {  9, L"da_DK" }, { 10, L"pt_BR" }, { 11, L"cs_CZ" }, { 13, L"en_GB" },
            { 14, L"ja_JP" }, { 15, L"ko_KR" }, { 16, L"ru_RU" }, { 17, L"zh_CN" },
            { 18, L"zh_TW" }, { 20, L"pl_PL" }, { 21, L"th_TH" }, { 22, L"no_NO" },
            { 23, L"pt_PT" }, { 24, L"hu_HU" },
        };
        DWORD language = chosen_language( setup_folder );
        const WCHAR *locale = L"en_US";
        unsigned int n;

        for (n = 0; n < sizeof(locales) / sizeof(locales[0]); n++)
            if (locales[n].number == language) locale = locales[n].locale;

        at = 0;
        wide_append( path, &at, MAX_PATH * 2, collection );
        wide_append( path, &at, MAX_PATH * 2, L"\\1.0" );
        ok &= set_dword( path, L"language", language );
        ok &= set_string( L"Software\\Maxis\\The Sims 2 Legacy", L"Locale", locale );
    }

    /* Which packs are there, before anything is written: the list below says
     * so, and a pack named in it whose key is missing sends the game looking
     * for a key that is not there -- which is how it decides that "some
     * required files have been deleted". */
    for (i = 0; i < PACK_COUNT; i++)
        pack_found[i] = find_pack( game, packs[i].exe, packs[i].folder, pack_path[i], MAX_PATH );
    installed_list( installed, INSTALLED_MAX, pack_found );

    for (i = 0; i < PACK_COUNT; i++)
    {
        unsigned int n = 0;

        wide_append( folder, &n, MAX_PATH, pack_path[i] );
        if (!pack_found[i])
        {
            /* A key for a pack that is not there would send the game to an
             * empty folder, which is worse than not knowing about it. */
            report( "skip", packs[i].exe, "not installed, error", 0 );
            continue;
        }
        found++;
        at = 0;
        wide_append( path, &at, MAX_PATH * 2, collection );
        wide_append( path, &at, MAX_PATH * 2, L"\\" );
        wide_append( path, &at, MAX_PATH * 2, packs[i].exe );
        ok &= set_dword( path, L"Installed", 1 );
        ok &= set_string( path, L"Path", folder );
        /* Where the base game and the newest expansion look for the rest. */
        if (!i || i + 1 == PACK_COUNT)
            ok &= set_string( path, L"Game Registry", collection );

        /* The same pack the way an executable kept from the retail discs asks
         * for it, under the machine rather than the user. Sims2EP9.exe names
         * DisplayName, Locale, Language, CacheSize, Region and EPsInstalled --
         * not Install Dir, which it does not contain at all -- and it stops
         * with "The Sims 2 is not installed on this system" at the first one
         * it cannot read, so all of them are written. The values are an
         * installer's: en_uk for a game whose data is UK English, and Region
         * "none", which is what that installer wrote for the stuff packs.
         * Install Dir goes with them for anything else that reads it. */
        at = 0;
        wide_append( path, &at, MAX_PATH * 2, ea_games );
        wide_append( path, &at, MAX_PATH * 2, packs[i].ea );
        ok &= set_machine_string( path, L"Install Dir", folder );
        ok &= set_machine_string( path, L"DisplayName", packs[i].ea );
        ok &= set_machine_string( path, L"Locale", L"en_uk" );
        ok &= set_machine_string( path, L"Language", L"English" );
        ok &= set_machine_string( path, L"Region", L"none" );
        ok &= set_machine_dword( path, L"CacheSize", 0x40000000 );
        /* The base game carries the list, as it does on the other side. */
        if (!i) ok &= set_machine_string( path, L"EPsInstalled", installed );
        /* And the version key the release's own instructions point at for a
         * language change: the number is the one its installer wrote. */
        wide_append( path, &at, MAX_PATH * 2, L"\\1.0" );
        ok &= set_machine_string( path, L"DisplayName", packs[i].ea );
        ok &= set_machine_string( path, L"LanguageName", L"English" );
        ok &= set_machine_dword( path, L"Language", 0x13 );

        /* And how that executable finds each pack in the first place: Windows'
         * own App Paths entry for it, which it opens by the executable's name
         * before it reads anything else -- the runtime's [REG] lines show it
         * opening App Paths\\Sims2.exe and App Paths\\Sims2EP9.exe, and then
         * reading EPsInstalled from the base game's. Without the entry that
         * read lands on the root and the game says it is not installed. The
         * values are what the collection's own setup writes for its stand-in
         * of this same key -- Path, the pack's folder, and Game Registry --
         * with the executable itself as the default, as Windows keeps it. */
        at = 0;
        wide_append( path, &at, MAX_PATH * 2, app_paths );
        wide_append( path, &at, MAX_PATH * 2, packs[i].exe );
        {
            unsigned int n = 0;

            wide_append( executable, &n, MAX_PATH, folder );
            wide_append( executable, &n, MAX_PATH, L"\\TSBin\\" );
            wide_append( executable, &n, MAX_PATH, packs[i].exe );
            n = 0;
            wide_append( registry, &n, MAX_PATH, ea_games );
            wide_append( registry, &n, MAX_PATH, packs[i].ea );
        }
        ok &= set_machine_string( path, L"", executable );
        ok &= set_machine_string( path, L"Path", folder );
        ok &= set_machine_string( path, L"Game Registry", registry );
        if (!i) ok &= set_machine_string( path, L"EPsInstalled", installed );
    }

    /* The movies' codec, by the two names Video for Windows opens it under. */
    /* DXVK's settings for the game, beside each pack's executable. */
    {
        static WCHAR source[MAX_PATH], target[MAX_PATH * 2];

        join( source, MAX_PATH, setup_folder, L"dxvk.conf" );
        for (i = 0; i < PACK_COUNT && file_exists( source ); i++)
        {
            unsigned int n = 0;

            if (!pack_found[i]) continue;
            wide_append( target, &n, MAX_PATH * 2, pack_path[i] );
            wide_append( target, &n, MAX_PATH * 2, L"\\TSBin\\dxvk.conf" );
            if (file_exists( target ))
                report( "keep", target, "already there, error", 0 );
            else if (CopyFileW( source, target, TRUE ))
                report( "copy", target, "ok, error", 0 );
            else
            {
                report( "copy", target, "failed, error", GetLastError() );
                ok = FALSE;
            }
        }
    }

    /* And each pack's screen size, in the game's own rules file. */
    for (i = 0; i < PACK_COUNT; i++)
        if (pack_found[i]) ok &= screen_rules( pack_path[i] );

    ok &= set_machine_string( drivers32, L"vidc.VP60", L"vp6vfw.dll" );
    ok &= set_machine_string( drivers32, L"vidc.VP61", L"vp6vfw.dll" );

    if (!found)
    {
        report( "done", NULL, "FAILED: no pack found beside this program, error", 0 );
        ExitProcess( 1 );
    }
    report( ok ? "done, all steps worked, packs" : "done, a step FAILED (see above), packs", NULL,
            "count", found );
    ExitProcess( !ok );
}
