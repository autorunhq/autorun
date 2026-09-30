from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'dlls/wmvcore/wmvcore_main.c').read_text()
functions = source[source.index('static HRESULT (WINAPI *p_create_wm_sync_reader)'):
                   source.index('HRESULT WINAPI WMCheckURLExtension')]
fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>
typedef int32_t HRESULT;
typedef unsigned int DWORD;
typedef int BOOL;
typedef void IUnknown;
typedef void IWMSyncReader;
typedef void *HMODULE;
typedef struct { int complete; } INIT_ONCE;
#define INIT_ONCE_STATIC_INIT {0}
#define WINAPI
#define CALLBACK
#define FALSE 0
#define TRUE 1
#define ERROR_MOD_NOT_FOUND 126
#define ERROR_PROC_NOT_FOUND 127
#define HRESULT_FROM_WIN32(x) ((HRESULT)(0x80070000u | (x)))
#define TRACE(...) do {} while (0)
static DWORD error;
static unsigned int loads, unloads, calls;
static int missing_module = 1, missing_export = 1;
static HRESULT backend_result;
static IUnknown *expected_outer;
static DWORD GetLastError(void) { return error; }
static void SetLastError(DWORD value) { error = value; }
static HMODULE LoadLibraryW(const wchar_t *name) {
    assert(!wcscmp(name, L"winegstreamer.dll"));
    loads++;
    if (missing_module) { error = ERROR_MOD_NOT_FOUND; return NULL; }
    return (void *)1;
}
static HRESULT backend(IUnknown *outer, void **out) {
    assert(outer == expected_outer && !*out);
    calls++;
    if (!backend_result) *out = (void *)2;
    return backend_result;
}
static void *GetProcAddress(HMODULE module, const char *name) {
    assert(module == (void *)1 && !strcmp(name, "winegstreamer_create_wm_sync_reader"));
    return missing_export ? NULL : (void *)backend;
}
static BOOL FreeLibrary(HMODULE module) { assert(module == (void *)1); unloads++; return TRUE; }
static BOOL InitOnceExecuteOnce(INIT_ONCE *once, BOOL (*callback)(INIT_ONCE *, void *, void **),
                                void *param, void **context) {
    if (!once->complete) once->complete = callback(once, param, context);
    return once->complete;
}
'''
tests = r'''
int main(void) {
    void *reader = (void *)3;
    assert(WMCreateSyncReader(NULL, 0, &reader) == HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND));
    assert(!reader && loads == 1 && !unloads && !calls);
    missing_module = 0;
    reader = (void *)3;
    assert(WMCreateSyncReaderPriv(&reader) == HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND));
    assert(!reader && loads == 2 && unloads == 1 && !calls);
    missing_export = 0;
    assert(!WMCreateSyncReader(NULL, 0, &reader));
    assert(reader == (void *)2 && loads == 3 && unloads == 1 && calls == 1);
    assert(!WMCreateSyncReaderPriv(&reader));
    assert(reader == (void *)2 && loads == 3 && calls == 2);
    expected_outer = (void *)4;
    assert(!create_wm_sync_reader(expected_outer, &reader));
    assert(reader == (void *)2 && loads == 3 && calls == 3);
    backend_result = (HRESULT)0x80004005;
    assert(create_wm_sync_reader(expected_outer, &reader) == backend_result);
    assert(!reader && loads == 3 && calls == 4);
}
'''
with tempfile.TemporaryDirectory(prefix='wmvcore-backend-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(fixture + functions + tests)
    subprocess.run([os.environ.get('WINE_NX_HOST_CC', '/usr/bin/clang'), '-std=gnu11',
                    '-g', '-O1', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True, timeout=10)
print('WM reader backend failure, retry and caching tests passed')
