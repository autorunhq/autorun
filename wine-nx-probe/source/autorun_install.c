#include "autorun_install.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <minizip/unzip.h>
#include <zlib.h>
#ifdef __SWITCH__
#include <switch.h>
#endif

#define INSTALL_MAX_FILES 4096
#define INSTALL_MAX_SIZE (4ull * 1024 * 1024 * 1024)
#define INSTALL_FILE_LIMIT (512ull * 1024 * 1024)
#define INSTALL_MAGIC 0x31555241
#define INSTALL_NRO "wine-nx-runtime.nro"

struct install_entry
{
    char path[192];
    uint64_t size, old_size;
    uint32_t crc, old_crc, existed;
};

struct install_header
{
    uint32_t magic, count, crc;
    char tag[64];
};

struct install_plan
{
    struct install_header header;
    struct install_entry entries[INSTALL_MAX_FILES];
    unz64_file_pos positions[INSTALL_MAX_FILES];
};

static int join( char *out, size_t size, const char *dir, const char *name )
{
    int n = snprintf( out, size, "%s/%s", dir, name );
    return n >= 0 && (size_t)n < size;
}

static int plain_path( const char *path )
{
    const char *p, *part = path;

    if (!*path) return 0;
    for (p = path;; p++)
    {
        unsigned char c = *p;
        if (!c || c == '/')
        {
            if (p == part || (p - part == 1 && part[0] == '.') ||
                (p - part == 2 && part[0] == '.' && part[1] == '.') || p[-1] == '.' || p[-1] == ' ') return 0;
            if (!c) return 1;
            part = p + 1;
        }
        else if (c < 32 || c == 127 || strchr( "\\:*?\"<>|", c )) return 0;
    }
}

static int allowed( const char *path )
{
    return plain_path( path ) && strcasecmp( path, "updates" ) && strncasecmp( path, "updates/", 8 );
}

static int managed( const char *path )
{
    static const struct { const char *dir, *extensions; } dirs[] =
    {
        { "drive_c/windows/system32/", ".dll;.drv;.acm;.exe;" },
        { "drive_c/windows/syswow64/", ".dll;.drv;.acm;.exe;" },
        { "drive_c/windows/fonts/", ".ttf;.fon;.fnt;" },
        { "share/wine/fonts/", ".ttf;.fon;.fnt;" },
        { "share/wine/nls/", ".nls;" },
        { "drive_c/dxvk/", ".dll;" }, { "drive_c/dxvk64/", ".dll;" },
        { "drive_c/vkd3d/", ".dll;" }, { "drive_c/vkd3d64/", ".dll;" },
        { "licenses/", ".txt;" },
    };
    unsigned int i;
    const char *ext;
    char suffix[12];
    if (!plain_path( path )) return 0;
    if (!strcasecmp( path, INSTALL_NRO ) || !strcasecmp( path, "build-manifest.json" )) return 1;
    if (!(ext = strrchr( path, '.' )) || strlen( ext ) > 8) return 0;
    snprintf( suffix, sizeof(suffix), "%s;", ext );
    for (i = 0; suffix[i]; i++) suffix[i] = tolower( (unsigned char)suffix[i] );
    for (i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++)
        if (!strncasecmp( path, dirs[i].dir, strlen( dirs[i].dir ) ) &&
            !strchr( path + strlen( dirs[i].dir ), '/' ) && strstr( dirs[i].extensions, suffix )) return 1;
    return 0;
}

static int directory( const char *path )
{
    struct stat st;
    if (!lstat( path, &st )) return S_ISDIR( st.st_mode );
    return errno == ENOENT && !mkdir( path, 0777 );
}

static int parents( const char *root, const char *relative )
{
    char path[768], *p;
    struct stat st;

    if (!join( path, sizeof(path), root, relative ) || !directory( root )) return 0;
    for (p = path + strlen( root ) + 1; *p; p++)
    {
        if (*p != '/') continue;
        *p = 0;
        if (!directory( path )) return 0;
        *p = '/';
    }
    if (!lstat( path, &st )) return S_ISREG( st.st_mode );
    return errno == ENOENT;
}

