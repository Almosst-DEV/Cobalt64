#!/usr/bin/env python3
"""Host tests for tools/gfx-readset.py and the header it generates, src/xlat12/xlat12_readset.h
(an internal design note item 2). Each planted-break case below is written to FAIL when the check it is
attached to is disabled or weakened, so a vacuous test cannot pass silently - (prove tests
non-vacuous").
"""
import importlib.util
import re
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[3]
LLVM = Path("/opt/homebrew/opt/llvm/bin")


def _load(name, file):
    spec = importlib.util.spec_from_file_location(name, ROOT / "tools" / file)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


gr = _load("gfx_readset", "gfx-readset.py")
ids = _load("gfx_shader_ids", "gfx-shader-ids.py")


def real_code(name):
    """The SHIPPED/candidate identity's own code dwords for `name` - the same list xlat12_shader_ids.h and
    xlat12_shader_desc.h are generated from, never hand-transcribed."""
    ents = ids.build() + ids.candidate_entries()
    for e in ents:
        if name in e["name"].split("="):
            return e["stage"], list(e["code"])
    raise KeyError(name)


def assemble(lines):
    """Raw gfx1201 ISA dwords for a short instruction sequence, via clang's integrated assembler - the exact
    same toolchain tools/gfx-symeval.py's disassemble_isa() uses for the round trip, just the other direction."""
    with tempfile.TemporaryDirectory() as d:
        s, o, b = Path(d) / "a.s", Path(d) / "a.o", Path(d) / "a.bin"
        s.write_text('\t.amdgcn_target "amdgcn-amd-amdhsa-unknown-gfx1201"\n\t.text\nf:\n' +
                     "".join(f"\t{ln}\n" for ln in lines))
        r = subprocess.run([str(LLVM / "clang"), "--target=amdgcn-amd-amdhsa", "-mcpu=gfx1201", "-c",
                            str(s), "-o", str(o)], capture_output=True, text=True)
        if r.returncode:
            raise RuntimeError(r.stderr)
        subprocess.run([str(LLVM / "llvm-objcopy"), "-O", "binary", "--only-section=.text", str(o), str(b)],
                        check=True)
        data = b.read_bytes()
    return list(struct.unpack(f"<{len(data) // 4}I", data))


class GeneratedHeaderRegeneration(unittest.TestCase):
    """(a) The committed header is exactly the generated rows, and a corrupted copy is caught.
    build 0.0.513 ( ORDER (1),): xlat12_readset.h's rows are no longer tools/gfx-readset.py's output
    (the hand route, whose program list is a hand list and whose aspace is 0xff outside ABI_SOURCE): they are
    tools/auto-ws/emit_ws.py --policy adopt's, and the committed generated set is src/xlat12/xlat12_adopt_rows.json. The
    regeneration check is therefore `emit_ws.py --check <tree> --adopted` (0 DIFFER / MISSING-FROM-AUTO / ADDED); the rows
    of programs auto-ws does not build are OUTSIDE-AUTO there and stay as they are."""

    FILES = ("xlat12_shader_ids.h", "xlat12_shader_desc.h", "xlat12_readset.h", "xlat12_abi_ptrs.h", "xlat12_ib.c",
             "xlat12_dtable_rows.inc", "xlat12_adopt_rows.json")

    def _check(self, tree):
        return subprocess.run([sys.executable, str(ROOT / "tools/auto-ws/emit_ws.py"), "--check", str(tree), "--adopted"],
                              capture_output=True, text=True)

    def test_check_reproduces_committed_header(self):
        p = self._check(ROOT / "src/xlat12")
        self.assertEqual(p.returncode, 0, p.stdout[-3000:] + p.stderr)
        self.assertRegex(p.stdout, r"readset \{'SAME': \d+, 'OUTSIDE-AUTO': \d+\}")

    def test_check_refuses_a_corrupted_copy(self):
        x = ROOT / "src/xlat12"
        original = (x / "xlat12_readset.h").read_text()
        wrong = original.replace('{ "ws_B_ColorFill", 0u, 11u, 0x2b838958u, 1u,\n      { 2u, 0u, 0u, 0u, 0u }',
                                  '{ "ws_B_ColorFill", 0u, 11u, 0x2b838958u, 1u,\n      { 3u, 0u, 0u, 0u, 0u }')
        self.assertNotEqual(original, wrong, "the replace target text was not found in the committed header")
        with tempfile.TemporaryDirectory() as d:
            for f in self.FILES:
                (Path(d) / f).write_text((x / f).read_text())
            (Path(d) / "xlat12_readset.h").write_text(wrong)
            p = self._check(d)
        self.assertEqual(p.returncode, 1)
        self.assertIn("DIFFER            readset PS 11/0x2b838958", p.stdout)
        print("regeneration: committed header = the generated rows (0); corrupted slot 2->3 copy DIFFER (1)")


