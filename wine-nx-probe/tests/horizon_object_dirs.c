/* Host test for the Horizon server's object directories (dlls/ntdll/unix/horizon_object_dirs.h). */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../dlls/ntdll/unix/horizon_object_dirs.h"

#define SUCCESS         HORIZON_OBJECT_DIR_STATUS_SUCCESS
#define MORE_ENTRIES    HORIZON_OBJECT_DIR_STATUS_MORE_ENTRIES
#define NO_MORE_ENTRIES HORIZON_OBJECT_DIR_STATUS_NO_MORE_ENTRIES

static unsigned int utf16( const char *ascii, unsigned char *out )
{
    unsigned int i;

    for (i = 0; ascii[i]; i++)
    {
        out[2 * i] = (unsigned char)ascii[i];
        out[2 * i + 1] = 0;
    }
    return 2 * i;
}

static int dir( const char *path )
{
    unsigned char name[256];
    unsigned int len = utf16( path, name );

    return horizon_object_dir_from_path( name, len );
}

/* GetLogicalDrives, reading the records the way NtQueryDirectoryObject lays them out. */
static unsigned int logical_drives( const unsigned char *data, unsigned int count )
{
    unsigned int i, pos = 0, mask = 0, name_len, type_len;

    for (i = 0; i < count; i++)
    {
        const unsigned char *name = data + pos + 8;

        memcpy( &name_len, data + pos, 4 );
        memcpy( &type_len, data + pos + 4, 4 );
        if (name_len == 4 && name[2] == ':' && !name[3]) mask |= 1u << (name[0] - 'A');
        pos += 8 + ((name_len + type_len + 3) & ~3u);
    }
    return mask;
}

int main(void)
{
    unsigned char data[256], name[64];
    unsigned int size, count, total, len;

    assert( dir( "\\??" ) == HORIZON_OBJECT_DIR_DOS_DEVICES );
    assert( dir( "\\DosDevices" ) == HORIZON_OBJECT_DIR_DOS_DEVICES );   /* GetLogicalDrives */
    assert( dir( "\\dosdevices" ) == HORIZON_OBJECT_DIR_DOS_DEVICES );
    assert( dir( "\\GLOBAL??" ) == HORIZON_OBJECT_DIR_DOS_DEVICES );
    assert( dir( "\\Sessions\\1\\BaseNamedObjects" ) == HORIZON_OBJECT_DIR_NAMED_OBJECTS );
    assert( dir( "\\sessions\\12\\basenamedobjects" ) == HORIZON_OBJECT_DIR_NAMED_OBJECTS );
    assert( dir( "\\BaseNamedObjects" ) == HORIZON_OBJECT_DIR_NAMED_OBJECTS );
    assert( dir( "\\Sessions\\\\BaseNamedObjects" ) == HORIZON_OBJECT_DIR_NONE );
    assert( dir( "\\Sessions\\1x\\BaseNamedObjects" ) == HORIZON_OBJECT_DIR_NONE );
    assert( dir( "\\Sessions\\1\\Windows\\WindowStations" ) == HORIZON_OBJECT_DIR_NONE );
    assert( dir( "\\KnownDlls" ) == HORIZON_OBJECT_DIR_NONE );
    assert( dir( "\\DosDevices\\" ) == HORIZON_OBJECT_DIR_NONE );
    assert( dir( "\\??\\C:" ) == HORIZON_OBJECT_DIR_NONE );
    assert( dir( "\\?" ) == HORIZON_OBJECT_DIR_NONE );
    assert( dir( "" ) == HORIZON_OBJECT_DIR_NONE );
    len = utf16( "\\??", name );
    assert( horizon_object_dir_from_path( name, len - 1 ) == HORIZON_OBJECT_DIR_NONE );  /* odd length */
    name[3] = 1;  /* U+013F is not '?' */
    assert( horizon_object_dir_from_path( name, len ) == HORIZON_OBJECT_DIR_NONE );

    /* the whole listing in one reply */
    assert( horizon_object_dir_entries( HORIZON_OBJECT_DIR_DOS_DEVICES, 1u << 2, 0, ~0u, sizeof(data),
                                        data, &size, &count, &total ) == SUCCESS );
    assert( count == 1 && size == 36 && total == 4 + 24 );
    assert( logical_drives( data, count ) == 1u << 2 );
    assert( !memcmp( data + 8, "C\0:\0", 4 ) && !memcmp( data + 12, "S\0y\0m\0b\0o\0l\0i\0c\0L\0i\0n\0k\0", 24 ) );

    /* single-entry reads: the next index has nothing */
    assert( horizon_object_dir_entries( HORIZON_OBJECT_DIR_DOS_DEVICES, 1u << 2, 0, 1, sizeof(data),
                                        data, &size, &count, &total ) == SUCCESS && count == 1 );
    assert( horizon_object_dir_entries( HORIZON_OBJECT_DIR_DOS_DEVICES, 1u << 2, 1, 1, sizeof(data),
                                        data, &size, &count, &total ) == NO_MORE_ENTRIES );
    assert( !count && !size && !total );

    /* a reply buffer too small for the entry still reports its length */
    assert( horizon_object_dir_entries( HORIZON_OBJECT_DIR_DOS_DEVICES, 1u << 2, 0, 1, 35,
                                        data, &size, &count, &total ) == MORE_ENTRIES );
    assert( !count && !size && total == 28 );

    assert( horizon_object_dir_entries( HORIZON_OBJECT_DIR_DOS_DEVICES, 1u << 2, 0, 0, sizeof(data),
                                        data, &size, &count, &total ) == NO_MORE_ENTRIES );
    assert( horizon_object_dir_entries( HORIZON_OBJECT_DIR_NAMED_OBJECTS, ~0u, 0, ~0u, sizeof(data),
                                        data, &size, &count, &total ) == NO_MORE_ENTRIES );
    assert( !count && !size );

    for (unsigned int usb = 0; usb < 32; usb++)
    {
        unsigned int drives = (1u << 2) | (1u << 25) | (usb << 3), found = 0, index = 0;
        unsigned int status;
        assert( horizon_object_dir_entries( HORIZON_OBJECT_DIR_DOS_DEVICES, drives, 0, ~0u, sizeof(data),
                                            data, &size, &count, &total ) == SUCCESS );
        assert( logical_drives( data, count ) == drives );
        assert( size == count * 36 && total == count * 28 );
        while (!(status = horizon_object_dir_entries( HORIZON_OBJECT_DIR_DOS_DEVICES, drives, index, 1,
                                                       sizeof(data), data, &size, &count, &total )))
        {
            unsigned int drive = logical_drives( data, count );
            assert( count == 1 && size == 36 && total == 28 && !(found & drive) );
            found |= drive;
            index++;
        }
        assert( status == NO_MORE_ENTRIES && !count && !size && !total && found == drives );
        assert( horizon_object_dir_entries( HORIZON_OBJECT_DIR_DOS_DEVICES, drives, ~0u, 1, sizeof(data),
                                            data, &size, &count, &total ) == NO_MORE_ENTRIES );
        assert( horizon_object_dir_entries( HORIZON_OBJECT_DIR_DOS_DEVICES, drives, 0, ~0u, 36,
                                            data, &size, &count, &total ) == MORE_ENTRIES );
        assert( count == 1 && size == 36 && total == 56 );
    }

    puts( "Horizon object directories: names, sessions, case, DOS device listing, reply limits and empty directories passed" );
    return 0;
}
