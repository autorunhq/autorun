/* Exercise the registry wire adapter, including actual protocol layouts, and
 * the hives it saves and loads. */
#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "wine/server_protocol.h"
#include "../../dlls/ntdll/unix/horizon_registry.h"
struct horizon_server_request_header { int req; unsigned int request_size, reply_size; };
struct horizon_server_reply_header { unsigned int error, reply_size; };
#include "../../dlls/ntdll/unix/horizon_registry_wire.h"
#define HORIZON_REQ_CREATE_KEY REQ_create_key
#define HORIZON_REQ_OPEN_KEY REQ_open_key
#define HORIZON_REQ_DELETE_KEY REQ_delete_key
#define HORIZON_REQ_ENUM_KEY REQ_enum_key
#define HORIZON_REQ_SET_KEY_VALUE REQ_set_key_value
#define HORIZON_REQ_GET_KEY_VALUE REQ_get_key_value
#define HORIZON_REQ_ENUM_KEY_VALUE REQ_enum_key_value
#define HORIZON_REQ_DELETE_KEY_VALUE REQ_delete_key_value
#define HORIZON_REQ_SET_REGISTRY_NOTIFICATION REQ_set_registry_notification
#define HORIZON_REQ_RENAME_KEY REQ_rename_key
#define HORIZON_STATUS_INVALID_HANDLE 0xc0000008u
#define HORIZON_STATUS_BUFFER_TOO_SMALL 0xc0000023u
#define HORIZON_STATUS_NOT_IMPLEMENTED 0xc0000002u
#define HORIZON_SERVER_OBJECT_REG_KEY 1
#define HORIZON_SERVER_OBJECT_EVENT 2
struct horizon_server_connection { int reply_fd; };
struct horizon_server_object { int type; unsigned int refs, signaled; struct horizon_reg_key *reg_key; };
struct horizon_server_handle_entry { unsigned int handle; struct horizon_server_object *object; };
struct horizon_object_attributes { unsigned int rootdir, attributes, sd_len, name_len; };
struct horizon_object_name { unsigned int rootdir, name_len; const unsigned char *name; };
static struct horizon_reg horizon_registry;
static pthread_mutex_t horizon_server_objects_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct horizon_server_handle_entry handles[64];
static unsigned int handle_count;
static unsigned char last_reply[64], payload[2048];
static unsigned int payload_size;
static struct horizon_server_handle_entry *horizon_server_find_handle_locked(unsigned int h)
{ return h && h <= handle_count && handles[h-1].object ? &handles[h-1] : NULL; }
static struct horizon_server_handle_entry *horizon_server_create_handle_locked(int type)
{
    struct horizon_server_handle_entry *e = &handles[handle_count++];
    assert(handle_count <= 64);
    e->handle = handle_count; e->object = calloc(1, sizeof(*e->object));
    e->object->type = type; e->object->refs = 1; return e;
}
static void horizon_server_free_object(struct horizon_server_object *o)
{ if(o->reg_key) horizon_reg_release(&horizon_registry, o->reg_key); free(o); }
static int horizon_server_write_reply(int fd, const void *r, unsigned int size, const void *data, unsigned int len)
{
    (void)fd; assert(size <= sizeof(last_reply) && len <= sizeof(payload));
    memset(last_reply,0,sizeof(last_reply)); memcpy(last_reply,r,size);
    if(len) memcpy(payload,data,len); payload_size = len; return 0;
}
static int horizon_server_write_status(int fd, unsigned int status)
{ struct horizon_server_reply_header r = {status,0}; return horizon_server_write_reply(fd,&r,sizeof(r),NULL,0); }
static unsigned int horizon_server_parse_object_attributes(const unsigned char *data, unsigned int size, struct horizon_object_name *n)
{
    const struct horizon_object_attributes *a = (const void *)data;
    if(size < sizeof(*a) || a->sd_len > size-sizeof(*a) || a->name_len > size-sizeof(*a)-a->sd_len) return HORIZON_REG_INVALID_PARAMETER;
    n->rootdir=a->rootdir; n->name=data+sizeof(*a)+a->sd_len; n->name_len=a->name_len; return 0;
}
static unsigned int horizon_server_object_attributes_size(const unsigned char *data, unsigned int size, unsigned int *offset)
{
    const struct horizon_object_attributes *a=(const void *)data;
    *offset=(sizeof(*a)+a->sd_len+a->name_len+3)&~3u;
    return *offset > size ? HORIZON_REG_INVALID_PARAMETER : 0;
}
static void horizon_server_signal_changed_locked(void) { }  /* horizon.c wakes pending waits */
/* The hives go to a scratch directory, where rename fails over an existing
 * file as Horizon's RenameFile does. */