class T1PointerLoss(unittest.TestCase):
    """(b)(i) A program whose pointer load is removed from the tool's view must lose that pointer, and an
    unanalysable program (disassembly that does not end in s_endpgm) must be REFUSED, never silently reported
    as '0 pointers' (the fail-OPEN direction xlat12_abi_ptrs.h's banner warns about)."""

    def test_real_colorfill_has_the_slot2_pointer(self):
        stage, code = real_code("ws_B_ColorFill")
        row = gr.analyze("ws_B_ColorFill", stage, code)
        self.assertEqual([p[0] for p in row["ptrs"]], [2])
        self.assertEqual(row["proof"], 1)

    def test_removing_the_load_loses_the_pointer(self):
        stage, code = real_code("ws_B_ColorFill")
        # ColorFill's own first instruction is `s_load_b128 s[0:3], s[2:3], 0x0`, 2 dwords (addr 0x0..0x8 in the
        # disassembly); replacing exactly those 2 dwords with two independently-assembled s_nop leaves every
        # later instruction's bytes, and the trailing s_endpgm, untouched.
        nops = assemble(["s_nop 0", "s_nop 0"])
        self.assertEqual(len(nops), 2)
        mutated = nops + code[2:]
        self.assertEqual(len(mutated), len(code))
        row = gr.analyze("ws_B_ColorFill (mutated: load removed)", stage, mutated)
        self.assertEqual(row["ptrs"], [], "the load is gone; the tool must report zero pointers, not the stale one")
        self.assertEqual(row["proof"], 1, "zero loads is a true, not a silent, zero-pointer proof here")
        # The planted break this guards against: a checker that CACHES a program's pointer list by NAME instead
        # of re-deriving it from the code it was actually handed. Demonstrate the break would have passed wrongly.
        cached_by_name = {"ws_B_ColorFill": [2]}
        broken_report = cached_by_name.get("ws_B_ColorFill".split(" (mutated")[0], [])
        self.assertEqual(broken_report, [2], "sanity: the broken by-name cache still holds the stale pointer")
        self.assertNotEqual(broken_report, [p[0] for p in row["ptrs"]],
                             "a name-keyed cache would report the pointer anyway; the real tool must not")
        print("T1: real ColorFill -> [slot 2]; load-removed mutant -> [] (name-cache mutant would wrongly say [2])")

    def test_unanalysable_code_is_refused_not_zero_pointers(self):
        stage, code = real_code("ws_B_ColorFill")
        truncated = code[:-1]  # drops the trailing s_endpgm dword: no longer analysable
        with self.assertRaises(SystemExit):
            gr.analyze("ws_B_ColorFill (mutated: truncated)", stage, truncated)
        print("T1: code not ending in s_endpgm -> SystemExit (refused), never a silent 0-pointer row")


class T2Depth2Proof(unittest.TestCase):
    """(b)(ii) A program with a depth-2 load (a loaded value used as an address) must get proof bit 0."""

    def test_real_P_is_depth2_and_proof_is_0(self):
        stage, code = real_code("ws_P_TimgXh_Ialp")
        row = gr.analyze("ws_P_TimgXh_Ialp", stage, code)
        self.assertEqual(row["proof"], 0)
        data = struct.pack(f"<{len(code)}I", *code)
        import importlib.util as _u
        se = _load("gfx_symeval", "gfx-symeval.py")
        insns = se.disassemble_isa(data, "gfx1201")
        facts = se.evaluate(insns, "gfx12")
        depth2 = [l for l in facts["loads"] if l["base"].startswith("*[")]
        self.assertTrue(depth2, "P's table-indirected fetch must show up as a *[...]-based load")
        self.assertFalse(gr.load_is_depth1(depth2[0]["base"], depth2[0]["terms"]))
        print(f"T2: ws_P_TimgXh_Ialp proof=0; depth-2 load: {depth2[0]['text'].strip()} -> base {depth2[0]['base']}")

    def test_planted_break_a_substring_check_would_wrongly_say_depth1(self):
        stage, code = real_code("ws_P_TimgXh_Ialp")
        data = struct.pack(f"<{len(code)}I", *code)
        se = _load("gfx_symeval", "gfx-symeval.py")
        insns = se.disassemble_isa(data, "gfx1201")
        facts = se.evaluate(insns, "gfx12")
        depth2 = [l for l in facts["loads"] if l["base"].startswith("*[")][0]

        def broken_load_is_depth1(base, _terms):
            return "P(" in base  # the planted defect: substring match instead of gr's re.fullmatch

        self.assertTrue(broken_load_is_depth1(depth2["base"], depth2["terms"]),
                         "sanity: the broken substring check does match inside '*[P(s0:s1)+0x10]'")
        self.assertFalse(gr.load_is_depth1(depth2["base"], depth2["terms"]),
                          "the real check (full match on the base) must refuse the same load")
        print("T2: planted break (substring 'P(' in base) -> wrongly depth1==True; the real fullmatch check -> False")


class T3ConstPSZeroPointers(unittest.TestCase):
    """(b)(iii) Const_PS's zero-pointer row appears only while its no-memory-read proof holds."""

    def test_real_const_ps_is_zero_pointers_and_proven(self):
        stage, code = real_code("Const_PS_gfx1201")
        row = gr.analyze("Const_PS_gfx1201", stage, code)
        self.assertEqual(row["ptrs"], [])
        self.assertEqual(row["proof"], 1)

    def test_adding_a_load_loses_the_zero_pointer_claim(self):
        stage, code = real_code("Const_PS_gfx1201")
        load = assemble(["s_load_b128 s[0:3], s[2:3], 0x0"])
        self.assertEqual(len(load), 2)
        mutated = load + code[2:]  # same ndw; the trailing s_endpgm is untouched
        self.assertEqual(len(mutated), len(code))
        row = gr.analyze("Const_PS_gfx1201 (mutated: load added)", stage, mutated)
        self.assertEqual(len(row["ptrs"]), 1, "a real load was added; the zero-pointer row must not survive it")
        # The planted break: a checker that special-cases the NAME instead of deriving the count from the code.
        def broken_nptr(name, _code):
            return 0 if name.split(" (mutated")[0] == "Const_PS_gfx1201" else None
        self.assertEqual(broken_nptr("Const_PS_gfx1201 (mutated: load added)", mutated), 0,
                         "sanity: the name-special-cased mutant still claims 0 pointers")
        self.assertNotEqual(broken_nptr("Const_PS_gfx1201 (mutated: load added)", mutated), len(row["ptrs"]))
        print(f"T3: real Const_PS -> 0 pointers, proof 1; load-added mutant -> {len(row['ptrs'])} pointer(s) "
              "(name-special-case mutant would wrongly still say 0)")


