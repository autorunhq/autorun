/*
 * Which FEX a 32-bit program runs on.
 *
 * wow64.dll loads its CPU module by one name from one place, system32's
 * libwow64fex.dll, and both come from the DLL repository. The build with
 * Carafe's patches (carafe/dlls) is a release the launcher installs beside it,
 * under drive_c/fex-carafe/versions, so running a program on it means having
 * that file in system32 when the program starts. This puts the wanted one
 * there, a program at a time: the stock DLL is kept in drive_c/fex-carafe while
 * a Carafe build stands in for it, and goes back when a program wants it.
 *
 * Nothing here knows about the network or the launcher; it copies files, so the
 * host test runs it as the console does.
 */
#ifndef WINE_NX_FEX_BUILD_H
#define WINE_NX_FEX_BUILD_H

enum fex_build_result
{
    FEX_BUILD_STOCK,      /* system32 holds the DLL repository's FEX */
    FEX_BUILD_CARAFE,     /* system32 holds the Carafe build that was asked for */
    FEX_BUILD_NO_CARAFE,  /* that Carafe build is not on the card; system32 is as it was */
    FEX_BUILD_NO_STOCK,   /* the stock FEX was asked for and no copy of it is kept;
                           * system32 still holds a Carafe build */
    FEX_BUILD_IO,         /* a file could not be read or written; system32 is as it was */
};

/* carafe: the Carafe libwow64fex.dll the program is to run on, or NULL for the
 * stock one. *changed is set when system32's file was replaced. */
enum fex_build_result wine_nx_fex_build_select( const char *runtime_dir, const char *carafe, int *changed );

#endif
