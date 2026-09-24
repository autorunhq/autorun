#ifndef WINE_NX_FORWARDER_LAUNCH_H
#define WINE_NX_FORWARDER_LAUNCH_H

#include <stddef.h>
#include <stdint.h>

#define AUTORUN_TITLE_ID UINT64_C(0x0548EABB35576000)
#define AUTORUN_NRO "sdmc:/switch/wine/wine-nx-runtime.nro"

struct autorun_game_launch
{
    uint32_t magic;
    uint32_t version;
    uint32_t game_id;
    uint32_t reserved;
};

static inline struct autorun_game_launch autorun_game_launch_make( uint32_t id )
{
    return (struct autorun_game_launch){ 0x47525541, 1, id, 0 };
}

static inline int autorun_game_launch_valid( const struct autorun_game_launch *launch, size_t size )
{
    return size == sizeof(*launch) && launch->magic == 0x47525541 &&
           launch->version == 1 && launch->game_id && !launch->reserved;
}

#endif