class T4OffsetFromLoadIsNotDepth1(unittest.TestCase):
    """CONDUCTOR: the data-only clause (`load_is_depth1`'s "M[" / "<" / "?" scan of a load's OFFSET,
    as distinct from its base) was untested - a planted deletion of "M[" from the token tuple at that line made
    every existing test still pass. Two real, minimal fixtures close that gap; a third token ("?") is shown
    unreachable in `terms` by the evaluator's own code shape, not asserted by a fixture."""

    def test_offset_from_an_earlier_load_is_not_depth1(self):
        # base = P(s0:s1), unmodified; but the SOFFSET operand (s4) was itself loaded from memory one instruction
        # earlier, out of a SEPARATE incoming pointer (s2:s3). This is exactly "a load whose base is an incoming
        # P(sK:sK+1) and whose offset is a register filled by an earlier load."
        code = assemble(["s_load_b32 s4, s[2:3], 0x0", "s_wait_kmcnt 0x0",
                          "s_load_b32 s5, s[0:1], s4", "s_wait_kmcnt 0x0", "s_endpgm"])
        row = gr.analyze("fixture_offset_from_load", 0, code)
        data = struct.pack(f"<{len(code)}I", *code)
        se = _load("gfx_symeval", "gfx-symeval.py")
        facts = se.evaluate(se.disassemble_isa(data, "gfx1201"), "gfx12")
        offender = next(l for l in facts["loads"] if l["base"] == "P(s0:s1)")
        self.assertIn("M[", offender["terms"], "sanity: the offset really is a loaded-value term")
        self.assertEqual(row["proof"], 0, "an offset derived from an earlier load must not be depth-1")
        # The reviewer's exact planted break: delete "M[" from the token tuple.
        def broken_load_is_depth1(base, terms):
            return bool(re.fullmatch(r"P\(s\d+:s\d+\)", base)) and not any(t in terms for t in ("<", "?"))
        self.assertTrue(broken_load_is_depth1(offender["base"], offender["terms"]),
                         "sanity: with 'M[' removed from the token tuple the broken check wrongly passes this load")
        self.assertFalse(gr.load_is_depth1(offender["base"], offender["terms"]))
        print(f"T4: offset-from-load fixture -> proof 0 (load {offender['text'].strip()!r}, terms "
              f"{offender['terms']!r}); planted break (drop 'M[') would wrongly pass it")

    def test_offset_from_an_unmodelled_op_is_not_depth1(self):
        # base = P(s0:s1), unmodified; the offset register (s4) was written by s_min_i32, which evaluate() does not
        # model as any known expression, so it becomes an opaque "<...>" token - the same shape an unmodelled ALU
        # result anywhere upstream of an offset would produce.
        code = assemble(["s_min_i32 s4, s2, s3", "s_wait_kmcnt 0x0",
                          "s_load_b32 s5, s[0:1], s4", "s_wait_kmcnt 0x0", "s_endpgm"])
        row = gr.analyze("fixture_offset_from_opaque_op", 0, code)
        data = struct.pack(f"<{len(code)}I", *code)
        se = _load("gfx_symeval", "gfx-symeval.py")
        facts = se.evaluate(se.disassemble_isa(data, "gfx1201"), "gfx12")
        offender = next(l for l in facts["loads"] if l["base"] == "P(s0:s1)")
        self.assertIn("<", offender["terms"], "sanity: the offset really is an opaque/unmodelled-op term")
        self.assertEqual(row["proof"], 0, "an offset derived from an unmodelled op must not be depth-1")

        def broken_load_is_depth1(base, terms):
            return bool(re.fullmatch(r"P\(s\d+:s\d+\)", base)) and not any(t in terms for t in ("M[", "?"))
        self.assertTrue(broken_load_is_depth1(offender["base"], offender["terms"]),
                         "sanity: with '<' removed from the token tuple the broken check wrongly passes this load")
        self.assertFalse(gr.load_is_depth1(offender["base"], offender["terms"]))
        print(f"T4: offset-from-opaque-op fixture -> proof 0 (terms {offender['terms']!r}); "
              "planted break (drop '<') would wrongly pass it")

    def test_the_question_mark_token_is_unreachable_in_terms_by_code_reading(self):
        """Not a fixture - a documented negative result, per the reviewer's "if not, say so." `?` is ONLY ever
        produced by gfx-symeval's LOAD-BASE fallback (`base = ("ptr", "?" + ..., ...)` when a load's own base
        register pair cannot be resolved as a pointer at all). `load_is_depth1` already requires `base` to
        FULLMATCH `P(sK:sK+1)` before it ever looks at `terms` - so any load whose *base* hit the "?" fallback is
        already refused by the base check, before the terms scan runs. And gfx-symeval's `lin()`/`S()` - the only
        functions that build an OFFSET term's string - have no code path that emits "?": every branch of `S()` is
        enumerated here (c, f, in, M, and, add/mul/shl/lshr/ashr/or/xor, sel, cmp, pkf16, lo/hi, op, addc, pair,
        and the bare `repr(e)` fallback for anything else), and none contains "?". So "?" cannot currently reach
        `terms` under any input; the check is defensive against a future evaluator change, not reachable today."""
        se = _load("gfx_symeval", "gfx-symeval.py")
        src = (ROOT / "tools/gfx-symeval.py").read_text()
        s_body = src[src.index("def S(e):"):src.index("def pS(p):")]
        self.assertNotIn('"?"', s_body)
        self.assertNotIn("'?'", s_body)
        print("T4: '?' cannot appear in a load's terms today (only in an unresolved BASE, already excluded by "
              "load_is_depth1's fullmatch); no fixture reaches it - documented, not asserted by execution")


