
# mc rework: plugins, C-like memory layout and language updates

You are taking over the **mc** programming language project ("modernized c",
files `.mc`). This prompt holds everything you need: the project, the decisions
the user has made, the specifications, and an ordered list of tasks with how to
verify each one. The user may be away while you work. **Do not stop to ask
questions.** Make a reasonable choice, write it down in `mc/questions.txt`
(section "decided during the rework" or "open questions"), and keep going.

Before you start, check that the repository contains the `mc/` directory with
`mc/c/` (the C implementation), `mc/stdlib/`, `mc/examples/` and the
Python implementation `mc/mc/`. If it is missing, stop and say so.


## 1. The project

```
mc/
  readme.txt          the language as implemented, the python implementation
  questions.txt       open questions with the user's answers (read it)
  mc/                 python implementation: the old reference, being dropped
  tests/              python unit tests (python -m unittest discover -s tests)
  stdlib/             memory.mc, io.mc, option.mc, result.mc
  examples/           tour/ (every feature), vector/
  c/                  the C implementation: THIS is what you work on
    readme.txt        architecture, how to extend, how to build and test (read it)
    makefile          make | make CONFIG=release | make test | make check | make clean
    include/mc/*.h    public header of each stage
    include/mc/*.def  x-macro tables: diagnostics, tokens, ast, operators, opcodes
    source/           check.h (checker internals), machine.h (vm internals), *.c
    tests/            cairo unit tests (*_tests.c, helpers.h runs whole programs),
                      differential.py (compares with the python implementation)
```

Pipeline: lexer -> parser (recursive descent, setjmp recovery) -> loader
(modules, import cycles) -> checker (types, generics by monomorphization,
references, mutability, flow, compile-time evaluation of constants in
`evaluate.c`) -> compiler (`compiler.c`, checked program -> stack bytecode) ->
virtual machine (`vm.c` instruction loop, `vm_memory.c` objects + mark-and-sweep
GC, `vm_builtins.c` builtins/natives/printing). `cli.c` has a commands table:
`tokens ast check mono build dis run explain`, options `-I --stdlib -o --format
human|short --color[=auto|always|never] --no-color --werror --limit --stats
--version -h`.

State at handover: C and Python agree byte for byte on 5208 differential
comparisons (all commands, mutated programs); 32 cairo unit tests pass in debug
(ASan + UBSan) and release; the release VM is ~13x faster than Python.

### The language in brief (read `mc/readme.txt` and `examples/tour/main.mc`)

- Declarations `name :: ...`: functions `add :: (a: int, b: int) -> int { }`,
  structs `Point :: { let x: int; let mut y: int = 0; }` (fields with `let`,
  optional `mut` and default; empty structs are errors), constants
  `TEN :: 10;` (evaluated at compile time, user functions included).
- Generics on the name, types only, monomorphized: `max<T> :: (a: T, b: T) -> T`,
  `Pair<A, B> :: { ... }`; calls `max<int>(1, 2)` or inferred.
- Scalars `s8..s64 u8..u64 int uint f32 f64 float bool`; C-style casts `(u8)x`
  between number types only. Integer overflow wraps; known-value overflow warns
  (W0701) unless `[wrap]`.
- Arrays `[N]T` / `[_]T` (values), slices `[]T` (views of arrays or heap
  memory, bounds and liveness checked at run time), `null` (the empty slice).
  No pointers. References `&T` / `mut &T` only as parameters; call site `&x` /
  `mut &x`.
- Go-style `mut` on names only (`let mut x`, `mut p: T`) plus `mut` fields.
- Operator functions `[op._+_]`, `[op._[_]]`, ... (mixfix, exact operand order;
  `!=`, `>`, `<=`, `>=`, `op=` are derived).
- Strict precedence: mixing operator groups without parentheses and chained
  comparisons are errors (E0202).
- Lambdas without captures; function values. Strings `"abc"` are `[3]u8`,
  `"abc"c` adds a NUL. `for i in a..b` / `a..=b` (counts down when a > b),
  `while`, `if/else`, `break`, `continue`. A discarded result is an error
  (`_ = f();`). Intrinsics `sizeof(T)`, `lenof(x)`, `assert(c, "msg")`.
- Today `[builtin]` (alloc/realloc/free/copy/print/println) and
  `[extern = "name"]` (write/write_line) functions exist. Variadics
  `values: ...` are allowed only on builtins; a `[]u8` parameter right before
  `...` is a compile-time checked format string with `{}` placeholders and
  `{{` `}}` escapes.


