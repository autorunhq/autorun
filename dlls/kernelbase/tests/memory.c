/*
 * Copyright (C) 2023 Paul Gofman for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <stdarg.h>
#include <stdlib.h>

#include <ntstatus.h>
#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#include <winuser.h>
#include <winerror.h>

#include "wine/test.h"

static UINT WINAPI (WINAPI *pEnumSystemFirmwareTables)(DWORD provider, void *buffer, DWORD size);

static void test_enum_system_firmware_tables(void)
{
    UINT res;
    DWORD err;

    if (!pEnumSystemFirmwareTables)
    {
        win_skip("Function EnumSystemFirmwareTables not available, skipping.\n");
        return;
    }

    /* Applications may use e.g. 'ACPI', which we currently don't support.
     * We should at the very least return valid error codes for it.
     * Test with a definitely-invalid provider so it also fails on real Windows. */
    SetLastError(0xdeadbeef);
    res = pEnumSystemFirmwareTables(~0u, NULL, 0);
    err = GetLastError();
    ok(res == 0, "Unexpected firmware table count for invalid provider: %d\n", res);
    ok(err == ERROR_INVALID_FUNCTION, "Unexpected error for invalid provider: %ld\n", err);
}

static void test_GetOsSafeBootMode(void)
{
    BOOL (WINAPI *get_os_safe_boot_mode)(DWORD *);
    HMODULE module;
    DWORD flags = 0xdeadbeef, error;
    BOOL ret;

    module = LoadLibraryA("api-ms-win-core-sysinfo-l1-2-0.dll");
    if (!module)
    {
        win_skip("System information API set is unavailable.\n");
        return;
    }
    get_os_safe_boot_mode = (void *)GetProcAddress(module, "GetOsSafeBootMode");
    ok(!!get_os_safe_boot_mode, "GetOsSafeBootMode missing from the API set.\n");
    if (get_os_safe_boot_mode)
    {
        SetLastError(0xdeadbeef);
        ret = get_os_safe_boot_mode(&flags);
        error = GetLastError();
        ok(ret, "GetOsSafeBootMode failed, error %lu.\n", error);
        ok(error == 0xdeadbeef, "Unexpected last error %lu.\n", error);
        ok(flags != 0xdeadbeef, "Boot flags were not set.\n");
        if (!GetSystemMetrics(SM_CLEANBOOT))
            ok(!flags, "Expected normal boot, got flags %#lx.\n", flags);
    }
    FreeLibrary(module);
}

START_TEST(memory)
{
    HMODULE hmod;

    hmod = LoadLibraryA("kernelbase.dll");
    pEnumSystemFirmwareTables = (void *)GetProcAddress(hmod, "EnumSystemFirmwareTables");

    test_enum_system_firmware_tables();
    test_GetOsSafeBootMode();
}
