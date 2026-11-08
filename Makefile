# `make test` builds each PC test suite in stm32/BoxCore/Tests (one binary
# per test_*.c, linked with every Box logic core source) and in
# esp32/lib/*/tests (linked with the ESP32's pure C libraries, into
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

# The ESP32's pure C libraries: the board link (bridge_link) and the
# offline Event queue (event_queue). Their tests share the harness above,
# link every ESP32 library, and build into build/esp32/, so a suite can't
# collide with a BoxCore one.
LINK         := esp32/lib/bridge_link
QUEUE        := esp32/lib/event_queue
LINK_SRCS    := $(wildcard $(LINK)/src/*.c) $(wildcard $(QUEUE)/src/*.c)
LINK_HDRS    := $(wildcard $(LINK)/src/*.h) $(wildcard $(QUEUE)/src/*.h) $(CORE)/Tests/test_harness.h
LINK_SUITES  := $(wildcard $(LINK)/tests/test_*.c)
QUEUE_SUITES := $(wildcard $(QUEUE)/tests/test_*.c)
TESTBINS     += $(patsubst $(LINK)/tests/%.c,build/esp32/%$(EXE),$(LINK_SUITES))
TESTBINS     += $(patsubst $(QUEUE)/tests/%.c,build/esp32/%$(EXE),$(QUEUE_SUITES))
ESP32_INCS   := -I$(LINK)/src -I$(QUEUE)/src -I$(CORE)/Tests

# The contract suite also links the STM32's line writer, line reader, Event
# sender and line checksum, so the two boards' ends of the UART protocol are
# tested against each other.
CONTRACT      := build/esp32/test_bridge_link_contract$(EXE)
CONTRACT_SRCS := $(CORE)/Src/box_lines.c $(CORE)/Src/box_rx.c $(CORE)/Src/box_sender.c \
                 $(CORE)/Src/box_checksum.c
$(CONTRACT): LINK_EXTRA_SRCS := $(CONTRACT_SRCS)
$(CONTRACT): LINK_EXTRA_INCS := -I$(CORE)/Inc
$(CONTRACT): $(CONTRACT_SRCS) $(wildcard $(CORE)/Inc/*.h)

# The board link suite feeds millions of garbled Event lines (every body
# bit with every check digit bit, for 4000 Events); optimised, that takes
# about a second instead of three.
build/esp32/test_bridge_link$(EXE): CFLAGS += -O2

.PHONY: test clean

# Run every suite, even after one fails, then fail if any did.
test: $(TESTBINS)
	@status=0; for t in $(TESTBINS); do echo "== $$t"; ./$$t || status=1; done; exit $$status

build/%$(EXE): $(CORE)/Tests/%.c $(HARNESS) $(SRCS) $(HDRS)
	mkdir -p build
	$(CC) $(CFLAGS) -I$(CORE)/Inc -o $@ $< $(HARNESS) $(SRCS)

build/esp32/%$(EXE): $(LINK)/tests/%.c $(HARNESS) $(LINK_SRCS) $(LINK_HDRS)
	mkdir -p build/esp32
	$(CC) $(CFLAGS) $(ESP32_INCS) $(LINK_EXTRA_INCS) -o $@ $< $(HARNESS) $(LINK_SRCS) $(LINK_EXTRA_SRCS)

# Both build/esp32/% rules build into the same folder, named after the test
# file, so test file names must be unique across esp32/lib/*/tests (a
# second test_x.c would never be built: the first rule that matches wins).
build/esp32/%$(EXE): $(QUEUE)/tests/%.c $(HARNESS) $(LINK_SRCS) $(LINK_HDRS)
	mkdir -p build/esp32
	$(CC) $(CFLAGS) $(ESP32_INCS) -o $@ $< $(HARNESS) $(LINK_SRCS)

clean:
	rm -rf build
