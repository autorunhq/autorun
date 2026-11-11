#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "winioctl.h"
#include "hidusage.h"
#include "ddk/wdm.h"
#include "ddk/hidclass.h"
#include "ddk/hidpddi.h"
#include "xinput.h"
#include "wine/hid.h"
#include "../../dlls/winebus.sys/unix_private.h"

#include "carafe_hid.h"
#include "carafe_hid_kernel.h"
#ifndef CARAFE_REG_DWORD
#define CARAFE_REG_HIVE_MACHINE 0
#define CARAFE_REG_SZ 1
#define CARAFE_REG_DWORD 4
#endif

#define HID_VENDOR 0x045e
#define HID_PRODUCT 0x028e
#define HID_VERSION 0x0114
#define HID_QUEUE_DEFAULT 32
#define HID_QUEUE_MIN 2
#define HID_QUEUE_MAX 512
#define HID_REPORT_MAX 64
#define HID_NAME_MAX 160
#define HID_TRIGGER_SCALE 257
#define HID_GAMEPAD_BUTTONS 10
#define HID_HAT_SHIFT 10
#define HID_AXIS_CENTER 32768
#define HID_AXIS_SPAN 65535
#define HID_TRIGGER_MAX 32767
#define HID_LEGACY_TRIGGER_MAX 255
#define HID_LEGACY_TRIGGER_CENTER 0x8000
#define HID_LEGACY_TRIGGER_STEP 128

extern int wine_nx_xinput_player(XINPUT_GAMEPAD* gamepad);
extern int wine_nx_xinput_presented(void);
extern void wine_nx_runtime_trace(const char* message);
extern void wine_nx_device_reports(void) __attribute__((weak));
extern unsigned int wine_nx_registry_set(int hive, const unsigned short* path, unsigned int path_len,
    const unsigned short* name, unsigned int name_len, unsigned int type, const void* data, unsigned int size)
    __attribute__((weak));
extern unsigned int wine_nx_registry_create_volatile(int hive, const unsigned short* path, unsigned int path_len)
    __attribute__((weak));

#include "psh_hid_macros.h"

static const BYTE g_gamepad_descriptor[] = {
    USAGE_PAGE(1, HID_USAGE_PAGE_GENERIC),
    USAGE(1, HID_USAGE_GENERIC_GAMEPAD),
    COLLECTION(1, Application),
        USAGE(1, 0),
        COLLECTION(1, Physical),
            USAGE(1, HID_USAGE_GENERIC_X),
            USAGE(1, HID_USAGE_GENERIC_Y),
            LOGICAL_MAXIMUM(2, 0xffff),
            PHYSICAL_MAXIMUM(2, 0xffff),
            REPORT_SIZE(1, 16),
            REPORT_COUNT(1, 2),
            INPUT(1, Data|Var|Abs),
        END_COLLECTION,

        COLLECTION(1, Physical),
            USAGE(1, HID_USAGE_GENERIC_RX),
            USAGE(1, HID_USAGE_GENERIC_RY),
            REPORT_COUNT(1, 2),
            INPUT(1, Data|Var|Abs),
        END_COLLECTION,

        COLLECTION(1, Physical),
            USAGE(1, HID_USAGE_GENERIC_Z),
            REPORT_COUNT(1, 1),
            INPUT(1, Data|Var|Abs),
        END_COLLECTION,

        USAGE_PAGE(1, HID_USAGE_PAGE_BUTTON),
        USAGE_MINIMUM(1, 1),
        USAGE_MAXIMUM(1, 10),
        LOGICAL_MAXIMUM(1, 1),
        PHYSICAL_MAXIMUM(1, 1),
        REPORT_COUNT(1, 10),
        REPORT_SIZE(1, 1),
        INPUT(1, Data|Var|Abs),

        USAGE_PAGE(1, HID_USAGE_PAGE_GENERIC),
        USAGE(1, HID_USAGE_GENERIC_HATSWITCH),
        LOGICAL_MINIMUM(1, 1),
        LOGICAL_MAXIMUM(1, 8),
        PHYSICAL_MAXIMUM(2, 0x103b),
        REPORT_SIZE(1, 4),
        REPORT_COUNT(4, 1),
        UNIT(1, 0x0e),
        INPUT(1, Data|Var|Abs|Null),

        REPORT_COUNT(1, 18),
        REPORT_SIZE(1, 1),
        INPUT(1, Cnst|Var|Abs),
    END_COLLECTION,
};

