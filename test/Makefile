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

CC           ?= cc
MARK_SECONDS ?= 55
CFLAGS        = -O2 -g -Wall -Wextra -I../src -I../src/vendor -DMARK_MAX_SECONDS=$(MARK_SECONDS)
BUILD         = ../build/test

.PHONY: test clean

test: $(BUILD)/test_alloc
	./$(BUILD)/test_alloc

$(BUILD)/test_alloc: test_alloc.c ../src/versio_alloc.c ../src/mark_core_alchemy.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $^ -o $@ -lm -lpthread

clean:
	rm -rf $(BUILD)
