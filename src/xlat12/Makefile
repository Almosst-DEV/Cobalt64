# src/xlat12 - host build and tests for the gfx10.3 -> gfx12 translator.
#   make test        unit + golden tests (ASan/UBSan) and the bounds fuzz (ASan/UBSan)
#   make kextcheck   compile the library as x86_64 kernel code (C and C++), prove no
#                    undefined symbols and no SSE/x87 use
#   make gen         regenerate tables and golden vectors (needs the re/ clones; README)
CC      ?= clang
CXX     ?= clang++
BUILD   := build
WARN    := -Wall -Wextra -Werror -Wshadow -Wcast-align -Wstrict-prototypes -Wmissing-prototypes -Wvla
SAN     := -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
CSTD    := -std=c11
LIBSRC  := xlat12.c
HDRS    := xlat12.h xlat12_tables.h xlat12_reused.h xlat12_repack.h xlat12_provenance.h xlat12_shader_desc.h xlat12_abi_ptrs.h xlat12_readset.h xlat12_shader_ids.h xlat12_shader_ids_candidates.h xlat12_desc.h xlat12_scissor.h \
           xlat12_dtable_rows.inc
# build 0.0.513: xlat12_dtable_rows.inc is kDTableAbi's GENERATED body (tools/auto-ws/emit_ws.py --policy adopt).
# Reviewer item 6 (0.0.442, review of 0.0.441, BUILD DEPENDENCIES): every fixture test_xlat12_ib.c #includes,
# by name, so `make -n` plans a rebuild when any ONE of them changes - not just the ones this build happened to add.
TEST_IB_FIXTURES := tests/fixture_r30_blit_ib.h tests/fixture_r44_drawinj.h tests/fixture_census1_suite.h \
                    tests/fixture_arm16_ws.h ../navi48-bringup/tests/fixture_arm13_f14_f15_gpupass_viewporttondc.h \
                    tests/fixture_arm25_f20_plane.h tests/fixture_arm25_f01_fill.h \
                    ../navi48-bringup/tests/fixture_arm32_f1_f16.h tests/fixture_desc_captures.h \
                    tests/fixture_dcc_trows.h tests/fixture_compute_n.h \
                    ../navi48-bringup/src/apple/ws_resprov.h
# MERGE 0.0.489: 0.0.486's test_tdesc_clamp486 #includes the kext's ws_resprov.h (its T6 reachability chain), so an edit to
# that header must rebuild test_xlat12_ib too (the stale-binary hazard's merge notes name).

.PHONY: all test kextcheck gen clean ibtool scissorcap

all: $(BUILD)/test_xlat12 $(BUILD)/fuzz_xlat12 $(BUILD)/test_xlat12_ib $(BUILD)/test_headless $(BUILD)/test_xlat12_b7b

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/test_xlat12: tests/test_xlat12.c tests/golden_vectors.h $(LIBSRC) $(HDRS) | $(BUILD)
	$(CC) $(CSTD) -O1 -g $(WARN) $(SAN) -I. tests/test_xlat12.c $(LIBSRC) -o $@

$(BUILD)/fuzz_xlat12: tests/fuzz_xlat12.c $(LIBSRC) $(HDRS) | $(BUILD)
	$(CC) $(CSTD) -O1 -g $(WARN) $(SAN) -I. tests/fuzz_xlat12.c $(LIBSRC) -o $@

#: the IB-level driver (census / blank / translate / pad), tested against Apple's real blit IB.
$(BUILD)/test_xlat12_ib: tests/test_xlat12_ib.c $(TEST_IB_FIXTURES) xlat12_ib.c xlat12_ib.h $(LIBSRC) $(HDRS) | $(BUILD)
	$(CC) $(CSTD) -O1 -g $(WARN) $(SAN) -I. tests/test_xlat12_ib.c xlat12_ib.c $(LIBSRC) -o $@

#: the headless-pass recogniser - OFFLINE ONLY (xlat12_headless.c is not in the kext's build), its negative controls
# over captured compositor, tri and SecurityAgent IBs, and its planted-defect suite. Reviewer item 6: also depends on
# r30/r44 (xlat12_ib.c's own #includes of them - test_headless.c links xlat12_ib.c, so a change to either fixture must
# rebuild this binary too).
$(BUILD)/test_headless: tests/test_headless.c tests/fixture_wsgc1_headless.h tests/fixture_r30_blit_ib.h tests/fixture_r44_drawinj.h xlat12_headless.c xlat12_headless.h xlat12_ib.c xlat12_ib.h $(LIBSRC) $(HDRS) | $(BUILD)
	$(CC) $(CSTD) -O1 -g $(WARN) $(SAN) -I. tests/test_headless.c xlat12_headless.c xlat12_ib.c $(LIBSRC) -o $@

# build 0.0.471 item 3 (the B7b test) — a SEPARATE binary, compiled WITH -DXLAT12_TEST_EXTRA_TBL_ROW so
# xlat12_ib.c's kDTableAbi carries the one test-only class-10 row this test drives. The kext's own build never
# defines this macro (see src/navi48-bringup's Makefile), so this flag never reaches the kext.
$(BUILD)/test_xlat12_b7b: tests/test_xlat12_b7b.c xlat12_ib.c xlat12_ib.h $(LIBSRC) $(HDRS) | $(BUILD)
	$(CC) $(CSTD) -O1 -g $(WARN) $(SAN) -DXLAT12_TEST_EXTRA_TBL_ROW -I. tests/test_xlat12_b7b.c xlat12_ib.c $(LIBSRC) -o $@

#: host census + translation check over a dumped Apple PM4 stream (tri --dump *.bin).
$(BUILD)/ibtool: tests/ibtool.c xlat12_ib.c xlat12_ib.h $(LIBSRC) $(HDRS) | $(BUILD)
	$(CC) $(CSTD) -O1 -g $(WARN) -I. tests/ibtool.c xlat12_ib.c $(LIBSRC) -o $@