static int regular( const char *path, uint64_t *size )
{
    struct stat st;
    if (lstat( path, &st )) return errno == ENOENT ? 0 : -1;
    if (!S_ISREG( st.st_mode ) || st.st_size < 0) return -1;
    if (size) *size = st.st_size;
    return 1;
}

static int check_parents( const char *root, const char *relative )
{
    char path[768], *p;
    struct stat st;
    if (lstat( root, &st ) || !S_ISDIR( st.st_mode ) || !join( path, sizeof(path), root, relative )) return 0;
    for (p = path + strlen(root) + 1; *p; p++)
    {
        if (*p != '/') continue;
        *p = 0;
        if (lstat( path, &st )) return errno == ENOENT;
        if (!S_ISDIR( st.st_mode )) return 0;
        *p = '/';
    }
    return 1;
}

static int close_written( FILE *f )
{
    int ok = !fflush( f );
    if (ok && fsync( fileno( f ) )) ok = 0;
    if (fclose( f )) ok = 0;
    return ok;
}

static int erase( const char *path )
{
    return !unlink( path ) || errno == ENOENT;
}

static int sync_parent( const char *path )
{
#ifdef __SWITCH__
    char device[32];
    const char *colon = strchr( path, ':' );
    if (!colon || colon - path >= (int)sizeof(device)) return 0;
    memcpy( device, path, colon - path );
    device[colon - path] = 0;
    return R_SUCCEEDED( fsdevCommitDevice( device ) );
#else
    char parent[768], *slash;
    int fd, ok;
    if (strlen( path ) >= sizeof(parent)) return 0;
    strcpy( parent, path );
    if (!(slash = strrchr( parent, '/' ))) return 0;
    *slash = 0;
    if ((fd = open( parent, O_RDONLY | O_DIRECTORY )) < 0) return 0;
    ok = !fsync( fd );
    close( fd );
    return ok;
#endif
}

static int replace( const char *from, const char *to )
{
    return erase( to ) && !rename( from, to ) && sync_parent( to ) && sync_parent( from );
}

static int local_path( char *out, size_t size, const char *root, const char *file )
{
    char relative[128];
    snprintf( relative, sizeof(relative), "updates/%s", file );
    return join( out, size, root, relative );
}

static void entry_path( char *out, size_t size, const char *root, unsigned int i, int backup )
{
    char file[64];
    snprintf( file, sizeof(file), "transaction/%04u.%s", i, backup ? "old" : "new" );
    local_path( out, size, root, file );
}

static int file_crc( const char *path, uint64_t size, uint32_t *crc )
{
    unsigned char buf[65536];
    uint64_t read = 0;
    size_t n;
    uLong sum = crc32( 0, NULL, 0 );
    FILE *f = fopen( path, "rb" );
    int ok;

    if (!f) return 0;
    while ((n = fread( buf, 1, sizeof(buf), f )))
    {
        read += n;
        if (read > size) break;
        sum = crc32( sum, buf, n );
    }
    ok = read == size && !ferror( f );
    fclose( f );
    *crc = sum;
    return ok;
}

static int copy_file( const char *from, const char *to, uint64_t size, uint32_t *crc,
                      autorun_install_progress progress, void *opaque, const char *phase,
                      uint64_t base, uint64_t total )
{
    unsigned char buf[65536];
    FILE *in = fopen( from, "rb" ), *out;
    uint64_t read = 0;
    size_t n;
    int ok = 1;
    uLong sum = crc32( 0, NULL, 0 );

    if (!in) return 0;
    if (!(out = fopen( to, "wb" ))) { fclose( in ); return 0; }
    while ((n = fread( buf, 1, sizeof(buf), in )))
    {
        read += n;
        if (read > size || fwrite( buf, 1, n, out ) != n) { ok = 0; break; }
        sum = crc32( sum, buf, n );
        if (progress && progress( opaque, phase, base + read, total )) { ok = -1; break; }
    }
    if (read != size || ferror( in )) { if (ok > 0) ok = 0; }
    fclose( in );
    if (!close_written( out ) && ok > 0) ok = 0;
    if (ok > 0) *crc = sum;
    else erase( to );
    return ok;
}