static char registry_dir[256];
#define HORIZON_REGISTRY_DIR registry_dir
static int horizon_rename(const char *from, const char *to)
{
    if (!access(to, F_OK)) { errno = EEXIST; return -1; }
    return rename(from, to);
}
static unsigned int hive_writes[2];
static int fail_write;
static void (*during_write)(void);
static FILE *checked_fopen(const char *path, const char *mode)
{
    if (!strcmp(mode, "wb"))
    {
        /* Disk I/O must never retain the shared server mutex. */
        assert(!pthread_mutex_trylock(&horizon_server_objects_mutex));
        pthread_mutex_unlock(&horizon_server_objects_mutex);
        hive_writes[strstr(path, "user.reg") != NULL]++;
        if (during_write) { void (*callback)(void) = during_write; during_write = NULL; callback(); }
        if (fail_write) { errno = EIO; return NULL; }
    }
    return fopen(path, mode);
}
#define fopen checked_fopen
#define rename horizon_rename
#include "../../dlls/ntdll/unix/horizon_registry_server.h"
/* The runtime's log; macOS will not link a weak reference nothing defines. */
int wine_nx_runtime_verbose;
void wine_nx_runtime_trace( const char *msg ) { (void)msg; }
#undef rename
#undef fopen
#define CHECK_LAYOUT(n) _Static_assert(sizeof(struct horizon_##n) == sizeof(struct n), #n)
CHECK_LAYOUT(create_key_request); CHECK_LAYOUT(create_key_reply);
CHECK_LAYOUT(open_key_request); CHECK_LAYOUT(open_key_reply);
CHECK_LAYOUT(enum_key_request); CHECK_LAYOUT(enum_key_reply);
CHECK_LAYOUT(get_key_value_reply); CHECK_LAYOUT(enum_key_value_reply);
CHECK_LAYOUT(set_registry_notification_request);
_Static_assert(offsetof(struct horizon_enum_key_reply, modif) == offsetof(struct enum_key_reply, modif), "timestamp offset");
static struct horizon_server_connection connection;
static unsigned int status(void) { return ((struct horizon_server_reply_header *)last_reply)->error; }
static unsigned int wide(unsigned short *out, const char *in)
{ unsigned int n=0; while(*in) out[n++]=(unsigned char)*in++; return n*2; }
static unsigned int open_path(unsigned int root, const char *path)
{
    unsigned short name[256];
    struct horizon_open_key_request req = {{REQ_open_key,0,0},root,0,0};
    horizon_server_handle_registry(&connection,(void *)&req,(void *)name,wide(name,path));
    assert(!status()); return ((struct horizon_open_key_reply *)last_reply)->hkey;
}
static const unsigned short none[1];
static struct horizon_reg_key *open_key(struct horizon_reg_key *base, const unsigned short *path, unsigned int len)
{
    struct horizon_reg_key *key;
    return horizon_reg_open(&horizon_registry, base, path, len, 0, &key) ? NULL : key;
}
static struct horizon_reg_key *machine_key(void)
{
    static const unsigned short name[] = u"Machine";
    unsigned int index;
    return horizon_reg_find_subkey(horizon_registry.root, name, sizeof(name) - 2, &index);
}
/* horizon_reg_create makes only the last element of a path. */
static struct horizon_reg_key *create_path(const unsigned short *path, unsigned int len, unsigned int options,
                                           const unsigned short *class, unsigned int classlen)
{
    struct horizon_reg_key *key = NULL;
    unsigned int end;
    for (end = 2; end <= len; end += 2)
    {
        if (end < len && path[end / 2] != '\\') continue;
        if (key) horizon_reg_release(&horizon_registry, key);
        horizon_reg_create(&horizon_registry, machine_key(), path, end, 0, end == len ? options : 0,
                           end == len ? class : NULL, end == len ? classlen : 0, &key);
        assert(key);
    }
    return key;
}
static void check_value(struct horizon_reg_key *base, const unsigned short *path, unsigned int path_len,
                        const unsigned short *name, unsigned int namelen, int type, const void *data, unsigned int len)
{
    struct horizon_reg_key *key = open_key(base, path, path_len);
    unsigned char buffer[256];
    unsigned int total = 0, size;
    int got;
    assert(key);
    assert(horizon_reg_get_value(key, name, namelen, &got, &total, buffer, sizeof(buffer), &size) == HORIZON_REG_SUCCESS);
    assert(got == type && total == len && size == len && !memcmp(buffer, data, len));
    horizon_reg_release(&horizon_registry, key);
}
static int key_exists(const unsigned short *path, unsigned int len)
{
    struct horizon_reg_key *key = open_key(machine_key(), path, len);
    if (key) horizon_reg_release(&horizon_registry, key);
    return key != NULL;
}
/* A restart of the runtime: the hives load again from the files. */
static void reload(void)
{
    horizon_reg_release(&horizon_registry, horizon_registry.root);
    horizon_registry.root = NULL;
    assert(!horizon_registry_init());
}
static void test_machine_identity(void)
{
    static const unsigned short *computer_paths[] = {
        u"System\\CurrentControlSet\\Control\\ComputerName\\ComputerName",
        u"System\\CurrentControlSet\\Control\\ComputerName\\ActiveComputerName"
    };
    static const unsigned short parameters[] = u"System\\CurrentControlSet\\Services\\Tcpip\\Parameters";
    static const unsigned short computer[] = u"ComputerName", hostname[] = u"Hostname", domain[] = u"Domain";
    static const unsigned short netbios[] = u"WINE-NX", dns[] = u"wine-nx", custom[] = u"MY-SWITCH";
    unsigned int i, len;
    struct horizon_reg_key *key;

    for (i = 0; i < 2; i++)
    {
        for (len = 0; computer_paths[i][len / 2]; len += 2) {}
        check_value(machine_key(), computer_paths[i], len, computer, sizeof(computer) - 2,
                    HORIZON_REG_SZ, netbios, sizeof(netbios));
        key = open_key(machine_key(), computer_paths[i], len);
        assert(!horizon_reg_set_value(&horizon_registry, key, computer, sizeof(computer) - 2,
                                      HORIZON_REG_SZ, custom, sizeof(custom)));
        horizon_reg_release(&horizon_registry, key);
    }
    check_value(machine_key(), parameters, sizeof(parameters) - 2, hostname, sizeof(hostname) - 2,
                HORIZON_REG_SZ, dns, sizeof(dns));
    check_value(machine_key(), parameters, sizeof(parameters) - 2, domain, sizeof(domain) - 2,
                HORIZON_REG_SZ, none, sizeof(none));
    assert(horizon_registry_flush());
    reload();
    for (i = 0; i < 2; i++)
    {
        for (len = 0; computer_paths[i][len / 2]; len += 2) {}
        check_value(machine_key(), computer_paths[i], len, computer, sizeof(computer) - 2,
                    HORIZON_REG_SZ, custom, sizeof(custom));
    }
    puts("Machine identity: NetBIOS/DNS defaults and saved overrides passed");
}
/* The DirectShow filters of horizon_directshow_reg.h are in the registry
 * after every start, found whatever the case of the path, and survive a
 * restart: what a filter graph reads before it connects a file to the screen. */
