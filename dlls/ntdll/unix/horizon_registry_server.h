/* Copyright 2026 Wine-NX contributors. LGPL-2.1-or-later.
 * Included by horizon.c with the server's wire and object definitions. */
#include <ctype.h>
#include <time.h>
#include <unistd.h>

/* The runtime's folder: system.reg and user.reg are in its registry/, the
 * classes the payload ships in config/. The host test uses a scratch folder. */
#ifndef HORIZON_REGISTRY_DIR
#define HORIZON_REGISTRY_DIR "sdmc:/switch/wine/"
#endif
#include "horizon_registry_paths.h"

static long long horizon_registry_now(void)
{
    return (long long)time(NULL) * 10000000 + HORIZON_REG_TICKS_1601_TO_1970;
}

/* The hive writer follows server/registry.c's save_all_subkeys, so its files
 * load in Wine and back through horizon_reg_load. This is server/unicode.c's
 * dump_strW: backslashes and the escape characters get a backslash, control
 * characters C or octal escapes, characters above ASCII \x escapes. */
static int horizon_registry_save_str( FILE *file, const unsigned short *str, unsigned int len,
                                      const char escape[2] )
{
    static const char escapes[] = ".......abtnvfr.............e....";
    int count = 0;

    for (len /= 2; len; str++, len--)
    {
        if (*str > 127)
        {
            if (len > 1 && str[1] < 128 && isxdigit( str[1] )) count += fprintf( file, "\\x%04x", *str );
            else count += fprintf( file, "\\x%x", *str );
        }
        else if (*str < 32)
        {
            if (escapes[*str] != '.') count += fprintf( file, "\\%c", escapes[*str] );
            else if (len > 1 && str[1] >= '0' && str[1] <= '7') count += fprintf( file, "\\%03o", *str );
            else count += fprintf( file, "\\%o", *str );
        }
        else
        {
            if (*str == '\\' || *str == escape[0] || *str == escape[1]) { fputc( '\\', file ); count++; }
            fputc( *str, file );
            count++;
        }
    }
    return count;
}

/* server/registry.c's dump_value. */
static void horizon_registry_save_value( FILE *file, const struct horizon_reg_value *value )
{
    const unsigned short *str = (const unsigned short *)value->data;
    unsigned int i, dword;
    int count;

    if (value->namelen)
    {
        fputc( '"', file );
        count = 1 + horizon_registry_save_str( file, value->name, value->namelen, "\"\"" );
        count += fprintf( file, "\"=" );
    }
    else count = fprintf( file, "@=" );

    switch (value->type)
    {
    case HORIZON_REG_SZ:
    case HORIZON_REG_EXPAND_SZ:
    case HORIZON_REG_MULTI_SZ:
        /* only properly terminated strings in string format */
        if (value->len < 2 || value->len % 2 || str[value->len / 2 - 1]) break;
        if (value->type != HORIZON_REG_SZ) fprintf( file, "str(%x):", value->type );
        fputc( '"', file );
        horizon_registry_save_str( file, str, value->len - 2, "\"\"" );
        fputs( "\"\n", file );
        return;
    case HORIZON_REG_DWORD:
        if (value->len != sizeof(dword)) break;
        memcpy( &dword, value->data, sizeof(dword) );
        fprintf( file, "dword:%08x\n", dword );
        return;
    }

    if (value->type == HORIZON_REG_BINARY) count += fprintf( file, "hex:" );
    else count += fprintf( file, "hex(%x):", value->type );
    for (i = 0; i < value->len; i++)
    {
        count += fprintf( file, "%02x", value->data[i] );
        if (i + 1 < value->len)
        {
            fputc( ',', file );
            if (++count > 76)
            {
                fputs( "\\\n  ", file );
                count = 2;
            }
        }
    }
    fputc( '\n', file );
}

/* A key's path below its hive, as a list from the key up. */
struct horizon_registry_path
{
    const struct horizon_reg_key *key;
    const struct horizon_registry_path *parent;  /* NULL for a child of the hive */
};

/* server/registry.c's dump_path: elements are separated by an escaped backslash. */
static void horizon_registry_save_path( FILE *file, const struct horizon_registry_path *path )
{
    if (path->parent)
    {
        horizon_registry_save_path( file, path->parent );
        fputs( "\\\\", file );
    }
    horizon_registry_save_str( file, path->key->name, path->key->namelen, "[]" );
}