#include "pop_hid_macros.h"

#pragma pack(push, 1)
typedef struct {
    WORD lx;
    WORD ly;
    WORD rx;
    WORD ry;
    WORD trigger;
    WORD buttons;
    WORD padding;
} LegacyState;
#pragma pack(pop)

typedef struct HidFile HidFile;

typedef struct {
    const char* name;
    const WCHAR* device_id;
    const WCHAR* interface_class;
    bool opened;
    bool read;
    WCHAR instance[HID_NAME_MAX];
    WCHAR link[HID_NAME_MAX];
    HIDP_DEVICE_DESC desc;
    BYTE report[HID_REPORT_MAX];
    HidFile* files;
} HidDevice;

struct HidFile {
    HidDevice* device;
    HidFile* next;
    BYTE (*reports)[HID_REPORT_MAX];
    ULONG length;
    ULONG first;
    ULONG count;
};

static const WCHAR g_hid_class[] = u"{745a17a0-74d3-11d0-b6fe-00a0c90f57da}";
static const WCHAR g_product[] = u"Controller (XBOX 360 For Windows)";
static const WCHAR g_instance_number[] = u"0000";

static HidDevice g_devices[] = {
    { .name = "IG_00", .device_id = u"HID\\VID_045E&PID_028E&IG_00",
        .interface_class = u"{4d1e55b2-f16f-11cf-88cb-001111000030}" },
    { .name = "XI_00", .device_id = u"HID\\VID_045E&PID_028E&XI_00",
        .interface_class = u"{6c53d5fd-6480-440f-b618-476750c5e1a6}" },
};

enum { DeviceGamepad, DeviceXinput, DeviceCount };

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct unix_device* g_xinput;
static bool g_ready;
static bool g_failed;
static const char* g_failure = "";
static XINPUT_GAMEPAD g_last;

static const struct hid_device_vtbl g_xinput_vtbl;

static void report(const char* format, ...);

PVOID WINAPI ExAllocatePool(POOL_TYPE type, SIZE_T size) {
    (void)type;
    return malloc(size);
}

void WINAPI ExFreePool(PVOID pointer) {
    free(pointer);
}

void* raw_device_create(const struct raw_device_vtbl* vtbl, SIZE_T size) {
    struct unix_device* device = calloc(1, size);
    if (device == NULL) {
        return NULL;
    }
    device->vtbl = vtbl;
    device->ref = 1;
    return device;
}

static void xinputDestroy(struct unix_device* iface) {
    (void)iface;
}

static NTSTATUS xinputStart(struct unix_device* iface) {
    (void)iface;
    return STATUS_SUCCESS;
}

static void xinputStop(struct unix_device* iface) {
    (void)iface;
}

static NTSTATUS xinputHapticsStart(struct unix_device* iface, UINT duration_ms, USHORT rumble_intensity,
    USHORT buzz_intensity, USHORT left_intensity, USHORT right_intensity) {
    (void)iface;
    (void)duration_ms;
    (void)rumble_intensity;
    (void)buzz_intensity;
    (void)left_intensity;
    (void)right_intensity;
    return STATUS_NOT_SUPPORTED;
}

static NTSTATUS xinputHapticsStop(struct unix_device* iface) {
    (void)iface;
    return STATUS_NOT_SUPPORTED;
}

static NTSTATUS xinputDeviceControl(struct unix_device* iface, USAGE control) {
    (void)iface;
    (void)control;
    return STATUS_NOT_SUPPORTED;
}

