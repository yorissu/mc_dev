
# mc: the c implementation
#
#   make [CONFIG=debug|release]   build the compiler: build/<config>/mc
#   make CONFIG=portable check    the same tests on the standard-c11 build (MC_PORTABLE)
#   make test                     build and run the unit tests
#   make plugins                  build the example plugins: build/<config>/plugins/<name>.so
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
	-Wnull-dereference -Winit-self -Wno-format-zero-length

# clang has neither these warnings nor -Wformat-truncation (cairo's reports
# truncate long operands on purpose).
ifeq ($(findstring clang,$(shell $(CC) --version 2>/dev/null)),)
WARNINGS     += -Wduplicated-cond -Wlogical-op
TEST_WARNINGS := -Wno-format-truncation
endif

FLAGS_debug   := -g -O0 -fsanitize=address,undefined -fno-omit-frame-pointer
FLAGS_release := -O2 -DNDEBUG
# standard c11 only, as msvc builds it: the portable 128-bit integers, no
# __typeof__ and no attributes (see include/mc/global.h).
FLAGS_portable := -g -O1 -DMC_PORTABLE -fsanitize=address,undefined -fno-omit-frame-pointer

STDLIB  := $(abspath ../stdlib)
CFLAGS  := -std=c11 $(WARNINGS) $(FLAGS_$(CONFIG)) -Iinclude -D_POSIX_C_SOURCE=200809L -MMD -MP \
	-DMC_DEFAULT_STDLIB='"$(STDLIB)"' -DMC_TEST_PLUGINS='"$(abspath $(BUILD)/plugins)"'
LDFLAGS := $(filter -fsanitize=%,$(FLAGS_$(CONFIG)))

LIBRARY := $(patsubst %.c,$(BUILD)/%.o,$(filter-out source/main.c,$(sort $(wildcard source/*.c source/plugins/*.c))))
TESTS   := $(patsubst %.c,$(BUILD)/%.o,$(sort $(wildcard tests/*.c)))

ifeq ($(OS),Windows_NT)
LIBS   := -lm
SHARED := .dll
else ifeq ($(shell uname -s),Darwin)
LIBS   := -lm
SHARED := .dylib
else
LIBS   := -lm -ldl
SHARED := .so
endif

PLUGINS := $(patsubst examples/plugins/%/,$(BUILD)/plugins/%$(SHARED),$(sort $(dir $(wildcard examples/plugins/*/*.c))))

.PHONY: all test check golden differential bench plugins clean

all: $(BUILD)/mc

test: $(BUILD)/mc_tests plugins
	$<

check: test $(BUILD)/mc plugins
	tests/golden.sh $(BUILD)/mc

golden: $(BUILD)/mc plugins
	tests/golden.sh $(BUILD)/mc --update

plugins: $(PLUGINS)

# a plugin is every .c file of examples/plugins/<name>/, as one shared library.
$(BUILD)/plugins/%$(SHARED): examples/plugins/%/*.c include/mc/plugin.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -fPIC -shared -fvisibility=hidden $(filter %.c,$^) -o $@ $(LDFLAGS) -lm

differential: $(BUILD)/mc
	python3 tests/differential.py $(BUILD)/mc

bench:
	$(MAKE) CONFIG=release build/release/mc
	bench/bench.sh build/release/mc

clean:
	rm -rf build

$(BUILD)/mc: $(BUILD)/source/main.o $(LIBRARY)
	$(CC) $(LDFLAGS) $^ -o $@ $(LIBS)

$(BUILD)/mc_tests: $(TESTS) $(LIBRARY)
	$(CC) $(LDFLAGS) $^ -o $@ $(LIBS)

$(BUILD)/tests/%.o: tests/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -Itests/cairo $(TEST_WARNINGS) -Wno-missing-prototypes -c $< -o $@

$(BUILD)/source/%.o: source/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

-include $(wildcard $(BUILD)/*/*.d $(BUILD)/*/*/*.d)