static int has_file( const struct install_plan *p, const char *path )
{
    unsigned int i;
    for (i = 0; i < p->header.count; i++) if (!strcmp( p->entries[i].path, path )) return 1;
    return 0;
}

static int valid_plan( const struct install_plan *p )
{
    unsigned int i, j;
    uint64_t total = 0;

    if (p->header.magic != INSTALL_MAGIC || !p->header.count || p->header.count > INSTALL_MAX_FILES ||
        !memchr( p->header.tag, 0, sizeof(p->header.tag) ) || !plain_path( p->header.tag ) ||
        strchr( p->header.tag, '/' )) return 0;
    for (i = 0; i < p->header.count; i++)
    {
        const struct install_entry *e = &p->entries[i];
        if (!memchr( e->path, 0, sizeof(e->path) ) || !allowed( e->path ) || e->existed > 1 ||
            e->size > INSTALL_FILE_LIMIT || e->old_size > INSTALL_FILE_LIMIT) return 0;
        total += e->size;
        if (total > INSTALL_MAX_SIZE) return 0;
        for (j = 0; j < i; j++) if (!strcasecmp( e->path, p->entries[j].path )) return 0;
    }
    return !strcmp( p->entries[p->header.count - 1].path, INSTALL_NRO );
}

static int load_plan( const char *root, struct install_plan *p )
{
    char path[768];
    FILE *f;
    int ok;

    if (!local_path( path, sizeof(path), root, "transaction/journal" )) return -1;
    if (!(f = fopen( path, "rb" ))) return errno == ENOENT ? 0 : -1;
    ok = fread( &p->header, 1, sizeof(p->header), f ) == sizeof(p->header) &&
         p->header.count <= INSTALL_MAX_FILES && p->header.count &&
         fread( p->entries, sizeof(p->entries[0]), p->header.count, f ) == p->header.count &&
         fgetc( f ) == EOF && !ferror( f );
    fclose( f );
    if (!ok || !valid_plan( p ) || crc32( 0, (const Bytef *)p->entries,
        p->header.count * sizeof(p->entries[0]) ) != p->header.crc) return -1;
    return 1;
}

static int save_plan( const char *root, struct install_plan *p )
{
    char path[768], final[768];
    FILE *f;
    int ok;

    local_path( path, sizeof(path), root, "transaction/journal.part" );
    local_path( final, sizeof(final), root, "transaction/journal" );
    p->header.crc = crc32( 0, (const Bytef *)p->entries, p->header.count * sizeof(p->entries[0]) );
    if (!(f = fopen( path, "wb" ))) return 0;
    ok = fwrite( &p->header, 1, sizeof(p->header), f ) == sizeof(p->header) &&
         fwrite( p->entries, sizeof(p->entries[0]), p->header.count, f ) == p->header.count;
    if (!close_written( f )) ok = 0;
    return ok && sync_parent( path ) && !rename( path, final ) && sync_parent( final );
}

static int clean_transaction( const char *root )
{
    char path[768], dir[768];
    struct dirent *e;
    DIR *d;
    int ok = 1;

    local_path( dir, sizeof(dir), root, "transaction" );
    if (!(d = opendir( dir ))) return errno == ENOENT;
    while ((e = readdir( d )))
    {
        const char *n = e->d_name;
        if (!strcmp( n, "." ) || !strcmp( n, ".." )) continue;
        if (strcmp( n, "journal.part" ) && strcmp( n, "journal" ) && strcmp( n, "committed" ) &&
            strcmp( n, "restore.part" ) && !(strlen( n ) == 8 && isdigit( (unsigned char)n[0] ) && isdigit( (unsigned char)n[1] ) &&
            isdigit( (unsigned char)n[2] ) && isdigit( (unsigned char)n[3] ) && (!strcmp( n + 4, ".old" ) || !strcmp( n + 4, ".new" ))))
        { ok = 0; continue; }
        if (!join( path, sizeof(path), dir, n ) || !erase( path )) ok = 0;
    }
    closedir( d );
    return ok && !rmdir( dir );
}