static NTSTATUS xinputDeviceGain(struct unix_device* iface, BYTE percent) {
    (void)iface;
    (void)percent;
    return STATUS_NOT_SUPPORTED;
}

static NTSTATUS xinputEffectControl(struct unix_device* iface, BYTE index, USAGE control, BYTE iterations) {
    (void)iface;
    (void)index;
    (void)control;
    (void)iterations;
    return STATUS_NOT_SUPPORTED;
}

static NTSTATUS xinputEffectUpdate(struct unix_device* iface, BYTE index, struct effect_params* params) {
    (void)iface;
    (void)index;
    (void)params;
    return STATUS_NOT_SUPPORTED;
}

static const struct hid_device_vtbl g_xinput_vtbl = {
    xinputDestroy,
    xinputStart,
    xinputStop,
    xinputHapticsStart,
    xinputHapticsStop,
    xinputDeviceControl,
    xinputDeviceGain,
    xinputEffectControl,
    xinputEffectUpdate,
};

static size_t wideLength(const WCHAR* text) {
    size_t length = 0;
    while (text[length]) {
        length++;
    }
    return length;
}

static void wideAppend(WCHAR* out, size_t capacity, size_t* at, const WCHAR* text) {
    while (*text && *at + 1 < capacity) {
        out[(*at)++] = *text++;
    }
    out[*at] = 0;
}

static WCHAR wideLower(WCHAR c) {
    return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}

static void buildNames(HidDevice* device) {
    static const WCHAR backslash[] = u"\\";
    static const WCHAR link_prefix[] = u"\\??\\";
    static const WCHAR hash[] = u"#";
    size_t at = 0;
    wideAppend(device->instance, sizeof(device->instance) / sizeof(WCHAR), &at, device->device_id);
    wideAppend(device->instance, sizeof(device->instance) / sizeof(WCHAR), &at, backslash);
    wideAppend(device->instance, sizeof(device->instance) / sizeof(WCHAR), &at, g_instance_number);

    at = 0;
    wideAppend(device->link, sizeof(device->link) / sizeof(WCHAR), &at, link_prefix);
    size_t start = at;
    wideAppend(device->link, sizeof(device->link) / sizeof(WCHAR), &at, device->instance);
    for (size_t i = start; i < at; i++) {
        if (device->link[i] == '\\') {
            device->link[i] = '#';
        }
    }
    wideAppend(device->link, sizeof(device->link) / sizeof(WCHAR), &at, hash);
    wideAppend(device->link, sizeof(device->link) / sizeof(WCHAR), &at, device->interface_class);
}

static bool describe(HidDevice* device, const BYTE* descriptor, ULONG size) {
    if (HidP_GetCollectionDescription((PHIDP_REPORT_DESCRIPTOR)descriptor, size, PagedPool, &device->desc)
        != HIDP_STATUS_SUCCESS) {
        return false;
    }
    return device->desc.CollectionDescLength == 1 && device->desc.CollectionDesc[0].InputLength <= HID_REPORT_MAX;
}

static bool buildXinputDescriptor(void) {
    static const USAGE_AND_PAGE usage = { .Usage = HID_USAGE_GENERIC_GAMEPAD, .UsagePage = HID_USAGE_PAGE_GENERIC };
    g_xinput = hid_device_create(&g_xinput_vtbl, sizeof(struct unix_device));
    if (g_xinput == NULL) {
        return false;
    }
    return hid_device_begin_report_descriptor(g_xinput, &usage) && hid_device_add_gamepad(g_xinput)
        && hid_device_end_report_descriptor(g_xinput);
}

static LONG mulDiv(LONG value, LONG multiplier, LONG divisor) {
    LONGLONG product = (LONGLONG)value * multiplier;
    return (LONG)((product + divisor / 2) / divisor);
}

static LONG triggerValue(BYTE trigger) {
    return trigger * HID_TRIGGER_SCALE / 2;
}

