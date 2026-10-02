#!/usr/bin/env python3
"""gen_dmub_layout.py - generate src/dcn41/dcn41_dmub_layout.h: static asserts measured from Linux's own DMUB headers.

  python3 tools/dcn41/gen_dmub_layout.py            measure, cross-check on x86_64, write the header
  python3 tools/dcn41/gen_dmub_layout.py --check    exit 1 if the checked-in header differs from a fresh generation

Steps, each compiler child under the dcn41lib guard, one at a time:
 1 compile and run a probe (host arm64) that includes Linux's dmub/inc/dmub_cmd.h and include/atomfirmware.h
   (238650ef6c7c, unmodified) and prints every size, field offset, header/GPINT/boot-status bit mask and enum value that
   src/dcn41/dcn41_dmub.h mirrors;
 2 turn those numbers into _Static_asserts against LINUX's structs and compile them with -target x86_64-apple-macos11
   -fsyntax-only, so the arm64 measurement is proven to hold for the x86_64 ABI the kext uses;
 3 write the same numbers as _Static_asserts against OUR structs (dcn41_dmub_layout.h, included by dcn41_dmub.h, so every
   build of the module - host, C++, x86_64 kernel flags - re-checks them).
Bitfield positions are measured by setting one field to all ones in a zeroed object and printing the dword.
"""
import pathlib, re, sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import dcn41lib as L  # noqa: E402

OUT = L.SRC / "dcn41_dmub_layout.h"
WORK = L.BUILD / "dmub_layout"
INC = [L.HERE / "linuxshim/include", L.DISPLAY / "dmub/inc", L.AMD / "include"]

