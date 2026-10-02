#!/usr/bin/env python3
"""AFD_POLL and AFD_EVENT_SELECT from a 32-bit program (dlls/ntdll/unix/horizon.c):
its ws2_32 sends afd_poll_params_32 and afd_event_select_params_32, whose
SOCKET and HANDLE are four bytes. As wineserver does (server/sock.c), the
process's machine picks the layout."""
from pathlib import Path

root = Path(__file__).resolve().parents[2]
source = (root / 'dlls/ntdll/unix/horizon.c').read_text()
afd = (root / 'include/wine/afd.h').read_text()

# The layouts this follows.
assert 'struct afd_poll_socket_32\n    {\n        ULONG socket;' in afd
assert 'struct afd_event_select_params_32\n{\n    ULONG event;' in afd

poll = source[source.index('static unsigned int horizon_sock_ioctl_poll'):]
poll = poll[:poll.index('\n}\n')]
assert 'horizon_process_machine == HORIZON_IMAGE_FILE_MACHINE_I386 ? 12 : 16' in poll
assert 'data_size < 16 + count * stride' in poll
assert '16 + count * 16' not in poll and 'i * 16' not in poll
assert '*out_size = 16 + signaled * stride;' in poll

event = source[source.index('case HORIZON_IOCTL_AFD_EVENT_SELECT:'):]
event = event[:event.index('break;\n    }')]
assert 'if (data_size < 8)' in event and 'memcpy( &mask, data + 4, sizeof(mask) );' in event
print('afd 32-bit: ok')
