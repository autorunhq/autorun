/* Host test for the launcher's program list (source/launcher_list.h). */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "../source/launcher_list.h"

static void test_names(void)
{
    assert( !strcmp( launcher_machine_name( 0x8664 ), "x64" ) );
    assert( !strcmp( launcher_machine_name( 0x014c ), "x86" ) );
    assert( !strcmp( launcher_machine_name( 0xaa64 ), "ARM64" ) );
    assert( !strcmp( launcher_machine_name( 0 ), "Unknown" ) );
    assert( launcher_is_exe( "notepad.exe" ) && launcher_is_exe( "7ZR.EXE" ) && launcher_is_exe( "a.Exe" ) );
    assert( !launcher_is_exe( ".exe" ) && !launcher_is_exe( "._notepad.exe" ) );  /* macOS resource forks */
    assert( !launcher_is_exe( "notepad.exe.txt" ) && !launcher_is_exe( "readme" ) && !launcher_is_exe( "x.dll" ) );
}

static void test_args(void)
{
    assert( launcher_args_match( "C:\\notepad.exe C:\\notepad-test.txt", "C:\\notepad.exe" ) );
    assert( launcher_args_match( "c:\\7zr.exe b 1 -mmt2 -md18", "C:\\7zr.exe" ) );
    assert( launcher_args_match( "  \"C:\\Program Files\\app.exe\" -x", "C:\\Program Files\\app.exe" ) );
    assert( launcher_args_match( "C:\\pe32-timers.exe", "C:\\pe32-timers.exe" ) );
    assert( !launcher_args_match( "C:\\notepad.exe C:\\notepad-test.txt", "C:\\pe32-timers.exe" ) );
    assert( !launcher_args_match( "C:\\notepad.exe2 x", "C:\\notepad.exe" ) );
    assert( !launcher_args_match( "\"C:\\notepad.exe x", "C:\\notepad.exe" ) );
}

static void test_program_args(void)
{
    char path[64], line[64];

    assert( launcher_args_path( "sdmc:/switch/wine/drive_c/openttd/openttd.exe", path, sizeof(path) ) );
    assert( !strcmp( path, "sdmc:/switch/wine/drive_c/openttd/openttd.args.txt" ) );
    assert( launcher_args_path( "sdmc:/x/APP.EXE", path, sizeof(path) ) && !strcmp( path, "sdmc:/x/APP.args.txt" ) );
    assert( !launcher_args_path( "sdmc:/x/readme.txt", path, sizeof(path) ) );
    assert( !launcher_args_path( "sdmc:/switch/wine/drive_c/openttd/openttd.exe", path, 20 ) );
    assert( launcher_keys_path( "sdmc:/x/SPEED2.EXE", path, sizeof(path) ) && !strcmp( path, "sdmc:/x/SPEED2.keys.txt" ) );
    assert( !launcher_keys_path( "sdmc:/x/a.exe", path, 18 ) );  /* sdmc:/x/a.keys.txt needs 19 */
    assert( launcher_keys_path( "sdmc:/x/a.exe", path, 19 ) && !strcmp( path, "sdmc:/x/a.keys.txt" ) );
    assert( !launcher_keys_path( "sdmc:/x/readme.txt", path, sizeof(path) ) );
    assert( launcher_command_line( "C:\\openttd\\openttd.exe", "-s null -m null", line, sizeof(line) ) );
    assert( !strcmp( line, "C:\\openttd\\openttd.exe -s null -m null" ) );
    assert( launcher_command_line( "C:\\Program Files\\a.exe", "-x", line, sizeof(line) ) );
    assert( !strcmp( line, "\"C:\\Program Files\\a.exe\" -x" ) );
    assert( !launcher_command_line( "C:\\openttd\\openttd.exe", "-v win32:no_threads -s null -m null -r 1280x720", line, 40 ) );
}

