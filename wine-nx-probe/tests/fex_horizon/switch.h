#ifndef FEX_TEST_SWITCH_H
#define FEX_TEST_SWITCH_H
#include <stddef.h>
#include <stdint.h>
typedef uint32_t Result, Handle;
#define INVALID_HANDLE 0
#define R_FAILED(rc) ((rc) != 0)
#define R_SUCCEEDED(rc) ((rc) == 0)
#define MAKERESULT(module, description) ((module) | ((description) << 9))
enum { Module_Kernel = 1, Module_Libnx = 345, KernelError_ResourceExhausted = 103, KernelError_OutOfMemory = 104,
       LibnxError_OutOfMemory = 2, LibnxError_JitUnavailable = 38 };
enum { CodeMapOperation_MapOwner, CodeMapOperation_MapSlave, CodeMapOperation_UnmapOwner, CodeMapOperation_UnmapSlave };
enum { Perm_Rw = 3, Perm_Rx = 5 };
int envIsSyscallHinted(unsigned int call);
Result svcCreateCodeMemory(Handle *handle, void *backing, size_t size);
Result svcControlCodeMemory(Handle handle, unsigned int op, void *address, size_t size, unsigned int perm);
Result svcCloseHandle(Handle handle);
void armDCacheFlush(void *address, size_t size);
void armICacheInvalidate(void *address, size_t size);
#endif
