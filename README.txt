Navi48-MacOS
The first public unofficial MacOS GPU driver.

What this repo contains
-----------------------
  src/navi48-bringup   Bring-up kext (IOKit): PCIe/BAR setup, IP discovery, PSP/SMU/GMC/
                       GFX/SDMA/MES initialisation, display (DCN 4.1) mode-setting, a
                       user client ("N48N") exposing buffers, command submission and
                       scanout, and hooks into Apple's AMDRadeonX6000 accelerator.
  src/dcn41            DCN 4.1 display-engine helpers and generated register headers
                       (derived from Linux amdgpu, MIT).
  src/xlat12           Command-stream translation from Apple's GFX10.3 PM4/register
                       programming to gfx12.
  src/g2capture        Small capture tool for Apple shader-compiler records.
  tools/native         navi48metal (a Metal device bundle that runs Metal on top of the
                       Vulkan driver), mtlprobe (API probe), autotranslate (AIR -> SPIR-V
                       translation scripts) and navi48accel (aux kext).
  tools/pc, tools/dcn41, tools/conductor
                       PC-side test CLI and run scripts, display-register tooling, test
                       suite runners and "planted break" mutation tests.
  mesa-patches         RADV (Mesa Vulkan) Darwin port as nine patches on a pinned Mesa
                       commit; see mesa-patches/README.txt.

Status
------
Reached on one x86_64 test PC with an RX 9070 XT: a 60 fps
GPU-composited macOS desktop on one DisplayPort display, macOS Tahoe 26.6.2, x86_64
PC booted through OpenCore. The Metal compositor runs on this driver stack. A lot is
still rough. Only one display path has been exercised at length, performance and
stability work is ongoing, and many pieces exist to work around one specific macOS
build.

Coming Soon
-----------
Multi-monitor support
Multiple display paths
Native GPU rendering for app content

Can I install this?
-------------------
Not reliably. Wait for the first official public release
for stability and usability. This is just the code.

External projects and dependencies (not vendored; clone them yourself)
----------------------------------------------------------------------
  RDNA4FB      https://github.com/somestupidgirl/RDNA4FB   (display-only RDNA4 kext, MacKernelSDK)
  mac-amdgpu   https://github.com/lemonade-sdk/mac-amdgpu  (Navi 48 bring-up on macOS)
  USBToolBox   https://github.com/USBToolBox/tool          (USB port mapping, optional)
  metal2vulkan https://github.com/steelbrain/metal2vulkan  (Metal AIR -> SPIR-V translator,
               LGPL-3.0-or-later; upstream is not included. Our changes and the n48xlate
               in-process wrapper are in third-party/metal2vulkan-n48/, LGPL-3.0, separate
               from the MIT code here; see its NOTICE)
  Mesa         https://gitlab.freedesktop.org/mesa/mesa    (RADV, MIT; see mesa-patches/)
  linux-firmware (AMD firmware blobs, own licence; see above)

Credits
-------
RDNA4FB (Sunneva N. Mariu), mac-amdgpu (lemonade-sdk / Geramy Loveless), the Mesa
project (RADV and the register databases), the Linux amdgpu driver authors at AMD and
the community (register definitions and documentation), and the metal2vulkan author.
