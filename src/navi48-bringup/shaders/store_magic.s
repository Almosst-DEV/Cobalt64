// ---------------------------------------------------------------------------
//  store_magic.s — the smallest useful gfx1201 (Navi 48 / GC 12.0.1) compute
//  shader: prove that the shader cores executed our code and wrote our data.
//
//  Build:   tools/build-shader.sh   (llvm-mc -mcpu=gfx1201, Homebrew LLVM)
//  Consumed by: src/amd/compute_test.cpp via the embedded blob
//               src/fw/fw_shader_store_magic.c (symbol fw_shader_store_magic).
//
//  CONTRACT WITH THE DISPATCH (compute_test.cpp must match this exactly)
//  --------------------------------------------------------------------
//    s[0:1]  = COMPUTE_USER_DATA_0/1 = 64-bit MC address of the result buffer
//              (=> COMPUTE_PGM_RSRC2.USER_SGPR = 2)
//    v0      = packed work-item id, X in bits [9:0]
//              (=> COMPUTE_PGM_RSRC2.TIDIG_COMP_CNT = 0, and the hardware
//                  always initialises VGPR0 for a compute wave)
//    wave32  (=> COMPUTE_DISPATCH_INITIATOR.CS_W32_EN = 1)
//    workgroup = COMPUTE_NUM_THREAD_X/Y/Z = 32/1/1  -> exactly ONE wave
//    grid      = DISPATCH_DIRECT 1,1,1              -> exactly ONE workgroup
//    no LDS (COMPUTE_PGM_RSRC2.LDS_SIZE = 0)
//    no scratch (COMPUTE_PGM_RSRC2.SCRATCH_EN = 0, COMPUTE_TMPRING_SIZE = 0)
//    3 VGPRs (v0..v2) -> 1 allocation granule of 8 -> RSRC1.VGPRS = 0
//    2 SGPRs (s0..s1) -> RSRC1.SGPRS is ignored on gfx10+ -> 0
//
//  WHAT IT WRITES
//  --------------
//    lane i (i = 0..31) stores  0xCA11ED47 + i  at result[i] (dword index).
//    So the host expects, in the first four dwords:
//        [0] 0xCA11ED47   ("called GPU")
//        [1] 0xCA11ED48
//        [2] 0xCA11ED49
//        [3] 0xCA11ED4A
//    ...and 0xCA11ED47+i for i up to 31. That single pattern proves three
//    things at once: the wave ran, all 32 lanes of a wave32 ran, and the
//    64-bit base address handed over in the user SGPRs was the right one
//    (a wrong base writes nothing where we look).
//
//  CACHE VISIBILITY (why scope:SCOPE_SYS + global_wb)
//  --------------------------------------------------
//    The host reads the result back through the MM_INDEX/MM_DATA window,
//    i.e. through the memory controller, which does NOT snoop GL2. A plain
//    device-scope store would sit in GL2 until something wrote it back.
//    GFX12 lets the shader do that itself:
//      * scope:SCOPE_SYS on each store  -> performed at system scope
//      * global_wb scope:SCOPE_SYS      -> write back the caches
//    This makes the readback correct independently of whatever GCR bits the
//    trailing PM4 RELEASE_MEM happens to set (see amdgpu_pm4.h's
//    pm4_release_mem_dw1 — its GL2_WB bit position is not yet
//    hardware-confirmed on gfx12). Belt and braces, on purpose.
// ---------------------------------------------------------------------------

	.amdgcn_target "amdgcn-amd-amdhsa--gfx1201"

	.text
	.globl store_magic
	.p2align 8
	.type store_magic,@function
store_magic:
	// v0 = packed work-item id. Y/Z are not enabled (TIDIG_COMP_CNT = 0) so
	// bits [31:10] are zero, but mask anyway — a stray upper bit would turn
	// into a multi-megabyte store offset.
	v_and_b32     v0, 0x3ff, v0              // v0 = tid_x (0..31)
	v_lshlrev_b32 v1, 2, v0                  // v1 = byte offset = tid * 4
	v_add_nc_u32  v2, 0xca11ed47, v0         // v2 = magic + tid

	// global_store_b32 <voffset>, <vdata>, <s[base:base+1]>
	// address = s[0:1] + zext(v1)
	global_store_b32 v1, v2, s[0:1] scope:SCOPE_SYS

	s_wait_storecnt 0x0                      // the store has left the wave
	global_wb scope:SCOPE_SYS                // push it out to memory
	s_wait_storecnt 0x0                      // the writeback has completed
	s_endpgm

.Lstore_magic_end:
	.size store_magic, .Lstore_magic_end-store_magic

// ---------------------------------------------------------------------------
//  HSA kernel descriptor.
//
//  Nothing at run time consumes this — the kext programs COMPUTE_PGM_RSRC1/2/3
//  itself with PM4 SET_SH_REG. It is here so that LLVM, not a human, derives
//  the RSRC encodings: tools/build-shader.sh dumps this 64-byte descriptor and
//  prints RSRC1/RSRC2/RSRC3 out of it, and those are the exact constants
//  compute_test.cpp hardcodes (kComputePgmRsrc1 / 2 / 3). If LLVM ever
//  disagrees with the C++, the build script's output says so immediately.
//
//  Kernel-descriptor layout (AMDGPUUsage.rst): bytes 44-47 COMPUTE_PGM_RSRC3,
//  48-51 COMPUTE_PGM_RSRC1, 52-55 COMPUTE_PGM_RSRC2, 56-57 properties.
// ---------------------------------------------------------------------------
	.rodata
	.p2align 6
	.amdhsa_kernel store_magic
		.amdhsa_group_segment_fixed_size 0          // no LDS
		.amdhsa_private_segment_fixed_size 0        // no scratch
		.amdhsa_kernarg_size 0
		.amdhsa_next_free_vgpr 3                    // v0..v2
		.amdhsa_next_free_sgpr 2                    // s0..s1
		.amdhsa_user_sgpr_kernarg_segment_ptr 1     // s[0:1] = the buffer address
		.amdhsa_system_sgpr_workgroup_id_x 1        // RSRC2.TGID_X_EN
		.amdhsa_system_vgpr_workitem_id 0           // RSRC2.TIDIG_COMP_CNT = 0
		.amdhsa_float_round_mode_32 0
		.amdhsa_float_round_mode_16_64 0
		.amdhsa_float_denorm_mode_32 3
		.amdhsa_float_denorm_mode_16_64 3
		.amdhsa_wavefront_size32 1
		.amdhsa_workgroup_processor_mode 1          // RSRC1.WGP_MODE = 1
		.amdhsa_memory_ordered 1                    // RSRC1.MEM_ORDERED = 1
		.amdhsa_forward_progress 0
	.end_amdhsa_kernel
