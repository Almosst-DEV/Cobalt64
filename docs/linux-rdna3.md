# Linux RDNA3 backend build and run

This builds the exact Mesa base from `mesa-patches/README.txt`, applies all nine
Cobalt64 patches, and runs the repository's Vulkan workloads on Linux amdgpu.
It does not build or run the IOKit kext, Metal device bundle or WindowServer.
The Darwin backend is excluded by Mesa's platform selection on Linux.

## Dependencies

The verified host is Pop!_OS 24.04, x86_64, with an RX 7900 XTX (Navi 31).
Mesa requires Meson >= 1.4.0 and libdrm >= 2.4.133; this host has libdrm 2.4.134.
Ubuntu 24.04's original packages may be older. Verify both the development
package and the library loaded at runtime meet the requirement.

```sh
sudo apt-get install build-essential ninja-build pkg-config curl patch \
  python3-venv python3-mako python3-yaml python3-packaging flex bison \
  libdrm-dev libelf-dev libzstd-dev zlib1g-dev libexpat1-dev \
  libwayland-dev wayland-protocols libx11-xcb-dev libxcb-randr0-dev \
  libxcb-dri3-dev libxcb-present-dev libxcb-sync-dev libxcb-xfixes0-dev \
  libxshmfence-dev libvulkan-dev vulkan-tools glslang-tools
pkg-config --modversion libdrm libdrm_amdgpu
```

## Build

Run these Bash commands from the repository root, starting with a new
`build-linux` directory. Subsequent builds need only Ninja and Meson install.
The private prefix leaves the installed desktop driver in place.

```sh
set -euo pipefail
mkdir build-linux
python3 -m venv --system-site-packages build-linux/venv
build-linux/venv/bin/pip install meson==1.10.0

mesa_revision=f5cb8ee032adabef599ae892ec4e42d796b84da9
curl -fL --retry 2 \
  "https://gitlab.freedesktop.org/mesa/mesa/-/archive/$mesa_revision/mesa-$mesa_revision.tar.gz" \
  -o build-linux/mesa.tar.gz
tar -xzf build-linux/mesa.tar.gz -C build-linux
mv "build-linux/mesa-$mesa_revision" build-linux/mesa
(
  cd build-linux/mesa
  for patchfile in ../../mesa-patches/*.patch; do
    patch --batch --fuzz=0 -p1 < "$patchfile"
  done
)

build-linux/venv/bin/meson setup build-linux/radv build-linux/mesa \
  --prefix="$PWD/build-linux/install" --buildtype=release \
  -Dvulkan-drivers=amd -Dgallium-drivers=[] -Dplatforms=x11,wayland \
  -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dopengl=false \
  -Dgles1=disabled -Dgles2=disabled -Dllvm=disabled -Dbuild-tests=false
ninja -C build-linux/radv -j8
build-linux/venv/bin/meson install -C build-linux/radv

cc -O2 tools/native/navi48metal/test-vkimage.c -o build-linux/vkimage -lvulkan
cc -O2 tools/native/navi48metal/test-vkdepth.c -o build-linux/vkdepth -lvulkan -lm
```

Use `patch` for this archive nested inside the Cobalt64 worktree. `git apply`
from that subdirectory can report every path skipped while returning success.

## Select the runtime and GPU

Inspect `/dev/dri/by-path`, device permissions and active workloads first.
Choose a Navi 31 PCI address explicitly. The example selects `0000:07:00.0`;
the Mesa device-selection layer uses the trailing `!` to expose only that
device to these Vulkan 1.2 workloads.

Run in a subshell so the library selection does not affect later commands:

```sh
(
  set -euo pipefail
  export VK_DRIVER_FILES="$PWD/build-linux/install/share/vulkan/icd.d/radeon_icd.x86_64.json"
  export DRI_PRIME=pci-0000_07_00_0!
  export LD_LIBRARY_PATH=/usr/lib/x86_64-linux-gnu
  ldd build-linux/install/lib/x86_64-linux-gnu/libvulkan_radeon.so
  vulkaninfo --summary
  timeout 60s build-linux/vkimage
  timeout 60s build-linux/vkdepth
)
```

On the verified host, `/etc/ld.so.conf.d` puts AMD's libdrm 2.4.125 under
`/opt/amdgpu` ahead of the distribution's 2.4.134. Without the process-local
library path, Mesa fails with
`amdgpu_query_sw_info(amdgpu_sw_info_address_prt_wa_control_bit) failed`.
Do not remove the query or change the host's global loader configuration to
make the smoke pass. Select the runtime matching the build dependencies.

Both workloads must print the selected Navi 31 device, finish with `PASS` and
exit zero. They exercise real transfers and render passes using the Metal
bundle's texture and depth mapping headers, with GPU readback assertions.
`vkcube` additionally exercises windowed presentation and requires a display
session. The installed vkcube 1.3.275 requests Vulkan 1.0: the device-selection
layer cannot select by PCI address for this application. With `DRI_PRIME` alone,
it selected the first discrete GPU at `03:00.0`, even though vulkaninfo selected
`07:00.0`. For this host, disable that layer and select physical device index 1:

```sh
env LD_LIBRARY_PATH=/usr/lib/x86_64-linux-gnu \
  VK_DRIVER_FILES="$PWD/build-linux/install/share/vulkan/icd.d/radeon_icd.x86_64.json" \
  NODEVICE_SELECT=1 \
  timeout 30s vkcube --gpu_number 1 --c 1000 --width 640 --height 480
```

Device indices are host-dependent. Verify the process's `/proc/PID/fdinfo/*`
records name the intended `drm-pdev` and that its `drm-engine-gfx` counter grows.
On this host, device 1 used `/dev/dri/renderD129` at `0000:07:00.0`.
Wall times on a shared GPU are smoke measurements, not benchmarks.

See [the initial hardware findings](field-notes/2026-10-13.md) for the measured
result and remaining platform limits. The next build step is to package this
recipe with matching build/runtime dependency versions and a GPU smoke job.
