# Cobalt64 Agent Instructions

Cobalt64's full driver targets macOS and Navi 48 (RDNA4). Start with README.md,
BUILDING.md and mesa-patches/README.txt. Linux validation of the patched RADV
backend does not establish support for the macOS kext, Metal bundle or RDNA3
display and power management.

- `src/navi48-bringup` owns the IOKit kext and N48N ABI; `tools/native/navi48metal`
  owns the Metal bundle and its Vulkan mappings. `mesa-patches` owns the ordered
  patches against the Mesa revision named in its README.
- Keep Linux builds in the ignored `build-linux/` directory and install RADV to
  its private prefix. Select that ICD and a PCI-addressed GPU for each run.
  Check the loaded libdrm paths as well as their build-time versions.
- Use the existing Vulkan image and depth workloads for real GPU readback
  validation. Do not substitute host-only checks for device execution.
- Preserve active GPU workloads and the desktop GPU. Linux validation uses the
  kernel's existing amdgpu driver; it does not require GPU detachment or MMIO.
- Record commands, source revision, GPU PCI address, loaded driver, exit status
  and measured output. Put substantive findings in `docs/field-notes/YYYY-MM-DD.md`.
- Do not add unit tests. Keep local evidence and build products out of Git.