static void composeXinput(const XINPUT_GAMEPAD* pad) {
    static const WORD buttons[] = {
        XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_B, XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_Y, XINPUT_GAMEPAD_LEFT_SHOULDER,
        XINPUT_GAMEPAD_RIGHT_SHOULDER, XINPUT_GAMEPAD_BACK, XINPUT_GAMEPAD_START, XINPUT_GAMEPAD_LEFT_THUMB,
        XINPUT_GAMEPAD_RIGHT_THUMB, XINPUT_GAMEPAD_DPAD_UP, XINPUT_GAMEPAD_DPAD_RIGHT, XINPUT_GAMEPAD_DPAD_DOWN,
        XINPUT_GAMEPAD_DPAD_LEFT,
    };
    LONG x = ((pad->wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) != 0) - ((pad->wButtons & XINPUT_GAMEPAD_DPAD_LEFT) != 0);
    LONG y = ((pad->wButtons & XINPUT_GAMEPAD_DPAD_DOWN) != 0) - ((pad->wButtons & XINPUT_GAMEPAD_DPAD_UP) != 0);

    hid_device_set_abs_axis(g_xinput, 0, pad->sThumbLX);
    hid_device_set_abs_axis(g_xinput, 1, pad->sThumbLY);
    hid_device_set_abs_axis(g_xinput, 2, pad->sThumbRX);
    hid_device_set_abs_axis(g_xinput, 3, pad->sThumbRY);
    hid_device_set_abs_axis(g_xinput, 4, triggerValue(pad->bLeftTrigger));
    hid_device_set_abs_axis(g_xinput, 5, triggerValue(pad->bRightTrigger));
    hid_device_set_hatswitch_x(g_xinput, 0, x);
    hid_device_set_hatswitch_y(g_xinput, 0, y);
    for (ULONG i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++) {
        hid_device_set_button(g_xinput, i, (pad->wButtons & buttons[i]) != 0);
    }
    hid_device_sync_report(g_xinput);
    memcpy(g_devices[DeviceXinput].report, g_xinput->hid_device_state.report_buf,
        g_xinput->hid_device_state.report_len);
}

static BYTE hatValue(void) {
    BYTE value = g_xinput->hid_device_state.report_buf[g_xinput->hid_device_state.hatswitch_start];
    return value & 0x0f;
}

static void composeGamepad(const XINPUT_GAMEPAD* pad) {
    static const WORD buttons[HID_GAMEPAD_BUTTONS] = {
        XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_B, XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_Y, XINPUT_GAMEPAD_LEFT_SHOULDER,
        XINPUT_GAMEPAD_RIGHT_SHOULDER, XINPUT_GAMEPAD_BACK, XINPUT_GAMEPAD_START, XINPUT_GAMEPAD_LEFT_THUMB,
        XINPUT_GAMEPAD_RIGHT_THUMB,
    };
    LegacyState state = { 0 };
    LONG left = mulDiv(triggerValue(pad->bLeftTrigger), HID_LEGACY_TRIGGER_MAX, HID_TRIGGER_MAX);
    LONG right = mulDiv(triggerValue(pad->bRightTrigger), HID_LEGACY_TRIGGER_MAX, HID_TRIGGER_MAX);
    BYTE hat = hatValue();

    state.lx = (WORD)(pad->sThumbLX + HID_AXIS_CENTER);
    state.ly = (WORD)(HID_AXIS_SPAN - (pad->sThumbLY + HID_AXIS_CENTER));
    state.rx = (WORD)(pad->sThumbRX + HID_AXIS_CENTER);
    state.ry = (WORD)(HID_AXIS_SPAN - (pad->sThumbRY + HID_AXIS_CENTER));
    state.trigger = (WORD)(HID_LEGACY_TRIGGER_CENTER + (left - right) * HID_LEGACY_TRIGGER_STEP);
    if (hat >= 1 && hat <= 8) {
        state.buttons = (WORD)(hat << HID_HAT_SHIFT);
    }
    for (ULONG i = 0; i < HID_GAMEPAD_BUTTONS; i++) {
        if (pad->wButtons & buttons[i]) {
            state.buttons |= (WORD)(1 << i);
        }
    }
    g_devices[DeviceGamepad].report[0] = 0;
    memcpy(g_devices[DeviceGamepad].report + 1, &state, sizeof(state));
}