static void test_order_and_find(void)
{
    struct launcher_entry entries[4] = {
        { "sdmc:/switch/wine/drive_c/pe32-timers.exe", "C:\\pe32-timers.exe", 0x14c },
        { "sdmc:/switch/wine/drive_c/curl/curl.exe", "C:\\curl\\curl.exe", 0xaa64 },
        { "sdmc:/switch/wine/drive_c/Notepad.exe", "C:\\Notepad.exe", 0x14c },
        { "sdmc:/switch/wine/drive_c/7zr.exe", "C:\\7zr.exe", 0x14c },
    };

    qsort( entries, 4, sizeof(entries[0]), launcher_compare );
    assert( !strcmp( entries[0].dos, "C:\\7zr.exe" ) && !strcmp( entries[1].dos, "C:\\curl\\curl.exe" ) );
    assert( !strcmp( entries[2].dos, "C:\\Notepad.exe" ) && !strcmp( entries[3].dos, "C:\\pe32-timers.exe" ) );
    assert( launcher_find( entries, 4, "sdmc:/switch/wine/drive_c/notepad.exe" ) == 2 );
    assert( launcher_find( entries, 4, "C:\\PE32-TIMERS.EXE" ) == 3 );
    assert( launcher_find( entries, 4, "sdmc:/switch/wine/drive_c/gone.exe" ) == 0 );
}

static void test_scrolling(void)
{
    /* 100 programs, 36 rows. */
    assert( launcher_first_visible( 0, 0, 100, 36 ) == 0 );
    assert( launcher_first_visible( 0, 35, 100, 36 ) == 0 );
    assert( launcher_first_visible( 0, 36, 100, 36 ) == 1 );    /* one step past the bottom */
    assert( launcher_first_visible( 10, 20, 100, 36 ) == 10 );  /* inside: unchanged */
    assert( launcher_first_visible( 10, 5, 100, 36 ) == 5 );
    assert( launcher_first_visible( 0, 99, 100, 36 ) == 64 );   /* the last page */
    assert( launcher_first_visible( 80, 99, 100, 36 ) == 64 );
    /* Fewer programs than rows. */
    assert( launcher_first_visible( 3, 2, 5, 36 ) == 0 );
    assert( launcher_first_visible( 0, 0, 0, 36 ) == 0 );
}

static void test_dos_paths(void)
{
    char dos[32], long_dos[64];

    assert( launcher_dos_path( "sdmc:/switch/wine/drive_c/openttd/openttd.exe", dos, sizeof(dos) ) );
    assert( !strcmp( dos, "C:\\openttd\\openttd.exe" ) );
    assert( launcher_dos_path( "sdmc:/switch/wine/drive_c", dos, sizeof(dos) ) && !strcmp( dos, "C:\\" ) );
    assert( launcher_dos_path( "sdmc:/switch/wine/drive_c/", dos, sizeof(dos) ) && !strcmp( dos, "C:\\" ) );
    assert( launcher_dos_path( "sdmc:/SWITCH/wine/DRIVE_C/x.exe", dos, sizeof(dos) ) && !strcmp( dos, "C:\\x.exe" ) );
    /* Beside drive_c, not in it. */
    assert( launcher_dos_path( "sdmc:/switch/wine/drive_c2/x.exe", dos, sizeof(dos) ) );
    assert( !strcmp( dos, "Z:\\switch\\wine\\drive_c2\\x.exe" ) );
    assert( launcher_dos_path( "sdmc:/games/Game/", dos, sizeof(dos) ) && !strcmp( dos, "Z:\\games\\Game" ) );
    assert( launcher_dos_path( "sdmc:/", dos, sizeof(dos) ) && !strcmp( dos, "Z:\\" ) );
    assert( launcher_dos_path( "sdmc:", dos, sizeof(dos) ) && !strcmp( dos, "Z:\\" ) );
    assert( !launcher_dos_path( "romfs:/x.exe", dos, sizeof(dos) ) && !launcher_dos_path( "sdmcx:/a", dos, sizeof(dos) ) );
    assert( !launcher_dos_path( "ums0:/Wine/Dark Souls II/Game/DarkSoulsII.exe", dos, sizeof(dos) ) );
    assert( launcher_dos_path( "ums0:/Wine/Dark Souls II/Game/DarkSoulsII.exe", long_dos, sizeof(long_dos) ) &&
            !strcmp( long_dos, "D:\\Wine\\Dark Souls II\\Game\\DarkSoulsII.exe" ) );
    assert( launcher_dos_path( "ums4:/", dos, sizeof(dos) ) && !strcmp( dos, "H:\\" ) );
    assert( launcher_dos_path( "ums1:", dos, sizeof(dos) ) && !strcmp( dos, "E:\\" ) );
    assert( !launcher_dos_path( "ums5:/x.exe", dos, sizeof(dos) ) && !launcher_dos_path( "ums10:/x.exe", dos, sizeof(dos) ) );
    assert( !launcher_dos_path( "umsx:/x.exe", dos, sizeof(dos) ) && !launcher_dos_path( "ums0x/x.exe", dos, sizeof(dos) ) );
    assert( !launcher_dos_path( "sdmc:/a/very/long/path/that/does/not/fit.exe", dos, sizeof(dos) ) );
}