static int rollback( const char *root, const struct install_plan *p )
{
    unsigned int i;
    char source[768], dest[768], temp[768];
    uint32_t crc;

    /* Keep backups intact until every file has been restored. */
    for (i = 0; i < p->header.count; i++)
    {
        const struct install_entry *e = &p->entries[i];
        if (e->existed)
        {
            entry_path( source, sizeof(source), root, i, 1 );
            if (!file_crc( source, e->old_size, &crc ) || crc != e->old_crc) return 0;
        }
        else
        {
            if (!join( dest, sizeof(dest), root, e->path )) return 0;
            int exists = regular( dest, NULL );
            if (exists < 0 || (exists && (!file_crc( dest, e->size, &crc ) || crc != e->crc))) return 0;
        }
    }
    local_path( temp, sizeof(temp), root, "transaction/restore.part" );
    for (i = 0; i < p->header.count; i++)
    {
        const struct install_entry *e = &p->entries[i];
        if (!parents( root, e->path ) || !join( dest, sizeof(dest), root, e->path )) return 0;
        if (e->existed)
        {
            entry_path( source, sizeof(source), root, i, 1 );
            if (copy_file( source, temp, e->old_size, &crc, NULL, NULL, NULL, 0, 0 ) != 1 ||
                crc != e->old_crc || !replace( temp, dest )) return 0;
        }
        else
        {
            int exists = regular( dest, NULL );
            if (exists < 0) return 0;
            if (exists && (!file_crc( dest, e->size, &crc ) || crc != e->crc ||
                           !erase( dest ) || !sync_parent( dest ))) return 0;
        }
    }
    local_path( dest, sizeof(dest), root, "transaction/journal" );
    if (!erase( dest ) || !sync_parent( dest )) return 0;
    clean_transaction( root );
    return 1;
}

int autorun_install_recover( const char *root, int restore )
{
    struct install_plan *p = calloc( 1, sizeof(*p) );
    char path[768];
    int result;

    if (!p) return -1;
    result = load_plan( root, p );
    if (result > 0)
    {
        local_path( path, sizeof(path), root, "transaction/committed" );
        if (!restore && regular( path, NULL ) == 1) result = 1;
        else result = rollback( root, p ) ? 2 : -1;
    }
    free( p );
    return result;
}

int autorun_install_finish( const char *root )
{
    struct install_plan *p = calloc( 1, sizeof(*p) );
    char path[768], temp[768];
    uint32_t crc;
    int result, ok;
    FILE *f;

    if (!p) return 0;
    result = load_plan( root, p );
    if (result <= 0) { free( p ); return result == 0; }
    local_path( path, sizeof(path), root, "transaction/committed" );
    if (regular( path, NULL ) != 1) { free( p ); return 0; }
    join( path, sizeof(path), root, INSTALL_NRO );
    const struct install_entry *nro = &p->entries[p->header.count - 1];
    if (!file_crc( path, nro->size, &crc ) || crc != nro->crc) { free( p ); return 0; }
    local_path( temp, sizeof(temp), root, "installed-release.part" );
    if (!(f = fopen( temp, "wb" ))) { free( p ); return 0; }
    ok = fprintf( f, "%s\n%llu %08x\n", p->header.tag, (unsigned long long)nro->size, nro->crc ) > 0;
    if (!close_written( f )) ok = 0;
    local_path( path, sizeof(path), root, "installed-release" );
    if (ok) ok = replace( temp, path );
    /* Removing the journal commits cleanup, so a later boot never rolls back half a backup. */
    local_path( path, sizeof(path), root, "transaction/journal" );
    if (ok) ok = erase( path ) && sync_parent( path );
    if (ok) ok = clean_transaction( root );
    if (ok)
    {
        local_path( path, sizeof(path), root, "previous.nro" );
        erase( path );
        local_path( path, sizeof(path), root, "release.zip" );
        erase( path );
    }
    free( p );
    return ok;
}