# (ours, linux) type pairs whose sizes must match
SIZES = [
    ("struct dcn41_dmub_cmd", "union dmub_rb_cmd"),
    ("struct dcn41_dmub_encoder_stream_setup_v1_5", "struct dig_encoder_stream_setup_parameters_v1_5"),
    ("struct dcn41_dmub_encoder_stream_setup_v1_5", "union dig_encoder_control_parameters_v1_5"),
    ("struct dcn41_dmub_transmitter_control_v1_7", "struct dmub_dig_transmitter_control_data_v1_7"),
    ("struct dcn41_dmub_transmitter_control_v1_7", "union dmub_cmd_dig1_transmitter_control_data"),
    ("struct dcn41_dmub_set_pixel_clock_v1_7", "struct set_pixel_clock_parameter_v1_7"),
    ("struct dcn41_dmub_disp_power_gating_v2_1", "struct enable_disp_power_gating_parameters_v2_1"),
    ("struct dcn41_dmub_feature_caps", "struct dmub_feature_caps"),
    ("struct dcn41_dmub_feature_caps", "struct dmub_cmd_query_feature_caps_data"),
    ("struct dcn41_dmub_fw_meta_info", "struct dmub_fw_meta_info"),
]
# (ours type, our field, linux type, linux field)
OFFSETS = [
    ("struct dcn41_dmub_encoder_stream_setup_v1_5", f, "struct dig_encoder_stream_setup_parameters_v1_5", f)
    for f in ("digid", "action", "digmode", "lanenum", "pclk_10khz", "bitpercolor", "dplinkrate_270mhz", "reserved")
] + [
    ("struct dcn41_dmub_transmitter_control_v1_7", o, "struct dmub_dig_transmitter_control_data_v1_7", l)
    for o, l in (("phyid", "phyid"), ("action", "action"), ("mode_laneset", "mode_laneset"), ("lanenum", "lanenum"),
                 ("symclk", "symclk_units"), ("hpdsel", "hpdsel"), ("digfe_sel", "digfe_sel"),
                 ("connobj_id", "connobj_id"), ("hpo_instance", "HPO_instance"), ("txffe_lane_sel", "TxFFELaneSel"),
                 ("skip_phy_ssc_reduction", "skip_phy_ssc_reduction"), ("reserved2", "reserved2"),
                 ("reserved3", "reserved3"))
] + [
    ("struct dcn41_dmub_set_pixel_clock_v1_7", f, "struct set_pixel_clock_parameter_v1_7", f)
    for f in ("pixclk_100hz", "pll_id", "encoderobjid", "encoder_mode", "miscinfo", "crtc_id", "deep_color_ratio",
              "reserved1", "reserved2")
] + [
    ("struct dcn41_dmub_disp_power_gating_v2_1", f, "struct enable_disp_power_gating_parameters_v2_1", f)
    for f in ("disp_pipe_id", "enable", "padding")
] + [
    ("struct dcn41_dmub_feature_caps", f, "struct dmub_feature_caps", f)
    for f in ("psr", "fw_assisted_mclk_switch_ver", "reserved", "subvp_psr_support", "gecc_enable", "replay_supported",
              "replay_reserved", "abm_aux_backlight_support", "lsdma_support_in_dmu")
] + [
    ("struct dcn41_dmub_fw_meta_info", f, "struct dmub_fw_meta_info", f)
    for f in ("magic_value", "fw_region_size", "trace_buffer_size", "fw_version", "dal_fw", "reserved",
              "shared_state_size", "shared_state_features", "reserved2", "feature_bits")
]
# payload_bytes Linux's builders compute, as "sizeof(cmd.X) - sizeof(cmd.X.header)" or the explicit sizeof
PAYLOADS = [
    ("sizeof(struct dcn41_dmub_encoder_stream_setup_v1_5)",
     "sizeof(c.digx_encoder_control) - sizeof(c.digx_encoder_control.header)"),
    ("sizeof(struct dcn41_dmub_transmitter_control_v1_7)",
     "sizeof(c.dig1_transmitter_control) - sizeof(c.dig1_transmitter_control.header)"),
    ("sizeof(struct dcn41_dmub_set_pixel_clock_v1_7)", "sizeof(c.set_pixel_clock) - sizeof(c.set_pixel_clock.header)"),
    ("sizeof(struct dcn41_dmub_disp_power_gating_v2_1)",
     "sizeof(c.enable_disp_power_gating) - sizeof(c.enable_disp_power_gating.header)"),
    ("sizeof(struct dcn41_dmub_feature_caps)", "sizeof(struct dmub_cmd_query_feature_caps_data)"),
]
# (our macro, linux expression producing the dword with one bitfield set to all ones)
BITS = [
    ("DCN41_DMUB_HDR_TYPE_MASK", "c.cmd_common.header.type = ~0u"),
    ("DCN41_DMUB_HDR_SUB_TYPE_MASK", "c.cmd_common.header.sub_type = ~0u"),
    ("DCN41_DMUB_HDR_RET_STATUS_MASK", "c.cmd_common.header.ret_status = ~0u"),
    ("DCN41_DMUB_HDR_MULTI_CMD_PENDING_MASK", "c.cmd_common.header.multi_cmd_pending = ~0u"),
    ("DCN41_DMUB_HDR_IS_REG_BASED_MASK", "c.cmd_common.header.is_reg_based = ~0u"),
    ("DCN41_DMUB_HDR_PAYLOAD_BYTES_MASK", "c.cmd_common.header.payload_bytes = ~0u"),
    ("DCN41_DMUB_BOOT_DAL_FW", "b.bits.dal_fw = ~0u"),
    ("DCN41_DMUB_BOOT_MAILBOX_RDY", "b.bits.mailbox_rdy = ~0u"),
    ("DCN41_DMUB_BOOT_OPTIMIZED_INIT_DONE", "b.bits.optimized_init_done = ~0u"),
    ("DCN41_DMUB_BOOT_RESTORE_REQUIRED", "b.bits.restore_required = ~0u"),
    ("DCN41_DMUB_BOOT_DEFER_LOAD", "b.bits.defer_load = ~0u"),
    ("DCN41_DMUB_BOOT_FAMS_ENABLED", "b.bits.fams_enabled = ~0u"),
    ("DCN41_DMUB_BOOT_DETECTION_REQUIRED", "b.bits.detection_required = ~0u"),
    ("DCN41_DMUB_BOOT_HW_POWER_INIT_DONE", "b.bits.hw_power_init_done = ~0u"),
    ("DCN41_DMUB_BOOT_ONO_REGIONS_ENABLED", "b.bits.ono_regions_enabled = ~0u"),
    ("DCN41_DMUB_GPINT_PARAM_MASK", "g.bits.param = ~0u"),
    ("DCN41_DMUB_GPINT_COMMAND_MASK", "g.bits.command_code = ~0u"),
    ("DCN41_DMUB_GPINT_STATUS_MASK", "g.bits.status = ~0u"),
]
ENUMS = [
    ("DCN41_DMUB_CMD_SIZE", "DMUB_RB_CMD_SIZE"),
    ("DCN41_DMUB_RB_SIZE", "DMUB_RB_SIZE"),
    ("DCN41_DMUB_CMD_QUERY_FEATURE_CAPS", "DMUB_CMD__QUERY_FEATURE_CAPS"),
    ("DCN41_DMUB_CMD_VBIOS", "DMUB_CMD__VBIOS"),
    ("DCN41_DMUB_VBIOS_DIGX_ENCODER_CONTROL", "DMUB_CMD__VBIOS_DIGX_ENCODER_CONTROL"),
    ("DCN41_DMUB_VBIOS_DIG1_TRANSMITTER_CONTROL", "DMUB_CMD__VBIOS_DIG1_TRANSMITTER_CONTROL"),
    ("DCN41_DMUB_VBIOS_SET_PIXEL_CLOCK", "DMUB_CMD__VBIOS_SET_PIXEL_CLOCK"),
    ("DCN41_DMUB_VBIOS_ENABLE_DISP_POWER_GATING", "DMUB_CMD__VBIOS_ENABLE_DISP_POWER_GATING"),
    ("DCN41_DMUB_FW_META_MAGIC", "DMUB_FW_META_MAGIC"),
    ("DCN41_DMUB_GPINT_STOP_FW_RESPONSE", "DMUB_GPINT__STOP_FW_RESPONSE"),
    ("DCN41_DMUB_GPINT_GET_FW_VERSION", "DMUB_GPINT__GET_FW_VERSION"),
]


