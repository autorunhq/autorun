/*
 * Which module a static unixlib belongs to.
 *
 * Horizon has no dlopen, so ntdll serves its own unixlibs from a table
 * (virtual.c) and pairs a module with its entry by name. FEX's CPU modules
 * reach their unixlib twice: by the file name the loader gave them
 * (MemoryWineUnixFuncs), and by their own canonical stem,
 * "libwow64fex" with no extension (MemoryWineLoadUnixLibByName, which is what
 * FEXUnixLib::Init asks for first).
 *
 * A variant build sits beside the stock one as libwow64fex-<variant>.dll and
 * libarm64ecfex-<variant>.dll (fex-variant.txt, or a game's CPU translator).
 * Both spellings have to find the same entry. The original table held only
 * "libwow64fex.dll", so both lookup methods missed for a variant. The Horizon
 * CRT treats failure to acquire the unixlib as STATUS_DLL_INIT_FAILED before
 * it reads the config or executes any guest instructions.
 */
#ifndef WINE_NX_HORIZON_UNIXLIB_NAMES_H
#define WINE_NX_HORIZON_UNIXLIB_NAMES_H

#include <stddef.h>

/* Diagnostic buffer matching runtime_fex_cpu_module's names[80]. */
#define WINE_NX_UNIXLIB_NAME_MAX 80

static inline int wine_nx_unixlib_char_equal( unsigned short ch, char expected )
{
    if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
    if (expected >= 'A' && expected <= 'Z') expected += 'a' - 'A';
    return ch == (unsigned char)expected;
}

static inline int wine_nx_unixlib_has_prefix( const unsigned short *buffer, size_t length, const char *prefix )
{
    size_t i, count = 0;

    while (prefix[count]) count++;
    if (!buffer || length < count) return 0;
    for (i = 0; i < count; i++)
        if (!wine_nx_unixlib_char_equal( buffer[i], prefix[i] )) return 0;
    return 1;
}

/* The only extension a module name may carry here. */
static inline int wine_nx_unixlib_is_dll_extension( const unsigned short *rest, size_t length )
{
    return length == 4 && rest[0] == '.' && wine_nx_unixlib_char_equal( rest[1], 'd' ) &&
           wine_nx_unixlib_char_equal( rest[2], 'l' ) && wine_nx_unixlib_char_equal( rest[3], 'l' );
}

/* A variant suffix: the canonical stem on its own, "<stem>.dll", "<stem>-x" or
 * "<stem>-x.dll", where x is what the runtime calls a variant (letters, digits,
 * '-' and '_'). Nothing else, so a different module that merely begins with
 * the same letters does not pair. */
static inline int wine_nx_unixlib_variant_suffix_ok( const unsigned short *rest, size_t length )
{
    size_t i, name = length;

    if (!length) return 1;
    if (rest[0] == '.') return wine_nx_unixlib_is_dll_extension( rest, length );
    if (rest[0] != '-') return 0;
    if (length > 4 && wine_nx_unixlib_is_dll_extension( rest + length - 4, 4 )) name = length - 4;
    if (name <= 1) return 0;
    for (i = 1; i < name; i++)
    {
        unsigned short ch = rest[i];
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
            ch == '-' || ch == '_')
            continue;
        return 0;
    }
    return 1;
}

/* Does the module name pair with this table entry? `stem` is the expected name,
 * without its extension if `variants` is set. Variant entries are also shipped
 * under a suffixed name. Lengths are in characters, as
 * UNICODE_STRING.Length / sizeof(WCHAR). */
static inline int wine_nx_unixlib_name_matches( const unsigned short *buffer, size_t length,
                                                const char *stem, int variants )
{
    size_t count = 0;

    while (stem[count]) count++;
    if (!buffer || length < count) return 0;
    if (!wine_nx_unixlib_has_prefix( buffer, length, stem )) return 0;
    if (!variants) return length == count;
    return wine_nx_unixlib_variant_suffix_ok( buffer + count, length - count );
}

/* Names in FEX's namespace, even malformed suffixes, for diagnostics only. */
static inline int wine_nx_unixlib_name_is_fex( const unsigned short *buffer, size_t length )
{
    return wine_nx_unixlib_has_prefix( buffer, length, "libwow64fex" ) ||
           wine_nx_unixlib_has_prefix( buffer, length, "libarm64ecfex" );
}

/* A name for the log, truncated rather than overrunning `size`. */
static inline void wine_nx_unixlib_name_format( const unsigned short *buffer, size_t length,
                                                char *out, size_t size )
{
    size_t i;

    if (!out || !size) return;
    if (!buffer) length = 0;
    if (length > size - 1) length = size - 1;
    for (i = 0; i < length; i++) out[i] = buffer[i] < 0x20 || buffer[i] > 0x7e ? '?' : (char)buffer[i];
    out[length] = 0;
}

#endif /* WINE_NX_HORIZON_UNIXLIB_NAMES_H */