static void compose(const XINPUT_GAMEPAD* pad) {
    composeXinput(pad);
    composeGamepad(pad);
}

static bool readyLocked(void) {
    if (g_ready || g_failed) {
        return g_ready;
    }
    g_failed = true;
    for (int i = 0; i < DeviceCount; i++) {
        buildNames(&g_devices[i]);
    }
    if (!describe(&g_devices[DeviceGamepad], g_gamepad_descriptor, sizeof(g_gamepad_descriptor))) {
        g_failure = "the gamepad descriptor does not parse";
        return false;
    }
    if (!buildXinputDescriptor()
        || !describe(&g_devices[DeviceXinput], g_xinput->hid_report_descriptor.data,
            (ULONG)g_xinput->hid_report_descriptor.size)) {
        g_failure = "the XInput descriptor does not parse";
        return false;
    }
    if (sizeof(LegacyState) + 1 != g_devices[DeviceGamepad].desc.CollectionDesc[0].InputLength
        || g_xinput->hid_device_state.report_len != g_devices[DeviceXinput].desc.CollectionDesc[0].InputLength) {
        g_failure = "a report length differs from its descriptor";
        return false;
    }
    memset(&g_last, 0, sizeof(g_last));
    compose(&g_last);
    g_ready = true;
    g_failed = false;
    return true;
}

static ULONG inputLength(const HidDevice* device) {
    return device->desc.CollectionDesc[0].InputLength;
}

static void queuePush(HidFile* file, const BYTE* report) {
    ULONG slot;
    if (file->count == file->length) {
        file->first = (file->first + 1) % file->length;
        file->count--;
    }
    slot = (file->first + file->count) % file->length;
    memcpy(file->reports[slot], report, HID_REPORT_MAX);
    file->count++;
}

static bool queuePop(HidFile* file, BYTE* report) {
    if (!file->count) {
        return false;
    }
    memcpy(report, file->reports[file->first], HID_REPORT_MAX);
    file->first = (file->first + 1) % file->length;
    file->count--;
    return true;
}

static NTSTATUS queueResize(HidFile* file, ULONG length) {
    BYTE (*reports)[HID_REPORT_MAX];
    BYTE report[HID_REPORT_MAX];
    ULONG count = 0;

    if (length < HID_QUEUE_MIN || length > HID_QUEUE_MAX) {
        return STATUS_INVALID_PARAMETER;
    }
    if (length == file->length) {
        return STATUS_SUCCESS;
    }
    if ((reports = calloc(length, HID_REPORT_MAX)) == NULL) {
        return STATUS_NO_MEMORY;
    }
    while (file->count > length) {
        queuePop(file, report);
    }
    while (queuePop(file, report)) {
        memcpy(reports[count++], report, HID_REPORT_MAX);
    }
    free(file->reports);
    file->reports = reports;
    file->length = length;
    file->first = 0;
    file->count = count;
    return STATUS_SUCCESS;
}

static bool nameMatches(const WCHAR* link, const unsigned short* name, unsigned int bytes) {
    size_t length = wideLength(link);
    if (bytes != length * sizeof(WCHAR)) {
        return false;
    }
    for (size_t i = 0; i < length; i++) {
        if (wideLower(link[i]) != wideLower(name[i])) {
            return false;
        }
    }
    return true;
}