int autorun_installed_release( const char *root, char *tag, size_t size )
{
    char path[768], saved[64] = {0};
    unsigned long long length;
    uint32_t expected, crc;
    FILE *f;
    int ok;

    local_path( path, sizeof(path), root, "installed-release" );
    if (!(f = fopen( path, "rb" ))) return 0;
    ok = fgets( saved, sizeof(saved), f ) && fscanf( f, "%llu %x", &length, &expected ) == 2;
    fclose( f );
    saved[strcspn( saved, "\r\n" )] = 0;
    if (!ok || !plain_path( saved ) || length > INSTALL_FILE_LIMIT) return 0;
    join( path, sizeof(path), root, INSTALL_NRO );
    if (!file_crc( path, length, &crc ) || crc != expected) return 0;
    return snprintf( tag, size, "%s", saved ) < (int)size;
}

static int nro_valid( const char *path, uint64_t size )
{
    unsigned char header[128] = {0};
    uint32_t image_size;
    FILE *f = fopen( path, "rb" );
    int ok;

    if (!f) return 0;
    ok = fread( header, 1, sizeof(header), f ) == sizeof(header);
    fclose( f );
    memcpy( &image_size, header + 24, 4 );
    return ok && !memcmp( header + 16, "NRO0", 4 ) && image_size >= 4096 && image_size <= size;
}

