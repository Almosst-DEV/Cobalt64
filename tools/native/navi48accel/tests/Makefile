# host tests of the Navi48Accel aux kext: `make -C tests all` (run from tools/native/navi48accel or through `make test`).
#   all       the host test (fast) - BRING_ROOT=<bring-up tree root> compares the two Navi48MetalOps.h copies
#   plants    the planted breaks of the host test (about 1 minute)
#   gates     the planted breaks of the link gates A/B/C and of the host twin (about 5 minutes)
BRING_ROOT ?= $(HOME)/navi48-native/wt-s1
ROOT := ..
all:
	clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -I $(ROOT)/src $(ROOT)/tests/host_test.cpp -o /tmp/n48accel_host
	cd $(ROOT) && /tmp/n48accel_host . $(BRING_ROOT)
plants:
	BRING_ROOT=$(BRING_ROOT) ./plant.sh
gates:
	./gate_plants.sh
.PHONY: all plants gates