void* carafeHidOpen(const unsigned short* name, unsigned int bytes) {
    HidFile* file = NULL;
    const char* first = NULL;

    pthread_mutex_lock(&g_lock);
    if (readyLocked()) {
        for (int i = 0; i < DeviceCount; i++) {
            HidDevice* device = &g_devices[i];
            if (!nameMatches(device->link, name, bytes)) {
                continue;
            }
            if ((file = calloc(1, sizeof(*file))) == NULL) {
                break;
            }
            if ((file->reports = calloc(HID_QUEUE_DEFAULT, HID_REPORT_MAX)) == NULL) {
                free(file);
                file = NULL;
                break;
            }
            file->device = device;
            file->length = HID_QUEUE_DEFAULT;
            queuePush(file, device->report);
            file->next = device->files;
            device->files = file;
            if (!device->opened) {
                device->opened = true;
                first = device->name;
            }
            break;
        }
    }
    pthread_mutex_unlock(&g_lock);
    if (first != NULL) {
        report("[HID] the program opened the gamepad %s", first);
    }
    return file;
}

void carafeHidClose(void* opened) {
    HidFile* file = opened;
    HidFile** link;

    if (file == NULL) {
        return;
    }
    pthread_mutex_lock(&g_lock);
    for (link = &file->device->files; *link; link = &(*link)->next) {
        if (*link == file) {
            *link = file->next;
            break;
        }
    }
    pthread_mutex_unlock(&g_lock);
    free(file->reports);
    free(file);
}

static unsigned int copyString(const WCHAR* text, unsigned char* out, unsigned int out_max, unsigned int* out_size) {
    unsigned int size = (unsigned int)((wideLength(text) + 1) * sizeof(WCHAR));
    *out_size = size;
    if (size > out_max) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    memcpy(out, text, size);
    return STATUS_SUCCESS;
}

static unsigned int inputReport(HidFile* file, const unsigned char* in, unsigned int in_size, unsigned char* out,
    unsigned int out_max, unsigned int* out_size) {
    ULONG length = inputLength(file->device);
    if (!in_size || !out_max) {
        return STATUS_INVALID_USER_BUFFER;
    }
    if (in[0] != file->device->desc.ReportIDs[0].ReportID || out_max < length) {
        return STATUS_INVALID_PARAMETER;
    }
    memcpy(out, file->device->report, length);
    *out_size = length;
    return STATUS_SUCCESS;
}

