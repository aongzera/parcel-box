# `make test` builds each PC test suite in stm32/BoxCore/Tests (one binary
# per test_*.c, linked with every Box logic core source) with gcc, then runs
# them all. CI runs the same command (see .github/workflows/ci.yml).
# The firmware itself is built separately with `make -C stm32`.

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

.PHONY: test clean

# Run every suite, even after one fails, then fail if any did.
test: $(TESTBINS)
	@status=0; for t in $(TESTBINS); do echo "== $$t"; ./$$t || status=1; done; exit $$status

build/%$(EXE): $(CORE)/Tests/%.c $(HARNESS) $(SRCS) $(HDRS)
	mkdir -p build
	$(CC) $(CFLAGS) -I$(CORE)/Inc -o $@ $< $(HARNESS) $(SRCS)

clean:
	rm -rf build
