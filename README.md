# Cobalt64

Native macOS GPU acceleration for AMD Radeon GPUs that Apple does not support.
(Formerly Navi48-MacOS.)

The goal is to bring a GPU-accelerated macOS desktop to unsupported AMD GPUs. The first
supported family is RDNA4: the RX 9070 XT (Navi 48), which is where all development and
testing happens today. Other families (for example RDNA3 / RX 7000) are future work; much
of the stack is shared, but each GPU generation needs its own display and power-management
bring-up, and someone with that hardware to test it.

Internal names (the navi48-bringup kext, the navi48metal bundle, file paths) still carry
the Navi48 name; they will be renamed together with a planned restructure into shared
core code and per-GPU-family code.

## What this repo contains

| Path | Contents |
|---|---|
| `src/navi48-bringup` | Bring-up kext (IOKit): PCIe/BAR setup, IP discovery, PSP/SMU/GMC/GFX/SDMA/MES initialisation, display (DCN 4.1) mode-setting, a user client ("N48N") exposing buffers, command submission and scanout, and hooks into Apple's AMDRadeonX6000 accelerator. |
| `src/dcn41` | DCN 4.1 display-engine helpers and generated register headers (derived from Linux amdgpu, MIT). |
| `src/xlat12` | Command-stream translation from Apple's GFX10.3 PM4/register programming to gfx12. |
| `src/g2capture` | Small capture tool for Apple shader-compiler records. |
| `tools/native` | navi48metal (a Metal device bundle that runs Metal on top of the Vulkan driver), mtlprobe (API probe), autotranslate (AIR -> SPIR-V translation scripts) and navi48accel (aux kext). |
| `tools/pc`, `tools/dcn41`, `tools/conductor` | PC-side test CLI and run scripts, display-register tooling, test suite runners and "planted break" mutation tests. |
| `mesa-patches` | RADV (Mesa Vulkan) Darwin port as nine patches on a pinned Mesa commit; see `mesa-patches/README.txt`. |

## Status

Reached on one x86_64 test PC with an RX 9070 XT, macOS Tahoe 26.6.2, booted through
OpenCore: a GPU-composited macOS desktop on three displays at once (one DisplayPort,
two HDMI), with the Metal compositor running on this driver stack, Resizable BAR, and
the driver arming itself at boot. A set of built-in apps (Maps, Preview, Photos,
System Settings, Music and others) render through it. This is a preview: a lot is
still rough, the display modes supported are limited to what the test PC's monitors
needed, and many pieces exist to work around one specific macOS build.

## Coming Soon

- 4K output over HDMI
- Higher refresh rates and smoother frame pacing
- Native GPU rendering for more apps (Safari, Chromium/Electron apps)
- Display support driven by each monitor's EDID instead of fixed modes

## Can I install this?

Not reliably. Wait for the first official public release
for stability and usability. This is just the code.

## External projects and dependencies (not vendored; clone them yourself)

| Project | Where | Notes |
|---|---|---|
| RDNA4FB | https://github.com/somestupidgirl/RDNA4FB | display-only RDNA4 kext, MacKernelSDK |
| mac-amdgpu | https://github.com/lemonade-sdk/mac-amdgpu | Navi 48 bring-up on macOS |
| USBToolBox | https://github.com/USBToolBox/tool | USB port mapping, optional |
| metal2vulkan | https://github.com/steelbrain/metal2vulkan | Metal AIR -> SPIR-V translator, LGPL-3.0-or-later; upstream is not included. Our changes and the n48xlate in-process wrapper are in `third-party/metal2vulkan-n48/`, LGPL-3.0, separate from the MIT code here; see its NOTICE |
| Mesa | https://gitlab.freedesktop.org/mesa/mesa | RADV, MIT; see `mesa-patches/` |
| linux-firmware | | AMD firmware blobs, own licence; see above |

## Credits

RDNA4FB (Sunneva N. Mariu), mac-amdgpu (lemonade-sdk / Geramy Loveless), the Mesa
project (RADV and the register databases), the Linux amdgpu driver authors at AMD and
the community (register definitions and documentation), and the metal2vulkan author.
