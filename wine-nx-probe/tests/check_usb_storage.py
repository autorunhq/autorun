#!/usr/bin/env python3
"""Exercise the patched USB mount lifecycle and concurrent disk lookup."""
from pathlib import Path
import re
import subprocess
import tempfile

probe = Path(__file__).resolve().parents[1]
vendor = probe / 'vendor/libusbhsfs'
patches = sorted((probe / 'usbhsfs-uasp').glob('*.patch'))


def definition(source, name):
    match = re.search(r'^.*\b' + name + r'\([^;]*?\)\n\{', source, re.M)
    assert match, name
    pos = source.index('{', match.start()) + 1
    depth = 1
    while depth:
        depth += (source[pos] == '{') - (source[pos] == '}')
        pos += 1
    return source[match.start():pos]


fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef uint8_t u8, BYTE;
typedef uint16_t WORD;
typedef uint32_t u32, UINT;
typedef uint64_t u64, LBA_t;
typedef int FRESULT, DRESULT, DSTATUS;
typedef pthread_mutex_t Mutex;
#define FF_VOLUMES 4
#define FF_FS_NORTC 1
#define FR_OK 0
#define FR_DISK_ERR 1
#define RES_OK 0
#define RES_PARERR 4
#define CTRL_SYNC 0
#define GET_SECTOR_COUNT 1
#define GET_SECTOR_SIZE 2
#define MOUNT_NAME_LENGTH 16
#define UsbHsFsMountFlags_ReadOnly 1
#define UsbHsFsDriveLogicalUnitFileSystemType_FAT 1
#define USBHSFS_LOG_MSG(...) ((void)0)
#define NX_IGNORE_ARG(x) ((void)(x))
#define NX_INLINE static inline

typedef struct { u8 pdrv, ro_flag; LBA_t winsect; BYTE win[512]; } FATFS;
typedef struct { BYTE data[512]; } VolumeBootRecord;
typedef struct UsbHsFsDriveLogicalUnitFileSystemContext {
    void *lun_ctx;
    FATFS *fatfs;
    u32 flags, fs_idx, fs_type;
} UsbHsFsDriveLogicalUnitFileSystemContext;
typedef struct UsbHsFsDriveLogicalUnitContext {
    u32 block_length, fs_count;
    u64 block_count;
    int usb_if_id, lun;
    bool write_protect;
    UsbHsFsDriveLogicalUnitFileSystemContext **fs_ctx;
    BYTE contents[512];
} UsbHsFsDriveLogicalUnitContext;
typedef struct { Mutex mutex; u8 lun_count; UsbHsFsDriveLogicalUnitContext **lun_ctx; } UsbHsFsDriveContext;

static Mutex g_managerMutex = PTHREAD_MUTEX_INITIALIZER;
static UsbHsFsDriveContext **g_driveContexts;
static u32 g_driveCount;
static Mutex *contended_drive;
static atomic_int drive_waiting;
static _Thread_local bool watch_drive;
static _Thread_local Mutex *owned[16];
static _Thread_local unsigned owned_count;

static bool mutexIsLockedByCurrentThread(Mutex *mutex)
{
    for (unsigned i = 0; i < owned_count; i++) if (owned[i] == mutex) return true;
    return false;
}

static void mutexLock(Mutex *mutex)
{
    if (watch_drive && mutex == contended_drive)
    {
        assert(mutexIsLockedByCurrentThread(&g_managerMutex));
        atomic_store(&drive_waiting, 1);
    }
    assert(!pthread_mutex_lock(mutex));
    assert(owned_count < 16);
    owned[owned_count++] = mutex;
}

static void mutexUnlock(Mutex *mutex)
{
    unsigned i;
    for (i = 0; i < owned_count && owned[i] != mutex; i++);
    assert(i < owned_count);
    owned[i] = owned[--owned_count];
    assert(!pthread_mutex_unlock(mutex));
}

static u64 armGetSystemTick(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000 + ts.tv_nsec;
}
static u64 armTicksToNs(u64 ticks) { return ticks; }
static FATFS *mounted[FF_VOLUMES];
static bool mount_fail, devoptab_fail;
static DSTATUS ff_disk_initialize(BYTE pdrv);
static DRESULT ff_disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count);
static DRESULT ff_disk_ioctl(BYTE pdrv, BYTE cmd, void *buff);
static UsbHsFsDriveLogicalUnitContext *usbHsFsManagerGetLogicalUnitContextForFatFsDriveNumber(u8 pdrv);
static UsbHsFsDriveLogicalUnitContext *usbHsFsMountGetLogicalUnitContextForFatFsDriveNumber(u8 pdrv);

static FRESULT ff_mount(FATFS *fs, const char *name, int opt)
{
    u8 pdrv = (u8)strtoul(name, NULL, 10);
    BYTE buffer[512];
    WORD size;
    assert(opt == 1 && pdrv < FF_VOLUMES);
    assert(!mounted[pdrv]);
    fs->pdrv = pdrv;
    mounted[pdrv] = fs;
    assert(ff_disk_initialize(pdrv) == RES_OK);
    assert(ff_disk_ioctl(pdrv, GET_SECTOR_SIZE, &size) == RES_OK && size == 512);
    assert(ff_disk_read(pdrv, buffer, 0, 1) == RES_OK);
    return mount_fail ? FR_DISK_ERR : FR_OK;
}

