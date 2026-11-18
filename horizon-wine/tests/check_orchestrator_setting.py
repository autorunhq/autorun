#!/usr/bin/env python3
"""Carafe's thread orchestrator and its lock wait boost are a program's
settings, off unless turned on, and Carafe's reports follow this runtime's
profiler switch. The runtime answers for all three (source/runtime.c): "not
yet" until the settings of the program about to start have been read, so that
nothing of Carafe's starts while the launcher is on the screen."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
wine = root / 'horizon-wine'
runtime = (wine / 'source/runtime.c').read_text()
settings = (wine / 'source/launcher_settings.h').read_text()
launcher = (wine / 'source/launcher.c').read_text()
carafe = (wine / 'source/carafe_settings.c').read_text()
diag = (wine / 'source/carafe_diag.c').read_text()
boost = (wine / 'source/carafe_lock_boost.c').read_text()
cmake = (wine / 'CMakeLists.txt').read_text()

# Read from and written to each program's settings: only "1" turns one on, and
# a setting that is off leaves no line.
for field, key in (('orchestrator', 'orchestrator'), ('lock_boost', 'lock-boost')):
    assert f'settings->{field} = launcher_setting_state( kv, "{key}" ) == 1;' in settings, key
    assert f'launcher_kv_set( kv, "{key}", settings->{field} ? "1" : NULL )' in settings, key

# A switch for each in the launcher; Reset turns it off.
for row, field in (('ROW_ORCHESTRATOR', 'orchestrator'), ('ROW_LOCK_BOOST', 'lock_boost')):
    assert f'ADD_ROW( {row}, SECTION_EMULATION,' in launcher, row
    handler = launcher[launcher.index(f'case {row}:'):]
    handler = handler[:handler.index('break;')]
    assert f'p->settings.{field} = action == UI_ACTION_RESET ? 0 : !p->settings.{field};' in handler, row
    assert 'save_program_settings( l, p );' in handler, row

# The runtime starts each program with both off, then takes the program's own.
block = runtime[runtime.index("/* The program's own settings, written by the launcher next to it"):]
block = block[:block.index('horizon_server_profile_enabled = runtime_profile;')]
assert block.index('runtime_orchestrator = 0;') < block.index('runtime_orchestrator = settings.orchestrator;')
assert block.index('__atomic_store_n( &carafe_lock_boost_enabled, 0, __ATOMIC_RELAXED )') < \
       block.index('__atomic_store_n( &carafe_lock_boost_enabled, settings.lock_boost, __ATOMIC_RELAXED )')
# The answer is released after the last place that changes the profiler switch.
released = runtime.index('__atomic_store_n( &runtime_program_settings_read, 1, __ATOMIC_RELEASE );')
assert runtime.count('__atomic_store_n( &runtime_program_settings_read,') == 1
for change in re.finditer(r'\bruntime_(?:profile|orchestrator) = ', runtime):
    assert change.start() < released, runtime[change.start():change.start() + 60]
assert '[INIT] thread orchestrator %s, lock wait boost %s' in runtime

# The boost is off where it is defined, and the build links the orchestrator
# in front of the balancer.
assert re.search(r'^int carafe_lock_boost_enabled;$', boost, re.M)
assert 'source/orchestrator.c\n    source/orchestrator_policy.c\n' in cmake
wraps = cmake[cmake.index('foreach(symbol wine_nx_threads_report_stalled'):]
wraps = wraps[:wraps.index(')')]
assert 'wine_nx_thread_balance' in wraps

# Carafe's reports ask every time; an answer kept from the launcher's time
# would be "off" for good.
measuring = diag[diag.index('static bool measuring(void)'):]
measuring = measuring[:measuring.index('\n}\n')]
assert 'static' not in measuring[len('static bool measuring(void)'):] and 'carafeMeasuring()' in measuring
report = diag[diag.index('void __wrap_wine_nx_thread_report'):]
assert report.index('wine_nx_program_profiling() < 0') < report.index('++reports')


def function(text, start):
    body = text[text.index(start):]
    return body[:body.index('\n}\n') + 3]


# The two questions Carafe's code asks, with the runtime's answers behind them.
harness = '''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int runtime_profile, runtime_program_settings_read, runtime_orchestrator;
static unsigned int file_reads;
static const char *file_text = "";

static char *programText( void ) { file_reads++; return strdup( file_text ); }
static bool textValue( const char *text, const char *key, char *out, size_t size )
{
    size_t length = strlen( key );
    const char *line = text;

    for (; line && *line; line = strchr( line, '\\n' ) ? strchr( line, '\\n' ) + 1 : NULL)
        if (!strncmp( line, key, length ) && line[length] == '=')
        {
            snprintf( out, size, "%.*s", (int)strcspn( line + length + 1, "\\n" ), line + length + 1 );
            return true;
        }
    return false;
}

@WEAK@
@QUESTIONS@
@ANSWERS@

int main( void )
{
    /* The launcher is on the screen: neither, whatever a file says. */
    file_text = "profile=1\\norchestrator=1\\n";
    runtime_profile = runtime_orchestrator = 1;
    assert( wine_nx_program_profiling() == -1 && wine_nx_program_orchestrating() == -1 );
    assert( !carafeMeasuring() && !carafeOrchestrating() );

    /* The program's settings are read: each follows its own switch. */
    runtime_program_settings_read = 1;
    assert( carafeMeasuring() && carafeOrchestrating() );
    runtime_orchestrator = 0;
    assert( carafeMeasuring() && !carafeOrchestrating() && wine_nx_program_orchestrating() == 0 );
    runtime_profile = 0;
    runtime_orchestrator = 1;
    assert( !carafeMeasuring() && carafeOrchestrating() && wine_nx_program_profiling() == 0 );

    /* Carafe turns its orchestrator on where the file says nothing; here the
     * runtime's answer stands and the file is never opened. */
    file_text = "";
    runtime_orchestrator = 0;
    assert( !carafeOrchestrating() && !file_reads );
    return 0;
}
'''
answers = function(runtime, 'int wine_nx_program_profiling( void )') + \
          function(runtime, 'int wine_nx_program_orchestrating( void )')
questions = function(carafe, 'bool carafeMeasuring(void)') + function(carafe, 'bool carafeOrchestrating(void)')
# Weak, as in Carafe's file: without this runtime it reads its own file.
weak = ''.join(f'int {name}(void) __attribute__((weak));\n'
               for name in ('wine_nx_program_profiling', 'wine_nx_program_orchestrating'))
assert weak in carafe
cc = os.environ.get('WINE_NX_HOST_CC', 'cc')
with tempfile.TemporaryDirectory() as tmp:
    src = Path(tmp) / 'orchestrator_setting.c'
    exe = Path(tmp) / 'orchestrator_setting'
    src.write_text(harness.replace('@WEAK@', weak).replace('@ANSWERS@', answers).replace('@QUESTIONS@', questions))
    subprocess.run([cc, '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
                    '-fsanitize=address,undefined', str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print('orchestrator setting: ok')
