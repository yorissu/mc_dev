
mia
===

the mia language: lexer, parser, module loader, type checker, generic
instantiation, compile-time evaluation, a compiler to stack-based bytecode
and a virtual machine that runs it, written in c11. it builds with gcc or
clang (linux, macos, wsl, mingw) and with msvc (windows), and needs nothing
but the c library (and libdl on linux, for plugins).


usage
-----

build it with 'make' (or build.bat with msvc, see building), then:

	mia run <file.mia|file.miab> [arguments...]
	                            check, compile and run a program; exits with what main returns
	mia build <file.mia> [-o out] compile to bytecode (.miab)
	mia check <file.mia>        check a program, or a module, and every module it imports
	mia ast <file.mia>          print the syntax tree of one file
	mia dis <file.mia|file.miab>
	                            print the bytecode of a program: types, functions, instructions
	mia explain [code]          explain a diagnostic code, or list them all

	-M, --module-dir <dir>      another module directory (after the file's and the stdlib's); repeatable
	-P, --plugin-dir <dir>      another plugin directory (after the file's and the stdlib's); repeatable
	--stdlib <dir>              another standard library (default: the installation folder's stdlib/)
	--eval-steps <n>            the steps compile-time evaluation may take (default 1000000)
	--call-depth <n>            run: the calls that may run at once (default 10000)
	--limit <n>                 run: stop after n instructions (0: no limit, the default)
	--stats                     run: print the instructions run and the heap allocations made
	--profile                   run: count what runs (slower) and print what ran most
	--format human|short        diagnostics as annotated snippets or one line each
	--color[=auto|always|never] colors (auto: stderr is a terminal and NO_COLOR is unset)
	--no-color                  the same as --color=never
	--werror                    warnings count as errors
	-h, --help                  'mia --help', 'mia <command> --help'
	--version

mia is installed as one folder: the executable and its standard library
(stdlib/), whose modules and plugins sit side by side (io.mia, io.so).
mia has two lists of directories, where it looks for modules and where it
looks for plugins. both start with the directory of the program's file,
then the standard library; '-M <dir>' adds module directories and
'-P <dir>' plugin directories after those ('-Mdir' and '-Pdir' too). the
standard library is the installation folder's and no other ('--stdlib'
names another one). build/<config>/ is such a folder, and 'make install
PREFIX=<dir>' copies one. 'check' takes a module without 'main' too;
'build' and 'run' need one.

'mia run' gives what follows the file to 'main :: (arguments: [][]u8)', the
file first: 'mia run tool.mia a b' calls main with ["tool.mia", "a", "b"].
mia's own options go before the file; '--' ends them (for a file whose name
starts with '-').

exit status: 0 success, 1 errors, 2 bad usage, 101 runtime error; 'run'
exits with main's int result (& 0xff).

runtime errors show the source line, the chain of calls (a recursion is
collapsed into one line with a count) and advice for the usual causes:

	runtime error: use of freed heap memory
	 --> main.mia:9:10
	  |
	9 |         return data[1];
	  |                ^^^^^^^ here
	  |
	  = note: in `deep` at main.mia:9:10
	  = note: called from `deep` at main.mia:11:9 (5 times)
	  = note: called from `main` at main.mia:16:9
	  = help: the memory was given back by `free` or `realloc` before this access

examples: examples/tour (the language in one program) and
examples/plugins/math (a plugin); stdlib/vector.mia is a generic module.
editors/vscode has syntax highlighting for visual studio code.


the language
------------

declarations are 'name :: ...':

	Point :: { let x: f32; let y: f32 = 0; }           struct (fields are 'let')
	add<T> :: (a: T, b: T) -> T { return a + b; }       function (generic)
	LIMIT :: 4 * 16;                                    constant
	[plugin = "memory"] pub alloc<T> :: (count: uint) -> []T;   provided by the plugin 'memory'

- types: s8..s64, u8..u64, int and uint (word sized), f32, f64, float (word
  sized, the default for float literals), bool; [N]T arrays (values), [_]T
  takes the length from the initializer; []T slices (views of array or heap
  memory, storable); (int, &T) -> int function types. no pointers, no void,
  no enums, no unions, no empty structs.
- everything is copied (shallowly). '&T' parameters are references; the
  caller writes '&x'. a function passes its own reference parameter on as
  it is: f(vector), never f(&vector). references exist only as parameters,
  so they never outlive what they refer to.
