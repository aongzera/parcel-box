# `make test` builds the Box logic core and its PC test suite with gcc, then
# runs it. CI runs the same command (see .github/workflows/ci.yml).

CC      := gcc
CFLAGS  := -std=c99 -Wall -Wextra -Werror -g
CORE    := stm32/BoxCore
SRCS    := $(wildcard $(CORE)/Src/*.c) $(wildcard $(CORE)/Tests/*.c)
HDRS    := $(wildcard $(CORE)/Inc/*.h) $(wildcard $(CORE)/Tests/*.h)
# gcc on Windows adds .exe; naming it here stops make relinking every run.
ifeq ($(OS),Windows_NT)
EXE     := .exe
endif
TESTBIN := build/box_core_tests$(EXE)

.PHONY: test clean

test: $(TESTBIN)
	./$(TESTBIN)

$(TESTBIN): $(SRCS) $(HDRS)
	mkdir -p build
	$(CC) $(CFLAGS) -I$(CORE)/Inc -o $@ $(SRCS)

clean:
	rm -rf build