/* server/registry.c's save_subkeys; path is NULL for the hive itself. */
static void horizon_registry_save_subkeys( FILE *file, const struct horizon_reg_key *key,
                                           const struct horizon_registry_path *path )
{
    struct horizon_registry_path child = { NULL, path };
    unsigned int i;

    if (key->flags & HORIZON_REG_FLAG_VOLATILE) return;
    /* keys with values, no subkeys, a class or a link; the others are implied by their subkeys */
    if (key->value_count || !key->subkey_count || key->class || (key->flags & HORIZON_REG_FLAG_SYMLINK))
    {
        fputs( "\n[", file );
        if (path) horizon_registry_save_path( file, path );
        fprintf( file, "] %u\n", (unsigned int)((key->modif - HORIZON_REG_TICKS_1601_TO_1970) / 10000000) );
        fprintf( file, "#time=%x%08x\n", (unsigned int)((unsigned long long)key->modif >> 32), (unsigned int)key->modif );
        if (key->class)
        {
            fputs( "#class=\"", file );
            horizon_registry_save_str( file, key->class, key->classlen, "\"\"" );
            fputs( "\"\n", file );
        }
        if (key->flags & HORIZON_REG_FLAG_SYMLINK) fputs( "#link\n", file );
        for (i = 0; i < key->value_count; i++) horizon_registry_save_value( file, &key->values[i] );
    }
    for (i = 0; i < key->subkey_count; i++)
    {
        child.key = key->subkeys[i];
        horizon_registry_save_subkeys( file, child.key, &child );
    }
}

/* Mutations only advance the affected hive's generation. The runtime's
 * maintenance thread serializes a consistent snapshot under the object lock,
 * then does every SD operation after releasing it. A concurrent mutation or
 * failed write leaves that generation dirty for the next pass. */
static struct horizon_reg_key *horizon_registry_hives[2];
static unsigned long long horizon_registry_generation[2], horizon_registry_saved[2];
static pthread_mutex_t horizon_registry_flush_mutex = PTHREAD_MUTEX_INITIALIZER;

static void horizon_registry_changed( const struct horizon_reg_key *key )
{
    for (; key; key = key->parent)
    {
        if (key == horizon_registry_hives[0]) { horizon_registry_generation[0]++; return; }
        if (key == horizon_registry_hives[1]) { horizon_registry_generation[1]++; return; }
    }
}

static int horizon_registry_write_snapshot( const char *name, const char *data, size_t size )
{
    char path[256], tmp[sizeof(path) + 4];
    FILE *file;
    int error;

    snprintf( path, sizeof(path), "%s%s", HORIZON_REGISTRY_DIR, name );
    snprintf( tmp, sizeof(tmp), "%s.tmp", path );
    if (!access( tmp, F_OK ) && access( path, F_OK ))
    {
        if (errno != ENOENT || rename( tmp, path )) return 0;
    }
    if (!(file = fopen( tmp, "wb" ))) return 0;
    error = fwrite( data, 1, size, file ) != size;
    if (fclose( file )) error = 1;
    if (error) { unlink( tmp ); return 0; }
    /* Horizon cannot rename over an existing file. The loader recovers the
     * complete .tmp if shutdown happens between unlink and the second rename. */
    if (!rename( tmp, path )) return 1;
    if (errno != EEXIST) return 0;
    if (unlink( path )) return 0;
    return !rename( tmp, path );
}

int horizon_registry_flush(void)
{
    static const char *const names[] = { HORIZON_REGISTRY_SUBDIR "system.reg", HORIZON_REGISTRY_SUBDIR "user.reg" };
    static const char *const roots[] = { "\\\\Machine", "\\\\User\\\\S-1-5-21-0-0-0-1000" };
    unsigned int i;
    int success = 1;

    /* Only one writer may use the .tmp files or advance saved generations. */
    pthread_mutex_lock( &horizon_registry_flush_mutex );
    for (i = 0; i < 2; i++)
    {
        char *data = NULL;
        size_t size = 0;
        unsigned long long generation;
        FILE *file;
        int error = 1, dirty;

        pthread_mutex_lock( &horizon_server_objects_mutex );
        generation = horizon_registry_generation[i];
        dirty = horizon_registry_hives[i] && generation != horizon_registry_saved[i];
        if (dirty)
        {
            if ((file = open_memstream( &data, &size )))
            {
                fprintf( file, "WINE REGISTRY Version 2\n;; All keys relative to %s\n", roots[i] );
                horizon_registry_save_subkeys( file, horizon_registry_hives[i], NULL );
                error = ferror( file );
                if (fclose( file )) error = 1;
            }
        }
        pthread_mutex_unlock( &horizon_server_objects_mutex );
        if (!error && horizon_registry_write_snapshot( names[i], data, size ))
        {
            pthread_mutex_lock( &horizon_server_objects_mutex );
            horizon_registry_saved[i] = generation;
            pthread_mutex_unlock( &horizon_server_objects_mutex );
        }
        else if (dirty) success = 0;
        free( data );
    }
    pthread_mutex_unlock( &horizon_registry_flush_mutex );
    return success;
}