def probe_source():
    lines = ['#include <stdio.h>', '#include <string.h>', '#include "dmub_cmd.h"', "int main(void) {",
             "  union dmub_rb_cmd c; union dmub_fw_boot_status b; union dmub_gpint_data_register g;", "  uint32_t w;",
             "  (void)c; (void)b; (void)g; (void)w;"]
    for i, (ours, lin) in enumerate(SIZES):
        lines.append(f'  printf("SIZE {i} %zu\\n", sizeof({lin}));')
    for i, (ot, of, lt, lf) in enumerate(OFFSETS):
        lines.append(f'  printf("OFF {i} %zu\\n", offsetof({lt}, {lf}));')
    for i, (ours, expr) in enumerate(PAYLOADS):
        lines.append(f'  printf("PAYLOAD {i} %zu\\n", (size_t)({expr}));')
    for i, (ours, stmt) in enumerate(BITS):
        var = stmt.split(".")[0]
        lines.append(f"  memset(&{var}, 0, sizeof({var})); {stmt}; memcpy(&w, &{var}, 4);")
        lines.append(f'  printf("BITS {i} 0x%08x\\n", w);')
    for i, (ours, lin) in enumerate(ENUMS):
        lines.append(f'  printf("ENUM {i} %llu\\n", (unsigned long long)({lin}));')
    lines += ["  return 0;", "}"]
    return "\n".join(lines) + "\n"


def measure():
    L.need_linux()
    WORK.mkdir(parents=True, exist_ok=True)
    src = WORK / "probe.c"
    src.write_text(probe_source())
    exe = WORK / "probe"
    inc = [f"-I{p}" for p in INC]
    r = L.run([L.CLANG, "-std=gnu11", "-w", *inc, str(src), "-o", str(exe)], "dmub layout probe compile",
              log=WORK / "probe-compile.log")
    L.check(r, "dmub layout probe compile")
    r = L.run([str(exe)], "dmub layout probe run", log=WORK / "probe-run.log")
    L.check(r, "dmub layout probe run")
    got = {}
    for line in r.stdout.splitlines():
        kind, idx, val = line.split()
        got[(kind, int(idx))] = int(val, 0)
    return got


