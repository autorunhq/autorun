/* Copyright 2026 Wine-NX contributors. LGPL-2.1-or-later.
 *
 * Where the registry's hives live under the runtime's folder. They were
 * system.reg and user.reg in that folder itself, next to everything else; they
 * are in registry/ now, and the first start of a runtime that says so moves
 * them there, so every key a setup program or a game has written stays. A hive
 * already in registry/ is never written over, and a .tmp left by a save that
 * was cut short goes with its hive, since the loader falls back to it.
 */
#ifndef WINE_NX_HORIZON_REGISTRY_PATHS_H
#define WINE_NX_HORIZON_REGISTRY_PATHS_H

#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#define HORIZON_REGISTRY_SUBDIR "registry/"

/* dir ends with a slash. */
static inline void horizon_registry_move_hives( const char *dir )
{
    static const char *const names[] = { "system.reg", "user.reg" };
    static const char *const suffixes[] = { "", ".tmp" };
    char from[512], to[512];
    unsigned int i, j;

    snprintf( to, sizeof(to), "%s" HORIZON_REGISTRY_SUBDIR, dir );
    mkdir( to, 0777 );
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
    {
        /* A hive is moved whole or not at all: if the new place has either
         * file, the old ones are left where they are. */
        snprintf( to, sizeof(to), "%s" HORIZON_REGISTRY_SUBDIR "%s", dir, names[i] );
        if (!access( to, F_OK )) continue;
        snprintf( to, sizeof(to), "%s" HORIZON_REGISTRY_SUBDIR "%s.tmp", dir, names[i] );
        if (!access( to, F_OK )) continue;
        for (j = 0; j < sizeof(suffixes) / sizeof(suffixes[0]); j++)
        {
            snprintf( from, sizeof(from), "%s%s%s", dir, names[i], suffixes[j] );
            snprintf( to, sizeof(to), "%s" HORIZON_REGISTRY_SUBDIR "%s%s", dir, names[i], suffixes[j] );
            if (!access( from, F_OK )) rename( from, to );
        }
    }
}

#endif
