//
//  amdgpu_doorbell.h — BAR2 doorbell index map (port of mac-amdgpu's
//  doorbell_init in amdgpu_init.cpp, MIT, commit 3bdeed2). DoorbellState itself
//  is the verbatim struct in amdgpu_ip.h.
//
#pragma once
#include "amdgpu_regs.h"
namespace amdgpu {
// Requires dev.bar2 mapped (Navi48Bringup::mapDoorbells). Fills the RDNA4
// doorbell assignments the reference uses (gfx 0/1, sdma 0x100.., ih 6, mes 0x20, compute 0x200).
kern_return_t doorbell_init(DeviceContext &dev, DoorbellState &db);
}