ibtool: $(BUILD)/ibtool

# build 0.0.499: the capture test for the gfx12 inclusive scissor rule - every draw of a run's captured
# frames through the draw policy, coverage checked against its colour target. Needs a run's capdec/ (outside the tree):
#   make scissorcap && python3 tests/scissorcap.py [--all] <run dir> ...
$(BUILD)/scissorcap: tests/scissorcap.c xlat12_ib.c xlat12_ib.h $(LIBSRC) $(HDRS) | $(BUILD)
	$(CC) $(CSTD) -O1 -g $(WARN) $(SAN) -I. tests/scissorcap.c xlat12_ib.c $(LIBSRC) -o $@

scissorcap: $(BUILD)/scissorcap

test: $(BUILD)/test_xlat12 $(BUILD)/fuzz_xlat12 $(BUILD)/test_xlat12_ib $(BUILD)/test_headless $(BUILD)/test_xlat12_b7b
	$(BUILD)/test_xlat12
	$(BUILD)/fuzz_xlat12 200000
	$(BUILD)/test_xlat12_ib 100000
	$(BUILD)/test_headless
	$(BUILD)/test_xlat12_b7b
	@# an internal design note item 2: tools/gfx-readset.py's generator + xlat12_readset.h regeneration and proof tests.
	python3 tests/test_xlat12_readset.py
	@# build 0.0.470 (an internal design note N5): every kDTableAbi row and its ABI-pointer row against
	@# the program's own ABI JSON and object (the guard on risk R7, a no-sampler row for a program that samples).
	python3 tests/test_kdtableabi_rows.py

kextcheck: | $(BUILD)
	xcrun clang -arch x86_64 -std=c11 -mkernel -ffreestanding -fno-builtin -fno-common \
	    -Wall -Wextra -Werror -O2 -c $(LIBSRC) -o $(BUILD)/xlat12_kext_c.o
	xcrun clang -arch x86_64 -std=c11 -mkernel -ffreestanding -fno-builtin -fno-common \
	    -fno-stack-protector -Wall -Wextra -Werror -O2 -c xlat12_ib.c -o $(BUILD)/xlat12_ib_kext_c.o
	xcrun clang++ -arch x86_64 -x c++ -std=c++17 -mkernel -fno-exceptions -fno-rtti \
	    -fno-builtin -fno-stack-protector -Wall -Wextra -Werror -O2 -c xlat12_ib.c -o $(BUILD)/xlat12_ib_kext_cxx.o
	xcrun clang++ -arch x86_64 -x c++ -std=c++17 -mkernel -fno-exceptions -fno-rtti \
	    -fno-builtin -Wall -Wextra -Werror -O2 -c $(LIBSRC) -o $(BUILD)/xlat12_kext_cxx.o
	@# 0.0.369 (notes 804): xlat12_headless.c is IN THE KEXT now (behind gXdHeadless, OFF by default), so it is held to
	@# exactly the same C rules as xlat12_ib.c - kernel C and C++, no undefined symbols beyond the xlat12 API, no SSE/x87.
	xcrun clang -arch x86_64 -std=c11 -mkernel -ffreestanding -fno-builtin -fno-common \
	    -fno-stack-protector -Wall -Wextra -Werror -O2 -c xlat12_headless.c -o $(BUILD)/xlat12_hl_kext_c.o
	xcrun clang++ -arch x86_64 -x c++ -std=c++17 -mkernel -fno-exceptions -fno-rtti \
	    -fno-builtin -fno-stack-protector -Wall -Wextra -Werror -O2 -c xlat12_headless.c -o $(BUILD)/xlat12_hl_kext_cxx.o
	@echo "undefined symbols (C):";   nm -u $(BUILD)/xlat12_kext_c.o;   test -z "$$(nm -u $(BUILD)/xlat12_kext_c.o)"
	@echo "undefined symbols (C++):"; nm -u $(BUILD)/xlat12_kext_cxx.o; test -z "$$(nm -u $(BUILD)/xlat12_kext_cxx.o)"
	@echo "undefined symbols (ib C, xlat12 API expected):"; nm -u $(BUILD)/xlat12_ib_kext_c.o; test -z "$$(nm -u $(BUILD)/xlat12_ib_kext_c.o | grep -v -E '(_xlat12_(translate|lookup|status_name|table_len|table_entry|repack_value)|_?_bzero)$$')"
	@echo "undefined symbols (headless C, none expected):"; nm -u $(BUILD)/xlat12_hl_kext_c.o; test -z "$$(nm -u $(BUILD)/xlat12_hl_kext_c.o)"
	@echo "undefined symbols (headless C++, none expected):"; nm -u $(BUILD)/xlat12_hl_kext_cxx.o; test -z "$$(nm -u $(BUILD)/xlat12_hl_kext_cxx.o)"
	@# 0.0.218 : count real instructions only; switch jump tables in __text (LC_DATA_IN_CODE) are data
	@n=$$(python3 tests/fpu_count.py /opt/homebrew/opt/llvm/bin/llvm-objdump $(BUILD)/xlat12_kext_c.o $(BUILD)/xlat12_kext_cxx.o $(BUILD)/xlat12_ib_kext_c.o $(BUILD)/xlat12_ib_kext_cxx.o \
	        $(BUILD)/xlat12_hl_kext_c.o $(BUILD)/xlat12_hl_kext_cxx.o); \
	    echo "SSE/x87 instructions: $$n"; test "$$n" = 0

gen:
	python3 gen/gen_tables.py
	python3 gen/gen_golden.py

clean:
	rm -rf $(BUILD)