static void test_grid(void)
{
    int count, columns, rows, selection, next, direction;

    assert( launcher_grid_move( 0, 23, 5, 2, 1, 0 ) == 1 );
    assert( launcher_grid_move( 4, 23, 5, 2, 1, 0 ) == 10 );
    assert( launcher_grid_move( 9, 23, 5, 2, 1, 0 ) == 15 );
    assert( launcher_grid_move( 10, 23, 5, 2, -1, 0 ) == 4 );
    assert( launcher_grid_move( 15, 23, 5, 2, -1, 0 ) == 9 );
    assert( launcher_grid_move( 5, 23, 5, 2, -1, 0 ) == 5 );
    assert( launcher_grid_move( 0, 23, 5, 2, -1, 0 ) == 0 );
    assert( launcher_grid_move( 19, 23, 5, 2, 1, 0 ) == 22 );
    assert( launcher_grid_move( 22, 23, 5, 2, 1, 0 ) == 22 );
    assert( launcher_grid_move( 2, 23, 5, 2, 0, 1 ) == 7 );
    assert( launcher_grid_move( 7, 23, 5, 2, 0, 1 ) == 12 );
    assert( launcher_grid_move( 7, 23, 5, 2, 0, -1 ) == 2 );
    assert( launcher_grid_move( 2, 23, 5, 2, 0, -1 ) == 2 );
    assert( launcher_grid_move( 21, 23, 5, 2, 0, 1 ) == 21 );
    assert( launcher_grid_move( 4, 8, 5, 2, 0, 1 ) == 7 );
    assert( launcher_grid_move( 4, 8, 5, 2, 1, 0 ) == 4 );
    assert( launcher_grid_move( 0, 0, 5, 2, 1, 0 ) == 0 );
    assert( launcher_grid_page( 3, 23, 5, 2, 1 ) == 13 );
    assert( launcher_grid_page( 13, 23, 5, 2, 1 ) == 22 );
    assert( launcher_grid_page( 18, 23, 5, 2, -1 ) == 8 );
    assert( launcher_grid_page( 5, 23, 5, 2, -1 ) == 5 );
    assert( launcher_grid_page( 22, 23, 5, 2, 1 ) == 22 );
    assert( launcher_grid_page( 0, 0, 5, 2, 1 ) == 0 );

    for (count = 1; count <= LAUNCHER_MAX_ENTRIES; count++)
        for (columns = 1; columns <= 6; columns++)
            for (rows = 1; rows <= 3; rows++)
                for (selection = 0; selection < count; selection++)
                    for (direction = -1; direction <= 1; direction += 2)
                    {
                        next = launcher_grid_move( selection, count, columns, rows, direction, 0 );
                        assert( next >= 0 && next < count );
                        if (next / (columns * rows) == selection / (columns * rows))
                            assert( next / columns == selection / columns );
                        next = launcher_grid_move( selection, count, columns, rows, 0, direction );
                        assert( next >= 0 && next < count );
                        next = launcher_grid_page( selection, count, columns, rows, direction );
                        assert( next >= 0 && next < count );
                        if (next != selection)
                            assert( next / (columns * rows) == selection / (columns * rows) + direction );
                    }
}

int main(void)
{
    test_names();
    test_args();
    test_program_args();
    test_order_and_find();
    test_scrolling();
    test_dos_paths();
    test_grid();
    puts( "launcher list: program names, args.txt matching, program argument files, order, preselection, "
          "scrolling, DOS paths and grid moves passed" );
    return 0;
}
