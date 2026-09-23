from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'vendor/libusbhsfs/source/usbhsfs_request.c').read_text()
start = source.index('Result usbHsFsRequestEndpointDataXfer(')
source = source[start:source.index('/* Reference:', start)]
fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef uint32_t u32, Result;
typedef int Event;
typedef struct { int active; } Service;
typedef struct { Service s; struct { u32 bEndpointAddress; } desc; } UsbHsClientEpSession;
typedef struct { u32 xferId, id, transferredSize; Result res; } UsbHsXferReport;
#define R_FAILED(x) ((x) != 0)
#define R_SUCCEEDED(x) ((x) == 0)
#define MAKERESULT(m,r) (r)
#define LibnxError_BadInput 1
#define LibnxError_BadUsbCommsRead 2
#define USB_POSTBUFFER_TIMEOUT UINT64_C(10000000000)
#define USBHSFS_LOG_MSG(...) ((void)0)
static Result submit_error, wait_error, report_error, transfer_error;
static u32 count = 1, report_id = 42, reported_size = 8;
static unsigned int pending, closed, cleared, logged;
static bool serviceIsActive(Service *s) { return s->active; }
static bool hosversionBefore(int a, int b, int c) { (void)a; (void)b; (void)c; return false; }
static Result __usbHsEpSubmitRequest(UsbHsClientEpSession *s, void *b, u32 n, u32 t, u32 *out)
{ (void)s; (void)b; (void)n; (void)t; (void)out; assert(false); return 0; }
static Event *usbHsEpGetXferEvent(UsbHsClientEpSession *s) { return &s->s.active; }
static Result usbHsEpPostBufferAsync(UsbHsClientEpSession *s, void *buf, u32 size, uint64_t id, u32 *out)
{
    (void)s; (void)buf; (void)size;
    assert(!pending && id == 0);
    *out = 42;
    pending = !submit_error;
    return submit_error;
}
static Result eventWait(Event *e, uint64_t timeout) { (void)e; assert(timeout == USB_POSTBUFFER_TIMEOUT); return wait_error; }
static void eventClear(Event *e) { (void)e; assert(!wait_error); cleared++; }
static Result usbHsEpGetXferReport(UsbHsClientEpSession *s, UsbHsXferReport *out, u32 max, u32 *n)
{
    (void)s;
    assert(max == 1 && cleared);
    *out = (UsbHsXferReport){report_id, 0, reported_size, transfer_error};
    *n = count;
    if (!report_error && count == 1 && report_id == 42) pending = 0;
    return report_error;
}
static void usbHsEpClose(UsbHsClientEpSession *s) { pending = 0; closed++; s->s.active = 0; }
static void log_error(u32 ep, Result rc) { assert(ep == 0x81 && rc && !pending && closed); logged++; }
static void (*usbHsFsRequestTransferError)(u32, Result) = log_error;
/* IMPLEMENTATION */
static void run(Result expected, bool cancel)
{
    UsbHsClientEpSession ep = {{1}, {0x81}};
    char buffer[8];
    u32 transferred = 0;
    pending = closed = cleared = logged = 0;
    assert(usbHsFsRequestEndpointDataXfer(&ep, buffer, sizeof(buffer), &transferred) == expected);
    assert(!pending && closed == cancel && logged == cancel);
    if (!expected) assert(transferred == sizeof(buffer));
}
int main(void)
{
    run(0, false);
    submit_error = 7; run(7, false); submit_error = 0;
    wait_error = 8; run(8, true); wait_error = 0;
    report_error = 9; run(9, true); report_error = 0;
    count = 0; run(LibnxError_BadInput, true); count = 1;
    report_id = 43; run(LibnxError_BadInput, true); report_id = 42;
    transfer_error = 10; run(10, false); transfer_error = 0;
    reported_size = 9; run(LibnxError_BadUsbCommsRead, false); reported_size = 8;
    puts("USB transfers: completion, timeout, report failure, wrong ID and buffer reuse passed");
}
'''
with tempfile.TemporaryDirectory(prefix='autorun-usb-transfer-') as tmp:
    unit, binary = Path(tmp) / 'usb.c', Path(tmp) / 'usb'
    unit.write_text(fixture.replace('/* IMPLEMENTATION */', source))
    subprocess.run([os.environ.get('CC', 'clang'), '-std=gnu11', '-O1', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', str(unit), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
