# `make test` builds each PC test suite in stm32/BoxCore/Tests (one binary
# per test_*.c, linked with every Box logic core source) and in
# esp32/lib/bridge_link/tests (linked with the ESP32's link module, into
# build/esp32/) with gcc, then runs them all. CI runs the same command
# (see .github/workflows/ci.yml).
# The firmware itself is built separately with `make -C stm32` and
# `pio run -d esp32`.

CC      := gcc
CFLAGS  := -std=c99 -Wall -Wextra -Werror -g
CORE    := stm32/BoxCore
SRCS    := $(wildcard $(CORE)/Src/*.c)
HARNESS := $(CORE)/Tests/test_harness.c
HDRS    := $(wildcard $(CORE)/Inc/*.h) $(wildcard $(CORE)/Tests/*.h)
SUITES  := $(filter-out $(HARNESS),$(wildcard $(CORE)/Tests/test_*.c))
# gcc on Windows adds .exe; naming it here stops make relinking every run.
ifeq ($(OS),Windows_NT)
EXE     := .exe
endif
TESTBINS := $(patsubst $(CORE)/Tests/%.c,build/%$(EXE),$(SUITES))

# The ESP32 end of the board link. Its tests share the harness above and
# build into build/esp32/, so a suite can't collide with a BoxCore one.
LINK        := esp32/lib/bridge_link
LINK_SRCS   := $(wildcard $(LINK)/src/*.c)
LINK_HDRS   := $(wildcard $(LINK)/src/*.h) $(CORE)/Tests/test_harness.h
LINK_SUITES := $(wildcard $(LINK)/tests/test_*.c)
TESTBINS    += $(patsubst $(LINK)/tests/%.c,build/esp32/%$(EXE),$(LINK_SUITES))

# The contract suite also links the STM32's line writer and line reader, so
# the two boards' ends of the UART protocol are tested against each other.
CONTRACT      := build/esp32/test_bridge_link_contract$(EXE)
CONTRACT_SRCS := $(CORE)/Src/box_lines.c $(CORE)/Src/box_rx.c
$(CONTRACT): LINK_EXTRA_SRCS := $(CONTRACT_SRCS)
$(CONTRACT): LINK_EXTRA_INCS := -I$(CORE)/Inc
$(CONTRACT): $(CONTRACT_SRCS) $(wildcard $(CORE)/Inc/*.h)

.PHONY: test clean

# Run every suite, even after one fails, then fail if any did.
test: $(TESTBINS)
	@status=0; for t in $(TESTBINS); do echo "== $$t"; ./$$t || status=1; done; exit $$status

build/%$(EXE): $(CORE)/Tests/%.c $(HARNESS) $(SRCS) $(HDRS)
	mkdir -p build
	$(CC) $(CFLAGS) -I$(CORE)/Inc -o $@ $< $(HARNESS) $(SRCS)

build/esp32/%$(EXE): $(LINK)/tests/%.c $(HARNESS) $(LINK_SRCS) $(LINK_HDRS)
	mkdir -p build/esp32
	$(CC) $(CFLAGS) -I$(LINK)/src -I$(CORE)/Tests $(LINK_EXTRA_INCS) -o $@ $< $(HARNESS) $(LINK_SRCS) $(LINK_EXTRA_SRCS)

clean:
	rm -rf build
