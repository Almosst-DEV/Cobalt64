//  tests/dalshim/IOKit/IOReturn.h - the REAL kIOReturn values (sys_iokit | sub_iokit_common | code), so a host build of smu_dal.cpp / smu_v14_0.cpp
//  returns exactly what the kernel build returns. Used only by tests/native_s2_dal_test.cpp (a host clang++ build); never by the kext.
#pragma once
#include <stdint.h>
typedef int kern_return_t;
typedef kern_return_t IOReturn;
#define iokit_common_err(code) ((kern_return_t)(0xe0000000u | 0x00000000u | (uint32_t)(code)))
#define KERN_SUCCESS 0
#define kIOReturnSuccess         KERN_SUCCESS
#define kIOReturnError           iokit_common_err(0x2bc)
#define kIOReturnNoMemory        iokit_common_err(0x2bd)
#define kIOReturnNoResources     iokit_common_err(0x2be)
#define kIOReturnNoDevice        iokit_common_err(0x2c0)
#define kIOReturnNotPrivileged   iokit_common_err(0x2c1)
#define kIOReturnBadArgument     iokit_common_err(0x2c2)
#define kIOReturnExclusiveAccess iokit_common_err(0x2c5)
#define kIOReturnUnsupported     iokit_common_err(0x2c7)
#define kIOReturnIOError         iokit_common_err(0x2ca)
#define kIOReturnNoSpace         iokit_common_err(0x2db)
#define kIOReturnBusy            iokit_common_err(0x2d5)
#define kIOReturnTimeout         iokit_common_err(0x2d6)
#define kIOReturnNotReady        iokit_common_err(0x2d8)
#define kIOReturnAborted         iokit_common_err(0x2eb)
#define kIOReturnNotPermitted    iokit_common_err(0x2e2)
#define kIOReturnNotFound        iokit_common_err(0x2f0)
#define kIOReturnInternalError   iokit_common_err(0x2c9)