class T5ImagesInlineProof(unittest.TestCase):
    """CONDUCTOR: a second, independent generated bit, `proof_images_inline` - 1 only when every
    image op's T#/S# is provably the row's declared, unmodified inline slot; 0 for a table-sourced descriptor or
    any traced mismatch. UPDATED for XLAT12_READSET_PROOF_VERSION 2 (D4-PRIME-FIXES.md item 3(a):
    proof_depth1_data_only is NO LONGER forced to 0 by an image op alone - Tex_PS_gfx1201 has no LOAD at all (its
    one image_sample is its only memory instruction), so it now gets proof_depth1_data_only 1 too; P still gets 0,
    but via its own depth-2 LOAD (T2 below), not via "has an image op"."""

    def test_tex_ps_is_inline_and_proven(self):
        stage, code = real_code("Tex_PS_gfx1201")
        row = gr.analyze("Tex_PS_gfx1201", stage, code)
        self.assertEqual(row["proof"], 1, "PROOF_VERSION 2: zero loads and no store/atomic -> pd1 1 even with an "
                                          "inline image op (image ops are covered by proof_images_inline instead)")
        self.assertEqual((row["itex"], row["isamp"]), (0, 8))
        self.assertEqual(row["proof_img"], 1)

    def test_P_is_table_sourced_and_not_proven(self):
        stage, code = real_code("ws_P_TimgXh_Ialp")
        row = gr.analyze("ws_P_TimgXh_Ialp", stage, code)
        self.assertEqual(row["itex"], 0xFF, "P is not an INLINE row - its T# comes from the class-19 table")
        self.assertEqual(row["proof_img"], 0)

    def test_a_clobbered_T_register_loses_the_inline_proof(self):
        # Mirrors Tex_PS's real inline shape (T# s0:7, S# s8:11, xlat12_shader_desc.h) but with s0 - one register of
        # the T# range - overwritten by a load, out of a second incoming pointer (s2:s3), before the sample. This is
        # a fresh minimal program (not an edit of Tex_PS's own dwords: its real setup/interpolation instructions
        # are not safely spliceable), built to isolate exactly one thing: the T#'s first register no longer holds
        # its incoming launch value.
        ok_code = assemble(["image_sample v[0:3], [v0, v1], s[0:7], s[8:11] dmask:0xf dim:SQ_RSRC_IMG_2D",
                             "s_endpgm"])
        clobber_code = assemble(["s_load_b32 s0, s[2:3], 0x0", "s_wait_kmcnt 0x0",
                                  "image_sample v[0:3], [v0, v1], s[0:7], s[8:11] dmask:0xf dim:SQ_RSRC_IMG_2D",
                                  "s_endpgm"])
        for code, name in ((ok_code, "fixture_inline_ok"), (clobber_code, "fixture_inline_clobbered")):
            se = _load("gfx_symeval", "gfx-symeval.py")
            data = struct.pack(f"<{len(code)}I", *code)
            texts = [t.split("//")[0].strip() for _a, t, _w in se.disassemble_isa(data, "gfx1201")]
            facts = se.evaluate(se.disassemble_isa(data, "gfx1201"), "gfx12")
            proof, why = gr.program_proof_images_inline(facts, 0, 8)
            if name == "fixture_inline_ok":
                self.assertEqual(proof, 1, why)
            else:
                self.assertEqual(proof, 0, "a clobbered T# register must lose the inline proof")
                self.assertIn("s0", why)
        # Planted break: skip the register trace entirely (the defect the reviewer named).
        def broken_proof_images_inline(facts, itex, isamp):
            return (1, "skipped the trace") if facts["samples"] else (1, "no samples")
        se = _load("gfx_symeval", "gfx-symeval.py")
        data = struct.pack(f"<{len(clobber_code)}I", *clobber_code)
        facts = se.evaluate(se.disassemble_isa(data, "gfx1201"), "gfx12")
        self.assertEqual(broken_proof_images_inline(facts, 0, 8)[0], 1,
                         "sanity: skipping the trace wrongly passes the clobbered fixture")
        self.assertEqual(gr.program_proof_images_inline(facts, 0, 8)[0], 0)
        print("T5: Tex_PS -> proof_img 1; ws_P_TimgXh_Ialp -> proof_img 0; T#-clobbered fixture -> proof_img 0 "
              "(a trace-skipping break would wrongly say 1)")


