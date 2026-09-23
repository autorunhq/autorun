#ifndef WINE_NX_SWAP_IPC_H
#define WINE_NX_SWAP_IPC_H

#include <switch/sf/hipc.h>
#include <string.h>

/* Validate the complete layout before invoking callbacks which may perform I/O. */
static inline int swap_ipc_visit( void *message, size_t size,
                                 int (*visit)( void *, const void *, size_t ), void *context )
{
    HipcHeader hdr;
    HipcSpecialHeader special = {0};
    size_t at = sizeof(hdr), recv_at, end;
    unsigned int i, buffers, receive;
    const unsigned char *bytes = message;
    if (size < sizeof(hdr)) return -1;
    memcpy( &hdr, bytes, sizeof(hdr) );
    if (hdr.has_special_header)
    {
        if (size - at < sizeof(special)) return -1;
        memcpy( &special, bytes + at, sizeof(special) );
        at += sizeof(special) + (special.send_pid ? 8 : 0) +
              4 * (special.num_copy_handles + special.num_move_handles);
    }
    buffers = hdr.num_send_buffers + hdr.num_recv_buffers + hdr.num_exch_buffers;
    receive = hdr.recv_static_mode == 2 ? 1 : hdr.recv_static_mode > 2 ? hdr.recv_static_mode - 2 : 0;
    end = at + hdr.num_send_statics * sizeof(HipcStaticDescriptor) +
          buffers * sizeof(HipcBufferDescriptor) + hdr.num_data_words * 4;
    recv_at = hdr.recv_list_offset ? hdr.recv_list_offset * 4 : end;
    if (end > size || (receive && (recv_at < end || recv_at > size ||
                                 receive * sizeof(HipcRecvListEntry) > size - recv_at))) return -1;
    for (i = 0; i < hdr.num_send_statics; i++, at += sizeof(HipcStaticDescriptor))
    {
        HipcStaticDescriptor desc;
        memcpy( &desc, bytes + at, sizeof(desc) );
        if (visit( context, hipcGetStaticAddress( &desc ), hipcGetStaticSize( &desc ) )) return -1;
    }
    for (i = 0; i < buffers; i++, at += sizeof(HipcBufferDescriptor))
    {
        HipcBufferDescriptor desc;
        memcpy( &desc, bytes + at, sizeof(desc) );
        if (visit( context, hipcGetBufferAddress( &desc ), hipcGetBufferSize( &desc ) )) return -1;
    }
    for (i = 0; i < receive; i++, recv_at += sizeof(HipcRecvListEntry))
    {
        HipcRecvListEntry desc;
        memcpy( &desc, bytes + recv_at, sizeof(desc) );
        if (visit( context, (void *)(uintptr_t)(desc.address_low | ((uint64_t)desc.address_high << 32)), desc.size ))
            return -1;
    }
    return 0;
}
#endif
