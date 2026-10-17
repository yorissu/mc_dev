
mc - the c implementation
=========================

the compiler and virtual machine of the mc language, in c11. it started as a
port of the python implementation (../mc), which it matched byte for byte;
since then it has moved on (plugins, a c-like memory layout, binary
bytecode, format specifiers, command-line arguments for 'main'), and the
python implementation is no longer the reference.

it builds with gcc or clang (linux, macos, wsl, mingw) and with msvc
(windows). it needs nothing but the c library (and libdl on linux, for
plugins).


building
--------

with gcc or clang (linux, macos, wsl, mingw/msys2):

	make                        debug build (address and undefined behaviour sanitizers): build/debug/mc
	make CONFIG=release         optimized build: build/release/mc
	make CC=clang CONFIG=clang  the same with clang (any CONFIG name works: build/<CONFIG>/)
	make CONFIG=portable        standard c11 only (MC_PORTABLE): what msvc compiles, with sanitizers
	make plugins                the example plugins: build/<config>/plugins/<name>.so (.dylib, .dll)
	make clean

every build is warning-free with -Wall -Wextra -Wpedantic (and more, see
the makefile) and -Werror. the standard library is ../stdlib: its absolute
path is compiled in, and 'mc --stdlib <dir>' or $MC_STDLIB choose another.

with msvc (visual studio 2019 16.8 or newer), from c\ in a "developer
command prompt":

	build.bat                   build\msvc-release\mc.exe and the example plugins (.dll)
	build.bat debug             the same unoptimized: build\msvc-debug\
	build.bat [debug] test      ...then the unit tests
	build.bat [debug] check     ...then the unit tests and the golden tests (these need
	                            'sh' on the path, as git for windows has it)

msvc has no __int128: it builds the portable 128-bit integers of
include/mc/int128.h, which every other build tests too (see testing).
'make CONFIG=portable' is the closest check of what msvc compiles that a
non-windows machine can run. what differs between systems is in the
platform layer: include/mc/platform.h, source/platform_posix.c and
source/platform_windows.c.


