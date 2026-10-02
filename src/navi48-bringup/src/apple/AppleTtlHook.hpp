//
//  AppleTtlHook.hpp — give Apple's accelerator our hardware library instead of its own.
//
//  AMDRadeonX6000_AMDHardware::initializeExternalInterfaces() finds the
//  AMDRadeonX6000HWServices object among its provider's children and calls
//  hwServices->getTtl() — vtable slot 266, byte offset 0x850 — to obtain the
//  object it will drive the GPU through. Apple's getTtl returns AmdTtlServices,
//  which cannot initialise on gfx1201.
//
//  We cannot subclass HWServices in C++: Apple's vtable is full of stripped
//  local functions with no symbols to inherit from. But we do not need to. We
//  copy the live instance's vtable, replace one slot, and point that instance
//  at the copy. Apple's class, Apple's every other method, one function ours.
//
//  Nothing in Apple's binary is modified: the copy is our memory and only the
//  one instance we patched sees it.
//
#pragma once
#include <IOKit/IOService.h>

class Navi48Ttl;

namespace n48 {

struct TtlHookResult {
    bool        installed   { false };
    const char *why         { "not attempted" };  // failure reason, for the log
    void       *hwServices  { nullptr };
    void       *originalGetTtl { nullptr };
};

// Locate the HWServices instance under `pciNub` and redirect its getTtl() to
// return `ttl`. Retries for up to ~2 s because HWServices may still be matching
// when we run. Idempotent: a second call on an already-hooked instance is a no-op.
//
// Callers MUST check .installed before publishing LoadAccelerator. Letting
// Apple's accelerator start against Apple's TTL is the configuration that hangs
// the graphics node and leaves the machine at the verbose console.
TtlHookResult install_ttl_hook(IOService *pciNub, Navi48Ttl *ttl);

} // namespace n48
