/*
 * Making a forwarder: an application the console installs and lists on the home
 * menu, whose only job is to start the homebrew loader with Autorun's NRO, in
 * a 39-bit address space. Fixed-low Win32 images require the low-window patch,
 * or a game's 32-bit forwarder.
 *
 * The pieces are sphaira's (src/owo.cpp, ISC, TotalJustice), as is the loader
 * the program NCA carries (hbl/, nx-hbloader).
 */
#ifndef WINE_NX_FORWARDER_H
#define WINE_NX_FORWARDER_H

#include <stddef.h>

/* The address space Horizon makes the forwarder's process with (its NPDM). */
enum wine_nx_forwarder_address_space
{
    /* 39 bits: Autorun's own, for 64- and 32-bit games. */
    WINE_NX_FORWARDER_39_BIT,
    /* 32 bits without the alias region (AddressSpace32BitNoReserved): every
     * mapping below 4 GiB, where a 32-bit game's image finds the address it
     * was linked at with no patch to the loader, and a heap of 2 GiB rather
     * than 1. 64-bit games cannot run in it. */
    WINE_NX_FORWARDER_32_BIT_NO_ALIAS,
};

struct wine_nx_forwarder
{
    const char *nro_path;   /* sdmc:/switch/wine/wine-nx-runtime.nro */
    const char *args;       /* what follows the path, or NULL */
    const char *name;       /* what the home menu shows */
    const char *author;
    const unsigned char *icon;  /* a 256x256 JPEG */
    size_t icon_size;
    unsigned int game_id;
    /* A game's forwarder in 39 bits asks Autorun's application to start the
     * game, which is where the low-window patch applies. Its 32-bit one is a
     * loader of its own that starts the NRO with --library-game=ID, in a
     * process of its own address space and under a title id of its own. */
    enum wine_nx_forwarder_address_space address_space;
};

unsigned long long wine_nx_forwarder_title_id( const char *nro_path, const char *args );
unsigned long long wine_nx_forwarder_game_title_id( unsigned int game_id,
                                                    enum wine_nx_forwarder_address_space address_space );

/* What a game's 32-bit forwarder puts after the NRO's path, which the runtime
 * reads back: "--library-game=" and the game's id in the library. */
#define WINE_NX_FORWARDER_GAME_ARG "--library-game="

/* Where the installer says what the console made of what it wrote. */
extern void (*wine_nx_forwarder_report)( const char *line );

/* Build the forwarder and install it. Returns 0, or the Result that failed;
 * step, when given, is left pointing at what was being done. */
unsigned int wine_nx_forwarder_install( const struct wine_nx_forwarder *request, const char **step );

/* What the launcher carries to build one with (generated: forwarder_embed.c).
 * The host test supplies its own, so it declares them as pointers. */
#ifndef WINE_NX_FORWARDER_EMBED_EXTERN
extern const unsigned char wine_nx_hbl_main[];
extern const size_t wine_nx_hbl_main_size;
extern const unsigned char wine_nx_game_forwarder_main[];
extern const size_t wine_nx_game_forwarder_main_size;
extern const unsigned char wine_nx_hbl_npdm[];
extern const size_t wine_nx_hbl_npdm_size;
extern const unsigned char wine_nx_icon_any[];
extern const size_t wine_nx_icon_any_size;
/* The mark the launcher shows in the corner of its own screens. */
extern const unsigned char wine_nx_logo[];
extern const size_t wine_nx_logo_size;
#endif

#endif
