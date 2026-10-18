
# mia: the compiler and virtual machine
#
#   make [CONFIG=release|debug|portable]   mia, its standard library and the example plugins in build/<config>/
#   make stdlib                            the standard library: its modules and plugins in build/<config>/stdlib/
#   make plugin-<name>                     one plugin of stdlib/ (io, memory), examples/plugins/ or tests/plugins/
#   make example-plugins                   the example plugins: build/<config>/examples/plugins/
#   make test-plugins                      the test plugins: build/<config>/tests/plugins/
#   make check                             the tests: every program of tests/programs
#   make lint                              the code style (tests/style.py, tests/format.py), cppcheck and shellcheck
#   make golden                            rewrite the expectations of the tests (review the diff!)
#   make install PREFIX=<dir>              copy the installation folder (mia and stdlib/) to <dir>
#   make bench                             time the programs of bench/ with the release build
#   make clean                             remove the build directory
#
# configs: 'release' (the default) optimizes; 'debug' adds the address and
# undefined behaviour sanitizers; 'portable' builds standard c11 only (what
# msvc compiles: the portable 128-bit integers), with the sanitizers. any
# other name works too, with the flags of FLAGS_<name>.
#
# build/<config>/ is an installation folder: mia finds its standard library
# beside itself, in stdlib/: the modules (io.mia) and their plugins (io.so).

VERSION := 0.1.0

CONFIG ?= release
BUILD  := build/$(CONFIG)

WARNINGS := -Wall -Wextra -Werror -Wpedantic -Wshadow -Wundef -Wformat=2 -Wstrict-prototypes   \
	-Wold-style-definition -Wmissing-prototypes -Wwrite-strings -Wcast-qual -Wvla -Walloca        \
	-Wnull-dereference -Winit-self -Wno-format-zero-length

# clang does not have these.
ifeq ($(findstring clang,$(shell $(CC) --version 2>/dev/null)),)
WARNINGS += -Wduplicated-cond -Wlogical-op
endif

FLAGS_debug    := -g -O0 -fsanitize=address,undefined -fno-omit-frame-pointer
FLAGS_release  := -O2 -DNDEBUG
FLAGS_portable := -g -O1 -Dmia_portable -fsanitize=address,undefined -fno-omit-frame-pointer

CFLAGS  := -std=c11 $(WARNINGS) $(FLAGS_$(CONFIG)) -Iinclude -D_POSIX_C_SOURCE=200809L -MMD -MP '-Dmia_cli_version="$(VERSION)"'
LDFLAGS := $(filter -fsanitize=%,$(FLAGS_$(CONFIG)))

# the system layer: one file per system (source/system/)
ifeq ($(OS),Windows_NT)
SYSTEM := windows
LIBS   := -lm
# mingw: c99's printf (%zu, %lld), not msvcrt's
CFLAGS += -D__USE_MINGW_ANSI_STDIO=1
SHARED := .dll
PIC    :=
else ifeq ($(shell uname -s),Darwin)
SYSTEM := posix
LIBS   := -lm
SHARED := .dylib
PIC    := -fPIC
else
SYSTEM := posix
LIBS   := -lm -ldl
SHARED := .so
PIC    := -fPIC
endif

