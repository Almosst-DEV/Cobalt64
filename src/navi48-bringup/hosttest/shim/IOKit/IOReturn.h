//
//  hosttest/shim/IOKit/IOReturn.h — minimal host-userspace stand-in for the
//  kernel KPI header <IOKit/IOReturn.h> pulled in (transitively) by
//  src/amd/amdgpu_regs.h and src/amd/amdgpu_sysmem.h.
//
//  This file is ONLY used for the host-side (macOS userspace, arm64/x86_64,
//  plain clang++) unit test of amdgpu_ucode_extract() under hosttest/. It is
//  never linked into the real Navi48Bringup.kext build (that build uses the
//  real MacKernelSDK headers via -I$(MKSDK)/Headers, not this directory).
//
//  Exact numeric values of the kIOReturn* constants do NOT matter for this
//  test — amdgpu_ucode_extract.cpp never returns or compares against them
//  directly (it's a pure header-parsing/table-building module). What matters
//  is that every symbol the include chain references resolves to something,
//  and that the values are mutually distinct so nothing accidentally aliases.
//
#pragma once

typedef int kern_return_t;

enum {
    kIOReturnSuccess     = 0,
    kIOReturnError        = 0x2bc,
    kIOReturnNoMemory,
    kIOReturnBadArgument,
    kIOReturnUnsupported,
    kIOReturnTimeout,
    kIOReturnNotReady,
    kIOReturnIOError,
    kIOReturnNotFound,
};
