# fkv - flash-aware key-value store
#
#   make            build library, tests, bench and example
#   make test       run the full test suite
#   make bench      run the wear / write-amplification benchmark
#   make sanitize   run tests under AddressSanitizer + UBSan (gcc/clang, Linux/macOS)
#   make coverage   line coverage of src/fkv.c via gcov
#   make mutants    mutation testing: seeded crash-safety bugs must fail the suite

CC      ?= cc
CFLAGS  ?= -O2 -g
WARN    := -std=c99 -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes \
           -Wmissing-prototypes -Wcast-qual -Wconversion -Wno-sign-conversion -Werror
INC     := -Iinclude -Isim
BUILD   ?= build

ifeq ($(OS),Windows_NT)
EXE := .exe
endif

LIB_SRC   := src/fkv.c
SIM_SRC   := sim/fkv_sim.c
TEST_SRC  := $(wildcard tests/*.c)
BENCH_SRC := bench/bench.c
EX_SRC    := examples/basic.c

TEST_BIN  := $(BUILD)/fkv_test$(EXE)
BENCH_BIN := $(BUILD)/fkv_bench$(EXE)
EX_BIN    := $(BUILD)/fkv_example$(EXE)
LIB       := $(BUILD)/libfkv.a

.PHONY: all test bench example sanitize coverage mutants size clean

all: $(LIB) $(TEST_BIN) $(BENCH_BIN) $(EX_BIN)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/fkv.o: $(LIB_SRC) include/fkv.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(INC) -c $< -o $@

$(LIB): $(BUILD)/fkv.o
	$(AR) rcs $@ $^

$(TEST_BIN): $(LIB_SRC) $(SIM_SRC) $(TEST_SRC) include/fkv.h sim/fkv_sim.h tests/test.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) -Wno-missing-prototypes $(INC) $(LIB_SRC) $(SIM_SRC) $(TEST_SRC) -o $@

$(BENCH_BIN): $(LIB_SRC) $(SIM_SRC) $(BENCH_SRC) include/fkv.h sim/fkv_sim.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(INC) $(LIB_SRC) $(SIM_SRC) $(BENCH_SRC) -o $@ -lm

$(EX_BIN): $(LIB_SRC) $(SIM_SRC) $(EX_SRC) include/fkv.h sim/fkv_sim.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(INC) $(LIB_SRC) $(SIM_SRC) $(EX_SRC) -o $@

test: $(TEST_BIN)
	./$(TEST_BIN)

bench: $(BENCH_BIN)
	./$(BENCH_BIN)

example: $(EX_BIN)
	./$(EX_BIN)

sanitize:
	$(MAKE) BUILD=build-san CFLAGS="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all" test

coverage:
	rm -rf build-cov && mkdir -p build-cov
	cd build-cov && $(CC) -O0 -g --coverage -std=c99 -I../include -I../sim ../$(LIB_SRC) \
	  ../$(SIM_SRC) $(addprefix ../,$(TEST_SRC)) -o t$(EXE) && ./t$(EXE) > /dev/null && \
	  gcov -b -o . *fkv.gcda | grep -A4 "fkv.c'"

mutants:
	CC=$(CC) bash tests/mutants.sh

# Code size of the library for a Cortex-M4 (needs arm-none-eabi-gcc).
size: | $(BUILD)
	arm-none-eabi-gcc -Os -mcpu=cortex-m4 -mthumb -std=c99 -Iinclude -c $(LIB_SRC) -o $(BUILD)/fkv-arm.o
	arm-none-eabi-size $(BUILD)/fkv-arm.o

clean:
	rm -rf build build-san build-cov build-mut