def x86_crosscheck(got):
    lines = ['#include "dmub_cmd.h"', "static union dmub_rb_cmd c;"]
    for i, (ours, lin) in enumerate(SIZES):
        lines.append(f'_Static_assert(sizeof({lin}) == {got[("SIZE", i)]}, "{lin}");')
    for i, (ot, of, lt, lf) in enumerate(OFFSETS):
        lines.append(f'_Static_assert(offsetof({lt}, {lf}) == {got[("OFF", i)]}, "{lt}.{lf}");')
    for i, (ours, expr) in enumerate(PAYLOADS):
        lines.append(f'_Static_assert(({expr}) == {got[("PAYLOAD", i)]}, "payload {i}");')
    for i, (ours, lin) in enumerate(ENUMS):
        lines.append(f'_Static_assert(({lin}) == {got[("ENUM", i)]}ull, "{lin}");')
    src = WORK / "x86_check.c"
    src.write_text("\n".join(lines) + "\n")
    inc = [f"-I{p}" for p in INC]
    r = L.run([L.CLANG, "-target", "x86_64-apple-macos11.0", "-std=gnu11", "-w", "-fsyntax-only", *inc, str(src)],
              "dmub layout x86_64 cross-check", log=WORK / "x86-check.log")
    L.check(r, "dmub layout x86_64 cross-check")
    return len(lines) - 2


def header(got):
    out = ["/* dcn41_dmub_layout.h - GENERATED by tools/dcn41/gen_dmub_layout.py; do not edit (regenerate, then --check).",
           " *",
           f" * Every number below was MEASURED by compiling Linux {L.LINUX_COMMIT} display/dmub/inc/dmub_cmd.h and",
           " * include/atomfirmware.h (unmodified) into a probe and printing sizeof/offsetof/bit masks/enum values, then",
           " * re-asserted against Linux's structs under -target x86_64. Here they are asserted against OUR structs, so a",
           " * layout drift in dcn41_dmub.h fails every build of the module. */",
           "#ifndef N48_DCN41_DMUB_LAYOUT_H", "#define N48_DCN41_DMUB_LAYOUT_H", "",
           "#ifdef __cplusplus", "#define DCN41_LAYOUT_ASSERT(c, m) static_assert(c, m)", "#else",
           "#define DCN41_LAYOUT_ASSERT(c, m) _Static_assert(c, m)", "#endif", ""]
    for i, (ours, lin) in enumerate(SIZES):
        out.append(f'DCN41_LAYOUT_ASSERT(sizeof({ours}) == {got[("SIZE", i)]}u, "sizeof({lin}) == {got[("SIZE", i)]}");')
    out.append("")
    for i, (ot, of, lt, lf) in enumerate(OFFSETS):
        out.append(f'DCN41_LAYOUT_ASSERT(offsetof({ot}, {of}) == {got[("OFF", i)]}u, '
                   f'"offsetof({lt}, {lf}) == {got[("OFF", i)]}");')
    out.append("")
    for i, (ours, expr) in enumerate(PAYLOADS):
        out.append(f'DCN41_LAYOUT_ASSERT({ours} == {got[("PAYLOAD", i)]}u, "Linux payload_bytes: {expr}");')
    out.append("")
    for i, (ours, stmt) in enumerate(BITS):
        out.append(f'DCN41_LAYOUT_ASSERT({ours.split(" ")[0]} == 0x{got[("BITS", i)]:08x}u, "{stmt} sets 0x{got[("BITS", i)]:08x}");')
    out.append("")
    for i, (ours, lin) in enumerate(ENUMS):
        out.append(f'DCN41_LAYOUT_ASSERT({ours} == {got[("ENUM", i)]}u, "{lin} == {got[("ENUM", i)]}");')
    out += ["", "#undef DCN41_LAYOUT_ASSERT", "#endif /* N48_DCN41_DMUB_LAYOUT_H */", ""]
    return "\n".join(out)


def main():
    got = measure()
    n = x86_crosscheck(got)
    text = header(got)
    if "--check" in sys.argv:
        cur = OUT.read_text() if OUT.exists() else ""
        if cur != text:
            print(f"gen_dmub_layout --check: {OUT} is STALE")
            return 1
        print(f"gen_dmub_layout --check: fresh; {len(got)} measurements, {n} x86_64 Linux-side asserts hold")
        return 0
    OUT.write_text(text)
    print(f"gen_dmub_layout: {len(got)} measurements; {n} asserts hold for Linux's structs on x86_64; wrote {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