## 2. Conventions (follow them exactly)

- Tabs for indentation. Every file starts with one empty line and ends with
  exactly one newline. LF line endings.
- C style of the codebase (mira style): `mc_` prefix for public names,
  `_mc_` for static helpers, `_s` struct typedefs, `_e` enums, `const`
  parameters (`const T* const p`), aligned struct fields, `///` doc comments
  on public declarations, x-macro `.def` tables for every list. C11, builds
  warning-free with the makefile's strict flags (`-Werror`), in both
  `CONFIG=debug` (sanitizers) and `CONFIG=release` (which adds
  `-Wnull-dereference` false positives at -O2; fix them, do not disable).
- Lowercase `makefile`. Unit tests with cairo (`tests/cairo/cairo.h`), one
  `*_tests.c` per area; tests register themselves.
- Comments explain why, lowercase sentences, like the existing code.
- **Do not commit or push unless the user tells you to in this session.**
  Keep the work in the working tree (or on a branch if the environment needs
  one), and say at the end what is where.


## 3. Decisions the user made (these are settled)

From `mc/questions.txt` and the follow-up discussion:

1. **`[builtin]` and `[extern]` are removed.** Replaced by **plugins**
   (section 4). Functions provided by a plugin are declared in mc with the
   attribute **`[plugin]`** and no body. A bodiless function without `[plugin]`
   is an error ("`area` has no body"). `[plugin = "name"]` may name the plugin
   that must provide it (optional).
2. **Plugin functions can be used as values** (the user wanted externs usable
   as values; this carries over), e.g. `apply(math.sqrt, 2.0)`.
3. **C-like raw memory layout in the VM** (section 5): values stored the way C
   stores them, for speed and memory efficiency. The user wants mc to be as
   fast as possible.
4. **Format specifiers** in format strings (section 6.1), checked at compile time.
5. **Attributes on any statement**: `[wrap]` (and `[uninit]` where it makes
   sense) may be put on blocks, loops and ifs and then cover everything inside.
6. **Trailing NUL stripping only for `"..."c` strings** (section 6.3).
7. **`main` may take the command line**: `pub main :: (arguments: [][]u8) -> int`.
8. **Limits become options**: compile-time evaluation steps (today a fixed
   1,000,000, E0705), call depth (today 10,000), instruction limit (`--limit`).
9. **Binary `.mcb` format** instead of JSON, for speed.
10. **Cross-platform including MSVC.**
11. **E0705 (evaluation too long) points at the loop or call that ran too long**,
    not at whatever node happened to take the last step.
12. **Runtime overflow keeps wrapping silently** ("runtime is the user's land").
13. **Identifiers stay ASCII.**
14. **Field-level `mut`: leave as is**, it will be reworked separately later.
15. **The Python implementation is being dropped.** Do not update it, do not
    keep parity with it, do not delete it either (the user decides that). The
    differential tests stop being the safety net (task 0 replaces them).
16. **The user writes the standard library himself.** You only provide the
    plugin mechanism, the built-in `std` plugin with the functions the current
    stdlib needs, and adapt `stdlib/*.mc` to the new declarations so the
    examples keep working.

Not decided; **keep the current behaviour and do not change it**: `null` vs
empty slice (today `s == null` is true for every empty slice; the user leans
towards one kind of empty), JSON diagnostics, `alloc` with a negative count,
the 2^28 element allocation cap, leak reports. Record anything new you run into
in `questions.txt`.


## 4. Spec: plugins

### 4.1 mc side
```
# stdlib/memory.mc
[plugin]
pub alloc<T> :: (count: uint) -> []T;

[plugin]
pub free<T> :: (data: []T) -> []T;

# stdlib/io.mc
[plugin]
pub println :: (format: []u8, values: ...);

# a user's own module, provided by their plugin
[plugin = "mymath"]
pub sqrt :: (x: f64) -> f64;
```
- An intrinsic is identified by its **qualified name**: the dotted module name
  plus the function name, e.g. `memory.alloc`, `io.println`, `math.sqrt`.
  (Decide how module names are formed for files outside the stdlib, document
  it; the module name the loader already computes is the natural choice.)
- The checker treats a `[plugin]` declaration like any function signature:
  calls are type-checked from the mc declaration, generics instantiate
  normally, `values: ...` is allowed on `[plugin]` functions (and only there,
  as today on builtins).