unsigned int carafeHidIoctl(void* opened, unsigned int code, const unsigned char* in, unsigned int in_size,
    unsigned char* out, unsigned int out_max, unsigned int* out_size) {
    HidFile* file = opened;
    HIDP_COLLECTION_DESC* collection = &file->device->desc.CollectionDesc[0];
    unsigned int status;

    *out_size = 0;
    pthread_mutex_lock(&g_lock);
    switch (code) {
    case IOCTL_HID_GET_POLL_FREQUENCY_MSEC: {
        ULONG interval = 0;
        if (out_max < sizeof(interval)) {
            status = STATUS_BUFFER_OVERFLOW;
            break;
        }
        memcpy(out, &interval, sizeof(interval));
        *out_size = sizeof(interval);
        status = STATUS_SUCCESS;
        break;
    }
    case IOCTL_HID_SET_POLL_FREQUENCY_MSEC:
        status = in_size < sizeof(ULONG) ? STATUS_BUFFER_TOO_SMALL : STATUS_SUCCESS;
        break;
    case IOCTL_HID_GET_PRODUCT_STRING:
        status = copyString(g_product, out, out_max, out_size);
        break;
    case IOCTL_HID_GET_SERIALNUMBER_STRING:
    case IOCTL_HID_GET_MANUFACTURER_STRING:
        status = STATUS_INVALID_PARAMETER;
        break;
    case IOCTL_HID_GET_COLLECTION_INFORMATION: {
        HID_COLLECTION_INFORMATION info = {
            .DescriptorSize = collection->PreparsedDataLength,
            .Polled = FALSE,
            .VendorID = HID_VENDOR,
            .ProductID = HID_PRODUCT,
            .VersionNumber = HID_VERSION,
        };
        *out_size = sizeof(info);
        if (out_max < sizeof(info)) {
            status = STATUS_BUFFER_OVERFLOW;
            break;
        }
        memcpy(out, &info, sizeof(info));
        status = STATUS_SUCCESS;
        break;
    }
    case IOCTL_HID_GET_COLLECTION_DESCRIPTOR:
        *out_size = collection->PreparsedDataLength;
        if (out_max < collection->PreparsedDataLength) {
            status = STATUS_INVALID_BUFFER_SIZE;
            break;
        }
        memcpy(out, collection->PreparsedData, collection->PreparsedDataLength);
        status = STATUS_SUCCESS;
        break;
    case IOCTL_SET_NUM_DEVICE_INPUT_BUFFERS: {
        ULONG length;
        if (in_size != sizeof(length)) {
            status = STATUS_BUFFER_OVERFLOW;
            break;
        }
        memcpy(&length, in, sizeof(length));
        status = queueResize(file, length);
        break;
    }
    case IOCTL_GET_NUM_DEVICE_INPUT_BUFFERS:
        if (out_max < sizeof(ULONG)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        memcpy(out, &file->length, sizeof(ULONG));
        *out_size = sizeof(ULONG);
        status = STATUS_SUCCESS;
        break;
    case IOCTL_HID_GET_INPUT_REPORT:
        status = inputReport(file, out, out_max, out, out_max, out_size);
        break;
    case IOCTL_HID_GET_FEATURE:
    case IOCTL_HID_SET_FEATURE:
    case IOCTL_HID_SET_OUTPUT_REPORT:
        status = STATUS_INVALID_PARAMETER;
        break;
    default:
        status = STATUS_NOT_SUPPORTED;
        break;
    }
    pthread_mutex_unlock(&g_lock);
    return status;
}

unsigned int carafeHidRead(void* opened, unsigned char* out, unsigned int out_max, unsigned int* out_size) {
    HidFile* file = opened;
    BYTE next[HID_REPORT_MAX];
    ULONG length = inputLength(file->device);
    unsigned int status = STATUS_PENDING;
    bool first = false;

    *out_size = 0;
    if (out_max < length) {
        return STATUS_INVALID_BUFFER_SIZE;
    }
    pthread_mutex_lock(&g_lock);
    if (queuePop(file, next)) {
        memcpy(out, next, length);
        *out_size = length;
        status = STATUS_SUCCESS;
        first = !file->device->read;
        file->device->read = true;
    }
    pthread_mutex_unlock(&g_lock);
    if (first) {
        report("[HID] the program reads reports of the gamepad %s", file->device->name);
    }
    return status;
}

unsigned int carafeHidWrite(void* opened, const unsigned char* in, unsigned int size, unsigned int* written) {
    (void)opened;
    *written = 0;
    return in == NULL || !size ? STATUS_INVALID_USER_BUFFER : STATUS_INVALID_PARAMETER;
}

static void registerValue(unsigned int* status, const WCHAR* path, const WCHAR* name, unsigned int type,
    const void* data, unsigned int size) {
    if (*status) {
        return;
    }
    *status = wine_nx_registry_set(CARAFE_REG_HIVE_MACHINE, path, (unsigned int)(wideLength(path) * sizeof(WCHAR)),
        name, (unsigned int)(wideLength(name) * sizeof(WCHAR)), type, data, size);
}

static void registerText(unsigned int* status, const WCHAR* path, const WCHAR* name, const WCHAR* text) {
    registerValue(status, path, name, CARAFE_REG_SZ, text, (unsigned int)((wideLength(text) + 1) * sizeof(WCHAR)));
}

static void registerKey(unsigned int* status, const WCHAR* path) {
    if (*status) {
        return;
    }
    *status = wine_nx_registry_create_volatile(CARAFE_REG_HIVE_MACHINE, path,
        (unsigned int)(wideLength(path) * sizeof(WCHAR)));
}

static unsigned int registerDevice(const HidDevice* device) {
    static const WCHAR enum_root[] = u"System\\CurrentControlSet\\Enum\\";
    static const WCHAR classes_root[] = u"System\\CurrentControlSet\\Control\\DeviceClasses\\";
    static const WCHAR backslash[] = u"\\";
    static const WCHAR reference[] = u"\\#";
    static const WCHAR control[] = u"\\Control";
    static const WCHAR interface_prefix[] = u"##?#";
    static const WCHAR hid_class_name[] = u"HIDClass";
    WCHAR path[HID_NAME_MAX * 2];
    WCHAR link[HID_NAME_MAX];
    size_t at = 0;
    DWORD linked = 1;
    unsigned int status = 0;

    size_t link_at = 0;
    wideAppend(link, sizeof(link) / sizeof(WCHAR), &link_at, u"\\\\?\\");
    wideAppend(link, sizeof(link) / sizeof(WCHAR), &link_at, device->link + wideLength(u"\\??\\"));

    wideAppend(path, sizeof(path) / sizeof(WCHAR), &at, enum_root);
    wideAppend(path, sizeof(path) / sizeof(WCHAR), &at, device->instance);
    registerKey(&status, path);
    registerText(&status, path, u"ClassGUID", g_hid_class);
    registerText(&status, path, u"Class", hid_class_name);
    registerText(&status, path, u"DeviceDesc", g_product);

    at = 0;
    wideAppend(path, sizeof(path) / sizeof(WCHAR), &at, classes_root);
    wideAppend(path, sizeof(path) / sizeof(WCHAR), &at, device->interface_class);
    wideAppend(path, sizeof(path) / sizeof(WCHAR), &at, backslash);
    wideAppend(path, sizeof(path) / sizeof(WCHAR), &at, interface_prefix);
    wideAppend(path, sizeof(path) / sizeof(WCHAR), &at, device->link + wideLength(u"\\??\\"));
    registerText(&status, path, u"DeviceInstance", device->instance);
    wideAppend(path, sizeof(path) / sizeof(WCHAR), &at, reference);
    registerKey(&status, path);
    registerText(&status, path, u"SymbolicLink", link);
    wideAppend(path, sizeof(path) / sizeof(WCHAR), &at, control);
    registerKey(&status, path);
    registerValue(&status, path, u"Linked", CARAFE_REG_DWORD, &linked, sizeof(linked));
    return status;
}

static void report(const char* format, ...) {
    char message[160];
    va_list args;

    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    wine_nx_runtime_trace(message);
}

void carafeHidRegister(void) {
    unsigned int status = 0;

    if (wine_nx_registry_set == NULL || wine_nx_registry_create_volatile == NULL) {
        return;
    }
    if (!wine_nx_xinput_presented()) {
        report("[HID] the controls are keyboard and mouse: no gamepad is announced");
        return;
    }
    pthread_mutex_lock(&g_lock);
    bool ready = readyLocked();
    pthread_mutex_unlock(&g_lock);
    if (!ready) {
        report("[HID] no gamepad is announced: %s", g_failure);
        return;
    }
    for (int i = 0; i < DeviceCount && !status; i++) {
        status = registerDevice(&g_devices[i]);
    }
    if (status) {
        report("[HID] the gamepad could not be announced: registry status %08x", status);
        return;
    }
    report("[HID] the controller is announced as an Xbox 360 HID gamepad: IG_00 reports of %u bytes, XI_00 of %u",
        (unsigned int)inputLength(&g_devices[DeviceGamepad]), (unsigned int)inputLength(&g_devices[DeviceXinput]));
}

void carafeHidPoll(void) {
    XINPUT_GAMEPAD pad = { 0 };
    bool changed = false;

    if (!wine_nx_xinput_player(&pad)) {
        memset(&pad, 0, sizeof(pad));
    }
    pthread_mutex_lock(&g_lock);
    if (g_ready && memcmp(&pad, &g_last, sizeof(pad)) != 0) {
        g_last = pad;
        compose(&pad);
        for (int i = 0; i < DeviceCount; i++) {
            for (HidFile* file = g_devices[i].files; file; file = file->next) {
                queuePush(file, g_devices[i].report);
                changed = true;
            }
        }
    }
    pthread_mutex_unlock(&g_lock);
    if (changed && wine_nx_device_reports != NULL) {
        wine_nx_device_reports();
    }
}