class T6ConstPSUnorm16ExportShape(unittest.TestCase):
    """COVERAGE TASK S4 (an internal design note): Const_PS_unorm16_gfx1201 (Apple's
    gShaderCode_gfx10_Const_PS[patch 5@4], key 0x42b75958085306e5) must export mrt0 with write mask
    0x3, DONE, and both packed words produced by v_cvt_pk_norm_u16_f32 - the shape
    tools/air-gfx.py's _export_packed_unorm16() lowers to and tools/gfx-io-check.py's "packed ...
    colour" row grades against the marker "pk_norm_u16". This tool has no AIR path for a
    hand-written row (gfx-shader-ids.py HAND[]'s own banner: "these have neither [.registers nor
    .abi.json], because we wrote them in assembly"), so gfx-io-check.py's check() cannot run on it
    directly - Const_PS_gfx1201 never went through it either. This test replicates the one
    export-shape assertion that check() makes for a packed colour target, directly over
    tools/gfx-symeval.py's own facts (the same evaluator check() uses internally)."""

    def _export_shape(self, name):
        stage, code = real_code(name)
        data = struct.pack(f"<{len(code)}I", *code)
        insns = gr.se.disassemble_isa(data, "gfx1201")
        facts = gr.se.evaluate(insns, "gfx12")
        exports = [e for e in facts["exports"] if e["target"] == 0]
        self.assertEqual(len(exports), 1, f"{name}: expected exactly one target-0 (mrt0) export")
        e = exports[0]
        producers = [p for p, o in zip(e["producers"], e["operands"]) if o != "off"]
        return e["mask"], e["done"], producers

    def test_unorm16_export_is_pk_norm_u16_mask3_done(self):
        mask, done, producers = self._export_shape("Const_PS_unorm16_gfx1201")
        self.assertEqual(mask, 0x3, "UNORM16 colour must export write mask 0x3 (two packed words)")
        self.assertTrue(done, "the colour export must carry DONE")
        self.assertEqual(len(producers), 2, f"expected 2 live export operands, got {producers}")
        for p in producers:
            self.assertIn("pk_norm_u16", p,
                          f"export word not produced by the UNORM16 pack instruction: {p!r}")

    def test_planted_break_fp16_sibling_fails_the_unorm16_marker(self):
        """Non-vacuity : grading the OLD Const_PS_gfx1201 (fp16, v_cvt_pk_rtz_f16_f32)
        against the same UNORM16 marker must FAIL, not silently pass."""
        mask, done, producers = self._export_shape("Const_PS_gfx1201")
        self.assertEqual(mask, 0x3)
        self.assertTrue(done)
        self.assertTrue(producers, "fp16 sibling has no live export producers to compare")
        self.assertFalse(any("pk_norm_u16" in p for p in producers),
                          f"fp16 export unexpectedly matched the UNORM16 pack marker: {producers}")
        print(f"T6: Const_PS_unorm16_gfx1201 export -> mask 0x3, done, "
              f"{self._export_shape('Const_PS_unorm16_gfx1201')[2]}; "
              f"Const_PS_gfx1201 (fp16) export -> {producers} (correctly fails the UNORM16 marker)")


class T7StoreDisqualifiesProof(unittest.TestCase):
    """D4-PRIME-FIXES.md item 3(a) (XLAT12_READSET_PROOF_VERSION 2: proof_depth1_data_only now also
    requires NO disqualifying STORE. A fragment program (never CLS_RING - gfx-shader-desc.py: "a VERTEX row...")
    with a planted global_store must lose the proof."""

    def test_colorfill_mutant_with_global_store_loses_the_proof(self):
        stage, code = real_code("ws_B_ColorFill")
        self.assertEqual(gr.analyze("ws_B_ColorFill", stage, code)["proof"], 1, "sanity: the real row is proven")
        store = assemble(["global_store_b32 v[0:1], v2, off"])
        mutated = store + code   # prepended: the trailing s_endpgm and every real instruction are untouched
        row = gr.analyze("ws_B_ColorFill (mutated: global_store)", stage, mutated)
        self.assertEqual(row["proof"], 0, "a planted global_store must disqualify proof_depth1_data_only")
        self.assertIn("store", row["proof_why"])
        # Planted break (the exact defect this test guards): a checker that only scans facts["loads"] for depth-1-
        # ness and never looks at instruction text for a store at all - exactly STORE_RE's absence.
        def broken_program_proof_ignores_stores(texts, facts, _cls):
            for l in facts["loads"]:
                if not gr.load_is_depth1(l["base"], l["terms"]):
                    return 0
            return 1
        se = _load("gfx_symeval", "gfx-symeval.py")
        data = struct.pack(f"<{len(mutated)}I", *mutated)
        insns = se.disassemble_isa(data, "gfx1201")
        texts = [t.split("//")[0].strip() for _a, t, _w in insns]
        facts = se.evaluate(insns, "gfx12")
        cls, _ni, _nb, _nr, _why = gr.desc.classify(texts, stage)
        self.assertEqual(broken_program_proof_ignores_stores(texts, facts, cls), 1,
                         "sanity: a loads-only checker wrongly still says 1 with the store present")
        self.assertEqual(gr.program_proof(texts, facts, cls)[0], 0)
        print(f"T7: real ColorFill -> proof 1; global_store-mutant -> proof 0 ({row['proof_why']}); "
              "a loads-only checker (no store scan) would wrongly still say 1")

    def test_the_proven_ring_store_is_not_a_false_disqualifier(self):
        """Non-vacuity the OTHER direction: STORE_RE's CLS_RING carve-out must not be so broad it lets a real
        client store through. ws_G_VfxXh's own buffer_store (the attribute-ring write every vertex TARGET here
        performs) is CLS_RING and must NOT disqualify it - if it did, no vertex row in TARGETS could ever reach
        proof 1, contradicting the real, committed header this test file's own GeneratedHeaderRegeneration class
        checks byte for byte."""
        stage, code = real_code("ws_G_VfxXh")
        row = gr.analyze("ws_G_VfxXh", stage, code)
        self.assertEqual(row["proof"], 1, "the proven attribute-ring buffer_store must not disqualify pd1")
        se = _load("gfx_symeval", "gfx-symeval.py")
        data = struct.pack(f"<{len(code)}I", *code)
        texts = [t.split("//")[0].strip() for _a, t, _w in se.disassemble_isa(data, "gfx1201")]
        self.assertTrue(any(gr.STORE_RE.search(t.split(None, 1)[0]) for t in texts),
                        "sanity: ws_G_VfxXh really does contain a store matching STORE_RE")
        print(f"T7: ws_G_VfxXh (a real buffer_store to the attribute ring) -> proof {row['proof']} "
              "(the CLS_RING carve-out correctly lets its OWN write through)")