- When checking, every `[plugin]` function that is used must be provided by a
  loaded plugin; otherwise a diagnostic (new code in `diagnostics.def`), e.g.
  "no plugin provides `math.sqrt`" with help "load one with `--plugin <file>`".
  If `[plugin = "x"]` names a plugin, the message names it.
- The format-string check moves out of the checker into the `std` plugin's
  compile-time check hook (4.3). The checker keeps the generic part: a `[]u8`
  parameter right before `...` must be a compile-time known value, and the
  types of the `...` values are recorded on the call (as `note.variadic` today).

### 4.2 plugin API (`include/mc/plugin.h`, stable, versioned)
A plugin is C code with one entry point:
```c
#include "mc/plugin.h"

static mc_plugin_value_s _sqrt(mc_plugin_call_s* const call) {
	return mc_plugin_make_f64(call, sqrt(mc_plugin_get_f64(call, 0)));
}

MC_PLUGIN_EXPORT bool mc_plugin_init(mc_plugin_api_s* const api) {
	if (!mc_plugin_compatible(api, MC_PLUGIN_API_VERSION)) return false;
	mc_plugin_register(api, "mymath", "math.sqrt", _sqrt, NULL);   // run, optional check
	return true;
}
```
The API must give plugins, through functions and opaque handles only (never
VM structs, so the VM's layout can change without breaking plugins):
- **run time**: argument count; read argument i as integer (signed/unsigned
  64-bit), float, bool, slice (element type, length, a pointer to its bytes for
  reading/writing, honoring read-only), array, struct field, function value;
  the call's **type arguments** (`T` of `alloc<T>`) and the **types of the
  `...` values** as type descriptor handles; make results (integer, float,
  bool, slice, nothing); allocate a VM-tracked heap block (so slices into it
  keep bounds and use-after-free checks; the plugin may get the memory from
  `malloc` but the VM owns the bookkeeping) and mark one freed; write to the
  program's output; stop with a runtime error (message, like today's
  `mc_vm_fail`); calling an mc function value (optional, if cheap).
- **type descriptors**: kind (int/float/bool/array/slice/struct/function),
  name, size and alignment (C layout), integer bits/signedness, element type,
  array length, field count, field name/type/offset.
- **compile time (optional check hook)**: per call, the argument count, the
  static type of each argument, the compile-time value of an argument if known
  (at least integers, floats, bools and `[]u8`/`[N]u8` byte strings), the
  variadic types, and reporting an error or warning with a label on argument i
  or on the call, plus notes/help, through the normal diagnostics.
- versioning: `MC_PLUGIN_API_VERSION`; incompatible plugins are refused with a
  clear message.

### 4.3 the `std` plugin
Compiled into `mc` (static registration through the same API, e.g. a list of
init functions), always loaded. Provides what the stdlib needs today, with the
same runtime behaviour and error messages: `memory.alloc`, `memory.realloc`,
`memory.free`, `memory.copy`, `io.print`, `io.println` (with the compile-time
format check, now including specifiers, section 6.1), `io.write`,
`io.write_line`. Put it in its own source files (e.g. `source/plugins/std_*.c`)
so it is the model for user plugins.

### 4.4 loading and bytecode
- `--plugin <file>` (repeatable) loads a shared library (`dlopen`/`dlsym` on
  POSIX, `LoadLibrary`/`GetProcAddress` on Windows) behind one small platform
  wrapper. Also accept a plugin directory (e.g. `MC_PLUGINS` env var or
  `--plugin-dir`); document the choice.
- Plugins are needed both when checking (check hooks) and when running.
- Bytecode: a `CALL_INTRINSIC [index, n]` and a `INTRINSIC [index]` (function
  value) instruction; the module has an intrinsics table with the qualified
  names (plus type argument/variadic type descriptors per use, like builtins
  today). `mc run file.mcb` loads plugins, links every name before the program
  starts, and reports a missing one as an error (not a crash).
- Remove `[builtin]`, `[extern]`, `CALL_BUILTIN`, `CALL_NATIVE`,
  `FUNCTION_BUILTIN` and their tables; update `diagnostics.def` (retire or
  reuse codes; keep codes stable where meaning is unchanged).
- Provide an example user plugin under `c/examples/plugins/` (e.g. `math`
  with sqrt/sin/pow) with a makefile target that builds it as a shared library,
  and a test that loads it and runs a program using it.


## 5. Spec: C-like raw memory layout in the VM

