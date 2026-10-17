
# mc: the c implementation
#
#   make [CONFIG=debug|release]   build the compiler: build/<config>/mc
#   make test                     build and run the unit tests
#   make check                    unit tests, then the golden tests (tests/programs)
#   make golden                   rewrite the golden expectations (review the diff!)
#   make differential             the old comparison with the python implementation (needs python3)
#   make bench                    time the programs of bench/ with the release build
#   make clean                    remove the build directory
#
# the standard library is ../stdlib (the one the python implementation uses);
# 'mc --stdlib <dir>' or the MC_STDLIB environment variable choose another.

CONFIG ?= debug
BUILD  := build/$(CONFIG)

WARNINGS := -Wall -Wextra -Werror -Wpedantic -Wshadow -Wundef -Wformat=2 -Wstrict-prototypes   \
	-Wold-style-definition -Wmissing-prototypes -Wwrite-strings -Wcast-qual -Wvla -Walloca        \
	-Wnull-dereference -Winit-self -Wduplicated-cond -Wlogical-op -Wno-format-zero-length          \
	-Wno-pedantic

FLAGS_debug   := -g -O0 -fsanitize=address,undefined -fno-omit-frame-pointer
FLAGS_release := -O2 -DNDEBUG

STDLIB  := $(abspath ../stdlib)
CFLAGS  := -std=c11 $(WARNINGS) $(FLAGS_$(CONFIG)) -Iinclude -D_POSIX_C_SOURCE=200809L -MMD -MP \
	-DMC_DEFAULT_STDLIB='"$(STDLIB)"'
LDFLAGS := $(filter -fsanitize=%,$(FLAGS_$(CONFIG)))

LIBRARY := $(patsubst %.c,$(BUILD)/%.o,$(filter-out source/main.c,$(sort $(wildcard source/*.c))))
TESTS   := $(patsubst %.c,$(BUILD)/%.o,$(sort $(wildcard tests/*.c)))

.PHONY: all test check golden differential bench clean

all: $(BUILD)/mc

test: $(BUILD)/mc_tests
	$<

check: test $(BUILD)/mc
	tests/golden.sh $(BUILD)/mc

golden: $(BUILD)/mc
	tests/golden.sh $(BUILD)/mc --update

differential: $(BUILD)/mc
	python3 tests/differential.py $(BUILD)/mc

bench:
	$(MAKE) CONFIG=release build/release/mc
	bench/bench.sh build/release/mc

clean:
	rm -rf build

$(BUILD)/mc: $(BUILD)/source/main.o $(LIBRARY)
	$(CC) $(LDFLAGS) $^ -o $@ -lm

$(BUILD)/mc_tests: $(TESTS) $(LIBRARY)
	$(CC) $(LDFLAGS) $^ -o $@ -lm

# cairo truncates long operands of its failure reports on purpose.
$(BUILD)/tests/%.o: tests/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -Itests/cairo -Wno-format-truncation -Wno-missing-prototypes -c $< -o $@

$(BUILD)/source/%.o: source/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

-include $(wildcard $(BUILD)/*/*.d)
