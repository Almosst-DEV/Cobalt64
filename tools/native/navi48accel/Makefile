# Navi48Accel: the Aux-KC accelerator kext (com.navi48.accelprobe 0.0.2) of milestone #9, route A. Same recipe as tools/native/e3auxkc and
# src/navi48-bringup/Makefile (RDNA4FB's MacKernelSDK, macOS 11 ABI, -mkernel -fapple-kext, -Xlinker -kext, ad-hoc codesign).
#   make        build/Navi48Accel.kext + gates A/B/C + the host twin of the runtime layout gate (each FAILS the make)
#   make test   host tests (tests/host_test.cpp) + planted breaks (tests/plant.sh)
#   make clean
PRODUCT := Navi48Accel
ARCH    := x86_64
DEPLOY  := 11.0
SDK     := $(shell xcrun --sdk macosx --show-sdk-path)
MKSDK   := ../../../src/RDNA4FB/MacKernelSDK
LAYOUT  := ../ioaccel-layout
BUILD   := build
KEXT    := $(BUILD)/$(PRODUCT).kext
EXEC    := $(KEXT)/Contents/MacOS/$(PRODUCT)
GEN     := $(BUILD)/gen
COMMON  := -arch $(ARCH) -target $(ARCH)-apple-macos$(DEPLOY) -isysroot $(SDK) -I$(MKSDK)/Headers -Isrc -I$(GEN) -I$(LAYOUT)/generated \
  -mmacosx-version-min=$(DEPLOY) -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT \
  -D_FORTIFY_SOURCE=0 -nostdinc -fno-builtin -fno-common -fno-stack-protector -mkernel -fapple-kext -Wall -Os -g
CXXFLAGS := $(COMMON) -std=c++17 -fno-exceptions -fno-rtti -fcheck-new -fno-c++-static-destructors
LDFLAGS := -arch $(ARCH) -target $(ARCH)-apple-macos$(DEPLOY) -isysroot $(SDK) \
  -nostdlib -Xlinker -kext -L$(MKSDK)/Library/x86_64 -lkmodc++ -lkmod -Wl,-no_deduplicate
GENHDR  := $(LAYOUT)/generated/IOAccelFamily2_decl.h

all: $(KEXT) gates

# the generated declarations must be fresh: gen_header.py output == the committed header (gate C's input)
$(GEN)/n48_gate.inc: gates/gen_gate.py gates/common.py gates/leaves.py $(LAYOUT)/generated/model.json $(LAYOUT)/generated/forwarded.json
	@mkdir -p $(GEN)
	python3 gates/gen_gate.py $@

$(GEN)/n48_tramp.inc: gates/gen_tramp.py gates/gen_gate.py gates/common.py gates/leaves.py $(LAYOUT)/generated/slots.json $(LAYOUT)/generated/forwarded.json $(LAYOUT)/gen_header.py
	@mkdir -p $(GEN)
	python3 gates/gen_tramp.py $@

$(BUILD)/accel.o: src/Navi48Accel.cpp src/n48accel_pure.h src/Navi48MetalOps.h $(GENHDR) $(GEN)/n48_gate.inc $(GEN)/n48_tramp.inc
	@mkdir -p $(BUILD)
	clang++ $(CXXFLAGS) -c src/Navi48Accel.cpp -o $@
$(BUILD)/kmod_info.o: src/kmod_info.c
	@mkdir -p $(BUILD)
	clang $(COMMON) -c src/kmod_info.c -o $@

$(KEXT): $(BUILD)/accel.o $(BUILD)/kmod_info.o Info.plist
	@mkdir -p $(KEXT)/Contents/MacOS
	clang++ $(LDFLAGS) $(BUILD)/accel.o $(BUILD)/kmod_info.o -o $(EXEC)
	@# our symbols must all be defined (a virtual declared and never defined links here and then silently fails in the kernel linker)
	@bad=$$(nm -u -arch $(ARCH) $(EXEC) | c++filt | grep -E 'Navi48|n48' || true); \
	 if [ -n "$$bad" ]; then echo "LINK ERROR: our symbols undefined: $$bad"; rm -f $(EXEC); exit 1; fi
	cp Info.plist $(KEXT)/Contents/Info.plist
	-xattr -d com.apple.FinderInfo $(KEXT) 2>/dev/null
	xattr -cr $(KEXT)
	codesign --force --sign - $(KEXT) || { xattr -cr $(KEXT); codesign --force --sign - $(KEXT); }
	@echo "Built $(KEXT)"

# gates A and B on the LINKED kext, gate C = the static_asserts already compiled into the object (sizes, slot arithmetic) + the header freshness check
gates: $(KEXT)
	python3 gates/gate_link.py $(EXEC) $(BUILD)/accel.o
	python3 gates/layout_gate_host.py $(EXEC)

test:
	$(MAKE) -C tests all

clean:
	rm -rf $(BUILD)
.PHONY: all gates test clean
