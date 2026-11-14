#ifndef CARAFE_HID_KERNEL_H
#define CARAFE_HID_KERNEL_H

#include <stdarg.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "ddk/wdm.h"

PVOID WINAPI ExAllocatePool(POOL_TYPE type, SIZE_T size);
void WINAPI ExFreePool(PVOID pointer);

#endif