static void test_directshow_filters(void)
{
    static const unsigned short categories[] =
        u"software\\classes\\clsid\\{da4e3da0-d07d-11d0-bd50-00a0c911ce86}\\instance\\{083863f1-70de-11d0-bd40-00a0c911ce86}";
    static const unsigned short audio[] =
        u"Software\\Classes\\CLSID\\{DA4E3DA0-D07D-11D0-BD50-00A0C911CE86}\\Instance\\{E0F158E1-CB04-11D0-BD4E-00A0C911CE86}";
    static const unsigned short *filters[] = {
        u"Software\\Classes\\CLSID\\{083863F1-70DE-11D0-BD40-00A0C911CE86}\\Instance\\{F9D8D64E-A144-47DC-8EE0-F53498372C29}",
        u"Software\\Classes\\CLSID\\{083863F1-70DE-11D0-BD40-00A0C911CE86}\\Instance\\{70E102B0-5556-11CE-97C0-00AA0055595A}",
        u"Software\\Classes\\CLSID\\{083863F1-70DE-11D0-BD40-00A0C911CE86}\\Instance\\{6BC1CFFA-8FC1-4261-AC22-CFB4CC38DB50}",
        u"Software\\Classes\\CLSID\\{083863F1-70DE-11D0-BD40-00A0C911CE86}\\Instance\\{E436EBB5-524F-11CE-9F53-0020AF0BA770}",
        u"Software\\Classes\\CLSID\\{083863F1-70DE-11D0-BD40-00A0C911CE86}\\Instance\\{6A08CF80-0E18-11CF-A24D-0020AFD79767}",
        u"Software\\Classes\\CLSID\\{083863F1-70DE-11D0-BD40-00A0C911CE86}\\Instance\\{CF49D4E0-1115-11CE-B03A-0020AF0BA770}",
    };
    /* Version 2, MERIT_NORMAL - 1, three pins; the first takes a stream. */
    static const unsigned char splitter_start[] = { 2,0,0,0, 0xff,0xff,0x5f,0, 3,0,0,0, 0,0,0,0, '0','p','i','3' };
    static const unsigned short merit[] = u"Merit", friendly[] = u"FriendlyName", clsid[] = u"CLSID", data[] = u"FilterData";
    static const unsigned short category_name[] = u"DirectShow Filters", splitter_name[] = u"GStreamer splitter filter";
    static const unsigned short splitter_clsid[] = u"{F9D8D64E-A144-47DC-8EE0-F53498372C29}";
    static const unsigned int normal = 0x600000;
    unsigned char buffer[512];
    unsigned int pass, i, len, total, size;
    struct horizon_reg_key *key;
    int type;

    for (pass = 0; pass < 2; pass++)
    {
        check_value(machine_key(), categories, sizeof(categories) - 2, merit, sizeof(merit) - 2,
                    HORIZON_REG_DWORD, &normal, sizeof(normal));
        check_value(machine_key(), categories, sizeof(categories) - 2, friendly, sizeof(friendly) - 2,
                    HORIZON_REG_SZ, category_name, sizeof(category_name));
        check_value(machine_key(), audio, sizeof(audio) - 2, merit, sizeof(merit) - 2,
                    HORIZON_REG_DWORD, &normal, sizeof(normal));
        for (i = 0; i < sizeof(filters) / sizeof(filters[0]); i++)
        {
            for (len = 0; filters[i][len / 2]; len += 2) {}
            key = open_key(machine_key(), filters[i], len);
            assert(key);
            total = size = 0;
            assert(horizon_reg_get_value(key, data, sizeof(data) - 2, &type, &total, buffer, sizeof(buffer), &size)
                   == HORIZON_REG_SUCCESS);
            /* The fixed part and at least the two GUIDs one media type names. */
            assert(type == HORIZON_REG_BINARY && total == size && size >= 16 + 24 + 16 + 32);
            assert(buffer[0] == 2 && !buffer[1] && !buffer[2] && !buffer[3]);
            if (!i) assert(size == 200 && !memcmp(buffer, splitter_start, sizeof(splitter_start)));
            horizon_reg_release(&horizon_registry, key);
        }
        for (len = 0; filters[0][len / 2]; len += 2) {}
        check_value(machine_key(), filters[0], len, friendly, sizeof(friendly) - 2,
                    HORIZON_REG_SZ, splitter_name, sizeof(splitter_name));
        check_value(machine_key(), filters[0], len, clsid, sizeof(clsid) - 2,
                    HORIZON_REG_SZ, splitter_clsid, sizeof(splitter_clsid));
        if (!pass)
        {
            assert(horizon_registry_flush());
            reload();
        }
    }
    puts("DirectShow: two categories and six filters registered at start passed");
}
static void read_file(const char *path, char *buffer, size_t size)
{
    FILE *file = fopen(path, "rb");
    size_t len;
    assert(file);
    len = fread(buffer, 1, size - 1, file);
    buffer[len] = 0;
    fclose(file);
}
/* Keys and values survive a restart in Wine's format, after saves over
 * existing hives and a stop between removing a hive and moving its successor in. */