/* Returns whether path held a registry file. */
static int horizon_registry_load_file( struct horizon_reg_key *base, const char *path )
{
    FILE *file;
    long length;
    char *buffer;
    unsigned int errors = 0;
    int loaded = 0;

    if (!(file = fopen( path, "rb" ))) return 0;
    if (fseek( file, 0, SEEK_END ) || (length = ftell( file )) < 0 || length > 16 * 1024 * 1024 ||
        fseek( file, 0, SEEK_SET ) || !(buffer = malloc( (size_t)length + 1 ))) { fclose( file ); return 0; }
    if (fread( buffer, 1, (size_t)length, file ) == (size_t)length)
        loaded = horizon_reg_load( &horizon_registry, base, buffer, (size_t)length, &errors ) != HORIZON_REG_NOT_REGISTRY_FILE;
    free( buffer );
    fclose( file );
    return loaded;
}

static void horizon_registry_load_hive( struct horizon_reg_key *base, const char *name )
{
    char path[256];

    snprintf( path, sizeof(path), "%s%s", HORIZON_REGISTRY_DIR, name );
    if (horizon_registry_load_file( base, path )) return;
    snprintf( path, sizeof(path), "%s%s.tmp", HORIZON_REGISTRY_DIR, name );
    horizon_registry_load_file( base, path );
}

/* Notifications retain event objects, not handles which can be closed/reused.
 * The caller holds horizon_server_objects_mutex; pending waits are woken. */
static void horizon_registry_signal( void *event )
{
    struct horizon_server_object *object = event;
    object->signaled = 1;
    horizon_server_signal_changed_locked();
    if (!--object->refs) horizon_server_free_object( object );
}

static unsigned int horizon_registry_init(void)
{
    struct horizon_reg_key *machine, *user;
    if (horizon_registry.root) return 0;
    if (!horizon_reg_init( &horizon_registry, horizon_registry_now, horizon_registry_signal ))
        return HORIZON_REG_NO_MEMORY;
    machine = horizon_reg_create_ascii( &horizon_registry, horizon_registry.root, "Machine" );
    user = horizon_reg_create_ascii( &horizon_registry, horizon_registry.root, "User\\S-1-5-21-0-0-0-1000" );
    if (!machine || !user ||
        !horizon_reg_create_ascii( &horizon_registry, machine, "Software\\Classes" ) ||
        !horizon_reg_create_ascii( &horizon_registry, machine, "System\\CurrentControlSet" ) ||
        !horizon_reg_create_ascii( &horizon_registry, user, "Software" ) ||
        !horizon_reg_create_ascii( &horizon_registry, horizon_registry.root, "User\\.Default" ))
    {
        horizon_reg_release( &horizon_registry, horizon_registry.root );
        horizon_registry.root = NULL;
        return HORIZON_REG_NO_MEMORY;
    }
    {
        static const char machine_seed[] =
            "WINE REGISTRY Version 2\n"
            "[Software\\\\Classes\\\\CLSID\\\\{BCDE0395-E52F-467C-8E3D-C4579291692E}\\\\InprocServer32]\n"
            "@=\"mmdevapi.dll\"\n"
            "\"ThreadingModel\"=\"Both\"\n";
        static const char user_seed[] =
            "WINE REGISTRY Version 2\n[Software\\\\Wine\\\\Drivers]\n\"Audio\"=\"nxaudio\"\n";
        unsigned int errors = 0;
        if (horizon_reg_load( &horizon_registry, machine, machine_seed, sizeof(machine_seed) - 1, &errors ) ||
            horizon_reg_load( &horizon_registry, user, user_seed, sizeof(user_seed) - 1, &errors ) || errors)
        {
            horizon_reg_release( &horizon_registry, horizon_registry.root );
            horizon_registry.root = NULL;
            return HORIZON_REG_NO_MEMORY;
        }
    }
    /* The classes the staged DLLs serve, which on Windows are written by each
     * DLL's own DllRegisterServer when it is installed. Nothing here installs
     * anything, so they are written out beside the runtime instead and read
     * before a program asks: a game that creates a filter graph or a text
     * service gets one rather than a null pointer it does not check. Loaded
     * before system.reg, so anything a program has written for itself since
     * still wins. */
    horizon_registry_load_hive( machine, "config/classes.reg" );
    horizon_registry_move_hives( HORIZON_REGISTRY_DIR );
    horizon_registry_load_hive( machine, HORIZON_REGISTRY_SUBDIR "system.reg" );
    horizon_registry_load_hive( user, HORIZON_REGISTRY_SUBDIR "user.reg" );
    horizon_registry_hives[0] = machine;
    horizon_registry_hives[1] = user;
    memset( horizon_registry_generation, 0, sizeof(horizon_registry_generation) );
    memset( horizon_registry_saved, 0, sizeof(horizon_registry_saved) );
    horizon_registry.changed = horizon_registry_changed;
    return 0;
}