class T8VertexSlotOffset(unittest.TestCase):
    """D4-8 (CONFIRMED, an internal design note): a vertex row's EMITTED slot is the raw SGPR minus 8
    (vertex user data starts at s8 in these programs) - xlat12_ib.c indexes the vertex stage's own 0-based
    user-data array with this value. UberCompositeVertex's ABI (src/xlat12/xlat12_abi_ptrs.h) independently
    declares user-data slots 4 and 6 for the SAME identity, so this is a real, cross-checked expectation."""

    def test_ubercompositevertex_slots_are_4_and_6(self):
        stage, code = real_code("UberCompositeVertex")
        self.assertEqual(stage, 1, "sanity: this is a vertex-stage identity")
        row = gr.analyze("UberCompositeVertex", stage, code)
        raw_slots = [p[0] for p in row["ptrs"]]
        self.assertEqual(raw_slots, [12, 14], "sanity: the code's OWN raw SGPRs are s12 and s14")
        emitted = sorted(s - 8 for s in raw_slots)
        self.assertEqual(emitted, [4, 6], "D4-8: the emitted slot is raw SGPR minus 8 for a vertex row")
        # Planted break: D4-8's own defect - forgetting the -8 and emitting the raw SGPR for a vertex row.
        broken_emitted = sorted(raw_slots)
        self.assertEqual(broken_emitted, [12, 14], "sanity: the pre-D4-8 defect wrongly emits the raw SGPR")
        self.assertNotEqual(broken_emitted, emitted)
        print(f"T8: UberCompositeVertex raw SGPRs {raw_slots} -> emitted slots {emitted} (D4-8's -8); "
              f"the pre-fix defect would wrongly emit {broken_emitted}")

    def test_committed_header_emits_4_and_6_not_the_raw_sgpr(self):
        hdr = (ROOT / "src/xlat12/xlat12_readset.h").read_text()
        i = hdr.index('{ "UberCompositeVertex"')
        row_text = hdr[i:hdr.index("},", i) + 2]
        self.assertIn("{ 4u, 6u, 0u, 0u, 0u }", row_text)   # 0.0.504: XLAT12_READSET_PTR_MAX 5
        self.assertNotIn("{ 12u, 14u", row_text)


class T9AbiPtrsCrossCheck(unittest.TestCase):
    """D4-PRIME-FIXES.md item 3(b), CONDUCTOR RULING, ws_P_TimgXh_Ialp): the cross-check is SUBSET, not
    equality - every slot xlat12_readset.h would emit must be ONE OF the pointers xlat12_abi_ptrs.h declares for
    that identity; a declared-but-UNUSED pointer (P declares s0/s4/s6 but its code only ever loads through s0) is
    harmless and must NOT be refused - equality would have wrongly refused P for listing FEWER slots than
    declared, exactly the fail-open direction xlat12_abi_ptrs.h's own banner warns against. What remains a refusal
    is the other direction: this tool finding a pointer the ABI never declared at all. gr.check_abi_agreement() is
    that assertion, called from build_rows() for every TARGETS row. Tested directly here (not by corrupting the
    committed header) so the test does not depend on which identities happen to be in TARGETS today."""

    def test_real_h_and_ubercompositevertex_rows_agree(self):
        abi_rows = gr.load_abi_ptrs_rows()
        for name in ("ws_H_UberCompositeVertex", "UberCompositeVertex"):
            stage, code = real_code(name)
            row = gr.analyze(name, stage, code)
            emitted = sorted(p[0] - 8 for p in row["ptrs"])
            gr.check_abi_agreement(name, row["ndw"], row["fnv"], stage, emitted, abi_rows)  # must not raise

    def test_p_passes_with_fewer_slots_than_declared(self):
        """The exact case the reviewer's ruling names: xlat12_abi_ptrs.h's ws_P_TimgXh_Ialp row declares THREE
        pointers ({0, 4, 6}, transcribed from the ABI JSON's three `ptr addrspace` roles), but P's own compiled
        code only ever dereferences slot 0 (s4:s5/s6:s7 never appear in the instructions at all - the reviewer's
        own reason for adding all three). The real row's emitted {0} must pass, not be refused."""
        abi_rows = gr.load_abi_ptrs_rows()
        stage, code = real_code("ws_P_TimgXh_Ialp")
        row = gr.analyze("ws_P_TimgXh_Ialp", stage, code)
        emitted = sorted(p[0] for p in row["ptrs"])   # fragment row: no -8 offset
        self.assertEqual(emitted, [0], "sanity: P's own code really does dereference only slot 0")
        self.assertEqual(abi_rows[(row["ndw"], row["fnv"], stage)], [0, 4, 6],
                         "sanity: xlat12_abi_ptrs.h's own row really does declare three slots")
        gr.check_abi_agreement("ws_P_TimgXh_Ialp", row["ndw"], row["fnv"], stage, emitted, abi_rows)  # must not raise
        print(f"T9: ws_P_TimgXh_Ialp emits {emitted}, xlat12_abi_ptrs.h declares [0, 4, 6] -> SUBSET, not refused")

    def test_planted_undeclared_slot_is_refused(self):
        """The reviewer's own planted-break example: a read-set slot the ABI never declared at all (e.g. [2]
        against P's real declared [0, 4, 6]) must refuse."""
        abi_rows = gr.load_abi_ptrs_rows()
        stage, code = real_code("ws_P_TimgXh_Ialp")
        row = gr.analyze("ws_P_TimgXh_Ialp", stage, code)
        wrong = [2]
        self.assertNotIn(2, abi_rows[(row["ndw"], row["fnv"], stage)], "sanity: slot 2 really is undeclared")
        with self.assertRaises(SystemExit):
            gr.check_abi_agreement("ws_P_TimgXh_Ialp", row["ndw"], row["fnv"], stage, wrong, abi_rows)
        # Planted break 1 (reviewer-named): equality instead of subset - would ALSO wrongly refuse the REAL row
        # (P's real {0} is a subset of, but not equal to, the declared {0, 4, 6}).
        def broken_equality_check(name, ndw, fnv, stg, emitted_slots, rows):
            key = (ndw, fnv, stg)
            if key not in rows:
                return
            if sorted(emitted_slots) != rows[key]:
                raise SystemExit(f"{name}: equality cross-check FAILED")
        with self.assertRaises(SystemExit):
            broken_equality_check("ws_P_TimgXh_Ialp", row["ndw"], row["fnv"], stage, [0], abi_rows)
        gr.check_abi_agreement("ws_P_TimgXh_Ialp", row["ndw"], row["fnv"], stage, [0], abi_rows)  # must NOT raise
        # Planted break 2 (reviewer-named): no check at all - the undeclared slot would wrongly pass.
        def broken_no_check(*_a, **_k):
            return None
        broken_no_check("ws_P_TimgXh_Ialp", row["ndw"], row["fnv"], stage, wrong, abi_rows)
        print("T9: planted slot [2] (undeclared) against P's real [0, 4, 6] -> SystemExit; equality would ALSO "
              "wrongly refuse P's real, subset-only [0]; no check at all would wrongly let [2] through")

    def test_no_row_for_this_identity_is_not_an_error(self):
        abi_rows = gr.load_abi_ptrs_rows()
        gr.check_abi_agreement("ws_G_VfxXh", 999999, 0xdeadbeef, 1, [1, 2, 3], abi_rows)  # must not raise


