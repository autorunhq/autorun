/* Autorun's Windows components setup: what Wine's first-run setup (wineboot,
 * from wine.inf) registers on a computer and does not get to on the Switch,
 * for the programs that need it -- Fallout New Vegas, Fable, WarCraft III's
 * movies and anything else that plays video or sound through DirectShow.
 *
 * The runtime runs it by itself before the first program on a card, and
 * again when a build adds a step (wine-nx-probe/source/runtime.c); running it
 * by hand from C:\windows is harmless.
 *
 * - The MP3 decoder l3codeca.acm under Drivers32, which DirectShow's MPEG
 *   audio path and the ACM find it by.
 * - DirectShow and DirectX Media Objects, registered as regsvr32 would: each
 *   DLL's DllRegisterServer writes its filters and categories, which only it
 *   knows how to lay out. devenum goes first, since the filter mapper quartz
 *   registers through is devenum's; a DLL that is not staged is skipped.
 *
 * Each step is reported to autorun_runtime.log as an [AUTORUN SETUP] line; the
 * exit code is 0 when every step that matters worked. */
#include <windows.h>
#include <winternl.h>
#include <ole2.h>

__declspec(dllimport) NTSTATUS NTAPI NtDisplayString( const UNICODE_STRING *str );

/* Built without a C runtime; the compiler still expects these for structures. */
void *memset( void *dst, int c, size_t n )
{
    unsigned char *p = dst;
    while (n--) *p++ = (unsigned char)c;
    return dst;
}

void *memcpy( void *dst, const void *src, size_t n )
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    while (n--) *d++ = *s++;
    return dst;
}

/* One line: "[AUTORUN SETUP] <label> <name>: <result>", with the value in hex. */
static void report( const char *label, const WCHAR *name, const char *result, DWORD value )
{
    static const char hex[] = "0123456789abcdef";
    const char *prefix = "[AUTORUN SETUP] ";
    WCHAR buffer[400];
    UNICODE_STRING str;
    unsigned int n = 0, i;

    while (*prefix) buffer[n++] = *prefix++;
    while (*label && n < 100) buffer[n++] = *label++;
    if (name)
    {
        buffer[n++] = ' ';
        while (*name && n < 300) buffer[n++] = *name++;
    }
    buffer[n++] = ':';
    buffer[n++] = ' ';
    while (*result && n < 380) buffer[n++] = *result++;
    buffer[n++] = ' ';
    buffer[n++] = '0';
    buffer[n++] = 'x';
    for (i = 0; i < 8; i++) buffer[n++] = hex[(value >> (28 - i * 4)) & 15];
    str.Buffer = buffer;
    str.Length = n * sizeof(WCHAR);
    str.MaximumLength = str.Length;
    NtDisplayString( &str );
}

static unsigned int wide_length( const WCHAR *text )
{
    unsigned int n = 0;
    while (text[n]) n++;
    return n;
}

static BOOL set_string( HKEY root, const WCHAR *path, const WCHAR *name, const WCHAR *value )
{
    HKEY key;
    LONG status;

    if (!(status = RegCreateKeyExW( root, path, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL )))
    {
        status = RegSetValueExW( key, name, 0, REG_SZ, (const BYTE *)value,
                                 (wide_length( value ) + 1) * sizeof(WCHAR) );
        RegCloseKey( key );
    }
    report( "set", name, status ? "failed, error" : "ok", (DWORD)status );
    return !status;
}

/* What regsvr32 does for a DLL: load it and call its DllRegisterServer. One
 * that is not on the card is left out and does not count as a failure. */
static BOOL register_dll( const WCHAR *name )
{
    HRESULT (WINAPI *register_server)(void);
    HMODULE module;
    HRESULT hr;

    if (!(module = LoadLibraryExW( name, NULL, 0 )))
    {
        DWORD error = GetLastError();

        report( "skip", name, error == ERROR_MOD_NOT_FOUND ? "not staged, error" : "failed to load, error", error );
        return error == ERROR_MOD_NOT_FOUND;
    }
    if (!(register_server = (void *)GetProcAddress( module, "DllRegisterServer" )))
    {
        report( "register", name, "has no DllRegisterServer, error", GetLastError() );
        FreeLibrary( module );
        return FALSE;
    }
    hr = register_server();
    report( "register", name, SUCCEEDED(hr) ? "ok, hr" : "failed, hr", (DWORD)hr );
    FreeLibrary( module );
    return SUCCEEDED(hr);
}

void __stdcall start(void)
{
    /* devenum's filter mapper first, then what registers filters through it. */
    static const WCHAR *const dlls[] =
    {
        L"devenum.dll", L"quartz.dll", L"msdmo.dll", L"qasf.dll", L"qcap.dll", L"qedit.dll",
        L"amstream.dll", L"dsdmo.dll", L"mciqtz32.dll",
    };
    static const WCHAR drivers32[] = L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Drivers32";
    BOOL ok = TRUE;
    HRESULT hr;
    unsigned int i;

    report( "start", NULL, "version", 1 );
    ok &= set_string( HKEY_LOCAL_MACHINE, drivers32, L"msacm.l3acm", L"l3codeca.acm" );

    hr = OleInitialize( NULL );
    report( "OleInitialize", NULL, SUCCEEDED(hr) ? "ok, hr" : "failed, hr", (DWORD)hr );
    for (i = 0; i < sizeof(dlls) / sizeof(dlls[0]); i++) ok &= register_dll( dlls[i] );
    if (SUCCEEDED(hr)) OleUninitialize();

    report( ok ? "done, all steps worked" : "done, a step FAILED (see above)", NULL, "exit code", !ok );
    ExitProcess( !ok );
}
