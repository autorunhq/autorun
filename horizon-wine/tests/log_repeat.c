/* Host test for the log's flood folding (horizon-wine/source/log_repeat.h):
 * the folds fold what floods, keep what matters, and say what they folded. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../source/log_repeat.h"

int main(void)
{
    struct log_repeat_table table = {0};
    char out[256], s1[256], s2[256];
    enum log_repeat_action a;

    /* One map storm, the addresses differing between lines: two stand, the
     * rest fold until the summary's while is up. */
    a = log_repeat_next( &table, 0, "[HMAP] map_code failed addr=0x3fff0000 rc=0xd401 errno=17", out, sizeof(out) );
    assert( a == LOG_REPEAT_PRINT );
    a = log_repeat_next( &table, 0, "[HMAP] map_code failed addr=0x3ffe0000 rc=0xd401 errno=17", out, sizeof(out) );
    assert( a == LOG_REPEAT_PRINT );
    a = log_repeat_next( &table, 0, "[HMAP] map_code failed addr=0x3ffd0000 rc=0xd401 errno=17", out, sizeof(out) );
    assert( a == LOG_REPEAT_SUPPRESS );
    a = log_repeat_next( &table, 5, "[HMAP] map_code failed addr=0x3ffc0000 rc=0xd401 errno=17", out, sizeof(out) );
    assert( a == LOG_REPEAT_SUPPRESS );
    a = log_repeat_next( &table, 31000, "[HMAP] map_code failed addr=0x3ffb0000 rc=0xd401 errno=17", out, sizeof(out) );
    assert( a == LOG_REPEAT_SUMMARY );
    assert( strstr( out, "x3 more like" ) && strstr( out, "0x3fff0000" ) );

    /* A fold still owed is said at the next one, however long the wait. */
    a = log_repeat_next( &table, 40000, "[HMAP] map_code failed addr=0x3ffa0000 rc=0xd401 errno=17", out, sizeof(out) );
    assert( a == LOG_REPEAT_SUPPRESS );
    a = log_repeat_next( &table, 400000, "[HMAP] map_code failed addr=0x3ff90000 rc=0xd401 errno=17", out, sizeof(out) );
    assert( a == LOG_REPEAT_SUMMARY );
    assert( strstr( out, "x2 more like" ) );

    /* A shape quiet for a whole window with nothing owed starts over. */
    a = log_repeat_next( &table, 700000, "[HMAP] map_code failed addr=0x3ff80000 rc=0xd401 errno=17", out, sizeof(out) );
    assert( a == LOG_REPEAT_PRINT );

    /* [EXC] faults fold per code, address and pc: the counters and the stacks
     * around the fault may run on, one storm of one fault is one shape. */
    a = log_repeat_next( &table, 700000,
                         "[EXC] fault 1 to the program: tid=0004 code=0xc0000005 writing 0xfee60ff0 pc=0x20aa787c sp=0x1 lr=0x2 fp=0x3",
                         out, sizeof(out) );
    assert( a == LOG_REPEAT_PRINT );
    a = log_repeat_next( &table, 700001,
                         "[EXC] fault 2 to the program: tid=0004 code=0xc0000005 writing 0xfee60ff0 pc=0x20aa787c sp=0x9 lr=0x8 fp=0x7",
                         out, sizeof(out) );
    assert( a == LOG_REPEAT_PRINT );
    a = log_repeat_next( &table, 700002,
                         "[EXC] fault 4 to the program: tid=0004 code=0xc0000005 writing 0xfee60ff0 pc=0x20aa787c sp=0x5 lr=0x6 fp=0x7",
                         out, sizeof(out) );
    assert( a == LOG_REPEAT_SUPPRESS );
    /* Another faulting address beside it stands on its own. */
    a = log_repeat_next( &table, 700002,
                         "[EXC] fault 1 to the program: tid=0004 code=0xc0000005 writing 0xfee60fff0 pc=0x20aa787c sp=0x5 lr=0x6 fp=0x7",
                         out, sizeof(out) );
    assert( a == LOG_REPEAT_PRINT );

    /* Wine's own channels fold too. */
    a = log_repeat_next( &table, 700002, "0004:err:module:alloc_tls_slot NtQueryInformationThread failed.", out, sizeof(out) );
    assert( a == LOG_REPEAT_PRINT );
    a = log_repeat_next( &table, 700003, "0004:err:module:alloc_tls_slot NtQueryInformationThread failed.", out, sizeof(out) );
    assert( a == LOG_REPEAT_PRINT );
    a = log_repeat_next( &table, 700004, "0004:err:module:alloc_tls_slot NtQueryInformationThread failed.", out, sizeof(out) );
    assert( a == LOG_REPEAT_SUPPRESS );

    /* Lines outside the kinds above keep every one of themselves. */
    a = log_repeat_next( &table, 700004, "[NXINPUT] the cursor is at 895,629, the stick moved 32,-1", out, sizeof(out) );
    assert( a == LOG_REPEAT_PRINT );
    a = log_repeat_next( &table, 700004, "[NXINPUT] the cursor is at 927,624, the stick moved 32,-5", out, sizeof(out) );
    assert( a == LOG_REPEAT_PRINT );
    a = log_repeat_next( &table, 700004, "[NXGL] the crop is 240,0-1680,1080", out, sizeof(out) );
    assert( a == LOG_REPEAT_PRINT );

    /* The way out still says what it folded: the loader's and the fault
     * storm's summaries, in either order. */
    assert( log_repeat_flush( &table, s1, sizeof(s1) ) );
    assert( log_repeat_flush( &table, s2, sizeof(s2) ) );
    assert( !log_repeat_flush( &table, out, sizeof(out) ) );
    assert( strstr( s1, "x1 more like" ) && strstr( s2, "x1 more like" ) );
    assert( (strstr( s1, "NtQueryInformationThread" ) && strstr( s2, "writing 0xfee60ff0" )) ||
            (strstr( s2, "NtQueryInformationThread" ) && strstr( s1, "writing 0xfee60ff0" )) );

    printf( "log_repeat: all cases pass\n" );
    return 0;
}