/* The in-process server has a single local user and no impersonation tokens.
 * This identity also names HKCU; do not accept arbitrary token handles. */
static int horizon_server_handle_registry_user( struct horizon_server_connection *connection,
                                                const unsigned char *message )
{
    const struct { struct horizon_server_request_header header; unsigned int handle, which_sid; } *req = (const void *)message;
    struct { struct horizon_server_reply_header header; unsigned int sid_len, pad; } reply = {0};
    static const struct { unsigned char revision, count, authority[6]; unsigned int sub[5]; } sid =
        {1, 5, {0,0,0,0,0,5}, {21,0,0,0,1000}};
    if (req->handle != 0xfffffffau && req->handle != 0xfffffffcu)
        reply.header.error = HORIZON_STATUS_INVALID_HANDLE;
    else if (req->which_sid != 1) reply.header.error = HORIZON_STATUS_NOT_IMPLEMENTED;
    else
    {
        reply.sid_len = sizeof(sid);
        if (req->header.reply_size < sizeof(sid)) reply.header.error = HORIZON_STATUS_BUFFER_TOO_SMALL;
        else reply.header.reply_size = sizeof(sid);
    }
    return horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply),
                                        &sid, reply.header.reply_size );
}

static unsigned int horizon_registry_key( unsigned int handle, struct horizon_reg_key **key )
{
    struct horizon_server_handle_entry *entry = horizon_server_find_handle_locked( handle );
    *key = NULL;
    if (!entry || !entry->object || entry->object->type != HORIZON_SERVER_OBJECT_REG_KEY)
        return HORIZON_STATUS_INVALID_HANDLE;
    *key = entry->object->reg_key;
    if ((*key)->flags & HORIZON_REG_FLAG_DELETED) return HORIZON_REG_KEY_DELETED;
    return 0;
}

/* RegFlushKey is synchronous even though ordinary mutations are batched. */
unsigned int horizon_registry_flush_key( unsigned int handle )
{
    struct horizon_reg_key *key;
    unsigned int status;
    pthread_mutex_lock( &horizon_server_objects_mutex );
    status = horizon_registry_key( handle, &key );
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    if (status) return status;
    return horizon_registry_flush() ? 0 : 0xc0000001u; /* STATUS_UNSUCCESSFUL */
}

/* Verbose runs name every key opened and value read, with what came of it:
 * the [SYSCALL] lines give a status and nothing else, which leaves a program
 * that gives up on a missing key saying only that something was missing. A
 * name that is not ASCII is written as \u escapes so it can be compared with
 * the one on the card. */
extern int wine_nx_runtime_verbose __attribute__((weak));
extern void wine_nx_runtime_trace( const char *msg ) __attribute__((weak));

static unsigned int horizon_registry_trace_name( char *out, unsigned int at, unsigned int max,
                                                 const unsigned short *name, unsigned int len )
{
    unsigned int i;

    for (i = 0; i < len / 2 && at + 7 < max; i++)
    {
        if (name[i] >= 0x20 && name[i] < 0x7f) out[at++] = (char)name[i];
        else at += snprintf( out + at, max - at, "\\u%04x", name[i] );
    }
    out[at < max ? at : max - 1] = 0;
    return at;
}

