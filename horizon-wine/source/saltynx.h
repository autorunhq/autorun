/* Copyright 2026 Wine-NX contributors. LGPL-2.1-or-later.
 *
 * The runtime's side of SaltyNX and FPSLocker (saltynx.c, fpslocker.h): while
 * a program runs, the frame rate goes to FPSLocker's block and FPSLocker's
 * target and display sync come back to the present paths. And the requests to
 * SaltySD's port, laid out as libnx's old IPC did for FPSLocker's own client
 * (include/SaltyNX.h there): pure, for tests/check_fpslocker.py.
 */
#ifndef WINE_NX_SALTYNX_H
#define WINE_NX_SALTYNX_H

#include <stdint.h>
#include <string.h>

#define SALTYSD_PORT               "SaltySD"
#define SALTYSD_IN_MAGIC           0x49434653u  /* "SFCI" */
#define SALTYSD_OUT_MAGIC          0x4f434653u  /* "SFCO" */
#define SALTYSD_GET_SHARED_MEMORY  7u
#define SALTYSD_GET_REFRESH_RATE   10u
#define SALTYSD_SET_REFRESH_RATE   11u

/* Started for a program once its settings are read, when the process is an
 * application (FPSLocker sees only those), and stopped on every way out. */
void wine_nx_saltynx_start( void );
void wine_nx_saltynx_stop( void );

/* A request in a thread's 0x100-byte command buffer, as ipcInitialize,
 * ipcSendPid and ipcPrepareHeader made it: a plain request with the PID sent,
 * and the raw data, SFCI's magic, the command and two arguments, on the
 * 16-byte boundary after the PID, its size counting the 16 bytes libnx leaves
 * for that padding. */
static inline void saltysd_request( uint32_t *buffer, uint64_t command, uint64_t first, uint64_t second )
{
    uint64_t raw[4] = { SALTYSD_IN_MAGIC, command, first, second };

    memset( buffer, 0, 0x100 );
    buffer[0] = 4;                                    /* IpcCommandType_Request */
    buffer[1] = 0x80000000u | (sizeof(raw) / 4 + 4);  /* a handle descriptor follows */
    buffer[2] = 1;                                    /* the PID, which the kernel writes */
    memcpy( buffer + 8, raw, sizeof(raw) );           /* buffer + 5 words, rounded up to 16 bytes */
}

/* A reply in the same buffer: its result, after SFCO's magic, or ~0 when it is
 * not one; the first handle it copied (0 when none), and the words after the
 * result. */
static inline uint64_t saltysd_reply( const uint32_t *buffer, uint32_t *handle, uint64_t *extra,
                                      unsigned int extra_count )
{
    const uint32_t ctrl0 = buffer[0], ctrl1 = buffer[1];
    const uint32_t *cursor = buffer + 2;
    uint64_t raw[8];
    unsigned int bytes, offset;

    if (handle) *handle = 0;
    if (ctrl1 & 0x80000000u)
    {
        const uint32_t special = *cursor++;
        const unsigned int copied = (special >> 1) & 15, moved = (special >> 5) & 15;

        if (special & 1) cursor += 2;
        if (handle && copied) *handle = *cursor;
        cursor += copied + moved;
    }
    cursor += ((ctrl0 >> 16) & 15) * 2;
    cursor += (((ctrl0 >> 20) & 15) + ((ctrl0 >> 24) & 15) + ((ctrl0 >> 28) & 15)) * 3;
    offset = ((unsigned int)(cursor - buffer) * 4 + 15) & ~15u;
    bytes = (ctrl1 & 0x3ffu) * 4;
    if (extra_count > 6 || offset + (2 + extra_count) * 8 > 0x100 || bytes < (2 + extra_count) * 8)
        return ~0ull;
    memcpy( raw, (const uint8_t *)buffer + offset, (2 + extra_count) * 8 );
    if ((uint32_t)raw[0] != SALTYSD_OUT_MAGIC) return ~0ull;
    if (extra_count) memcpy( extra, raw + 2, extra_count * 8 );
    return raw[1];
}

#endif /* WINE_NX_SALTYNX_H */
