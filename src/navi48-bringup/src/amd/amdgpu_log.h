//
//  amdgpu_log.h — logging for ported IP-block modules. The reference uses
//  os_log with %{public}s; in the kernel we use IOLog, so ported format strings
//  must use plain %s / %u / %llu / %llx (no %{public}, no %zu for uint64_t).
//
#pragma once
#include <IOKit/IOLib.h>

#include "n48log.h"

// Every module's log macro funnels through the driver's own ring buffer, which
// also calls IOLog. See n48log.h for why we no longer trust macOS logging to
// still hold these lines when we go looking for them.
#define AMDGPU_LOG(tag, fmt, ...) \
    ::amdgpu::n48_logf("Navi48Bringup: " tag ": " fmt "\n", ##__VA_ARGS__)

#define PSP_LOG(fmt, ...)   AMDGPU_LOG("psp",  fmt, ##__VA_ARGS__)
#define SMU_LOG(fmt, ...)   AMDGPU_LOG("smu",  fmt, ##__VA_ARGS__)
#define GMC_LOG(fmt, ...)   AMDGPU_LOG("gmc",  fmt, ##__VA_ARGS__)
#define GART_LOG(fmt, ...)  AMDGPU_LOG("gart", fmt, ##__VA_ARGS__)
#define IH_LOG(fmt, ...)    AMDGPU_LOG("ih",   fmt, ##__VA_ARGS__)
#define IMU_LOG(fmt, ...)   AMDGPU_LOG("imu",  fmt, ##__VA_ARGS__)
#define RLC_LOG(fmt, ...)   AMDGPU_LOG("rlc",  fmt, ##__VA_ARGS__)
#define CP_LOG(fmt, ...)    AMDGPU_LOG("cp",   fmt, ##__VA_ARGS__)
#define MES_LOG(fmt, ...)   AMDGPU_LOG("mes",  fmt, ##__VA_ARGS__)
#define GFX_LOG(fmt, ...)   AMDGPU_LOG("gfx",  fmt, ##__VA_ARGS__)
#define SDMA_LOG(fmt, ...)  AMDGPU_LOG("sdma", fmt, ##__VA_ARGS__)
#define VRAM_LOG(fmt, ...)  AMDGPU_LOG("vram", fmt, ##__VA_ARGS__)
#define INIT_LOG(fmt, ...)  AMDGPU_LOG("init", fmt, ##__VA_ARGS__)
#define UCODE_LOG(fmt, ...) AMDGPU_LOG("ucode", fmt, ##__VA_ARGS__)
#define DB_LOG(fmt, ...)    AMDGPU_LOG("doorbell", fmt, ##__VA_ARGS__)
