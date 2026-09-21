# Native tests — run on the laptop, no hardware.
#
#   make -C test        (or `make test` from the repo root)
#
# test_alloc links mark_core_alchemy.c — the same translation unit the
# firmware builds — so the SDRAM bump-allocator redirect is exercised for
# real, with the firmware's pool (versio_alloc.h) and the firmware's track
# capacity (MARK_SECONDS, passed down from the root Makefile). On the host
# the engine's POSIX includes are the real ones: src/stubs is deliberately
# NOT on the include path here, so the stubs are proven by the ARM build
# and the engine's behaviour by this.
#
# The clock adapter is byte-identical to smack-alchemy's, where its suite
# runs against the Smack engine; it is not duplicated here.
#
# test_extras is the preset blob's app-owned part (src/extras.h): the TRACK
# page's per-track values. It needs one SDK header, alchemy/surface/
# serializable.h, which is self-contained. ALCHEMY_DIR follows the root
# Makefile's; override it when building from a worktree, where the submodule
# directories are empty.

CC           ?= cc
CXX          ?= c++
MARK_SECONDS ?= 55
ALCHEMY_DIR  ?= ../lib/alchemy-sdk
CFLAGS        = -O2 -g -Wall -Wextra -I../src -I../src/vendor -DMARK_MAX_SECONDS=$(MARK_SECONDS)
CXXFLAGS      = -std=gnu++17 -O2 -g -Wall -Wextra -I../src -I$(ALCHEMY_DIR)/framework/include
BUILD         = ../build/test

.PHONY: test clean

test: $(BUILD)/test_alloc $(BUILD)/test_extras
	./$(BUILD)/test_alloc
	./$(BUILD)/test_extras

$(BUILD)/test_alloc: test_alloc.c ../src/versio_alloc.c ../src/mark_core_alchemy.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $^ -o $@ -lm -lpthread

$(BUILD)/test_extras: test_extras.cpp ../src/extras.h
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) test_extras.cpp -o $@

clean:
	rm -rf $(BUILD)
