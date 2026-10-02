Navi48-MacOS
============

Experimental, proof-of-concept native macOS support for the AMD Radeon RX 9070 XT
(Navi 48, gfx1201) on an x86_64 PC running macOS Tahoe. Not affiliated with AMD or
Apple. Licence: MIT (see LICENSE; third-party notices in third-party/).

What is here
------------
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
  mesa-patches         RADV (Mesa Vulkan) Darwin port as five patches on a pinned Mesa
                       commit; see mesa-patches/README.txt.

Status
------
Reached on one x86_64 test PC with an RX 9070 XT: a 60 fps
GPU-composited macOS desktop on one DisplayPort display, macOS Tahoe 26.6.2, x86_64
PC booted through OpenCore. The Metal compositor runs on this driver stack. A lot is
still rough: only one display path has been exercised at length, performance and
stability work is ongoing, and many pieces exist to work around one specific macOS
build.

Can I install this?
-------------------
Honest answer: probably not, and you should not try on a machine you care about.

  * It is experimental research code, with no installer and no support. It can panic
    the machine, hang the GPU or the display, and corrupt a boot volume if you
    misconfigure the EFI partition. Do not use it as a daily driver.
  * It is pinned to one macOS build. Large parts hook Apple's own driver at fixed
    offsets; another macOS build needs those offsets redone.
  * It needs the same GPU family (RX 9070 / 9070 XT, Navi 48, gfx1201).
  * You need an OpenCore-booted Hackintosh-style setup; this repository does not
    include any OpenCore configuration, SMBIOS identity or EFI files.
  * It builds on the RDNA4FB kext (display-only prior art) and its MacKernelSDK; clone
    it separately (below) to src/RDNA4FB - the kext Makefile expects ../RDNA4FB/MacKernelSDK.
  * Apple's kext approval flow and AMFI must be relaxed for the unsigned kexts and
    the Metal bundle to load. That is a real reduction of macOS security.
  * AMD firmware is not included. Get it yourself from linux-firmware
    (tools/fetch-firmware.sh; src/navi48-bringup/firmware/README.txt).
  * Mesa must be built with the patches in mesa-patches/ (x86_64 build for macOS).
  * Several test fixtures, translation caches and shader-compiler outputs that came
    from captures of Apple's driver are deliberately not included for copyright
    reasons, so parts of the test suites (notably src/xlat12 and parts of
    src/navi48-bringup/tests) will not build or pass here without regenerating them
    yourself on your own machine. tools/native/navi48metal/build.sh also expects an
    spvcache/ you generate with tools/native/autotranslate.
  * The scripts in tools/ assume an ssh alias "navi48" for the test PC (override with
    NAVI48_HOST) and a build host Mac.

External projects and dependencies (not vendored; clone them yourself)
----------------------------------------------------------------------
  RDNA4FB      https://github.com/somestupidgirl/RDNA4FB   (display-only RDNA4 kext, MacKernelSDK)
  mac-amdgpu   https://github.com/lemonade-sdk/mac-amdgpu  (Navi 48 bring-up on macOS)
  USBToolBox   https://github.com/USBToolBox/tool          (USB port mapping, optional)
  metal2vulkan https://github.com/steelbrain/metal2vulkan  (Metal AIR -> SPIR-V translator,
               LGPL-3.0-or-later; used as an external tool by tools/native, not included)
  Mesa         https://gitlab.freedesktop.org/mesa/mesa    (RADV, MIT; see mesa-patches/)
  linux-firmware (AMD firmware blobs, own licence; see above)

Credits
-------
RDNA4FB (Sunneva N. Mariu), mac-amdgpu (lemonade-sdk / Geramy Loveless), the Mesa
project (RADV and the register databases), the Linux amdgpu driver authors at AMD and
the community (register definitions and documentation), and the metal2vulkan author.