class T10VOPDModelled(unittest.TestCase):
    """D4-PRIME-FIXES.md item 4: `v_dual_*` (VOPD) must be modelled in tools/gfx-symeval.py as two independent
    operations, not left opaque. A minimal fixture using the EXACT mnemonic ViewportToNDC's own compiled code uses
    (v_dual_mov_b32) demonstrates the mechanism; the second test grounds it in ViewportToNDC's REAL code."""

    def test_a_dual_mov_propagates_into_an_address(self):
        code = assemble(["v_dual_mov_b32 v0, s2 :: v_dual_mov_b32 v1, s3",
                          "global_load_b32 v2, v[0:1], off", "s_waitcnt 0", "s_endpgm"])
        row = gr.analyze("fixture_vopd_dual_mov", 0, code)
        se = _load("gfx_symeval", "gfx-symeval.py")
        data = struct.pack(f"<{len(code)}I", *code)
        facts = se.evaluate(se.disassemble_isa(data, "gfx1201"), "gfx12")
        self.assertEqual(len(facts["loads"]), 1)
        load = facts["loads"][0]
        self.assertEqual(load["base"], "P(s2:s3)", "a modelled v_dual_mov_b32 must propagate s2/s3 into v0/v1")
        self.assertEqual(row["proof"], 1, "the load traces cleanly once VOPD is modelled")
        # Planted break: the exact defect this fix closes - the mov handler's own regex, unmodified, never matches
        # a dual mnemonic (no `mnem_disp` normalization), so it would fall through to an OPAQUE "op" value instead.
        self.assertIsNone(re.match(r"^[sv]_mov_b(32|64)", "v_dual_mov_b32"),
                          "sanity: the mov handler's regex does not match the raw dual mnemonic")
        self.assertIsNotNone(re.match(r"^[sv]_mov_b(32|64)", "v_mov_b32"),
                             "sanity: it matches once 'dual_' is stripped - exactly what mnem_disp does")
        print(f"T10: v_dual_mov_b32 v0,s2 :: v_dual_mov_b32 v1,s3 -> load base {load['base']!r}, proof "
              f"{row['proof']}; the mov handler's own regex would refuse the raw dual mnemonic (the defect closed)")

    def test_real_viewporttondc_dual_ops_are_resolved_not_opaque(self):
        """ws_E_ViewportToNDC's real code (D4-PRIME-FIXES.md item 4's own example) has `v_dual_mov_b32 v1, 0 ::
        v_dual_add_nc_u32 v0, s8, v3` at 0x50, feeding a later `v_lshlrev_b64_e32` (a SEPARATE, still-unmodelled
        64-bit shift, not a VOPD form) that the two downstream global_load bases trace through. If the dual ops
        were still opaque, the unresolved term in those loads' base would name a `v_dual_*` producer; since VOPD
        is now modelled, it names `v_lshlrev_b64_e32` instead - the ONLY remaining unmodelled op in this chain."""
        stage, code = real_code("ws_E_ViewportToNDC")
        se = _load("gfx_symeval", "gfx-symeval.py")
        data = struct.pack(f"<{len(code)}I", *code)
        insns = se.disassemble_isa(data, "gfx1201")
        # gfx-symeval's own `insns` text is ONE line per disassembled address, with a dual-issue VOPD pair joined by
        # "::" ("v_dual_mov_b32 v1, 0 :: v_dual_add_nc_u32 v0, s8, v3") - split on it, exactly as evaluate() does,
        # before checking which mnemonics are present.
        halves = [h.strip() for _a, t, _w in insns for h in t.split("//")[0].split("::")]
        self.assertTrue(any(h.startswith("v_dual_mov_b32") for h in halves) and
                        any(h.startswith("v_dual_add_nc_u32") for h in halves),
                        "sanity: ViewportToNDC's real code really contains these two VOPD forms")
        facts = se.evaluate(insns, "gfx12")
        # (tools: gfx-symeval.py models the zero-extended 64-bit index shift and the carry-matched
        # add pair): the v_lshlrev_b64_e32 that used to leave these bases opaque is now resolved, so NO load base
        # may stay unresolved, and none may name a dual op.
        unresolved = [l for l in facts["loads"] if l["base"].startswith("?")]
        for l in unresolved:
            self.assertNotIn("v_dual", l["base"], "a modelled dual op must not appear as an opaque producer")
        self.assertFalse(unresolved, "ViewportToNDC's index shift is modelled now: no unresolved load base expected")
        row = gr.analyze("ws_E_ViewportToNDC", stage, code)
        self.assertEqual(row["proof"], 1, "with the shift modelled, ViewportToNDC's reads are data-only at depth 1")
        print(f"T10: ws_E_ViewportToNDC's real v_dual_mov_b32/v_dual_add_nc_u32 and its 64-bit index shift resolve; "
              f"no unresolved load base; proof {row['proof']}")