static void ff_unmount(const char *name)
{
    u8 pdrv = (u8)strtoul(name, NULL, 10);
    assert(pdrv < FF_VOLUMES);
    assert(usbHsFsManagerGetLogicalUnitContextForFatFsDriveNumber(pdrv));
    mounted[pdrv] = NULL;
}

static bool usbHsFsMountRegisterDevoptabDevice(UsbHsFsDriveLogicalUnitFileSystemContext *fs)
{
    assert(fs->fatfs);
    return !devoptab_fail;
}

static bool usbHsFsScsiReadLogicalUnitBlocks(UsbHsFsDriveLogicalUnitContext *lun, BYTE *buffer, LBA_t sector, UINT count)
{
    assert(sector == 0 && count == 1);
    memcpy(buffer, lun->contents, 512);
    return true;
}

static bool usbHsFsScsiWriteLogicalUnitBlocks(UsbHsFsDriveLogicalUnitContext *lun, const BYTE *buffer, LBA_t sector, UINT count)
{
    assert(sector == 0 && count == 1);
    memcpy(lun->contents, buffer, 512);
    return true;
}
'''

tests = r'''
static void *second_operation(void *arg)
{
    UsbHsFsDriveContext *drive = g_driveContexts[0];
    watch_drive = true;
    if (arg)
    {
        mutexLock(&g_managerMutex);
        mutexLock(&drive->mutex);
        char name[MOUNT_NAME_LENGTH];
        usbHsFsMountUnregisterFatVolume(name, drive->lun_ctx[0]->fs_ctx[0]);
        g_driveCount = 0;
        mutexUnlock(&drive->mutex);
        mutexUnlock(&g_managerMutex);
    }
    else
    {
        assert(usbHsFsManagerIsDriveContextPointerValid(drive));
        mutexUnlock(&drive->mutex);
    }
    return NULL;
}

static void concurrent_lookup(bool detach)
{
    UsbHsFsDriveContext *drive = g_driveContexts[0];
    pthread_t thread;
    BYTE buffer[512];
    assert(usbHsFsManagerIsDriveContextPointerValid(drive));
    contended_drive = &drive->mutex;
    atomic_store(&drive_waiting, 0);
    assert(!pthread_create(&thread, NULL, second_operation, detach ? drive : NULL));
    while (!atomic_load(&drive_waiting)) sched_yield();
    assert(ff_disk_read(0, buffer, 0, 1) == RES_OK);
    assert(buffer[0] == drive->lun_ctx[0]->contents[0]);
    mutexUnlock(&drive->mutex);
    assert(!pthread_join(thread, NULL));
}