static void test_save_and_load(void)
{
    static const unsigned short clsid[] = u"Software\\Classes\\CLSID\\{4315d437-5b8c-11d0-bd3b-00a0c911ce86}\\InprocServer32";
    static const unsigned short seed[] = u"Software\\Classes\\CLSID\\{BCDE0395-E52F-467C-8E3D-C4579291692E}\\InprocServer32";
    static const unsigned short odd[] = u"Software\\Wine-NX [\"x\"] \u00e9\\sub";
    static const unsigned short empty[] = u"Software\\Empty", volatile_key[] = u"Software\\Volatile";
    static const unsigned short user_path[] = u"\\Registry\\User\\S-1-5-21-0-0-0-1000", drivers[] = u"Software\\Wine\\Drivers";
    static const unsigned short class[] = u"cls\"\\";
    static const unsigned short dll[] = u"devenum.dll", mmdevapi[] = u"mmdevapi.dll", both[] = u"Both";
    static const unsigned short model[] = u"ThreadingModel", quote_name[] = u"quote\"back\\slash";
    static const unsigned short text[] = u"line\nnext \"q\" \\ \u263aa \u00e9g \u00e9b \x01" u"7 \x01";
    static const unsigned short multi[] = u"a\0b\0", ab[] = u"ab";
    static const unsigned short dword_name[] = u"dword", binary_name[] = u"binary", multi_name[] = u"multi";
    static const unsigned short unterminated_name[] = u"unterminated", odd_name[] = u"odd";
    static const unsigned short test_name[] = u"Test", saved_name[] = u"Saved";
    static const unsigned short memory[] = u"System\\CurrentControlSet\\Control\\Session Manager\\Memory Management";
    static const unsigned short paging_name[] = u"PagingFiles", paging[] = u"C:\\pagefile.sys 27 77";
    static const unsigned short physx[] = u"Software\\NVIDIA Corporation\\PhysX\\Runtimes";
    static const unsigned short core_path[] = u"PhysXCore Path", engine[] = u"C:\\physx\\Engine";
    static const unsigned short custom_engine[] = u"C:\\custom\\Engine";
    unsigned char binary[100], odd_data[3] = {1, 2, 3}, one[4] = {1, 0, 0, 0};
    unsigned int dword = 0x12345678, i;
    struct horizon_reg_key *key, *user;
    char path[512], tmp[512], user_file[512], text_file[8192];

    for (i = 0; i < sizeof(binary); i++) binary[i] = i * 7;
    check_value(machine_key(), physx, sizeof(physx) - 2, core_path, sizeof(core_path) - 2,
                HORIZON_REG_SZ, engine, sizeof(engine));
    key = open_key(machine_key(), physx, sizeof(physx) - 2);
    assert(!horizon_reg_set_value(&horizon_registry, key, core_path, sizeof(core_path) - 2,
                                  HORIZON_REG_SZ, custom_engine, sizeof(custom_engine)));
    horizon_reg_release(&horizon_registry, key);
    snprintf(path, sizeof(path), "%sregistry/system.reg", registry_dir);
    snprintf(tmp, sizeof(tmp), "%sregistry/system.reg.tmp", registry_dir);
    snprintf(user_file, sizeof(user_file), "%sregistry/user.reg", registry_dir);
    /* main's requests are persisted in one maintenance pass. */
    horizon_registry_flush();
    assert(!access(path, F_OK) && !access(user_file, F_OK) && access(tmp, F_OK));

    key = create_path(clsid, sizeof(clsid) - 2, 0, NULL, 0);
    assert(!horizon_reg_set_value(&horizon_registry, key, none, 0, HORIZON_REG_SZ, dll, sizeof(dll)));
    assert(!horizon_reg_set_value(&horizon_registry, key, model, sizeof(model) - 2, HORIZON_REG_SZ, both, sizeof(both)));
    horizon_reg_release(&horizon_registry, key);
    key = create_path(odd, sizeof(odd) - 2, 0, class, sizeof(class) - 2);
    assert(!horizon_reg_set_value(&horizon_registry, key, quote_name, sizeof(quote_name) - 2, HORIZON_REG_SZ, text, sizeof(text)));
    assert(!horizon_reg_set_value(&horizon_registry, key, dword_name, sizeof(dword_name) - 2, HORIZON_REG_DWORD, &dword, 4));
    assert(!horizon_reg_set_value(&horizon_registry, key, binary_name, sizeof(binary_name) - 2, HORIZON_REG_BINARY, binary, sizeof(binary)));
    assert(!horizon_reg_set_value(&horizon_registry, key, multi_name, sizeof(multi_name) - 2, HORIZON_REG_MULTI_SZ, multi, sizeof(multi)));
    assert(!horizon_reg_set_value(&horizon_registry, key, unterminated_name, sizeof(unterminated_name) - 2, HORIZON_REG_SZ, ab, 4));
    assert(!horizon_reg_set_value(&horizon_registry, key, odd_name, sizeof(odd_name) - 2, HORIZON_REG_EXPAND_SZ, odd_data, 3));
    assert(!horizon_reg_set_value(&horizon_registry, key, none, 0, HORIZON_REG_NONE, none, 0));
    horizon_reg_release(&horizon_registry, key);
    horizon_reg_release(&horizon_registry, create_path(empty, sizeof(empty) - 2, 0, NULL, 0));
    key = create_path(volatile_key, sizeof(volatile_key) - 2, HORIZON_REG_OPTION_VOLATILE, NULL, 0);
    assert(!horizon_reg_set_value(&horizon_registry, key, dword_name, sizeof(dword_name) - 2, HORIZON_REG_DWORD, &dword, 4));
    horizon_reg_release(&horizon_registry, key);

    horizon_registry_flush();  /* persist the batched mutations */
    assert(access(tmp, F_OK) && !access(path, F_OK));
    read_file(path, text_file, sizeof(text_file));
    assert(strstr(text_file, "WINE REGISTRY Version 2\n;; All keys relative to \\\\Machine\n") == text_file);
    assert(strstr(text_file, "\n[Software\\\\Classes\\\\CLSID\\\\{4315d437-5b8c-11d0-bd3b-00a0c911ce86}\\\\InprocServer32] "));
    assert(strstr(text_file, "\n\"quote\\\"back\\\\slash\"=\"line\\nnext \\\"q\\\" \\\\ \\x263aa \\xe9g \\x00e9b \\0017 \\1\"\n"));
    read_file(user_file, text_file, sizeof(text_file));
    assert(strstr(text_file, "WINE REGISTRY Version 2\n;; All keys relative to \\\\User\\\\S-1-5-21-0-0-0-1000\n") == text_file);
    assert(strstr(text_file, "\n[Software\\\\Wine\\\\Drivers] "));

    reload();
    check_value(machine_key(), physx, sizeof(physx) - 2, core_path, sizeof(core_path) - 2,
                HORIZON_REG_SZ, custom_engine, sizeof(custom_engine));
    check_value(machine_key(), seed, sizeof(seed) - 2, none, 0, HORIZON_REG_SZ, mmdevapi, sizeof(mmdevapi));
    /* wine.inf's paging file, which Aspyr's AWL.dll wants a drive in. */
    check_value(machine_key(), memory, sizeof(memory) - 2, paging_name, sizeof(paging_name) - 2, HORIZON_REG_SZ,
                paging, sizeof(paging));
    check_value(machine_key(), seed, sizeof(seed) - 2, saved_name, sizeof(saved_name) - 2, HORIZON_REG_DWORD, one, 4);
    check_value(machine_key(), clsid, sizeof(clsid) - 2, none, 0, HORIZON_REG_SZ, dll, sizeof(dll));
    check_value(machine_key(), clsid, sizeof(clsid) - 2, model, sizeof(model) - 2, HORIZON_REG_SZ, both, sizeof(both));
    check_value(machine_key(), odd, sizeof(odd) - 2, quote_name, sizeof(quote_name) - 2, HORIZON_REG_SZ, text, sizeof(text));
    check_value(machine_key(), odd, sizeof(odd) - 2, dword_name, sizeof(dword_name) - 2, HORIZON_REG_DWORD, &dword, 4);
    check_value(machine_key(), odd, sizeof(odd) - 2, binary_name, sizeof(binary_name) - 2, HORIZON_REG_BINARY, binary, sizeof(binary));
    check_value(machine_key(), odd, sizeof(odd) - 2, multi_name, sizeof(multi_name) - 2, HORIZON_REG_MULTI_SZ, multi, sizeof(multi));
    check_value(machine_key(), odd, sizeof(odd) - 2, unterminated_name, sizeof(unterminated_name) - 2, HORIZON_REG_SZ, ab, 4);
    check_value(machine_key(), odd, sizeof(odd) - 2, odd_name, sizeof(odd_name) - 2, HORIZON_REG_EXPAND_SZ, odd_data, 3);
    check_value(machine_key(), odd, sizeof(odd) - 2, none, 0, HORIZON_REG_NONE, none, 0);
    assert((key = open_key(machine_key(), odd, sizeof(odd) - 2)));
    assert(key->classlen >= sizeof(class) - 2 && !memcmp(key->class, class, sizeof(class) - 2));
    horizon_reg_release(&horizon_registry, key);
    assert(key_exists(empty, sizeof(empty) - 2) && !key_exists(volatile_key, sizeof(volatile_key) - 2));
    assert((user = open_key(NULL, user_path, sizeof(user_path) - 2)));
    check_value(user, drivers, sizeof(drivers) - 2, test_name, sizeof(test_name) - 2, HORIZON_REG_DWORD, one, 4);
    horizon_reg_release(&horizon_registry, user);

    /* stopped after removing system.reg, before moving system.reg.tmp in */
    assert(!rename(path, tmp));
    reload();
    check_value(machine_key(), clsid, sizeof(clsid) - 2, none, 0, HORIZON_REG_SZ, dll, sizeof(dll));
    horizon_registry_changed(machine_key());
    horizon_registry_flush();
    assert(access(tmp, F_OK) && !access(path, F_OK));

    horizon_reg_release(&horizon_registry, horizon_registry.root);
    horizon_registry.root = NULL;
    assert(!unlink(path) && !unlink(user_file));
    snprintf(path, sizeof(path), "%sregistry", registry_dir);
    assert(!rmdir(path) && !rmdir(registry_dir));
}
static struct horizon_reg_key *concurrent_key;
static void mutate_during_write(void)
{
    unsigned int value = 999;
    pthread_mutex_lock(&horizon_server_objects_mutex);
    assert(!horizon_reg_set_value(&horizon_registry, concurrent_key, none, 0,
                                  HORIZON_REG_DWORD, &value, sizeof(value)));
    pthread_mutex_unlock(&horizon_server_objects_mutex);
}
static void test_batching(void)
{
    static const unsigned short path[] = u"Software\\BatchTest";
    struct horizon_reg_key *key;
    unsigned int before[2], i, value;
    unsigned long long generation;
    horizon_registry_flush();
    memcpy(before, hive_writes, sizeof(before));
    key = create_path(path, sizeof(path) - 2, 0, NULL, 0);
    for (value = 0; value < 100; value++)
        assert(!horizon_reg_set_value(&horizon_registry, key, none, 0,
                                     HORIZON_REG_DWORD, &value, sizeof(value)));
    assert(!memcmp(before, hive_writes, sizeof(before)));
    generation = horizon_registry_generation[0];
    value = 99;
    assert(!horizon_reg_set_value(&horizon_registry, key, none, 0, HORIZON_REG_DWORD, &value, sizeof(value)));
    assert(generation == horizon_registry_generation[0]);
    concurrent_key = key;
    during_write = mutate_during_write;
    horizon_registry_flush();
    assert(hive_writes[0] == before[0] + 1 && hive_writes[1] == before[1]);
    assert(horizon_registry_saved[0] != horizon_registry_generation[0]);
    horizon_registry_flush();
    assert(hive_writes[0] == before[0] + 2);
    horizon_registry_flush();
    assert(hive_writes[0] == before[0] + 2);
    /* Empty unchanged values also avoid a save. */
    assert(!horizon_reg_set_value(&horizon_registry, key, none, 0, HORIZON_REG_NONE, NULL, 0));
    generation = horizon_registry_generation[0];
    assert(!horizon_reg_set_value(&horizon_registry, key, none, 0, HORIZON_REG_NONE, NULL, 0));
    assert(generation == horizon_registry_generation[0]);
    fail_write = 1;
    horizon_registry_flush();
    assert(horizon_registry_saved[0] != generation);
    fail_write = 0;
    horizon_registry_flush();
    assert(horizon_registry_saved[0] == generation);
    for (i = 0; i < 2; i++) before[i] = hive_writes[i];
    assert(!horizon_reg_delete(&horizon_registry, key));
    assert(!key->parent);
    horizon_registry_flush();
    assert(hive_writes[0] == before[0] + 1 && hive_writes[1] == before[1]);
    horizon_reg_release(&horizon_registry, key);
    puts("Registry batching: unchanged values, hive-specific delete, retry, concurrent mutation and unlocked I/O passed");
}
static void test_session_key(void)
{
    static const char path[] = "Software\\Valve\\Steam\\ActiveProcess";
    unsigned short wide_path[256], name[32];
    unsigned int length = wide(wide_path, path), name_length = wide(name, "pid"), index;
    uint32_t original = 27, running = 1;
    struct horizon_reg_key *key, *saved;
    char file_path[512], text[65536];
    FILE *file;
    size_t size;

    assert(horizon_registry_make_session_key(NULL) == HORIZON_REG_INVALID_PARAMETER);
    assert(horizon_registry_make_session_key("\\bad") == HORIZON_REG_INVALID_PARAMETER);
    assert(!horizon_registry_make_session_key(path));
    assert(!horizon_reg_open(&horizon_registry, horizon_registry_hives[1], wide_path, length, 0, &key));
    assert(key->flags & HORIZON_REG_FLAG_VOLATILE);
    assert(!horizon_reg_set_value(&horizon_registry, key, name, name_length, HORIZON_REG_DWORD, &running, 4));
    assert(horizon_registry_flush());
    snprintf(file_path, sizeof(file_path), "%s" HORIZON_REGISTRY_SUBDIR "user.reg", registry_dir);
    file = fopen(file_path, "rb"); assert(file);
    size = fread(text, 1, sizeof(text) - 1, file); fclose(file); text[size] = 0;
    assert(!strstr(text, "ActiveProcess"));
    assert(!horizon_reg_delete(&horizon_registry, key));
    horizon_reg_release(&horizon_registry, key);

    saved = horizon_reg_create_ascii(&horizon_registry, horizon_registry_hives[1], path);
    assert(saved);
    assert(!horizon_reg_set_value(&horizon_registry, saved, name, name_length, HORIZON_REG_DWORD, &original, 4));
    assert(!horizon_registry_make_session_key(path));
    assert(!horizon_registry_make_session_key(path));
    assert(!horizon_reg_open(&horizon_registry, horizon_registry_hives[1], wide_path, length, 0, &key));
    assert(key != saved && (key->flags & HORIZON_REG_FLAG_VOLATILE));
    assert(!memcmp(horizon_reg_find_value(key, name, name_length, &index)->data, &original, 4));
    assert(!horizon_reg_set_value(&horizon_registry, key, name, name_length, HORIZON_REG_DWORD, &running, 4));
    assert(horizon_registry_flush());
    file = fopen(file_path, "rb"); assert(file);
    size = fread(text, 1, sizeof(text) - 1, file); fclose(file); text[size] = 0;
    assert(strstr(text, "ActiveProcess") && strstr(text, "\"pid\"=dword:0000001b"));
    assert(!horizon_reg_delete(&horizon_registry, key));
    assert(horizon_registry_flush());
    file = fopen(file_path, "rb"); assert(file);
    size = fread(text, 1, sizeof(text) - 1, file); fclose(file); text[size] = 0;
    assert(strstr(text, "ActiveProcess") && strstr(text, "\"pid\"=dword:0000001b"));
    horizon_reg_release(&horizon_registry, key);
    horizon_reg_release(&horizon_registry, horizon_registry_session_key);
    horizon_reg_release(&horizon_registry, horizon_registry_session_saved->parent);
    horizon_reg_release(&horizon_registry, horizon_registry_session_saved);
    horizon_registry_session_key = horizon_registry_session_saved = NULL;
    puts("Registry session: transient values and existing persisted values preserved");
}