enum autorun_install_result autorun_install_archive( const char *root, const char *archive,
    const char *tag, int require_amd64, autorun_install_progress progress, void *opaque )
{
    struct install_plan *p = calloc( 1, sizeof(*p) );
    unzFile zip = NULL;
    unz_global_info64 global;
    struct statvfs space;
    char path[768], dest[768], name[512];
    uint64_t total = 0, backup_total = 0, current = 0;
    unsigned int i, j, nro_index = 0;
    int r, transaction = 0, prepared = 0;
    enum autorun_install_result result = AUTORUN_INSTALL_INVALID;

    if (!p) return AUTORUN_INSTALL_IO;
    if (strlen( root ) > 450 || !plain_path( tag ) || strchr( tag, '/' ) || strlen( tag ) >= sizeof(p->header.tag)) goto done;
    r = load_plan( root, p );
    if (r) { result = AUTORUN_INSTALL_RECOVERY; goto done; }
    memset( p, 0, sizeof(*p) );
    p->header.magic = INSTALL_MAGIC;
    strcpy( p->header.tag, tag );
    if (!(zip = unzOpen64( archive )) || unzGetGlobalInfo64( zip, &global ) != UNZ_OK ||
        !global.number_entry || global.number_entry > 8192 || unzGoToFirstFile( zip ) != UNZ_OK) goto done;
    for (i = 0; i < global.number_entry; i++)
    {
        unz_file_info64 info;
        struct install_entry *e;
        size_t len;
        int is_dir;
        if (unzGetCurrentFileInfo64( zip, &info, name, sizeof(name), NULL, 0, NULL, 0 ) != UNZ_OK ||
            !info.size_filename || info.size_filename >= sizeof(name) || strlen( name ) != info.size_filename) goto done;
        len = strlen( name );
        is_dir = name[len - 1] == '/';
        if (is_dir) name[--len] = 0;
        if (!plain_path( name ) || (info.flag & 1) || (info.compression_method != 0 && info.compression_method != 8) ||
            ((info.external_fa >> 16) & 0170000) == 0120000) goto done;
        if (is_dir)
        {
            if (strcmp( name, "switch" ) && strcmp( name, "switch/wine" ) &&
                (strncmp( name, "switch/wine/", 12 ) || !allowed( name + 12 ))) goto done;
        }
        else
        {
            if (strncmp( name, "switch/wine/", 12 ) || !allowed( name + 12 )) goto done;
            if (p->header.count >= INSTALL_MAX_FILES || strlen( name + 12 ) >= sizeof(e->path) ||
                info.uncompressed_size > INSTALL_FILE_LIMIT ||
                (info.external_fa & 0x10)) goto done;
            e = &p->entries[p->header.count];
            memset( e, 0, sizeof(*e) );
            strcpy( e->path, name + 12 );
            e->size = info.uncompressed_size;
            e->crc = info.crc;
            for (j = 0; j < p->header.count; j++) if (!strcasecmp( e->path, p->entries[j].path )) goto done;
            if (!check_parents( root, e->path ) || !join( dest, sizeof(dest), root, e->path )) goto done;
            r = regular( dest, &e->old_size );
            if (r < 0 || e->old_size > INSTALL_FILE_LIMIT) goto done;
            if (r && !managed( e->path )) goto next_entry;
            e->existed = r;
            backup_total += e->old_size;
            if (unzGetFilePos64( zip, &p->positions[p->header.count] ) != UNZ_OK) goto done;
            if (!strcmp( e->path, INSTALL_NRO )) nro_index = p->header.count;
            p->header.count++;
            total += e->size;
            if (total > INSTALL_MAX_SIZE) goto done;
        }
next_entry:
        if (i + 1 < global.number_entry && unzGoToNextFile( zip ) != UNZ_OK) goto done;
    }
    if (!has_file( p, INSTALL_NRO ) || !has_file( p, "drive_c/windows/system32/ntdll.dll" ) ||
        !has_file( p, "drive_c/windows/syswow64/ntdll.dll" ) ||
        !has_file( p, "drive_c/windows/syswow64/kernel32.dll" ) ||
        !has_file( p, "drive_c/windows/syswow64/kernelbase.dll" )) goto done;
    if (require_amd64 && (!has_file( p, "drive_c/windows/system32/winebox64ec.dll" ) ||
        !has_file( p, "drive_c/windows/system32/kernel32.dll" ) ||
        !has_file( p, "drive_c/windows/system32/kernelbase.dll" ))) goto done;
    if (!p->entries[nro_index].existed) goto done;
    {
        struct install_entry last = p->entries[nro_index];
        unz64_file_pos pos = p->positions[nro_index];
        i = p->header.count - 1;
        p->entries[nro_index] = p->entries[i]; p->positions[nro_index] = p->positions[i];
        p->entries[i] = last; p->positions[i] = pos;
    }
    if (!valid_plan( p )) goto done;
    if (statvfs( root, &space )) { result = AUTORUN_INSTALL_IO; goto done; }
    if ((uint64_t)space.f_bavail * space.f_frsize < total + backup_total +
        p->entries[p->header.count - 1].old_size + INSTALL_FILE_LIMIT + 16 * 1024 * 1024)
    { result = AUTORUN_INSTALL_SPACE; goto done; }
    result = AUTORUN_INSTALL_IO;
    if (!parents( root, "updates/transaction/journal" ) || !clean_transaction( root ) ||
        !parents( root, "updates/transaction/journal" )) goto done;
    prepared = 1;
    for (i = 0; i < p->header.count; i++)
    {
        struct install_entry *e = &p->entries[i];
        unsigned char buf[65536];
        FILE *out;
        uint64_t read = 0;
        int ok = 1, n;
        entry_path( dest, sizeof(dest), root, i, 0 );
        if (unzGoToFilePos64( zip, &p->positions[i] ) != UNZ_OK ||
            unzOpenCurrentFile( zip ) != UNZ_OK) { result = AUTORUN_INSTALL_INVALID; goto done; }
        if (!(out = fopen( dest, "wb" ))) { unzCloseCurrentFile( zip ); goto done; }
        while ((n = unzReadCurrentFile( zip, buf, sizeof(buf) )) > 0)
        {
            read += n;
            if (read > e->size || fwrite( buf, 1, n, out ) != (size_t)n) { ok = 0; break; }
            if (progress && progress( opaque, "Preparing update", current + read, total ))
            { result = AUTORUN_INSTALL_CANCELLED; ok = 0; break; }
        }
        if (!close_written( out )) ok = 0;
        if (unzCloseCurrentFile( zip ) != UNZ_OK || n < 0 || read != e->size)
        { if (result != AUTORUN_INSTALL_CANCELLED) result = AUTORUN_INSTALL_INVALID; ok = 0; }
        if (!ok) goto done;
        uint32_t written_crc;
        if (!file_crc( dest, e->size, &written_crc ) || written_crc != e->crc) goto done;
        if (!strcmp( e->path, INSTALL_NRO ) && !nro_valid( dest, e->size ))
        { result = AUTORUN_INSTALL_INVALID; goto done; }
        current += e->size;
    }
    current = 0;
    for (i = 0; i < p->header.count; i++)
    {
        struct install_entry *e = &p->entries[i];
        if (!e->existed) continue;
        join( path, sizeof(path), root, e->path );
        entry_path( dest, sizeof(dest), root, i, 1 );
        r = copy_file( path, dest, e->old_size, &e->old_crc, progress, opaque,
                       "Backing up runtime", current, backup_total );
        if (r != 1 || !sync_parent( dest )) { result = r < 0 ? AUTORUN_INSTALL_CANCELLED : AUTORUN_INSTALL_IO; goto done; }
        current += e->old_size;
    }
    i = p->header.count - 1;
    entry_path( path, sizeof(path), root, i, 1 );
    local_path( dest, sizeof(dest), root, "previous.nro" );
    uint32_t crc;
    r = copy_file( path, dest, p->entries[i].old_size, &crc, progress, opaque,
                   "Backing up launcher", 0, p->entries[i].old_size );
    if (r < 0) result = AUTORUN_INSTALL_CANCELLED;
    if (r != 1 || crc != p->entries[i].old_crc || !sync_parent( dest )) goto done;
    if (progress && progress( opaque, "Preparing update", total, total ))
    { result = AUTORUN_INSTALL_CANCELLED; goto done; }
    if (!save_plan( root, p )) goto done;
    transaction = 1;
    if (progress) progress( opaque, "Installing - do not power off", 0, total );
    if (unzGoToFirstFile( zip ) != UNZ_OK) goto done;
    for (i = 0; i < global.number_entry; i++)
    {
        if (unzGetCurrentFileInfo64( zip, NULL, name, sizeof(name), NULL, 0, NULL, 0 ) != UNZ_OK) goto done;
        if (strlen( name ) > 12 && name[strlen( name ) - 1] == '/')
        {
            if (snprintf( path, sizeof(path), "%s.empty", name + 12 ) >= (int)sizeof(path) || !parents( root, path )) goto done;
        }
        if (i + 1 < global.number_entry && unzGoToNextFile( zip ) != UNZ_OK) goto done;
    }
    current = 0;
    for (i = 0; i < p->header.count; i++)
    {
        const struct install_entry *e = &p->entries[i];
        entry_path( path, sizeof(path), root, i, 0 );
        join( dest, sizeof(dest), root, e->path );
        if (progress) progress( opaque, "Installing - do not power off", current, total );
        if (!parents( root, e->path ) || !replace( path, dest )) goto done;
        current += e->size;
    }
    local_path( path, sizeof(path), root, "transaction/committed" );
    FILE *marker = fopen( path, "wb" );
    if (!marker) goto done;
    if (!close_written( marker ) || !sync_parent( path )) goto done;
    result = AUTORUN_INSTALL_OK;
done:
    if (zip) unzClose( zip );
    if (result != AUTORUN_INSTALL_OK && transaction)
    {
        if (progress) progress( opaque, "Restoring previous runtime", 0, 0 );
        if (!rollback( root, p )) result = AUTORUN_INSTALL_RECOVERY;
    }
    else if (result != AUTORUN_INSTALL_OK && result != AUTORUN_INSTALL_RECOVERY && prepared)
        clean_transaction( root );
    free( p );
    return result;
}

const char *autorun_install_error( enum autorun_install_result result )
{
    switch (result)
    {
    case AUTORUN_INSTALL_OK: return "Update installed.";
    case AUTORUN_INSTALL_CANCELLED: return "Update cancelled. Your runtime was not changed.";
    case AUTORUN_INSTALL_INVALID: return "The release is damaged or does not contain a compatible Autorun runtime.";
    case AUTORUN_INSTALL_SPACE: return "Not enough free SD card space for the update and recovery files.";
    case AUTORUN_INSTALL_IO: return "The SD card could not complete the update. The previous runtime was kept.";
    case AUTORUN_INSTALL_RECOVERY: return "Update recovery is required. Restart Autorun before launching a game. If it cannot start, open switch/wine/updates/previous.nro from the Homebrew Menu.";
    }
    return "Update failed.";
}
