//
//  amdgpu_sysmem.h — DMA-able system memory for the GPU (kernel IOKit).
//  Replaces the reference's IOBufferMemoryDescriptor::Create + IODMACommand
//  pairs: one physically contiguous, zeroed buffer with a CPU pointer and the
//  bus address the GPU uses (no IOMMU on this platform: bus == physical).
//
#pragma once
#include <stdint.h>
#include <IOKit/IOReturn.h>

class IOBufferMemoryDescriptor;

namespace amdgpu {

struct SysMem {
    IOBufferMemoryDescriptor *md { nullptr };
    void     *cpu  { nullptr };   // kernel virtual address
    uint64_t  bus  { 0 };         // GPU-visible address (48-bit)
    uint64_t  size { 0 };         // bytes (page-rounded)
    bool valid() const { return md != nullptr && cpu != nullptr && bus != 0; }
};

// Allocate `size` bytes, physically contiguous, aligned to `align` (>= 4096,
// power of two), zero-filled. Returns kIOReturnSuccess or an error.
kern_return_t sysmem_alloc(SysMem &m, uint64_t size, uint64_t align = 4096);
void          sysmem_free (SysMem &m);

// Make CPU writes visible to the device / device writes visible to the CPU.
// Memory is mapped uncached-coherent on x86; these are compiler+store fences.
static inline void sysmem_wmb() { __asm__ __volatile__("sfence" ::: "memory"); }
static inline void sysmem_rmb() { __asm__ __volatile__("lfence" ::: "memory"); }

} // namespace amdgpu