int main(int argc, char **argv)
{
    UsbHsFsDriveLogicalUnitContext lun = { .block_length = 512, .block_count = 100 };
    UsbHsFsDriveLogicalUnitFileSystemContext fs = { .lun_ctx = &lun, .fs_type = 1 };
    UsbHsFsDriveLogicalUnitFileSystemContext *filesystems[] = { &fs };
    UsbHsFsDriveLogicalUnitContext *units[] = { &lun };
    UsbHsFsDriveContext drive = { .mutex = PTHREAD_MUTEX_INITIALIZER, .lun_count = 1, .lun_ctx = units };
    UsbHsFsDriveContext *drives[] = { &drive };
    BYTE block[512] = {0}, buffer[512];
    char name[MOUNT_NAME_LENGTH];
    bool detach = argc > 1 && !strcmp(argv[1], "detach");
    lun.fs_ctx = filesystems;
    lun.fs_count = 1;
    lun.contents[0] = 17;
    g_driveContexts = drives;
    g_driveCount = 1;
    mutexLock(&g_managerMutex);
    assert(usbHsFsMountRegisterFatVolume(&fs, block, 0));
    mutexUnlock(&g_managerMutex);

    concurrent_lookup(detach);
    if (!detach)
    {
        mutexLock(&g_managerMutex);
        mutexLock(&drive.mutex);
        usbHsFsMountUnregisterFatVolume(name, &fs);
        mutexUnlock(&drive.mutex);
        mutexUnlock(&g_managerMutex);
    }
    assert(!fs.fatfs && !mounted[0]);
    mutexLock(&g_managerMutex);
    assert(!usbHsFsManagerGetLogicalUnitContextForFatFsDriveNumber(0));
    assert(!usbHsFsManagerGetLogicalUnitContextForFatFsDriveNumber(FF_VOLUMES));
    assert(ff_disk_read(0, buffer, 0, 1) == RES_PARERR);
    g_driveCount = 1;
    lun.contents[0] = 42;
    assert(usbHsFsMountRegisterFatVolume(&fs, block, 0));
    assert(ff_disk_read(0, buffer, 0, 1) == RES_OK && buffer[0] == 42);
    buffer[0] = 53;
    assert(ff_disk_write(0, buffer, 0, 1) == RES_OK);
    assert(ff_disk_read(0, buffer, 0, 1) == RES_OK && buffer[0] == 53);

    UsbHsFsDriveLogicalUnitContext other_lun = { .block_length = 512, .block_count = 100, .contents = {99} };
    UsbHsFsDriveLogicalUnitFileSystemContext other_fs = { .lun_ctx = &other_lun, .fs_type = 1 };
    assert(usbHsFsMountRegisterFatVolume(&other_fs, block, 0));
    assert(other_fs.fatfs->pdrv == 1);
    assert(ff_disk_read(1, buffer, 0, 1) == RES_OK && buffer[0] == 99);
    usbHsFsMountUnregisterFatVolume(name, &fs);
    assert(ff_disk_read(1, buffer, 0, 1) == RES_OK && buffer[0] == 99);
    usbHsFsMountUnregisterFatVolume(name, &other_fs);

    mount_fail = true;
    assert(!usbHsFsMountRegisterFatVolume(&fs, block, 0));
    assert(!fs.fatfs && !mounted[0] && !usbHsFsManagerGetLogicalUnitContextForFatFsDriveNumber(0));
    mount_fail = false;
    devoptab_fail = true;
    assert(!usbHsFsMountRegisterFatVolume(&fs, block, 0));
    assert(!fs.fatfs && !mounted[0] && !usbHsFsManagerGetLogicalUnitContextForFatFsDriveNumber(0));
    devoptab_fail = false;
    assert(usbHsFsMountRegisterFatVolume(&fs, block, 0));
    usbHsFsMountUnregisterFatVolume(name, &fs);
    mutexUnlock(&g_managerMutex);
    puts("USB concurrent lookup, unmount, slot reuse and mount failure: passed");
    return 0;
}
'''


def harness(sources):
    manager, mount, utils, disk = (sources[name] for name in (
        'source/usbhsfs_manager.c', 'source/usbhsfs_mount.c',
        'source/usbhsfs_utils.h', 'source/fatfs/diskio.c'))
    locks = re.search(r'^#define SCOPED_LOCK.*$', utils, re.M)[0] + '\n'
    locks += re.search(r'typedef struct \{[^}]+\} UsbHsFsUtilsScopedLock;', utils)[0] + '\n'
    locks += definition(utils, 'usbHsFsUtilsLockScope') + '\n'
    locks += definition(utils, 'usbHsFsUtilsUnlockScope') + '\n'
    table = re.search(r'^static .*g_fatFsVolumeTable\[FF_VOLUMES\].*$', mount, re.M)[0]
    lookup = 'usbHsFsMountGetLogicalUnitContextForFatFsDriveNumber'
    functions = table + '\n'
    if lookup in mount:
        functions += definition(mount, lookup) + '\n'
    for name in ('usbHsFsManagerIsDriveContextPointerValid',
                 'usbHsFsManagerGetLogicalUnitContextForFatFsDriveNumber'):
        functions += definition(manager, name) + '\n'
    for name in ('usbHsFsMountRegisterFatVolume', 'usbHsFsMountUnregisterFatVolume'):
        functions += definition(mount, name) + '\n'
    disk = re.sub(r'^#include .*$', '', disk, flags=re.M)
    return fixture + locks + functions + disk + tests


with tempfile.TemporaryDirectory(prefix='wine-nx-usb-') as tmp:
    tmp = Path(tmp)
    paths = {'source/usbhsfs_utils.h'}
    for patch in patches:
        paths.update(re.findall(r'^\+\+\+ b/(.+)$', patch.read_text(), re.M))
    for path in paths:
        target = tmp / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(subprocess.check_output(['git', '-C', str(vendor), 'show', f'HEAD:{path}']))
    before = None
    for patch in patches:
        if patch.name.startswith('0005-'):
            before = {path: (tmp / path).read_text() for path in paths}
        subprocess.run(['git', 'apply', '-'], cwd=tmp, input=patch.read_text(), text=True, check=True)
    after = {path: (tmp / path).read_text() for path in paths}
    assert before is not None
    for name, sources in (('before', before), ('after', after)):
        source, binary = tmp / f'{name}.c', tmp / name
        source.write_text(harness(sources))
        subprocess.run(['cc', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Wno-unused-function',
                        '-Werror', '-pthread', str(source), '-o', str(binary)], check=True)
        for scenario in ('read', 'detach'):
            try:
                subprocess.run([str(binary), scenario], check=True, timeout=3)
            except subprocess.TimeoutExpired:
                assert name == 'before', f'Patched USB deadlocked during {scenario}'
                print(f'Original USB lock inversion reproduced ({scenario})', flush=True)
            else:
                assert name == 'after', 'Original USB unexpectedly avoided the lock inversion'
    print('USB patch series applies cleanly; regression tests passed')
