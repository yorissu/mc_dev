
mc - the c implementation
=========================

a port of the python implementation (../mc) to c11: the same language, the
same diagnostics, the same bytecode (.mcb files are interchangeable) and the
same program output. it is about 10 to 15 times faster than the python
implementation, and it adds better error reporting and a fuller command line.

builds with gcc (or clang) on linux or wsl; no dependencies besides libc and
libm.


building and testing
--------------------

	make                    debug build (address and undefined behaviour sanitizers): build/debug/mc
	make CONFIG=release     optimized build: build/release/mc
	make test               unit tests (cairo, tests/*_tests.c)
	make check              unit tests, then the differential tests
	make clean

the differential tests (tests/differential.py) run every program of the
python test suites, the examples and the standard library, plus randomly
mutated copies of them, through both implementations and compare what they
print: tokens, syntax trees, diagnostics, instantiated programs, bytecode,
program output, exit status and runtime errors.

	python3 tests/differential.py build/debug/mc [--commands tokens,ast,check,mono,dis,run]
		[--mutations N] [--seed S] [--only TEXT] [--verbose]

the python implementation must stay runnable from the parent directory
(python3 -m mc), since it is the reference.


usage
-----

	mc run <file.mc|file.mcb>   check, compile and run a program; exits with what main returns
	mc build <file.mc> [-o out] compile to bytecode
	mc dis <file.mc|file.mcb>   print the bytecode
	mc check <file.mc>          check a program and every module it imports
	mc mono <file.mc>           print every generic instance, every expression typed
	mc ast <file.mc>            print the syntax tree of one file
	mc tokens <file.mc>         print the tokens of one file
	mc explain [code]           explain a diagnostic code, or list them all

	-I <dir>                    another module directory (after the file's own)
	--stdlib <dir>              the standard library (default: $MC_STDLIB, then ../stdlib)
	--format human|short        diagnostics as annotated snippets or one line each
	--color[=auto|always|never] colors (auto: stderr is a terminal and NO_COLOR is unset)
	--werror                    warnings count as errors
	--limit <n>                 run: stop after n instructions
	--stats                     run: print the instructions executed and collections
	-h, --help                  'mc --help', 'mc <command> --help'
	--version

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

	include/mc/global.h        panics, assertions, checked allocation
	include/mc/vector.h        growable arrays (mc_vector_of)
	include/mc/arena.h         arena allocation (syntax trees, symbols, types)
	include/mc/string.h        views (mc_view_s) and growable strings (mc_string_s)
	include/mc/map.h           string-keyed hash map
	include/mc/source.h        source files, spans, line and column lookup
	include/mc/diag.h          diagnostics and their rendering (snippets, labels, notes)
	include/mc/format.h        python-compatible number and text formatting
	include/mc/lexer.h         tokens
	include/mc/ast.h           syntax tree nodes and their annotations
	include/mc/parser.h        recursive-descent parser with error recovery
	include/mc/printer.h       syntax tree printer
	include/mc/types.h         interned semantic types, unification
	include/mc/program.h       the checked program: modules, symbols, instances
	include/mc/bytecode.h      instructions (documented there), .mcb files, disassembler
	include/mc/compiler.h      checked program -> bytecode
	include/mc/vm.h            the virtual machine
	include/mc/cli.h           the command line

	source/check.h             the checker's internals, shared by:
	  source/load.c              module search and loading, import cycles
	  source/check.c             programs, lookup, declarations, instances
	  source/check_types.c       types, struct layouts, sizes
	  source/check_statements.c  statements, places, flow
	  source/check_expressions.c expressions, calls, operators, literals
	  source/evaluate.c          compile-time evaluation
	  source/mono.c              the 'mono' listing
	source/machine.h           the virtual machine's internals, shared by:
	  source/vm.c                loading, the instruction loop, runtime errors
	  source/vm_memory.c         objects, the garbage collector, slices, locations
	  source/vm_builtins.c       builtins, natives, printing values

	tests/main.c               runs the unit tests
	tests/*_tests.c            unit tests (they register themselves with cairo)
	tests/helpers.h            runs a program from its source text
	tests/differential.py      the comparison with the python implementation


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

a new diagnostic: add a row to diagnostics.def (and the same code to the
python CODES table), then report it with mc_error / mc_warning:

	mc_diagnostic_s* const d = mc_error(c->diag, mc_E0599, node->span, "`%s` is wrong", name);
	mc_label_main(d, "the label under the span");
	mc_label(d, other->span, "a secondary label");
	mc_note(d, "a note"); mc_help(d, "advice");

a new builtin or native: write the function in source/vm_builtins.c and add a
row to _mc_builtins or _mc_natives there; declare it in a module of stdlib/
with '[builtin]' or '[extern = "name"]'. the checker enforces the signature,
so the function can trust its arguments.

a new instruction: add it to opcodes.def, emit it in source/compiler.c and
run it in _mc_step (source/vm.c); bytecode.c reads, writes and disassembles
it from the table. keep the python implementation in step, or the
differential tests will say where they differ.

a new command: write it in source/cli.c and add a row to _mc_commands.


how it works
------------

memory: the front end allocates syntax trees, symbols and types in arenas,
freed all at once. the parser and the evaluator unwind with longjmp; every
vector they fill lives in an arena too, so unwinding leaks nothing. types are
interned, so two types are equal exactly when their pointers are.

numbers: literals and compile-time values are 128-bit integers and doubles,
wrapped to their type exactly like the python implementation (which uses
unbounded integers), so constant folding and overflow warnings agree.

the machine: values are integers, floats, bools, function values and
references to heap objects: lists (arrays and structs, copied on
assignment, written in place), blocks (the memory slices view, owned by the
heap, a call or another block), locations (assignment targets and reference
arguments) and frame records (the locals of a call, alive until it returns).
every access through a slice or a location checks that its memory is still
alive and in bounds. a mark-and-sweep collector runs between instructions
once enough has been allocated.

python compatibility: number formatting (repr of floats, shortest f32
digits), text decoding (invalid utf-8 becomes U+FFFD like python's
'replace'), the order of the constant pool and of compiled functions, and
even the order in which the checker probes values (which the compile-time
step limit counts) follow the python implementation, so both produce the
same output byte for byte.