/* The key's own path from the root, for a value read under it. */
static unsigned int horizon_registry_trace_path( char *out, unsigned int at, unsigned int max,
                                                 const struct horizon_reg_key *key )
{
    if (!key) return at;
    if (key->parent)
    {
        at = horizon_registry_trace_path( out, at, max, key->parent );
        if (at + 1 < max) out[at++] = '\\';
    }
    return horizon_registry_trace_name( out, at, max, key->name, key->namelen );
}

static int horizon_server_handle_registry( struct horizon_server_connection *connection,
                                           const unsigned char *message,
                                           const unsigned char *data, unsigned int data_size )
{
    const struct horizon_server_request_header *header = (const void *)message;
    union
    {
        struct horizon_server_reply_header header;
        struct horizon_create_key_reply create;
        struct horizon_enum_key_reply key;
        struct horizon_get_key_value_reply value;
        struct horizon_enum_key_value_reply enum_value;
    } reply;
    struct horizon_reg_key *key = NULL;
    struct horizon_server_handle_entry *entry;
    unsigned char *out = NULL;
    unsigned int status, size = 0, max = header->reply_size;
    int tracing = &wine_nx_runtime_verbose && wine_nx_runtime_verbose && &wine_nx_runtime_trace;
    char trace[512];
    unsigned int trace_at = 0;
    trace[0] = 0;
    memset( &reply, 0, sizeof(reply) );
    /* Bound peer-controlled allocations; regular values can be fetched in
     * smaller buffers with the full required length returned in the reply. */
    if (max > 16 * 1024 * 1024) max = 16 * 1024 * 1024;
    if (max && !(out = malloc(max)))
        return horizon_server_write_status( connection->reply_fd, HORIZON_REG_NO_MEMORY );
    pthread_mutex_lock( &horizon_server_objects_mutex );
    if ((status = horizon_registry_init())) goto done;
    if (header->req == HORIZON_REQ_CREATE_KEY || header->req == HORIZON_REQ_OPEN_KEY)
    {
        struct horizon_reg_key *base = NULL;
        unsigned int parent, attributes, options = 0, classlen = 0;
        const unsigned short *name, *class = NULL;
        unsigned int len;
        if (header->req == HORIZON_REQ_CREATE_KEY)
        {
            const struct horizon_create_key_request *req = (const void *)message;
            const struct horizon_object_attributes *attr = (const void *)data;
            struct horizon_object_name parsed;
            unsigned int offset;
            if (data_size < sizeof(*attr)) { status = HORIZON_REG_INVALID_PARAMETER; goto done; }
            if ((status = horizon_server_parse_object_attributes( data, data_size, &parsed )) ||
                (status = horizon_server_object_attributes_size( data, data_size, &offset ))) goto done;
            parent = attr->rootdir;
            attributes = attr->attributes;
            name = (const void *)parsed.name;
            len = parsed.name_len;
            options = req->options;
            class = (const void *)(data + offset);
            classlen = data_size - offset;
        }
        else
        {
            const struct horizon_open_key_request *req = (const void *)message;
            parent = req->parent;
            attributes = req->attributes;
            name = (const void *)data;
            len = data_size;
        }
        if ((len | classlen) & 1) { status = HORIZON_REG_INVALID_PARAMETER; goto done; }
        if (parent && (status = horizon_registry_key( parent, &base ))) goto done;
        if (header->req == HORIZON_REQ_CREATE_KEY)
            status = horizon_reg_create( &horizon_registry, base, name, len, attributes,
                                         options, class, classlen, &key );
        else status = horizon_reg_open( &horizon_registry, base, name, len, attributes, &key );
        if (tracing)
        {
            trace_at = snprintf( trace, sizeof(trace), "[REG] %s ",
                                 header->req == HORIZON_REQ_CREATE_KEY ? "create" : "open" );
            if (base)
            {
                trace_at = horizon_registry_trace_path( trace, trace_at, sizeof(trace), base );
                if (trace_at + 1 < sizeof(trace)) trace[trace_at++] = '\\';
            }
            trace_at = horizon_registry_trace_name( trace, trace_at, sizeof(trace), name, len );
        }
        if (!key) goto done;
        if (!(entry = horizon_server_create_handle_locked( HORIZON_SERVER_OBJECT_REG_KEY )))
        {
            horizon_reg_release( &horizon_registry, key );
            status = HORIZON_REG_NO_MEMORY;
            goto done;
        }
        entry->object->reg_key = key;
        reply.create.hkey = entry->handle;
        goto done;
    }
    /* All remaining registry requests begin with hkey at offset 12. */
    if ((status = horizon_registry_key( ((const struct horizon_delete_key_request *)message)->hkey, &key )))
        goto done;
    switch (header->req)
    {
    case HORIZON_REQ_DELETE_KEY:
        status = horizon_reg_delete( &horizon_registry, key );
        break;
    case HORIZON_REQ_RENAME_KEY:
        status = horizon_reg_rename( &horizon_registry, key, (const void *)data, data_size );
        break;
    case HORIZON_REQ_ENUM_KEY:
    {
        const struct horizon_enum_key_request *req = (const void *)message;
        struct horizon_reg_key_info info;
        status = horizon_reg_enum_key( key, req->index, req->info_class, &info, out, max, &size );
        reply.key.subkeys = info.subkeys;
        reply.key.max_subkey = info.max_subkey;
        reply.key.max_class = info.max_class;
        reply.key.values = info.values;
        reply.key.max_value = info.max_value;
        reply.key.max_data = info.max_data;
        reply.key.modif = info.modif;
        reply.key.total = info.total;
        reply.key.namelen = info.namelen;
        break;
    }
    case HORIZON_REQ_SET_KEY_VALUE:
    {
        const struct horizon_set_key_value_request *req = (const void *)message;
        if (req->namelen > data_size || (req->namelen & 1)) status = HORIZON_REG_INVALID_PARAMETER;
        else status = horizon_reg_set_value( &horizon_registry, key, (const void *)data, req->namelen,
                                             req->type, data + req->namelen, data_size - req->namelen );
        break;
    }
    case HORIZON_REQ_GET_KEY_VALUE:
        status = horizon_reg_get_value( key, (const void *)data, data_size, &reply.value.type,
                                        &reply.value.total, out, max, &size );
        if (tracing)
        {
            trace_at = snprintf( trace, sizeof(trace), "[REG] query " );
            trace_at = horizon_registry_trace_path( trace, trace_at, sizeof(trace), key );
            if (trace_at + 3 < sizeof(trace)) { trace[trace_at++] = ' '; trace[trace_at++] = ':'; trace[trace_at++] = ' '; }
            trace_at = horizon_registry_trace_name( trace, trace_at, sizeof(trace), (const void *)data, data_size );
        }
        break;
    case HORIZON_REQ_ENUM_KEY_VALUE:
    {
        const struct horizon_enum_key_value_request *req = (const void *)message;
        status = horizon_reg_enum_value( key, req->index, req->info_class, &reply.enum_value.type,
                                         &reply.enum_value.total, &reply.enum_value.namelen, out, max, &size );
        break;
    }
    case HORIZON_REQ_DELETE_KEY_VALUE:
        status = horizon_reg_delete_value( &horizon_registry, key, (const void *)data, data_size );
        break;
    case HORIZON_REQ_SET_REGISTRY_NOTIFICATION:
    {
        const struct horizon_set_registry_notification_request *req = (const void *)message;
        entry = horizon_server_find_handle_locked( req->event );
        if (!entry || entry->object->type != HORIZON_SERVER_OBJECT_EVENT)
            status = HORIZON_STATUS_INVALID_HANDLE;
        else
        {
            status = horizon_reg_notify( key, req->hkey, entry->object, req->subtree, req->filter );
            if (status == HORIZON_REG_PENDING)
            {
                entry->object->refs++;
                entry->object->signaled = 0;
            }
        }
        break;
    }
    default: status = HORIZON_REG_INVALID_PARAMETER; break;
    }
done:
    pthread_mutex_unlock( &horizon_server_objects_mutex );
    if (trace[0])
    {
        if (trace_at + 16 < sizeof(trace)) snprintf( trace + trace_at, sizeof(trace) - trace_at, " -> %08x", status );
        wine_nx_runtime_trace( trace );
    }
    reply.header.error = status;
    reply.header.reply_size = size;
    status = horizon_server_write_reply( connection->reply_fd, &reply, sizeof(reply), out, size );
    free(out);
    return status;
}
