from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'dlls/quartz/filtermapper.c').read_text()
constructor = source[source.index('static HRESULT enum_moniker_create('):
                     source.index('typedef struct FilterMapper3Impl')]
start = source.index('    hr = ICreateDevEnum_CreateClassEnumerator(pCreateDevEnum, &CLSID_ActiveMovieCategories')
branch = source[start:source.index('    while (IEnumMoniker_Next(pEnumCat', start)]
fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
typedef int32_t HRESULT;
typedef void IMoniker;
typedef struct { const void *lpVtbl; } IEnumMoniker;
struct enum_moniker {
    IEnumMoniker IEnumMoniker_iface;
    unsigned int refcount, count;
    IMoniker **filters;
};
#define S_OK 0
#define S_FALSE 1
#define E_FAIL ((HRESULT)0x80004005)
#define E_OUTOFMEMORY ((HRESULT)0x8007000e)
#define TRACE(...) do {} while (0)
static const int enum_moniker_vtbl;
static int fail_allocation, releases;
static HRESULT enumeration_result;
static IEnumMoniker category;
static const int CLSID_ActiveMovieCategories;
static void *checked_calloc(size_t n, size_t size) {
    return fail_allocation ? NULL : calloc(n, size);
}
#define calloc checked_calloc
static HRESULT ICreateDevEnum_CreateClassEnumerator(void *dev, const void *clsid,
                                                   IEnumMoniker **out, int flags) {
    assert(dev == (void *)1 && clsid == &CLSID_ActiveMovieCategories && !flags);
    *out = enumeration_result == S_OK ? &category : NULL;
    return enumeration_result;
}
static void ICreateDevEnum_Release(void *dev) { assert(dev == (void *)1); releases++; }
'''
wrapper = r'''
static HRESULT enumerate(IEnumMoniker **ppEnum) {
    void *pCreateDevEnum = (void *)1;
    IEnumMoniker *pEnumCat;
    HRESULT hr;
    *ppEnum = NULL;
'''
tail = r'''
    assert(pEnumCat == &category);
    *ppEnum = pEnumCat;
    ICreateDevEnum_Release(pCreateDevEnum);
    return S_OK;
}
int main(void) {
    IEnumMoniker *out;
    struct enum_moniker *object;
    IMoniker *filters[] = {(void *)2, (void *)3};
    enumeration_result = S_FALSE;
    assert(enumerate(&out) == S_OK && out && releases == 1);
    object = (struct enum_moniker *)out;
    assert(!object->count && !object->filters && object->refcount == 1);
    assert(out->lpVtbl == &enum_moniker_vtbl);
    free(object);
    fail_allocation = 1;
    assert(enumerate(&out) == E_OUTOFMEMORY && !out && releases == 2);
    fail_allocation = 0;
    enumeration_result = E_FAIL;
    assert(enumerate(&out) == E_FAIL && !out && releases == 3);
    enumeration_result = S_OK;
    assert(enumerate(&out) == S_OK && out == &category && releases == 4);
    assert(enum_moniker_create(filters, 2, &out) == S_OK);
    object = (struct enum_moniker *)out;
    assert(object->count == 2 && !memcmp(object->filters, filters, sizeof(filters)));
    free(object->filters);
    free(object);
}
'''
with tempfile.TemporaryDirectory(prefix='quartz-empty-categories-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(fixture + constructor + wrapper + branch + tail)
    subprocess.run([os.environ.get('WINE_NX_HOST_CC', '/usr/bin/clang'), '-std=gnu11',
                    '-g', '-O1', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True, timeout=10)
print('DirectShow empty category, allocation failure and error propagation tests passed')
