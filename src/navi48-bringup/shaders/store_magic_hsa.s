// ---------------------------------------------------------------------------
//  store_magic_hsa.s — the SAME kernel as store_magic.s, but produced by LLVM
//  from LLVM IR instead of hand-written, and therefore using the standard HSA
//  kernel-argument convention.
//
//  GENERATED. Source of truth: shaders/llvm/lean.ll. Rebuild with
//      llc -mtriple=amdgcn-amd-amdhsa -mcpu=gfx1201 shaders/llvm/lean.ll -o shaders/store_magic_hsa.s
//      tools/build-shader.sh store_magic_hsa
//
//  WHY IT EXISTS
//  -------------
//  This is the first half of B1: prove that LLVM can produce a correct gfx1201
//  compute kernel and that our dispatch can run it. Everything about it matches
//  the hand-written shader (USER_SGPR = 2, TIDIG_COMP_CNT = 0, wave32, no LDS,
//  no scratch) EXCEPT one thing:
//
//      hand-written:  s[0:1] = the result buffer address
//      LLVM (here):   s[0:1] = a pointer to a KERNARG BUFFER whose first
//                              8 bytes are the result buffer address
//
//  That is the standard convention every real driver implements and the only
//  one that generalises past a single argument, so the dispatch side is what
//  changes, not the compiler. See notes/B1-COMPILER-BRIDGE.md.
// ---------------------------------------------------------------------------
	.amdgcn_target "amdgcn-amd-amdhsa-unknown-gfx1201"
	.amdhsa_code_object_version 6
	.text
	.globl	store_magic                     ; -- Begin function store_magic
	.p2align	8
	.type	store_magic,@function
store_magic:                            ; @store_magic
; %bb.0:                                ; %entry
	s_load_b64 s[0:1], s[0:1], 0x0
	v_add_nc_u32_e32 v1, 0xca6aed47, v0
	v_lshlrev_b32_e32 v0, 2, v0
	s_wait_kmcnt 0x0
	global_store_b32 v0, v1, s[0:1]
	s_endpgm
.Lfunc_end0:
	.size	store_magic, .Lfunc_end0-store_magic
	.section	.rodata,"a",@progbits
	.p2align	6, 0x0
	.amdhsa_kernel store_magic
		.amdhsa_group_segment_fixed_size 0
		.amdhsa_private_segment_fixed_size 0
		.amdhsa_kernarg_size 8
		.amdhsa_user_sgpr_count 2
		.amdhsa_user_sgpr_dispatch_ptr 0
		.amdhsa_user_sgpr_queue_ptr 0
		.amdhsa_user_sgpr_kernarg_segment_ptr 1
		.amdhsa_user_sgpr_dispatch_id 0
		.amdhsa_user_sgpr_private_segment_size 0
		.amdhsa_wavefront_size32 1
		.amdhsa_uses_dynamic_stack 0
		.amdhsa_enable_private_segment 0
		.amdhsa_system_sgpr_workgroup_id_x 1
		.amdhsa_system_sgpr_workgroup_id_y 1
		.amdhsa_system_sgpr_workgroup_id_z 1
		.amdhsa_system_sgpr_workgroup_info 0
		.amdhsa_system_vgpr_workitem_id 0
		.amdhsa_next_free_vgpr 2
		.amdhsa_next_free_sgpr 2
		.amdhsa_reserve_vcc 0
		.amdhsa_float_round_mode_32 0
		.amdhsa_float_round_mode_16_64 0
		.amdhsa_float_denorm_mode_32 3
		.amdhsa_float_denorm_mode_16_64 3
		.amdhsa_fp16_overflow 0
		.amdhsa_workgroup_processor_mode 1
		.amdhsa_memory_ordered 1
		.amdhsa_forward_progress 1
		.amdhsa_inst_pref_size ((instprefsize(.Lfunc_end0-store_magic)<<4)&4080)>>4
		.amdhsa_round_robin_scheduling 0
		.amdhsa_exception_fp_ieee_invalid_op 0
		.amdhsa_exception_fp_denorm_src 0
		.amdhsa_exception_fp_ieee_div_zero 0
		.amdhsa_exception_fp_ieee_overflow 0
		.amdhsa_exception_fp_ieee_underflow 0
		.amdhsa_exception_fp_ieee_inexact 0
		.amdhsa_exception_int_div_zero 0
	.end_amdhsa_kernel
	.text
                                        ; -- End function
	.set .Lstore_magic.num_vgpr, 2
	.set .Lstore_magic.num_agpr, 0
	.set .Lstore_magic.numbered_sgpr, 2
	.set .Lstore_magic.num_named_barrier, 0
	.set .Lstore_magic.private_seg_size, 0
	.set .Lstore_magic.uses_vcc, 0
	.set .Lstore_magic.uses_flat_scratch, 0
	.set .Lstore_magic.has_dyn_sized_stack, 0
	.set .Lstore_magic.has_recursion, 0
	.set .Lstore_magic.has_indirect_call, 0
	.section	.AMDGPU.csdata,"",@progbits
; Kernel info:
; codeLenInByte = 40
; TotalNumSgprs: 2
; NumVgprs: 2
; ScratchSize: 0
; MemoryBound: 0
; FloatMode: 240
; IeeeMode: 1
; LDSByteSize: 0 bytes/workgroup (compile time only)
; SGPRBlocks: 0
; VGPRBlocks: 0
; NumSGPRsForWavesPerEU: 2
; NumVGPRsForWavesPerEU: 2
; Occupancy: 16
; WaveLimiterHint : 0
; COMPUTE_PGM_RSRC2:SCRATCH_EN: 0
; COMPUTE_PGM_RSRC2:USER_SGPR: 2
; COMPUTE_PGM_RSRC2:TRAP_HANDLER: 0
; COMPUTE_PGM_RSRC2:TGID_X_EN: 1
; COMPUTE_PGM_RSRC2:TGID_Y_EN: 1
; COMPUTE_PGM_RSRC2:TGID_Z_EN: 1
; COMPUTE_PGM_RSRC2:TIDIG_COMP_CNT: 0
	.text
	.p2alignl 7, 3214868480
	.fill 96, 4, 3214868480
	.section	.AMDGPU.gpr_maximums,"",@progbits
	.set amdgpu.max_num_vgpr, 0
	.set amdgpu.max_num_agpr, 0
	.set amdgpu.max_num_sgpr, 0
	.set amdgpu.max_num_named_barrier, 0
	.text
	.section	".note.GNU-stack","",@progbits
	.amdgpu_metadata
---
amdhsa.kernels:
  - .args:
      - .address_space:  global
        .name:           out
        .offset:         0
        .size:           8
        .value_kind:     global_buffer
    .group_segment_fixed_size: 0
    .kernarg_segment_align: 8
    .kernarg_segment_size: 8
    .max_flat_workgroup_size: 1024
    .name:           store_magic
    .private_segment_fixed_size: 0
    .sgpr_count:     2
    .sgpr_spill_count: 0
    .symbol:         store_magic.kd
    .uniform_work_group_size: 1
    .uses_dynamic_stack: false
    .vgpr_count:     2
    .vgpr_spill_count: 0
    .wavefront_size: 32
    .workgroup_processor_mode: 1
amdhsa.target:   amdgcn-amd-amdhsa-unknown-gfx1201
amdhsa.version:
  - 1
  - 2
...

	.end_amdgpu_metadata