int main(void)
{
    unsigned int key, user, i;
    unsigned short name[256];
    struct horizon_get_key_value_request get = {{REQ_get_key_value,0,sizeof(payload)},0};
    struct horizon_set_key_value_request set = {{REQ_set_key_value,0,0},0,4,0};
    struct horizon_enum_key_request query = {{REQ_enum_key,0,2},0,-1,3};
    struct horizon_set_registry_notification_request notify = {{REQ_set_registry_notification,0,0},0,0,0,4,{0}};
    struct horizon_server_handle_entry *event;
    struct {struct horizon_server_request_header header; unsigned int handle, which;} token = {{REQ_get_token_sid,0,0},0xfffffffa,1};
    snprintf(registry_dir, sizeof(registry_dir), "%s/wine-nx-registry.XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    assert(mkdtemp(registry_dir));
    strcat(registry_dir, "/");
    horizon_server_handle_registry_user(&connection,(void *)&token);
    assert(status()==HORIZON_STATUS_BUFFER_TOO_SMALL);
    token.header.reply_size=28; horizon_server_handle_registry_user(&connection,(void *)&token);
    assert(!status() && payload_size==28 && payload[0]==1 && payload[1]==5);
    key = open_path(0,"\\Registry\\Machine\\Software\\Classes\\CLSID\\{BCDE0395-E52F-467C-8E3D-C4579291692E}\\InprocServer32");
    get.hkey=key; horizon_server_handle_registry(&connection,(void *)&get,NULL,0);
    assert(!status() && payload_size==26 && !memcmp(payload,"m\0m\0d\0",6));
    user=open_path(0,"\\Registry\\User\\S-1-5-21-0-0-0-1000\\Software\\Wine\\Drivers");
    get.hkey=user; horizon_server_handle_registry(&connection,(void *)&get,(void *)name,wide(name,"Audio"));
    assert(!status() && payload_size==16 && !memcmp(payload,"n\0x\0",4));
    query.hkey=key; horizon_server_handle_registry(&connection,(void *)&query,NULL,0);
    assert(!status() && payload_size==2 && ((struct horizon_enum_key_reply *)last_reply)->total>2);
    event=horizon_server_create_handle_locked(HORIZON_SERVER_OBJECT_EVENT); event->object->signaled=1;
    notify.hkey=user; notify.event=event->handle;
    horizon_server_handle_registry(&connection,(void *)&notify,NULL,0);
    assert(status()==HORIZON_REG_PENDING && event->object->refs==2 && !event->object->signaled);
    set.hkey=user; set.namelen=wide(name,"Test"); memcpy((char *)name+set.namelen,"\1\0\0\0",4);
    horizon_server_handle_registry(&connection,(void *)&set,(void *)name,set.namelen+4);
    assert(!status() && event->object->refs==1 && event->object->signaled);
    set.hkey=key; set.namelen=wide(name,"Saved"); memcpy((char *)name+set.namelen,"\1\0\0\0",4);
    horizon_server_handle_registry(&connection,(void *)&set,(void *)name,set.namelen+4);
    assert(!status());
    set.namelen=100; horizon_server_handle_registry(&connection,(void *)&set,(void *)name,4);
    assert(status()==HORIZON_REG_INVALID_PARAMETER);
    assert(!hive_writes[0] && !hive_writes[1]); /* requests did no SD writes */
    assert(horizon_registry_flush_key(0xffffffffu) == HORIZON_STATUS_INVALID_HANDLE);
    fail_write = 1;
    assert(horizon_registry_flush_key(key) == 0xc0000001u);
    fail_write = 0;
    assert(!horizon_registry_flush_key(key));
    for(i=0;i<handle_count;i++) horizon_server_free_object(handles[i].object);
    test_batching();
    test_machine_identity();
    test_directshow_filters();
    test_session_key();
    test_save_and_load();
    puts("Registry server: protocol layouts, HKCU identity, COM/audio seeds, truncated replies, notifications and saved hives passed");
    return 0;
}