class T11ProgramJ(unittest.TestCase):
    """build 0.0.504: J (ws_J_VfxXghb, VERTEX, 291/0xfa0982ef) - the one row that needs
    XLAT12_READSET_PTR_MAX 5. Its read-set is proven (pd1 1) with FIVE pointers, emitted slots 4/6/8/10/12 (raw
    s12..s20 minus 8), EQUAL to xlat12_abi_ptrs.h's row (every declared buffer is dereferenced), and the committed
    header carries exactly that row. A planted break dropping the fifth pointer (s14:s15, the d16 reads) must be
    refused by the cross-check's own consumer: here, the kext-side admission needs pd1 1 AND all five slots."""

    def test_j_row_generated_and_committed(self):
        stage, code = real_code("ws_J_VfxXghb")
        self.assertEqual(stage, 1)
        row = gr.analyze("ws_J_VfxXghb", stage, code)
        self.assertEqual((row["ndw"], row["fnv"]), (291, 0xfa0982ef))
        self.assertEqual(row["proof"], 1, row["proof_why"])
        self.assertEqual(row["proof_img"], 1)
        emitted = sorted(p[0] - 8 for p in row["ptrs"])
        self.assertEqual(emitted, [4, 6, 8, 10, 12])
        abi_rows = gr.load_abi_ptrs_rows()
        self.assertEqual(abi_rows[(291, 0xfa0982ef, 1)], [4, 6, 8, 10, 12], "xlat12_abi_ptrs.h's J row")
        gr.check_abi_agreement("ws_J_VfxXghb", 291, 0xfa0982ef, 1, emitted, abi_rows)   # must not raise
        self.assertEqual(max(len(r["ptrs"]) for r in gr.build_rows()[0]), 5,
                         "J is the row that sets XLAT12_READSET_PTR_MAX: the max over every emitted row is 5")
        self.assertEqual(gr.XLAT12_READSET_PTR_MAX, 5)
        hdr = (ROOT / "src/xlat12/xlat12_readset.h").read_text()
        self.assertIn("#define XLAT12_READSET_PTR_MAX 5u", hdr)
        i = hdr.index('{ "ws_J_VfxXghb", 1u, 291u, 0xfa0982efu, 5u,')
        row_text = hdr[i:hdr.index("},\n    /*", i)]
        self.assertIn("{ 4u, 6u, 8u, 10u, 12u }, { 1u, 1u, 0u, 0u, 0u }", row_text)
        self.assertIn("0xffu, 0xffu, 0u, 1u, 1u", row_text)
        print("T11: J pd1 1, 5 pointers, slots [4, 6, 8, 10, 12] = xlat12_abi_ptrs.h's row; committed header agrees")

    def test_planted_fifth_pointer_dropped_refuses(self):
        """The fifth pointer (s14:s15) is read ONLY by the three global_load_d16_u8 loads. Drop them from the
        evaluator's facts (the pre-0.0.504 width_of) and the row must come out pd1 0 - never a proven 4-pointer
        row the kext would admit without s14's page."""
        stage, code = real_code("ws_J_VfxXghb")
        brk = _load("gfx_readset_brk504", "gfx-readset.py")
        orig = brk.se.width_of
        brk.se.width_of = lambda m: None if "_load_d16" in m else orig(m)
        try:
            row = brk.analyze("ws_J_VfxXghb", stage, code)
        finally:
            brk.se.width_of = orig
        self.assertEqual(sorted(p[0] for p in row["ptrs"]), [12, 16, 18, 20], "sanity: the break drops s14")
        self.assertEqual(row["proof"], 0, "a 4-pointer J row proved: the dropped pointer would be admitted (fail-open)")
        print(f"T11: planted break (s14:s15 d16 loads unmodelled) -> 4 pointers, pd1 0: {row['proof_why'][:70]}")


if __name__ == "__main__":
    unittest.main()