testing
-------

	make test                   unit tests (cairo, tests/*_tests.c)
	make check                  unit tests, then the golden tests
	make golden                 rewrite the golden expectations (review the diff before committing it)
	make bench                  time the programs of bench/ with the release build
	make differential           the old comparison with the python implementation (needs python3)

any CONFIG works with them: 'make CONFIG=release check', 'make
CONFIG=portable check', 'make CC=clang CONFIG=clang check'.

golden tests: tests/programs/<name>/ holds a program (main.mc and the
modules it imports), an optional file 'options' and the file 'expected'.
tests/golden.sh runs the program through mc and compares everything it
printed (exit status, stdout, and stderr in the short format without
color) with 'expected'. 'options' has lines

	commands: check run dis mcb     what to run (default: check run; 'mcb' builds
	                                a .mcb file and runs it)
	flags: --plugin @plugins@/math@shared@
	                                more options for every command; @plugins@ is the
	                                directory of the example plugins built beside mc,
	                                @shared@ the file ending of shared libraries
	args: a b c                     the program's command line (after the file)

	tests/golden.sh build/debug/mc [--update] [name...]

the names say what a program is about: checker_*, parser_* and stdlib_* came
from the python test suites, runtime_* hit every runtime error and the
machine's layout, language_* the newer language features, plugin_* the
plugins, example_* the examples. a change of behaviour on purpose is a 'make
golden' and a reviewed diff; never regenerate to hide a change.

unit tests: one file per area (basics, front, machine, plugin, int128),
registered with cairo by themselves. tests/helpers.h runs a whole program
from its source text. the 128-bit integers are tested twice
(int128_tests.c and int128_portable_tests.c share int128_cases.h): with the
compiler's __int128 and with the portable code, which is also compared
with __int128 on random values.


usage
-----

	mc run <file.mc|file.mcb> [arguments...]
	                            check, compile and run a program; exits with what main returns
	mc build <file.mc> [-o out] compile to bytecode (.mcb)
	mc dis <file.mc|file.mcb>   print the bytecode
	mc check <file.mc>          check a program and every module it imports
	mc mono <file.mc>           print every generic instance, every expression typed
	mc ast <file.mc>            print the syntax tree of one file
	mc tokens <file.mc>         print the tokens of one file
	mc explain [code]           explain a diagnostic code, or list them all

	-I <dir>                    another module directory (after the file's own)
	--stdlib <dir>              the standard library (default: $MC_STDLIB, then the bundled one)
	--plugin <file>             load a plugin (a shared library); repeatable
	--plugin-dir <dir>          load every plugin in <dir>; repeatable ($MC_PLUGINS: a list of
	                            files and directories, ':' or ';' between them as in PATH)
	--eval-steps <n>            the steps compile-time evaluation may take (default 1000000)
	--call-depth <n>            run: the calls that may run at once (default 10000)
	--limit <n>                 run: stop after n instructions (0: no limit, the default)
	--stats                     run: print the instructions run and the heap allocations made
	--format human|short        diagnostics as annotated snippets or one line each
	--color[=auto|always|never] colors (auto: stderr is a terminal and NO_COLOR is unset)
	--no-color                  the same as --color=never
	--werror                    warnings count as errors
	-h, --help                  'mc --help', 'mc <command> --help'
	--version

'mc run' gives what follows the file to 'main :: (arguments: [][]u8)', the
file first: 'mc run tool.mc a b' calls main with ["tool.mc", "a", "b"].
mc's own options go before the file; '--' ends them (for a file whose name
starts with '-').

exit status: 0 success, 1 errors, 2 bad usage, 101 runtime error; 'run'
exits with main's int result (& 0xff).

runtime errors show the source line, the chain of calls (a recursion is
collapsed into one line with a count) and advice for the usual causes:

	runtime error: use of freed heap memory
	 --> main.mc:9:10
	  |
	9 |         return data[1];
	  |                ^^^^^^^ here
	  |
	  = note: in `deep` at main.mc:9:10
	  = note: called from `deep` at main.mc:11:9 (5 times)
	  = note: called from `main` at main.mc:16:9
	  = help: the memory was given back by `free` or `realloc` before this access


layout
------

the public interface of each part is a header in include/mc; what the parts
of one stage share among themselves is an internal header in source/.

	include/mc/global.h        panics, assertions, checked allocation, MC_PORTABLE
	include/mc/vector.h        growable arrays (mc_vector_of)
	include/mc/arena.h         arena allocation (syntax trees, symbols, types)
	include/mc/string.h        views (mc_view_s) and growable strings (mc_string_s)
	include/mc/map.h           string-keyed hash map
	include/mc/int128.h        128-bit integers: __int128 or two 64-bit halves
	include/mc/platform.h      what differs between systems (libraries, files, terminals)
	include/mc/source.h        source files, spans, line and column lookup
	include/mc/diag.h          diagnostics and their rendering (snippets, labels, notes)
	include/mc/format.h        number and text formatting (shortest floats, repr)
	include/mc/lexer.h         tokens
	include/mc/ast.h           syntax tree nodes and their annotations
	include/mc/parser.h        recursive-descent parser with error recovery
	include/mc/printer.h       syntax tree printer
	include/mc/types.h         interned semantic types, unification
	include/mc/program.h       the checked program: modules, symbols, instances
	include/mc/bytecode.h      instructions (documented there), .mcb files, disassembler
	include/mc/compiler.h      checked program -> bytecode
	include/mc/vm.h            the virtual machine
	include/mc/plugin.h        the plugin api (what a plugin includes; stable, versioned)
	include/mc/plugins.h       the set of loaded plugins
	include/mc/cli.h           the command line

	source/check.h             the checker's internals, shared by:
	  source/load.c              module search and loading, import cycles
	  source/check.c             programs, lookup, declarations, attributes, instances
	  source/check_types.c       types, struct layouts, sizes
	  source/check_statements.c  statements, places, flow
	  source/check_expressions.c expressions, calls, operators, literals
	  source/check_plugins.c     calls of plugin functions: their checks (the compile-time api)
	  source/evaluate.c          compile-time evaluation
	  source/mono.c              the 'mono' listing
	source/machine.h           the virtual machine's internals, shared by:
	  source/vm.c                loading and verifying, the instruction loop, runtime errors
	  source/vm_memory.c         blocks and handles, frames, constants
	  source/vm_plugins.c        calls of plugin functions (the run-time api)
	source/plugin_host.h       what the checker and the machine share about plugins
	source/plugins.c           loading plugins, finding their functions
	source/plugins/std*.c      the 'std' plugin, built in: memory.* and io.*
	source/platform_posix.c    the platform layer on posix systems
	source/platform_windows.c  ... and on windows

	examples/plugins/math/     an example plugin (math.c) with its module and a program
	bench/                     benchmark programs and bench.sh
	build.bat                  the msvc build

	tests/main.c               runs the unit tests
	tests/*_tests.c            unit tests (they register themselves with cairo)
	tests/helpers.h            runs a program from its source text
	tests/golden.sh            the golden tests; tests/programs/ their programs
	tests/differential.py      the old comparison with the python implementation


tables: how to extend it
------------------------

most lists live in one place, as x-macro tables (.def files) that the code
expands where it needs them:

	include/mc/diagnostics.def  every diagnostic code with its name and summary
	                            (the summary is what 'mc explain' prints)
	include/mc/tokens.def       keywords, punctuators (longest first), scalar types
	include/mc/ast.def          node kinds and their printed names
	include/mc/operators.def    the operator patterns of '[op...]' functions
	include/mc/opcodes.def      instructions and the shape of their operands

a new diagnostic: add a row to diagnostics.def, then report it with
mc_error / mc_warning:

	mc_diagnostic_s* const d = mc_error(c->diag, mc_E0599, node->span, "`%s` is wrong", name);
	mc_label_main(d, "the label under the span");
	mc_label(d, other->span, "a secondary label");
	mc_note(d, "a note"); mc_help(d, "advice");

a new function of the standard library implemented in c: add it to the
'std' plugin (source/plugins/std*.c, registered in std.c under its
qualified name 'module.function') and declare it in a module of stdlib/
with '[plugin]'. a function of a user's own is a plugin of its own; see
'plugins' below.

a new instruction: add it to opcodes.def, emit it in source/compiler.c, give
its stack effect to _mc_effect and run it in _mc_step (source/vm.c), with a
fast case in _mc_loop if it is frequent; bytecode.c reads, writes and
disassembles it from the table.

a new command: write it in source/cli.c and add a row to _mc_commands.

a new system: implement include/mc/platform.h in a source/platform_*.c
(each file compiles to nothing on the other systems).


plugins
-------

a plugin provides functions to mc programs, written in c (or anything that
can export a c function). the program declares them in an mc module with
'[plugin]' and no body; the plugin is a shared library loaded with
'--plugin'. the standard library works the same way: memory.alloc,
io.println and the rest come from the 'std' plugin, which is compiled into
mc.

a complete example, a plugin 'geometry' with two functions (a check, which
runs at compile time, is in examples/plugins/math/math.c). the mc side,
geometry.mc in a module directory of the program:

	# geometry: functions of the plugin 'geometry'.

	pub Point :: {
		let x: f64;
		let y: f64;
	}

	# the distance between two points.
	[plugin = "geometry"]
	pub distance :: (a: Point, b: Point) -> f64;

	# the sum of the coordinates of every point.
	[plugin = "geometry"]
	pub total :: (points: []Point) -> f64;

the c side, geometry.c:

	#include "mc/plugin.h"

	#include <math.h>
	#include <string.h>

	// the same layout as the mc struct: c and mc agree on it
	typedef struct {
		double x;
		double y;
	} point_s;

	static mc_plugin_value_s _distance(mc_plugin_call_s* const call) {
		point_s a, b;
		memcpy(&a, mc_plugin_get_memory(call, 0), sizeof(a));
		memcpy(&b, mc_plugin_get_memory(call, 1), sizeof(b));
		return mc_plugin_make_f64(call, hypot(a.x - b.x, a.y - b.y));
	}

	static mc_plugin_value_s _total(mc_plugin_call_s* const call) {
		const mc_plugin_slice_s points = mc_plugin_get_slice(call, 0);
		double sum = 0.0;
		for (uint64_t i = 0; i < points.length; ++i) {
			point_s point;
			memcpy(&point, (const char*)points.data + i * sizeof(point), sizeof(point));
			sum += point.x + point.y;
		}
		return mc_plugin_make_f64(call, sum);
	}

	MC_PLUGIN_EXPORT bool mc_plugin_init(mc_plugin_api_s* const api) {
		if (!mc_plugin_compatible(api, MC_PLUGIN_API_VERSION)) return false;
		return mc_plugin_register(api, "geometry", "geometry.distance", _distance, NULL)
			&& mc_plugin_register(api, "geometry", "geometry.total", _total, NULL);
	}

a program using it, main.mc beside geometry.mc:

	import geometry
	from io import println

	pub main :: () -> int {
		let a: geometry.Point = { x: 0, y: 0, };
		let b: geometry.Point = { x: 3, y: 4, };
		let points: [2]geometry.Point = { a, b, };
		println("{} {}", geometry.distance(a, b), geometry.total(points[..]));   # 5.0 7.0
		return 0;
	}

building the plugin (it needs only include/mc/plugin.h: it calls mc through
the table the handles carry, so it links against nothing of mc):

	linux, wsl:     cc -std=c11 -O2 -fPIC -shared -I<mc>/c/include geometry.c -o geometry.so -lm
	macos:          cc -std=c11 -O2 -fPIC -shared -I<mc>/c/include geometry.c -o geometry.dylib
	mingw:          gcc -std=c11 -O2 -shared -I<mc>/c/include geometry.c -o geometry.dll
	msvc:           cl /nologo /std:c11 /O2 /LD /I<mc>\c\include geometry.c /Fe:geometry.dll

and running it:

	mc run --plugin ./geometry.so main.mc
	mc run --plugin-dir ./plugins main.mc          (every library in it: .so, .dylib or .dll)
	MC_PLUGINS=./geometry.so mc run main.mc

the rules:

- names. a plugin function is registered under its qualified name: the
  dotted name of the module that declares it, then the function
  ('geometry.distance'; 'tools.text.split' for a function 'split' of
  tools/text.mc; the entry file's module is 'main'). '[plugin = "name"]'
  says which plugin must provide it; a bare '[plugin]' takes it from any.
  a call of a function that no loaded plugin provides is E0416, at check
  time, so 'check', 'build' and 'run' all need the plugins loaded. a
  plugin that cannot be loaded, needs a newer api or provides a function
  another plugin provides already is E0603.
- arguments. the checker types every call from the declaration, so the run
  function can trust the number and the types of its arguments. scalars
  come with mc_plugin_get_int / _uint / _f64 / _bool, slices with
  mc_plugin_get_slice (data, length, element type, read-only), and any
  argument as its c bytes with mc_plugin_get_memory. a variadic function
  ('values: ...') gets the variadic values after the fixed ones, each with
  its own type (mc_plugin_argument_type). type arguments of a generic
  function ('alloc<T>') come with mc_plugin_type_argument.
- memory. a struct, an array or a slice's elements are laid out as c lays
  out the same fields (see 'the machine'); the type functions
  (mc_plugin_type_size, _alignment, _field_name, _field_offset, ...) say
  where everything is, so a plugin can also read types it does not know in
  advance. a slice or a function value stored inside such memory is read
  with mc_plugin_slice_at and mc_plugin_function_name_at. the memory of an
  argument is valid during the call only.
- results. mc_plugin_make_int / _uint / _f64 / _bool / _none / _null; the
  value is converted to the declared result type (integers wrap, f32
  rounds). a new heap slice comes from mc_plugin_heap_alloc (zeroed,
  checked like memory from 'alloc', freed with memory.free) or
  mc_plugin_heap_adopt (memory the plugin allocated; mc calls its release
  function).
- errors. mc_plugin_fail stops the program with a runtime error, shown like
  mc's own (the call's line and the chain of calls).
- checks. a function may register a check, which runs at compile time for
  every direct call: it sees the static types of the arguments and their
  values when the compiler knows them (mc_plugin_check_constant), reports
  errors and warnings with labels, notes and help (E0531 and W0531 unless
  it gives a code of mc's own, like the std plugin's E0530 for format
  strings), and can mark arguments with flags that the run function reads
  back (mc_plugin_argument_flags; the std plugin marks "..."c strings that
  way).
- values. a plugin function can be used as a function value
  ('apply(geometry.distance, a, b)'), except a variadic one.
- versions. MC_PLUGIN_API_VERSION is 1. the table of functions only grows;
  a plugin asks mc_plugin_compatible with the version it was built with.
  MC_PLUGIN_EXPORT exports mc_plugin_init (__declspec(dllexport) on
  windows).

examples/plugins/math is a fuller example: numbers, slices, heap memory, a
struct read through its layout, a runtime error and a check that warns.
'make plugins' builds it, golden programs plugin_* run it, and
tests/plugin_tests.c tests the api.


the machine
-----------

data is laid out the way c lays it out, on every system:

	u8 s8 bool                  1 byte
	u16 s16                     2 bytes
	u32 s32 f32                 4 bytes
	u64 s64 int uint f64 float  8 bytes
	[N]T                        N elements of T, one after the other
	struct                      the fields in declaration order, each at the next offset
	                            that is a multiple of its alignment; the size is rounded up
	                            to the struct's alignment (the largest of its fields')
	[]T                         16 bytes, alignment 8: a block handle (8 bytes), then the
	                            byte offset (low 4 bytes) and the length (high 4 bytes)
	function value              8 bytes

so a { a: u8, b: f64, c: u16 } is 24 bytes (a at 0, b at 8, c at 16), as in
c. a slice views at most 2^32 - 1 elements, in the first 4 gib of its block.

memory is in blocks: the byte area of a call (on a stack of chunks that
never move), heap allocations ('alloc', 'realloc', plugins) and the images
of constants. a block is an entry of the table of its kind; its handle is a
64-bit word:

	bits 0-31   the index in the table
	bits 32-60  the generation of the entry
	bit 61      read-only (constants, the command line, slices of "..." literals)
	bits 62-63  the kind: none, heap, frame, constant

the generation of an entry changes when its block dies (free, realloc, the
call returns) and the entry is reused later, so an old handle no longer
matches: every access through a slice or a location checks the generation
and the bounds, and the kind says which message to give (use of freed heap
memory, a local of a call that returned, writing to a constant). a frame
gets a block only when a location or a slice of its locals is taken, so
most calls never touch the tables.

the operand stack holds 64-bit words: a scalar is one word (integers
extended by their type, f32 widened to a double, bools 0 or 1), a slice or
a location two, a struct or an array its bytes rounded up to whole words.
loading a module verifies every function (jump targets, stack depths at
every instruction, frame offsets and sizes, calls and returns), so the
instruction loop does not check them again; it runs decoded instructions
with fast cases for the frequent ones.

there is no garbage collector: nothing owns memory except heap blocks,
which 'free' gives back; what is left is freed when the machine stops
('--stats' counts the allocations). see source/machine.h and
include/mc/bytecode.h.

benchmarks (make bench; release build, one machine, milliseconds):

	              before the rework   after
	fib                 373             78
	sieve              1684            136
	sort                794            172
	structs             260             37


bytecode files
--------------

'mc build' writes the compiled program as a binary .mcb file, which 'run'
and 'dis' take like a source file. it is little-endian with fixed-width
integers: the magic "MCB\0", the format version (u32, now 2), then the
sections, each a u32 count of entries:

	files        the source files (for runtime errors)
	types        name, kind, type code, size, alignment, element, length, zero
	             value, fields (name, type, offset)
	constants    integers, floats, bools, text, functions, lists and slices
	intrinsics   the plugin functions the program calls: qualified name, result
	             and parameter types, flags marked by checks
	functions    name, parameters, frame size, parameter slots, instructions
	             (opcode, type codes, operands) and a source position per instruction

and finally the entry function and whether it returns a value. include/
mc/bytecode.h has every field and every instruction. the reader checks
every count, index and size, and the machine verifies the functions, so a
truncated or corrupted file is refused with a message (a unit test feeds
it thousands of corrupted files). plugin functions are found by name when
the file is loaded, so an .mcb file needs the same plugins as its source.


how it works
------------

the pipeline: lexer -> parser (recursive descent, setjmp recovery) ->
loader (modules, import cycles) -> checker (types, generics by
monomorphization, references, mutability, flow, plugin checks,
compile-time evaluation of constants in evaluate.c) -> compiler (checked
program -> stack bytecode) -> verifier -> virtual machine.

memory: the front end allocates syntax trees, symbols and types in arenas,
freed all at once. the parser and the evaluator unwind with longjmp; every
vector they fill lives in an arena too, so unwinding leaks nothing. types are
interned, so two types are equal exactly when their pointers are.

numbers: literals and compile-time values are 128-bit integers (mc_i128_s,
include/mc/int128.h) and doubles, wrapped to their type exactly, so constant
folding and overflow warnings are exact (an untyped constant can exceed 64
bits until it gets its type).

formatting: floats are written as the shortest text that reads back as the
same number (f32 with f32 digits), integers in full; nan and infinities
are spelled by mc, and programs write the same bytes on every system
(windows writes '\n', not '\r\n').