Today every value is a 32-byte tagged `mc_value_s` (kind + `__int128`), arrays
and structs are GC lists of those, slices view "blocks" wrapping lists. Goal:
store data the way C does.

- Scalars use their real size (`u8` = 1 byte, `s32` = 4, `int`/`uint`/`float`
  = 8, `bool` = 1). Arrays are contiguous elements; structs are fields in
  declaration order with C alignment and padding (the checker already computes
  size and alignment: `mc_size_of`; use one definition for both).
- Slices are `{block, offset (bytes or elements), length}`; a slice still
  carries what is needed for the safety checks: bounds, block alive (freed
  heap, returned call), read-only flag. Function values are a small tagged
  word. Locations (assignment targets, reference arguments) become
  `{block or frame, offset}` with the same liveness/read-only rules.
- Frames get a byte area for their locals laid out by the compiler (slots by
  offset and size, not one value per slot); the operand stack holds 64-bit
  words (integers, doubles, bools, function values), and aggregates are moved
  by address + `memcpy` (or a separate aggregate stack); choose the design that
  is simplest and fast, and document it in `machine.h`.
- Typed instructions already exist per scalar type; add width-specific loads
  and stores (or a load/store with a size operand) instead of tags. 128-bit
  arithmetic is no longer needed at run time: values are at most 64 bits and
  wrap by their type.
- **Keep every safety guarantee** the VM has today, with the same messages:
  index and slice bounds, use of freed heap memory, use of a local array of a
  function that has returned, write through a read-only slice, division by
  zero, negative shift count, stack overflow, the instruction limit, the
  runtime error trace. Keep the GC (or replace it by something simpler that is
  still leak-free: e.g. frame memory freed on return + heap blocks freed by
  `free` + liveness generations instead of tracing) and document the choice.
- Output of every program must stay identical (the golden tests of task 0
  are the proof), including float formatting and runtime error messages.
- Measure: add a small benchmark (e.g. `c/bench/*.mc` and a `make bench`
  target printing times) and report before/after numbers.


## 6. Spec: language updates

### 6.1 format specifiers
`{}` keeps working; add `{:spec}` with
`spec = [[fill]align][sign][#][0][width][.precision][type]`:
- align `<` left, `>` right, `^` center; fill any ASCII character except `{}`;
  sign `+` (always) or `-` (default); `#` alternate form (`0x`, `0b`, `0o`);
  `0` pads numbers with zeros after the sign; width and precision are decimal
  numbers (no nested `{}` arguments).