SOURCES := $(sort $(wildcard source/*.c)) source/system/$(SYSTEM).c
OBJECTS := $(patsubst %.c,$(BUILD)/%.o,$(SOURCES))
STDLIB  := $(patsubst stdlib/%,$(BUILD)/stdlib/%,$(wildcard stdlib/*.mia))

# a plugin is built as one shared library. the standard library's are one c
# file each, beside their modules (stdlib/io.c for stdlib/io.mia), and are
# built beside them too (build/<config>/stdlib/io.so), where mia looks them
# up. the example and test plugins are a directory of c files each, built
# into build/<config>/examples/plugins/ and tests/plugins/. the plugin 'io'
# also uses mia's own strings and number formatting: it gets its own copy.
plugins_in = $(notdir $(patsubst %/,%,$(sort $(dir $(wildcard $(1)/*/*.c)))))
STDLIB_PLUGINS  := $(basename $(notdir $(wildcard stdlib/*.c)))
EXAMPLE_PLUGINS := $(call plugins_in,examples/plugins)
TEST_PLUGINS    := $(call plugins_in,tests/plugins)
PLUGIN_SUPPORT_io := source/global.c source/arena.c source/view.c source/string.c source/format.c

.PHONY: all stdlib example-plugins test-plugins check golden lint install bench clean
.PHONY: $(addprefix plugin-,$(STDLIB_PLUGINS) $(EXAMPLE_PLUGINS) $(TEST_PLUGINS))

all: $(BUILD)/mia stdlib example-plugins

stdlib: $(STDLIB) $(patsubst %,$(BUILD)/stdlib/%$(SHARED),$(STDLIB_PLUGINS))

example-plugins: $(patsubst %,$(BUILD)/examples/plugins/%$(SHARED),$(EXAMPLE_PLUGINS))

test-plugins: $(patsubst %,$(BUILD)/tests/plugins/%$(SHARED),$(TEST_PLUGINS))

check: all test-plugins
	tests/golden.sh $(BUILD)/mia

golden: all test-plugins
	tests/golden.sh $(BUILD)/mia --update

# the code style, then cppcheck and shellcheck when they are installed (ci installs them)
CPPCHECK := cppcheck --std=c11 --enable=warning,portability,performance --inline-suppr --quiet --error-exitcode=1 \
	-Iinclude -D_POSIX_C_SOURCE=200809L '-Dmia_cli_version="$(VERSION)"'

lint:
	python3 tests/style.py
	python3 tests/format.py --check stdlib examples bench tests/programs
	@if command -v cppcheck > /dev/null; then echo "$(CPPCHECK) ..."; $(CPPCHECK) source stdlib examples/plugins tests/plugins; \
		else echo "lint: cppcheck is not installed, skipped"; fi
	@if command -v shellcheck > /dev/null; then echo "shellcheck ..."; shellcheck tests/golden.sh bench/bench.sh; \
		else echo "lint: shellcheck is not installed, skipped"; fi

install: all
	@test -n "$(PREFIX)" || { echo "make install needs PREFIX=<dir>"; exit 2; }
	mkdir -p $(PREFIX)/stdlib
	cp $(BUILD)/mia $(PREFIX)/
	cp $(BUILD)/stdlib/*.mia $(BUILD)/stdlib/*$(SHARED) $(PREFIX)/stdlib/

bench:
	$(MAKE) CONFIG=release all
	bench/bench.sh build/release/mia

clean:
	rm -rf build

$(BUILD)/mia: $(OBJECTS)
	$(CC) $(LDFLAGS) $^ -o $@ $(LIBS)

$(BUILD)/source/%.o: source/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/stdlib/%.mia: stdlib/%.mia
	@mkdir -p $(@D)
	cp $< $@

# plugin_rule(name, source files, output directory): one compiler run for
# all its files, so it depends on every header (and writes no .d file into
# the installation folder).
define plugin_rule
plugin-$(1): $(3)/$(1)$(SHARED)

$(3)/$(1)$(SHARED): $(2) $(PLUGIN_SUPPORT_$(1)) $(wildcard include/mia/*.h include/mia/*.def)
	@mkdir -p $$(@D)
	$$(CC) $$(filter-out -MMD -MP,$$(CFLAGS)) $(PIC) -shared -fvisibility=hidden $$(filter %.c,$$^) -o $$@ $$(LDFLAGS) -lm
endef
$(foreach plugin,$(STDLIB_PLUGINS),$(eval $(call plugin_rule,$(plugin),stdlib/$(plugin).c,$(BUILD)/stdlib)))
$(foreach plugin,$(EXAMPLE_PLUGINS),$(eval $(call plugin_rule,$(plugin),$(wildcard examples/plugins/$(plugin)/*.[ch]),$(BUILD)/examples/plugins)))
$(foreach plugin,$(TEST_PLUGINS),$(eval $(call plugin_rule,$(plugin),$(wildcard tests/plugins/$(plugin)/*.[ch]),$(BUILD)/tests/plugins)))

-include $(wildcard $(BUILD)/*/*.d $(BUILD)/*/*/*.d)
