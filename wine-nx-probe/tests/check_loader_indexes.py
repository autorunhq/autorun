#!/usr/bin/env python3
"""Check bootstrap module lookup and ARM64 unwinding through native DLLs."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
loader = (root / 'dlls/ntdll/loader.c').read_text()
rtl = (root / 'dlls/ntdll/rtl.c').read_text()
unwind = (root / 'dlls/ntdll/unwind.c').read_text()


def block(text, signature):
    start = text.index(signature)
    brace = text.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


fixture = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "rtlsupportapi.h"
#include "unwind.h"
#include "wine/list.h"
#undef DECLSPEC_ALLOCATE
#define DECLSPEC_ALLOCATE(name)
#define TRACE(...) ((void)0)
#define WARN(...) ((void)0)
#define FIXME(...) ((void)0)
#define ERR(...) abort()
#define __TRY if (1)
#define __EXCEPT_PAGE_FAULT else
#define __ENDTRY
#undef NtCurrentTeb
#define NtCurrentTeb() (&teb)
static TEB teb;
static PEB peb;
static PEB_LDR_DATA ldr;
static RTL_RB_TREE base_address_index_tree;
#define HASH_MAP_SIZE 32
static LIST_ENTRY hash_table[HASH_MAP_SIZE];
static RTL_CRITICAL_SECTION dynamic_unwind_section;
static _Alignas(16) BYTE images[7][4096];
static LDR_DATA_TABLE_ENTRY modules[7];
#define LdrInitializeThunk ((void*)images[0])
NTSTATUS WINAPI RtlEnterCriticalSection(RTL_CRITICAL_SECTION *section) { return 0; }
NTSTATUS WINAPI RtlLeaveCriticalSection(RTL_CRITICAL_SECTION *section) { return 0; }
NTSTATUS WINAPI NtProtectVirtualMemory(HANDLE process, void **addr, SIZE_T *size, ULONG prot, ULONG *old) {
    assert(prot == PAGE_READWRITE);
    *old = PAGE_READONLY;
    return STATUS_SUCCESS;
}
NTSTATUS WINAPI NtQueryVirtualMemory(HANDLE process, const void *address, MEMORY_INFORMATION_CLASS cls,
                                     void *data, SIZE_T size, SIZE_T *ret) {
    assert(address == LdrInitializeThunk && cls == MemoryBasicInformation);
    assert(size == sizeof(MEMORY_BASIC_INFORMATION));
    ((MEMORY_BASIC_INFORMATION*)data)->AllocationBase = images[0];
    return STATUS_SUCCESS;
}
NTSTATUS WINAPI RtlHashUnicodeString(const UNICODE_STRING *str, BOOLEAN insensitive, ULONG algorithm, ULONG *hash) {
    *hash = 0;
    for (unsigned i = 0; i < str->Length / sizeof(WCHAR); ++i) {
        WCHAR c = str->Buffer[i];
        if (insensitive && c >= 'a' && c <= 'z') c -= 'a' - 'A';
        *hash = *hash * 65599 + c;
    }
    return STATUS_SUCCESS;
}
static RUNTIME_FUNCTION *lookup_dynamic_function_table(ULONG_PTR pc, ULONG_PTR *base, ULONG *size) {
    return NULL;
}
void *WINAPI RtlLocateExtendedFeature(CONTEXT_EX *context, ULONG feature, ULONG *length) {
    abort();
}
'''

code = '\n'.join(block(rtl, signature) for signature in (
    'static RTL_BALANCED_NODE *rtl_node_parent(', 'static void rtl_set_node_parent(',
    'static void rtl_rotate(', 'static void rtl_flip_color(',
    'void WINAPI RtlRbInsertNodeEx(', 'void WINAPI RtlRbRemoveNode('))
code += '\n' + '\n'.join(block(loader, signature) for signature in (
    'static int rtl_rb_tree_put(', 'static RTL_BALANCED_NODE *rtl_rb_tree_get(',
    'static int base_address_compare(', 'static ULONG hash_basename(',
    'static int module_address_search_compare(', 'NTSTATUS WINAPI LdrFindEntryForAddress(',
    'PIMAGE_NT_HEADERS WINAPI RtlImageNtHeader(', 'PIMAGE_SECTION_HEADER WINAPI RtlImageRvaToSection(',
    'PVOID WINAPI RtlImageRvaToVa(', 'PVOID WINAPI RtlImageDirectoryEntryToData('))
code += '\n' + block(loader, 'void CDECL wine_nx_init_loader_indexes(')
code += '\n' + block(unwind, 'PRUNTIME_FUNCTION WINAPI RtlLookupFunctionTable(')
start = unwind.index('struct unwind_info_ext\n')
end = unwind.index('#ifdef __arm64ec__', start)
code += '\n' + unwind[start:end]
code += '\n' + block(unwind, 'NTSTATUS WINAPI RtlVirtualUnwind2(')
code += '\n' + block(unwind, 'PARM64_RUNTIME_FUNCTION WINAPI RtlLookupFunctionEntry(')

tests = r'''
static void prepare(unsigned i, const WCHAR *name) {
    IMAGE_DOS_HEADER *dos = (void*)images[i];
    IMAGE_NT_HEADERS64 *nt = (void*)(images[i] + 128);
    ARM64_RUNTIME_FUNCTION *fn = (void*)(images[i] + 512);
    dos->e_magic = IMAGE_DOS_SIGNATURE;
    dos->e_lfanew = 128;
    nt->Signature = IMAGE_NT_SIGNATURE;
    nt->FileHeader.Machine = IMAGE_FILE_MACHINE_ARM64;
    nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    nt->OptionalHeader.SizeOfImage = sizeof(images[i]);
    nt->OptionalHeader.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION].VirtualAddress = 512;
    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION].Size = sizeof(*fn);
    fn->BeginAddress = 0x800;
    fn->Flag = 1;
    fn->FunctionLength = 32;
    fn->FrameSize = 1;
    fn->CR = 1;
    modules[i].DllBase = images[i];
    modules[i].SizeOfImage = sizeof(images[i]);
    modules[i].BaseDllName.Buffer = (WCHAR*)name;
    while (name[modules[i].BaseDllName.Length / sizeof(WCHAR)]) modules[i].BaseDllName.Length += sizeof(WCHAR);
}
static void check_module(unsigned i) {
    LDR_DATA_TABLE_ENTRY *module;
    ULONG_PTR base = 0;
    ULONG size = 0;
    void *pc = images[i] + 0x830;
    assert(!LdrFindEntryForAddress(pc, &module) && module == &modules[i]);
    assert(!LdrFindEntryForAddress(images[i], &module) && module == &modules[i]);
    assert(!LdrFindEntryForAddress(images[i] + 4095, &module) && module == &modules[i]);
    assert(RtlLookupFunctionTable((ULONG_PTR)pc, &base, &size) == (void*)(images[i] + 512));
    assert(base == (ULONG_PTR)images[i] && size == sizeof(ARM64_RUNTIME_FUNCTION));
    assert(RtlLookupFunctionEntry((ULONG_PTR)pc, &base, NULL) == (void*)(images[i] + 512));
    assert(!RtlLookupFunctionEntry((ULONG_PTR)images[i] + 0x880, &base, NULL));
    LIST_ENTRY *head = &hash_table[hash_basename(&modules[i].BaseDllName)], *entry;
    unsigned found = 0, visited = 0;
    for (entry = head->Flink; entry != head; entry = entry->Flink) {
        assert(++visited <= 7 && entry->Flink->Blink == entry && entry->Blink->Flink == entry);
        if (entry == &modules[i].HashLinks) ++found;
    }
    assert(found == 1);
}
int main(void) {
    const WCHAR *names[] = {L"ntdll.dll", L"wow64.dll", L"libwow64fex.dll", L"wow64win.dll",
                            L"win32u.dll", L"libarm64ecfex.dll", L"later.dll"};
    RTL_RB_TREE bootstrap = {0};
    LDR_DATA_TABLE_ENTRY *module;
    ULONG_PTR base;
    teb.Peb = &peb;
    peb.LdrData = &ldr;
    InitializeListHead(&ldr.InLoadOrderModuleList);
    for (unsigned i = 0; i < 7; ++i) prepare(i, names[i]);
    for (unsigned j = 0; j < 6; ++j) {
        unsigned i = (j * 5) % 6;
        InsertTailList(&ldr.InLoadOrderModuleList, &modules[i].InLoadOrderLinks);
        assert(!rtl_rb_tree_put(&bootstrap, images[i], &modules[i].BaseAddressIndexNode, base_address_compare));
        assert(!RtlLookupFunctionEntry((ULONG_PTR)images[i] + 0x830, &base, NULL));
    }
    wine_nx_init_loader_indexes();
    for (unsigned i = 0; i < 6; ++i) check_module(i);
    assert(LdrFindEntryForAddress((void*)((ULONG_PTR)images[0] - 1), &module) == STATUS_NO_MORE_ENTRIES);
    assert(LdrFindEntryForAddress(images[6], &module) == STATUS_NO_MORE_ENTRIES);
    ULONG64 stack[8] = {0};
    ARM64_NT_CONTEXT ctx = {0};
    ctx.Pc = (ULONG_PTR)images[0] + 0x830;
    ctx.Sp = (ULONG_PTR)stack;
    for (unsigned i = 0; i < 3; ++i) {
        stack[2*i] = (ULONG_PTR)images[i+1] + 0x830;
        ARM64_RUNTIME_FUNCTION *fn = RtlLookupFunctionEntry(ctx.Pc, &base, NULL);
        void *data;
        ULONG_PTR frame;
        assert(fn);
        assert(!RtlVirtualUnwind2(UNW_FLAG_NHANDLER, base, ctx.Pc, fn, &ctx,
                                  NULL, &data, &frame, NULL, NULL, NULL, NULL, 0));
        assert(ctx.Pc == stack[2*i] && ctx.Sp == (ULONG_PTR)&stack[2*i+2]);
    }
    InsertTailList(&hash_table[hash_basename(&modules[6].BaseDllName)], &modules[6].HashLinks);
    assert(!rtl_rb_tree_put(&base_address_index_tree, images[6], &modules[6].BaseAddressIndexNode, base_address_compare));
    for (unsigned i = 0; i < 7; ++i) check_module(i);
    RtlRbRemoveNode(&base_address_index_tree, &modules[2].BaseAddressIndexNode);
    RemoveEntryList(&modules[2].HashLinks);
    assert(!RtlLookupFunctionEntry((ULONG_PTR)images[2] + 0x830, &base, NULL));
    for (unsigned i = 0; i < 7; ++i) if (i != 2) check_module(i);
    puts("Loader indexes: bootstrap adoption, names, address bounds, unwind lookup, ARM64 stack walk and DLL changes passed");
}
'''

spec = (root / 'dlls/ntdll/ntdll.spec').read_text()
assert '@ cdecl -private wine_nx_init_loader_indexes()' in spec
assert 'wine_nx_pe_hash_table' not in loader + spec
with tempfile.TemporaryDirectory(prefix='loader-indexes-') as temp:
    build = Path(temp)
    path = build / 'indexes.c'
    path.write_text(fixture + code + tests)
    subprocess.run([os.environ.get('WINE_NX_HOST_CC', '/usr/bin/clang'), '-std=gnu11', '-g', '-O2',
                    '-fms-extensions', '-fshort-wchar', '-fsanitize=address,undefined',
                    '-D__WINESRC__', '-D_WIN64', '-I' + str(root / 'include'),
                    '-I' + str(root / 'dlls/ntdll'),
                    str(path), '-o', str(build / 'indexes')], check=True)
    subprocess.run([str(build / 'indexes')], check=True, timeout=30)