- there is no 'mut': variables, parameters (the function's own copies),
  fields, loop variables and references can all be changed. only constants
  and string literals are read-only (at run time, through slices).
- generics take types only, are declared on the name, are instantiated once
  per set of type arguments, and each instance is checked like normal code.
- operators are built in: arithmetic and bit operators on numbers, '==' and
  '<' ... on numbers, '==' and '!=' on bool and functions, indexing on arrays
  and slices. none apply to slices otherwise (an empty one has lenof(s) == 0)
  or to structs, and none can be declared: write a function ('add(a, b)').
  a name is declared once, so functions are not overloaded either.
- pipes read left to right: 'x ~ f(a, b)' is 'f(x, a, b)' and 'x ~ f' is
  'f(x)'; a chain runs step by step: '&v ~ vector.pop ~ option.unwrap'. a
  pipe binds loosest ('a + b ~ f' is 'f(a + b)'); its result needs
  parentheses before another operator: '(x ~ f) == 3'. the piped value is
  the first argument: a temporary is lent to a '&T' parameter as it is, a
  variable needs '&' at the head of the pipe ('&v ~ ...'). a step that is a
  call gets the value too ('x ~ make()' is 'make(x)'); to call what a call
  returns, write '(make())'. intrinsics (lenof, sizeof, assert) are no steps.
  '~' after a value is the pipe; in front of one it is the bit-not ('~mask').
- lambdas '(x: int) -> int { ... }' or '(x: int) -> int then return x * 2'
  capture nothing: they see constants, functions and modules, not the
  variables around them.
- plugins: a function declared '[plugin = "name"]' without a body is
  provided by the plugin it names, a shared library (see plugins below);
  '[plugin]' without a name is an error. it is found by its qualified name,
  'module.function' ('memory.alloc'), in that plugin, whose library mia looks
  up in the plugin directories (memory.so, or .dylib, .dll). the standard
  library's alloc, realloc, free, copy, print and println come from its
  plugins memory and io (stdlib/memory.c, stdlib/io.c). a plugin function
  can be used as a function value unless it is variadic, and cannot run at
  compile time. using one that no plugin provides is E0416.
- only plugin functions can be variadic: 'values: ...' as the last parameter
  takes any number of values of any types.
- strings: "text" is a [N]u8 of the bytes between the quotes, with escapes
  (\n \t \r \0 \\ \" \' \xHH). a multiline string starts with """ at the end
  of a line and ends with """ on a line of its own; the indentation of that
  closing """ is removed from every line, as are the line breaks after the
  opening and before the closing """:

		let help = """
			usage: tool <file>
				-v  verbose
			""";    # "usage: tool <file>\n\t-v  verbose"

  quotes need no escape inside it. both kinds take the suffix c: "text"c
  and """...."""c end with a NUL. printing drops that NUL only for values
  that come from such a literal (also through a constant); other u8 arrays
  and slices, those in variables included, print every byte.
- print and println take a format string, which must be known at compile
  time: each '{}' is replaced by the next value ('{{' and '}}' write braces),
  and the compiler checks that they match the values:
  println("{} + {} = {}", a, b, a + b);
- format specifiers: '{:spec}' with
  spec = [[fill]align][sign][#][0][width][.precision][type]: align '<', '>',
  '^' with any printable ascii fill but braces; sign '+' or '-'; '#' for
  '0x', '0o', '0b'; '0' pads numbers with zeros; types 'd' 'x' 'X' 'o' 'b'
  for integers and 'f' 'e' 'E' 'g' for floats. width, fill and align apply
  to any value. a specifier that does not fit its value is E0530 at compile
  time: println("{:>8} {:08.3f} {:#x}|", "right", 3.14159, 255). stdlib/io.mia
  has the details.
- constants and array lengths are evaluated at compile time; user functions
  can run there. sizeof(T), lenof(x) and assert(cond[, "message"]) are
  intrinsics; a constant assert is checked by the compiler. integer
  literals and untyped constants are exact up to 128 bits (-2^127 to
  2^127 - 1) until they get their type; one that leaves that is E0511.
- operators follow c's order, but bit operators, shifts, mixing && with ||,
  and chained comparisons need parentheses.
- evaluation goes left to right, as the code reads, at run time and at
  compile time: operands before their operator, a function value before
  its arguments, a piped value before the step's own arguments, an array
  before its index, the place an assignment writes before the value
  (a[i()] = v(): i, then v), and the fields and elements of a literal in the
  order they are written. '&&' and '||' stop at the first part that
  decides.
- discarding a result is an error ('_ = f();' discards on purpose).
- integers wrap; the compiler warns when it can see an overflow ([wrap]
  silences it). let without a value warns when read too early ([uninit]
  silences it).
- attributes go before declarations and before any statement:
  [wrap] before a block, 'if', 'while', 'for', 'return' or an expression
  silences the overflow warnings of everything inside it, except the
  lambdas and local functions declared there. [uninit] is for 'let' only.
- 'pub main' in the entry module: '()', '() -> int', and
  '(arguments: [][]u8)' and '(arguments: [][]u8) -> int'. the arguments
  are read-only and cannot be freed.
- limits (options): compile-time evaluation takes at most --eval-steps
  steps (default 1,000,000; E0705 points at the loop or call still running),
  a program at most --call-depth nested calls (default 10,000) and --limit
  instructions (default: no limit).
- fixed limits, so that no program can crash the compiler (its passes
  recurse) or make sizes wrap around: expressions, statements and types
  nest at most 1000 deep, a chain of operators (1 + 2 + ...) counting each
  one (E0207); structs inside structs, constants defined through each
  other (E0705) and modules importing each other (E0307) chain at most
  1000 deep; a type takes at most 4 GiB (E0532) and a function's locals
  2 GiB, which is also what the locals of all running calls take together
  (a runtime 'stack overflow'); one compile-time evaluation makes at most
  4,194,304 values in arrays and structs (E0705; at run time they are not
  limited).


defaults chosen while implementing
----------------------------------

these were open or unspecified in the design discussion; each is easy to
change:

- source files end in '.mia'. 'import a.b' loads a/b.mia and is used as
  'a.b.name'; 'import a.b as c' as 'c.name'.
- octal is '0o17'. number suffixes are type names ('255u8', '2.0float');
  hexadecimal numbers take integer suffixes only.
- a body is a block, or 'then' and one statement (not a declaration, not a
  block): if c then x += 1; while c then step(); for i in 0..3 then f(i);
  functions too: main :: () then run(); square :: (x: int) -> int then
  return x * x; and lambdas, whose statement has no ';' of its own (it ends
  where the expression around the lambda goes on):
  map(s, (x: int) -> int then return x * 2).
  'else' takes its statement right away: else return; else if c then ...
  there is 'else if', no 'elif'.
- in a condition, a typed literal needs parentheses: if p == ((P){...}) { }.
- '{}' is the zero value of any type except function types; for a slice it
  is the empty slice (there is no null).
- struct literals need field names; array literals are positional and must
  have exactly N elements. a literal passed for a slice parameter becomes a
  hidden array: sum({ 1, 2 }).
- casts convert between number types only.
- '&5' (a reference to a temporary) is allowed; what the function writes to
  it is dropped. one call may pass the same variable twice: f(&a, &a).
- local constants and functions can be used anywhere in their block; local
  function bodies are checked where they are declared.
- sizes: a word is 8 bytes; a slice is 16, a function value 8, bool 1;
  structs use c layout.
- integer division truncates toward zero.
- compile-time evaluation stops after 1,000,000 steps (--eval-steps) or
  100 nested calls; plugin functions cannot run at compile time.


diagnostics
-----------

codes are E (error) or W (warning), an area and a number: 01 lexing,
02 syntax, 03 modules, 04 declarations and attributes, 05 types, 06 command
line, 07 compile-time evaluation. 'mia explain' lists every code with its
summary (include/mia/diagnostics.def).


building
--------

with gcc or clang (linux, macos, wsl, mingw/msys2):

	make                        the installation folder build/release/: mia and stdlib/ (with io.so
	                            and memory.so), and the example plugins: build/release/examples/plugins/
	make CONFIG=debug           the same with the address and undefined behaviour sanitizers: build/debug/
	make CONFIG=portable        standard c11 only (mia_portable): what msvc compiles, with sanitizers
	make CC=clang CONFIG=clang  the same with clang (any CONFIG name works: build/<CONFIG>/)
	make stdlib                 the standard library: its modules and plugins (.so, .dylib, .dll)
	make plugin-<name>          one plugin (plugin-io, plugin-math, plugin-probe, ...)
	make example-plugins        the example plugins: build/<config>/examples/plugins/
	make test-plugins           the test plugins: build/<config>/tests/plugins/
	make install PREFIX=<dir>   copy the installation folder to <dir>
	make clean

every build is warning-free with -Wall -Wextra -Wpedantic (and more, see
the makefile) and -Werror. the version is VERSION in the makefile (what
'mia --version' says). nothing is compiled in about where things are: mia
finds its stdlib/ beside itself (see usage).

with msvc (visual studio 2019 16.8 or newer), in a "developer command
prompt":

	build.bat                   build\msvc-release\: mia.exe and stdlib\ (with io.dll, memory.dll)
	build.bat debug             the same unoptimized: build\msvc-debug\
	build.bat [debug] check     ...then the tests (they need 'sh' on the path,
	                            as git for windows has it)

msvc has no __int128: it builds the portable 128-bit integers of
include/mia/int128.h. 'make CONFIG=portable check' runs every test on them,
the closest check of what msvc compiles that a non-windows machine can run.
what differs between systems is in the system layer: include/mia/system.h,
with one file per system in source/system/ (posix.c, windows.c); the build
compiles the one for the system it builds on.


testing
-------

the tests are mia programs: tests/programs/<name>/ holds a program
(main.mia and the modules it imports), an optional file 'options' and the
file 'expected', what mia printed for it.

	make check                  every program, with the build of CONFIG
	make golden                 rewrite the expectations (review the diff before keeping it)
	make bench                  time the programs of bench/ with the release build

	tests/golden.sh build/debug/mia [--update] [name...]

tests/golden.sh runs each program through mia and compares everything it
printed (exit status, stdout, and stderr in the short format without color)
with 'expected'. 'options' has lines

	commands: check run ast miab    commands run on main.mia: any command of mia that
	                                takes a file; 'miab' builds a .miab file and runs it.
	                                the default is 'check run' (none with 'invoke' lines)
	invoke: explain E0502           any other command line of mia, run as it is (repeatable)
	flags: -P @examples@            more options for every command; @plugins@ and @examples@
	                                are the directories of the test and the example plugins
	                                (build/<config>/tests/plugins, .../examples/plugins), @shared@
	                                the file ending of shared libraries. the programs use the
	                                standard library of the installation folder, which the
	                                output shows as @stdlib@
	args: a b c                     the program's command line (after the file)

the names say what a program is about: lexer_*, parser_*, checker_* and
diag_* the front end, language_* the language's features, runtime_* the
machine (every runtime error, the memory layout), bytecode_* .miab files,
plugin_* the plugin api (with the test plugins of tests/plugins/),
stdlib_* the standard library, cli_* the command line. the sanitizer
builds ('make CONFIG=debug check', 'make CONFIG=portable check') also catch
memory errors and leaks. a change of behaviour on purpose is a 'make
golden' and a reviewed diff; never regenerate to hide a change.

	make lint                   the code style (tests/style.py, tests/format.py), cppcheck and shellcheck
	python3 tests/style.py --fix   fixes the alignment the style wants, then checks
	python3 tests/format.py <file.mia>...   lays out mia code as written by hand (blocks over
	                            several lines, one statement per line); --check only lists

the continuous integration (.github/workflows/ci.yml) builds every config
with gcc and clang on linux, on macos, and with msvc and mingw on windows,
runs the tests in each, and runs the linters.


code style
----------

tests/style.py checks it ('make lint'):

- files: every file starts with an empty line (but scripts, json and dot
  files) and ends with one newline; no trailing whitespace; code is
  indented with tabs (of 4 columns); only .bat files have crlf line ends.
- names: lowercase, macros too (the system's feature macros aside). type
  names end in _s (struct), _e (enum), _u (union) or _f (function). no
  name ends in '_' (mia_ast_node_count, not ..._count_; a member named
  like a c keyword is spelled out: operation, structure, if_statement).
- modules: a module is a header in include/mia and the source file of the
  same name. its public names start with mia_<module>_ (mia_lexer_lex,
  mia_token_s), the private ones of its source file with _mia_<module>_
  (_mia_lexer_peek, _mia_vm_frame_s). the module 'global' (mia_assert,
  mia_malloc...) is the one whose names have no module part.
- const: parameters of function definitions are const ('const int x',
  'char* const p'; va_list and arrays aside).
- comments: '///' on every public declaration of include/mia (one comment
  may cover a group of declarations under it); '//' inside code.
- alignment: the fields of a struct ('type   name   ;') and their trailing
  comments (three spaces past the longest field), the backslashes of a
  macro, and a continuation line under the column after its open
  parenthesis (or one tab deeper than its statement).


layout
------

the code is in modules: a module is a header and the source file of the
same name (include/mia/lexer.h and source/lexer.c are the module 'lexer').
every name of a module starts with it: public functions, types and
constants with mia_<module>_ (mia_lexer_lex, mia_token_s, mia_ast_node_if),
the private ones of its source file with _mia_<module>_ (_mia_lexer_peek,
_mia_vm_frame_s). the one exception is the module 'global', whose names
have no module part (mia_assert, mia_panic, mia_malloc).

	include/mia/global.h        panics, assertions, checked allocation, mia_portable
	include/mia/vector.h        growable arrays (mia_vector_of)
	include/mia/arena.h         arena allocation (syntax trees, symbols, types)
	include/mia/view.h          views of text (mia_view_s): a pointer and a length
	include/mia/string.h        growable strings (mia_string_s)
	include/mia/map.h           hash maps: from text to pointers, from numbers or pointers to indexes
	include/mia/int128.h        128-bit integers: __int128 or two 64-bit halves
	include/mia/system.h        what differs between systems (libraries, files, the process, terminals, paths)
	include/mia/source.h        source files, line and column lookup
	include/mia/span.h          spans: byte ranges of a source file
	include/mia/diag.h          diagnostics and their rendering (snippets, labels, notes)
	include/mia/format.h        number and text formatting (shortest floats, repr)
	include/mia/scalar.h        the built-in scalar types (from tokens.def)
	include/mia/token.h         tokens: kinds, keywords, punctuators (from tokens.def)
	include/mia/lexer.h         source text -> tokens
	include/mia/ast.h           syntax tree nodes and their annotations
	include/mia/parser.h        recursive-descent parser with error recovery
	include/mia/printer.h       syntax tree printer
	include/mia/type.h          interned semantic types, unification
	include/mia/program.h       the checked program: modules, symbols, instances
	include/mia/checker.h       the checker's internals, shared with the evaluator and the compiler
	include/mia/evaluator.h     the compile-time evaluator's values and state
	include/mia/opcode.h        the instructions (from opcodes.def)
	include/mia/code.h          the type codes of instructions (from codes.def)
	include/mia/bytecode.h      bytecode modules (the instructions are documented there), .miab files
	include/mia/compiler.h      checked program -> bytecode
	include/mia/vm.h            the virtual machine
	include/mia/listing.h       bytecode as text: listings ('mia dis') and profiles ('run --profile')
	include/mia/plugin.h        the plugin api (what a plugin includes; stable, versioned)
	include/mia/plugins.h       the plugins: loading and looking them up, the functions they provide
	include/mia/cli.h           the command line

	source/checker.c           the checker: modules and their loading, lookup, declarations,
	                           instances, types and layouts, statements, expressions, the
	                           checks of plugin functions (the compile-time api); program.h's functions
	source/evaluator.c         compile-time evaluation
	source/vm.c                the virtual machine: loading and verifying, memory (blocks, handles,
	                           frames, constants), calls of plugin functions (the run-time api),
	                           the instruction loop, runtime errors
	source/plugins.c           loading plugins, looking them up, finding their functions
	source/system/posix.c      the module 'system' on posix systems
	source/system/windows.c    ... and on windows

	stdlib/                    the standard library: the modules memory, io, option, result and
	                           vector, and the plugins of memory and io beside them (memory.c, io.c)
	examples/                  example programs, and examples/plugins/math, an example plugin
	editors/vscode/            syntax highlighting for visual studio code
	bench/                     benchmark programs and bench.sh
	tests/golden.sh            runs the tests; tests/programs/ their programs,
	                           tests/plugins/ the plugins they load
	tests/style.py             checks the code style
	tests/format.py            lays out mia code (and checks that it is)
	build.bat                  the msvc build
	.github/workflows/ci.yml   the continuous integration


tables: how to extend it
------------------------

most lists live in one place, as x-macro tables (.def files) that the code
expands where it needs them:

	include/mia/diagnostics.def  every diagnostic code with its name and summary
	                             (the summary is what 'mia explain' prints)
	include/mia/tokens.def       keywords, punctuators (longest first), scalar types
	include/mia/ast.def          node kinds and their printed names
	include/mia/opcodes.def      instructions and the shape of their operands
	include/mia/codes.def        the type codes of instructions: size, kind, signedness

a new diagnostic: add a row to diagnostics.def, then report it with
mia_diag_error / mia_diag_warning:

	mia_diag_entry_s* const d = mia_diag_error(c->diag, mia_diag_E0599, node->span, "`%s` is wrong", name);
	mia_diag_label_main(d, "the label under the span");
	mia_diag_label(d, other->span, "a secondary label");
	mia_diag_note(d, "a note"); mia_diag_help(d, "advice");

a new function of the standard library implemented in c: declare it in
its module (stdlib/io.mia) with '[plugin = "io"]', and add it to that module's
plugin (stdlib/io.c, registered under its qualified name 'io.function'). a
new module with a plugin is a pair, stdlib/<name>.mia and stdlib/<name>.c;
the makefile builds the plugin beside the module. a function of a user's
own is a plugin of its own; see 'plugins' below.

a new instruction: add it to opcodes.def, emit it in source/compiler.c, give
its stack effect to _mia_vm_effect and run it in _mia_vm_step (source/vm.c),
with a fast case in _mia_vm_loop if it is frequent (an _mia_vm_x_ operation:
its case, its decoding in _mia_vm_decode and its entry in the loop's jump
table, which the debug build checks); bytecode.c reads and writes it from
the table. an instruction that moves values of any
type takes a type code (BYTES for structs and arrays) and the value's size
as its last operand, so one instruction serves every type.

a new scalar type: a row in tokens.def (the name in programs) and one in
codes.def (its type code); the compiler maps one to the other by name.

a new command: write it in source/cli.c and add a row to _mia_cli_commands.

a new system: implement include/mia/system.h in source/system/<name>.c
and pick it in the makefile (SYSTEM) and build.bat.


plugins
-------

a plugin provides functions to mia programs, written in c (or anything that
can export a c function). the program declares them in a mia module with
'[plugin = "name"]' and no body; the plugin is a shared library, best named
like that module and kept beside it (geometry.mia, geometry.so), so that a
library is one folder. mia looks a plugin up by its name in the plugin
directories: the program's, the standard library's, then those of '-P'.
only these directories are searched, never the directory of some module the
program imports: a plugin runs native code, and its checks run at compile
time. the standard library works the same way: memory.alloc, io.println
and the rest come from its plugins memory and io (stdlib/memory.c,
stdlib/io.c), which the makefile builds beside memory.mia and io.mia.

a complete example, a plugin 'geometry' with two functions (a check, which
runs at compile time, is in examples/plugins/math/math.c). the mia side,
geometry.mia in a module directory of the program:

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

	#include "mia/plugin.h"

	#include <math.h>
	#include <string.h>

	// the same layout as the mia struct: c and mia agree on it
	typedef struct {
		double x;
		double y;
	} point_s;

	static mia_plugin_value_s _distance(mia_plugin_call_s* const call) {
		point_s a, b;
		memcpy(&a, mia_plugin_get_memory(call, 0), sizeof(a));
		memcpy(&b, mia_plugin_get_memory(call, 1), sizeof(b));
		return mia_plugin_make_f64(call, hypot(a.x - b.x, a.y - b.y));
	}

	static mia_plugin_value_s _total(mia_plugin_call_s* const call) {
		const mia_plugin_slice_s points = mia_plugin_get_slice(call, 0);
		double sum = 0.0;
		for (uint64_t i = 0; i < points.length; ++i) {
			point_s point;
			memcpy(&point, (const char*)points.data + i * sizeof(point), sizeof(point));
			sum += point.x + point.y;
		}
		return mia_plugin_make_f64(call, sum);
	}

	mia_plugin_export bool mia_plugin_init(mia_plugin_api_s* const api) {
		if (!mia_plugin_compatible(api, mia_plugin_api_version)) return false;
		return mia_plugin_register(api, "geometry", "geometry.distance", _distance, NULL)
			&& mia_plugin_register(api, "geometry", "geometry.total", _total, NULL);
	}

a program using it, main.mia beside geometry.mia:

	import geometry
	from io import println

	pub main :: () -> int {
		let a: geometry.Point = { x: 0, y: 0, };
		let b: geometry.Point = { x: 3, y: 4, };
		let points: [2]geometry.Point = { a, b, };
		println("{} {}", geometry.distance(a, b), geometry.total(points[..]));   # 5.0 7.0
		return 0;
	}

building the plugin (it needs only include/mia/plugin.h: it calls mia through
the table the handles carry, so it links against nothing of mia):

	linux, wsl:     cc -std=c11 -O2 -fPIC -shared -I<mia>/include geometry.c -o geometry.so -lm
	macos:          cc -std=c11 -O2 -fPIC -shared -I<mia>/include geometry.c -o geometry.dylib
	mingw:          gcc -std=c11 -O2 -shared -I<mia>/include geometry.c -o geometry.dll
	msvc:           cl /nologo /std:c11 /O2 /LD /I<mia>\include geometry.c /Fe:geometry.dll

and running it:

	mia run main.mia                                 (geometry.so beside main.mia: .so, .dylib or .dll)
	mia run -P libs/geometry main.mia                (the plugin in another directory)

the rules:

- names. a plugin function is registered under its qualified name: the
  dotted name of the module that declares it, then the function
  ('geometry.distance'; 'tools.text.split' for a function 'split' of
  tools/text.mia; the entry file's module is 'main'). when no plugin
  loaded so far provides a function, mia looks up the plugin its
  '[plugin = "name"]' names ('tools.text' is tools/text.so in a plugin
  directory), which must be the one that provides it. a call of a function
  that no plugin provides is E0416, at check time, so 'check', 'build' and
  'run' all need the plugins. a plugin that cannot be loaded, needs a newer api, is not the
  plugin its file is named for, or provides a function another plugin
  provides already is E0603.
- arguments. the checker types every call from the declaration, so the run
  function can trust the number and the types of its arguments. scalars
  come with mia_plugin_get_int / _uint / _f64 / _bool, slices with
  mia_plugin_get_slice (data, length, element type, read-only), and any
  argument as its c bytes with mia_plugin_get_memory. a variadic function
  ('values: ...') gets the variadic values after the fixed ones, each with
  its own type (mia_plugin_argument_type). type arguments of a generic
  function ('alloc<T>') come with mia_plugin_type_argument.
- memory. a struct, an array or a slice's elements are laid out as c lays
  out the same fields (see 'the machine'); the type functions
  (mia_plugin_type_size, _alignment, _field_name, _field_offset, ...) say
  where everything is, so a plugin can also read types it does not know in
  advance. a slice or a function value stored inside such memory is read
  with mia_plugin_slice_at and mia_plugin_function_name_at. the memory of an
  argument is valid during the call only.
- results. mia_plugin_make_int / _uint / _f64 / _bool / _none / _empty; the
  value is converted to the declared result type (integers wrap, f32
  rounds). a new heap slice comes from mia_plugin_heap_alloc (zeroed,
  checked like memory from 'alloc', freed with memory.free) or
  mia_plugin_heap_adopt (memory the plugin allocated; mia calls its release
  function).
- errors. mia_plugin_fail stops the program with a runtime error, shown like
  mia's own (the call's line and the chain of calls).
- checks. a function may register a check, which runs at compile time for
  every direct call: it sees the static types of the arguments and their
  values when the compiler knows them (mia_plugin_check_constant), reports
  errors and warnings with labels, notes and help (E0531 and W0531 unless
  it gives a code of mia's own, like the io plugin's E0530 for format
  strings), and can mark arguments with flags that the run function reads
  back (mia_plugin_argument_flags; the io plugin marks "..."c strings that
  way).
- values. a plugin function can be used as a function value
  ('apply(geometry.distance, a, b)'), except a variadic one.
- exit. a plugin that keeps something between calls (a buffer, a handle)
  exports mia_plugin_exit, which mia calls before it unloads the library.
- versions. mia_plugin_api_version is 1. the table of functions only grows;
  a plugin asks mia_plugin_compatible with the version it was built with.
  mia_plugin_export exports mia_plugin_init (__declspec(dllexport) on
  windows).

examples/plugins/math is a fuller example: numbers, slices, heap memory, a
struct read through its layout, a runtime error and a check that warns.
'make' builds it (build/<config>/examples/plugins/math.so) and the golden
programs plugin_* run it; the plugins of tests/plugins/ ('make
test-plugins') test the api, misuse included. to run it:

	build/release/mia run -P build/release/examples/plugins examples/plugins/math/main.mia


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
with fast cases for the frequent ones. with gcc and clang every case ends in
its own jump to the next (computed goto; the portable build and msvc use a
switch), and an integer comparison with the conditional jump after it runs
as one case (still counted as two instructions). so do, when the module is
loaded, the runs of instructions that profiles show most: a word local and
a constant (or their sum or difference, also stored to a local), two word
locals, and an element of a slice local at a word local index (or its
location). every instruction of a run is counted, a limit inside a run
stops where it would have, and an error names the instruction that has it.
the compiler makes the conditions of 'if' and 'while' jumps: '&&', '||' and
'!' build no bool.

the compiler also folds an operation on literals and constants ('2 *
SIZE', 'ORIGIN.x', 'TABLE[3]') into one constant, computed by the
compile-time evaluator as the machine would compute it (what would stop the
machine, like a division by zero, is left to it), and a condition known at
compile time into a plain jump or nothing. 'while' is tested at the bottom
of its loop, so a round ends in the test's jump back; 'for' tests its range
once, then RANGE_NEXT steps it and jumps back in one instruction, and its
variable is the range's counter itself unless the body assigns it or lends
it. a last pass over each function takes a jump to a jump straight to the
end, makes a jump to a return a return, turns a conditional jump over a jump
into the opposite jump and removes what no path reaches.

two commands show what the machine gets and does. 'mia dis' lists a
program's bytecode: its types and plugin functions, then every function's
instructions under the source lines they come from, with the constant or
function an operand names and '>' where a jump lands. 'mia run --profile'
runs every instruction through the general case, counting it (slower, but
the same instructions, so the same counts and limit as a normal run), then
prints the functions, source lines and instructions that ran most, and the
pairs of instructions that ran one right after the other: the candidates
for a fast case or a fused one.

there is no garbage collector: nothing owns memory except heap blocks,
which 'free' gives back; what is left is freed when the machine stops
('--stats' counts the allocations). see source/vm.c and
include/mia/bytecode.h.

benchmarks (release build, the bytecode alone, best of nine runs, wsl on
one machine, milliseconds): before the work of 2026-10-19, after its
dispatch, call and condition part, and now, with the loop layout, folding
and fused runs too:

	              before   dispatch   now
	fib             185        86      85
	sieve           167       125      86
	sort            192       131      99
	structs          58        43      43


bytecode files
--------------

'mia build' writes the compiled program as a binary .miab file, which 'run'
takes like a source file. its numbers are varints (7 bits a byte; signed
ones zigzag, so small ones of either sign take a byte) and each source
position is written as what changed from the one before: the magic "MIA\0",
the format version (4 bytes, now 5), then the sections, each a count of
entries:

	files        the source files (for runtime errors)
	types        name, kind, type code, size, alignment, element, length, zero
	             value, fields (name, type, offset)
	constants    integers, floats, bools, text, functions, lists and slices
	intrinsics   the plugin functions the program calls: qualified name, plugin,
	             result and parameter types, flags marked by checks
	functions    name, parameters, frame size, parameter slots, instructions
	             (opcode, type codes, operands) and a source position per instruction

and finally the entry function and whether it returns a value. include/
mia/bytecode.h has every field and every instruction. the reader checks
every count, index and size, and the machine verifies the functions, so a
truncated or corrupted file is refused with a message (the program
bytecode_bad_files tests some, made by its fixtures.py). before anything
runs, the machine also checks every type's layout (fields and elements
inside its size, scalar sizes that match their codes) and every function's
parameters (inside its frame), since constants and plugins read values by
those layouts. a runtime error shows a snippet of the source only when the
file the bytecode names is a .mia file. plugin functions are found by name
when the file is loaded (their plugins looked up as for a source file), so
an .miab file needs the same plugins as its source.


how it works
------------

the pipeline: lexer -> parser (recursive descent, setjmp recovery) ->
loader (modules, import cycles) -> checker (types, generics by
monomorphization, references, assignability, flow, plugin checks,
compile-time evaluation of constants in evaluator.c) -> compiler (checked
program -> stack bytecode) -> verifier -> virtual machine.

memory: the front end allocates syntax trees, symbols and types in arenas,
freed all at once. the parser and the evaluator unwind with longjmp; every
vector they fill lives in an arena too, so unwinding leaks nothing. types are
interned, so two types are equal exactly when their pointers are.

numbers: literals and compile-time values are 128-bit integers (mia_int128_s,
include/mia/int128.h) and doubles, wrapped to their type exactly, so constant
folding and overflow warnings are exact (an untyped constant can exceed 64
bits until it gets its type).

formatting: floats are written as the shortest text that reads back as the
same number (f32 with f32 digits), integers in full; nan and infinities
are spelled by mia, and programs write the same bytes on every system
(windows writes '\n', not '\r\n').
