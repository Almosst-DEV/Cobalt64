#include "amdgpu_sysmem.h"
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLib.h>

namespace amdgpu {

kern_return_t sysmem_alloc(SysMem &m, uint64_t size, uint64_t align) {
    sysmem_free(m);
    if (size == 0) return kIOReturnBadArgument;
    if (align < 4096) align = 4096;
    if (align & (align - 1)) return kIOReturnBadArgument;
    // physical mask: 48-bit addresses, low bits force alignment
    const mach_vm_address_t mask = 0x0000FFFFFFFFFFFFULL & ~(align - 1);
    IOBufferMemoryDescriptor *md = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous | kIOMemoryMapperNone,
        size, mask);
    if (!md) return kIOReturnNoMemory;
    if (md->prepare() != kIOReturnSuccess) { md->release(); return kIOReturnNoMemory; }
    IOByteCount seg = 0;
    addr64_t phys = md->getPhysicalSegment(0, &seg, kIOMemoryMapperNone);
    void *cpu = md->getBytesNoCopy();
    if (!phys || !cpu || seg < size) { md->complete(); md->release(); return kIOReturnNoMemory; }
    bzero(cpu, (size_t)size);
    m.md = md; m.cpu = cpu; m.bus = phys; m.size = md->getLength();
    return kIOReturnSuccess;
}

void sysmem_free(SysMem &m) {
    if (m.md) { m.md->complete(); m.md->release(); }
    m.md = nullptr; m.cpu = nullptr; m.bus = 0; m.size = 0;
}

} // namespace amdgpu