- types: integers `d` (default) `x` `X` `o` `b`; floats `f` `e` `E` `g`
  (default = today's shortest repr); `.precision` on floats = digits after the
  point (`f`, `e`) or significant digits (`g`); width/fill/align apply to every
  value, including bools, slices of u8 (text) and structs (the whole rendered
  text).
- compile-time errors (E0530, with a label on the format string and, where
  possible, on the offending value): unknown spec character, a type letter
  that does not fit the value's type (`{:x}` on a float, `{:.2}` on an int),
  unclosed `{`, stray `}`, placeholder/value count mismatch (already exists).
- The check is the `std` plugin's check hook; the formatting itself is in the
  plugin's run function. Unit-test the spec parser and the formatting.

### 6.2 attributes on any statement
`[wrap]` before a block, `while`, `for` or `if` suppresses W0701 for every
statement inside (nested functions/lambdas excluded). `[uninit]` keeps its
current meaning on lets; reject it (existing "not allowed here" diagnostic)
where it means nothing. Update the parser (attributes before any statement) and
the checker; keep diagnostics for misplaced attributes.

### 6.3 trailing NUL only for c strings
Today printing any `[N]u8` / `[]u8` drops one trailing 0 byte. New rule: the
NUL is dropped only when the printed value comes from a `"..."c` literal (or a
constant initialized from one, or a slice of either), known at compile time.
Simplest acceptable implementation: the compiler/check hook marks such
arguments (e.g. a per-argument flag in the call's variadic info) and the
formatter strips one trailing NUL only for marked arguments; everything else
prints all bytes. Document the rule in `stdlib/io.mc`.

### 6.4 main with arguments
`pub main :: () -> int`, `pub main :: ()` and
`pub main :: (arguments: [][]u8) -> int` (and without `-> int`) are valid; any
other signature is an error with a help showing the valid forms.
`mc run file.mc a b c` passes `["file.mc", "a", "b", "c"]` (arguments after
the file; support `--` to end mc's own options). The argument data is
read-only heap memory the program cannot free.

### 6.5 limits as options
`--eval-steps <n>` (default 1,000,000), `--call-depth <n>` (default 10,000),
`--limit <n>` (exists). Messages state the limit and the option to raise it.

### 6.6 E0705 position
When the step limit is hit, report at the innermost **loop or call** that is
still running in the evaluation (the `while`/`for` statement, or the call
expression of the function that loops), with a note naming the function, e.g.
"the loop in `collatz` runs more than 1000000 steps", plus help mentioning
`--eval-steps`.


## 7. Spec: binary `.mcb`

- Little-endian, versioned: magic `"MCB\0"`, format version, then sections
  (strings, files, types, constants, intrinsics, functions with code and
  positions). Integers as fixed-width or LEB128; document the layout in
  `bytecode.h`.
- Must round-trip exactly (`build` then `dis` of the `.mcb` = `dis` of the
  source), reject other or corrupted files with a clear error (never crash:
  bounds-check every read), and load fast. Drop the JSON format (no need to
  stay compatible with the Python implementation).


## 8. Spec: cross-platform and MSVC

- No `__int128` in code compiled by MSVC. Introduce `include/mc/int128.h`: a
  small 128-bit integer type with the operations the lexer, checker and
  evaluator need (add, sub, mul, div/mod with C truncation, shifts, compare,
  to/from text), implemented with `__int128` where available and portable code
  otherwise; unit-test both paths (e.g. force the portable one with a define).
- A platform layer (`include/mc/platform.h` + `source/platform_*.c`):
  `isatty`, shared library loading, temporary directories, anything else
  POSIX-only. The tests' `mkdtemp`/`open_memstream` go through it or are
  replaced.
- Build: keep the `makefile` for gcc/clang (Linux, macOS, WSL, MinGW) and add
  an MSVC build (a `build.bat` for `cl` or an nmake `makefile.msvc`; document
  it). You may not be able to run MSVC in your environment: then at least
  compile with `-std=c11 -Wpedantic` without GNU extensions in the portable
  path and state clearly in the final report what was not verified.
- `MC_PLUGIN_EXPORT` expands to `__declspec(dllexport)` on Windows.


## 9. Tasks, in order

Work through these in order; after each, everything must build warning-free in
debug and release and all tests must pass. Keep `c/readme.txt` current as you
go.

0. **Golden tests (do this first, before changing behaviour).** Build the
   current C implementation. Create `c/tests/programs/`: one directory or file
   set per program with the source (`*.mc`, extra modules beside it) and the
   expected `stdout`, `stderr` (short format, no color) and exit status of
   `mc run`, `mc check`, and `mc dis` where useful. Seed it from the programs
   the differential test uses (python test suites' sources under `mc/tests/`,
   `examples/`, `stdlib/`) and add programs that hit every runtime error. A
   runner (C, inside `mc_tests`, or a small script with no Python dependency is
   preferred since Python is being dropped) compares, and `make golden`
   regenerates the expectations. When a later task changes behaviour on
   purpose (bytecode listings, new diagnostics), regenerate and review the
   diff; never regenerate to hide an unintended change. Remove `make check`'s
   dependency on Python (keep `tests/differential.py` in the tree but out of
   the default targets).
1. **Plugins** (section 4), including the `std` plugin, `[plugin]`, removal of
   `[builtin]`/`[extern]`, plugin functions as values, `--plugin`, the
   example plugin and its test, updated `stdlib/*.mc`.
2. **C-like memory layout** (section 5). Benchmark before and after.
3. **Binary `.mcb`** (section 7).
4. **Language updates** (section 6.1 to 6.4).
5. **Limits and E0705** (sections 6.5, 6.6).
6. **Cross-platform / MSVC** (section 8).
7. **Docs and wrap-up**: `c/readme.txt` (architecture, plugin guide with a
   complete example, memory layout, bytecode format, build on every platform),
   `stdlib/*.mc` comments, `mc/readme.txt` language section where the
   language changed, `mc/questions.txt` (mark answered items as done, add new
   questions and the decisions you made).

For every task add cairo unit tests for the new parts and golden programs for
the new behaviour. Run the sanitizers build on the full golden suite.


## 10. Final report

End with a short report: what was done per task, what changed for mc code
(declarations, attributes, CLI), benchmark numbers, what could not be verified
(e.g. MSVC), every decision you made on your own, and where the work is
(files, branch). Do not commit unless the user said so in this session.
