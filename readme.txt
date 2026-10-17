
mc - "modernized c"
===================

the mc language: lexer, parser, module loader, type checker, generic
instantiation, compile-time evaluation, a compiler to stack-based bytecode
and a virtual machine that runs it.

two implementations: c/ (c11; see c/readme.txt) is the one being
developed, and the one this file's language section describes. mc/ (python
3.11 or newer, standard library only) is the old reference, being dropped:
it does not have the changes marked "(c only)" below, and the usage and
layout sections are about it. it does not know '[plugin]', so it cannot use
today's stdlib/io.mc and stdlib/memory.mc (the stdlib of commit 379b053
works with it).


usage
-----

run from this directory:

	python -m mc run <file.mc>      check, compile and run a program
	python -m mc run <file.mcb>     run compiled bytecode
	python -m mc build <file.mc>    compile to bytecode (-o <file.mcb>)
	python -m mc dis <file>         print the bytecode ('.mc' or '.mcb')
	python -m mc check <file.mc>    check a program and every module it imports
	python -m mc mono <file.mc>     check, then print every instance with types
	python -m mc ast <file.mc>      print the syntax tree of one file
	python -m mc tokens <file.mc>   print the tokens of one file

options: -I <dir> (extra module directory), --stdlib <dir>, -o <file>,
--color, --no-color. exit status: 0 success, 1 errors, 2 usage, 101 runtime
error; 'run' exits with the result of 'main' when it returns an int.

tests:

	python -m unittest discover -s tests

examples: examples/vector (the vector module from the design discussion) and
examples/tour (every feature in one program).


layout
------

	mc/source.py     source files and spans
	mc/diag.py       diagnostics and their codes (the CODES table)
	mc/lexer.py      tokens
	mc/ast.py        syntax tree nodes
	mc/parser.py     recursive-descent parser, strict operator precedence
	mc/printer.py    syntax tree printer
	mc/loader.py     module search and loading, import cycles
	mc/types.py      semantic types, unification
	mc/checker.py    names, types, generics, references, mutability, flow
	mc/evaluate.py   compile-time evaluation (runs pure user functions)
	mc/bytecode.py   bytecode, opcodes (documented there), .mcb files, disassembler
	mc/compiler.py   checked program -> bytecode
	mc/vm.py         virtual machine, builtins and natives
	mc/cli.py        command line
	stdlib/          memory (alloc/realloc/free/copy), io (print, println,
	                 write, write_line), option, result
	tests/           unit tests (unittest)


the language, as implemented
----------------------------

declarations are 'name :: ...':

	Point :: { let mut x: f32; let y: f32 = 0; }       struct (fields are 'let')
	add<T> :: (a: T, b: T) -> T { return a + b; }       function (generic)
	LIMIT :: 4 * 16;                                    constant
	[plugin] pub alloc<T> :: (count: uint) -> []T;      provided by a plugin (c only)
	[plugin = "math"] pub sqrt :: (x: f64) -> f64;      ... by the plugin named 'math'

- types: s8..s64, u8..u64, int and uint (word sized), f32, f64, float (word
  sized, the default for float literals), bool; [N]T arrays (values), [_]T
  takes the length from the initializer; []T slices (views of array or heap
  memory, storable, elements always writable); (int, mut &T) -> int function
  types. no pointers, no void, no enums, no unions, no empty structs.
- everything is copied (shallowly). '&T' parameters are references; the
  caller writes '&x', or 'mut &x' for a 'mut p: &T' parameter. references
  exist only as parameters.
- 'mut' is on names only: 'let mut x', 'mut p: T' (the function's own copy),
  'let mut field: T' in structs. a non-mut field is fixed once created.
- generics take types only, are declared on the name, are instantiated once
  per set of type arguments, and each instance is checked like normal code.
- operators on structs call functions marked [op._+_], [op.-_], [op._[_]] ...
  with exact operand order. '!=', '>', '<=', '>=' and 'a += b' are derived.
  operator functions are found in the module, its imports, and the modules
  that define the operand types.
- lambdas '(x: int) -> int { ... }' capture nothing.
- plugins (c only; they replace '[builtin]' and '[extern]'): a function
  declared '[plugin]' without a body is provided by a plugin, a shared
  library loaded with 'mc run --plugin <file>' (c/readme.txt shows how to
  write one). it is found by its qualified name, 'module.function'
  ('memory.alloc'); '[plugin = "name"]' also names the plugin. the
  standard library's alloc, realloc, free, copy, print, println, write and
  write_line come from the 'std' plugin, built into mc. a plugin function
  can be used as a function value unless it is variadic, and cannot run
  at compile time. using one that no loaded plugin provides is E0416.
- only plugin functions can be variadic: 'values: ...' as the last parameter
  takes any number of values of any types.
- print and println take a format string, which must be known at compile
  time: each '{}' is replaced by the next value ('{{' and '}}' write braces),
  and the compiler checks that they match the values:
  println("{} + {} = {}", a, b, a + b);
- format specifiers (c only): '{:spec}' with
  spec = [[fill]align][sign][#][0][width][.precision][type]: align '<', '>',
  '^' with any printable ascii fill but braces; sign '+' or '-'; '#' for
  '0x', '0o', '0b'; '0' pads numbers with zeros; types 'd' 'x' 'X' 'o' 'b'
  for integers and 'f' 'e' 'E' 'g' for floats. width, fill and align apply
  to any value. a specifier that does not fit its value is E0530 at compile
  time: println("{:>8} {:08.3f} {:#x}|", "right", 3.14159, 255). stdlib/io.mc
  has the details.
- "text"c is a [N]u8 with a NUL at the end. printing drops that NUL only for
  values that come from such a literal (c only: also through a constant or a
  variable that is not 'mut'); other u8 arrays and slices print every byte.
- constants and array lengths are evaluated at compile time; user functions
  can run there. sizeof(T), lenof(x) and assert(cond[, "message"]) are
  intrinsics; a constant assert is checked by the compiler.
- operators follow c's order, but bit operators, shifts, mixing && with ||,
  and chained comparisons need parentheses.
- discarding a result is an error ('_ = f();' discards on purpose).
- integers wrap; the compiler warns when it can see an overflow ([wrap]
  silences it). let without a value warns when read too early ([uninit]
  silences it).
- attributes go before declarations and, in c only, before any statement:
  [wrap] before a block, 'if', 'while', 'for', 'return' or an expression
  silences the overflow warnings of everything inside it, except the
  lambdas and local functions declared there. [uninit] is for 'let' only.
- 'pub main' in the entry module: '()', '() -> int', and in c only
  '(arguments: [][]u8)' and '(arguments: [][]u8) -> int'. 'mc run tool.mc a
  b' gives main ["tool.mc", "a", "b"] (the file, then what follows it; '--'
  ends mc's own options). the arguments are read-only and cannot be freed.
- limits (c only, as options): compile-time evaluation takes at most
  --eval-steps steps (default 1,000,000; E0705 points at the loop or call
  still running), a program at most --call-depth nested calls (default
  10,000) and --limit instructions (default: no limit).


defaults chosen while implementing
----------------------------------

these were open or unspecified in the design discussion; each is easy to
change:

- source files end in '.mc'. 'import a.b' loads a/b.mc and is used as
  'a.b.name'; 'import a.b as c' as 'c.name'.
- octal is '0o17'. number suffixes are type names ('255u8', '2.0float');
  hexadecimal numbers take integer suffixes only.
- a braceless body (if c x += 1;) is any statement except a declaration. the
  condition is parsed as far as possible; a body that starts with '(' or '-'
  needs braces.
- in a condition, a typed literal needs parentheses: if p == ((P){...}) { }.
- '{}' is the zero value of any type except function types.
- struct literals need field names; array literals are positional and must
  have exactly N elements. a literal passed for a slice parameter becomes a
  hidden array: sum({ 1, 2 }).
- casts convert between number types only.
- 'let a: T;' may be assigned once later even without 'mut' (initialization),
  but not inside a loop around it.
- '&5' (a read-only reference to a temporary) is allowed; 'mut &5' is not.
  one call cannot pass two overlapping references when one is 'mut &'.
- slices of non-mut arrays are allowed; the vm is to mark them read-only at
  run time, like slices of constants.
- local constants and functions can be used anywhere in their block; local
  function bodies are checked where they are declared.
- sizes: a word is 8 bytes; a slice is 16, a function value 8, bool 1;
  structs use c layout.
- integer division truncates toward zero.
- compile-time evaluation stops after 1,000,000 steps (c: --eval-steps) or
  100 nested calls; plugin (python: builtin and extern) functions cannot run
  at compile time.
- unused 'mut' is a warning (W0503).


the virtual machine
-------------------

(the python implementation's; the c implementation lays out memory like c
and writes binary .mcb files, see c/readme.txt.)

a stack machine. arithmetic and comparison opcodes are typed ('ADD_S32',
'LT_F64'), so values are never inspected to choose an operation. generic
instances are separate functions, operators on structs are direct calls,
constants live in a constant pool. only code reachable from 'main' is
compiled. a '.mcb' file is the line 'mc-bytecode 1' and the module as json.

at run time:

- arrays and structs are copied on assignment and written in place, so a
  slice of an array keeps seeing it.
- every slice access checks bounds and that the memory is still alive: heap
  memory dies at free or realloc, a function's local arrays when it returns.
  using dead memory, freeing part of an allocation, freeing twice or freeing
  memory alloc did not return stop the program with a runtime error.
- slices of constants, string literals and non-mut arrays are read-only;
  writing through them is a runtime error.
- integer division by zero, failed asserts, out-of-bounds indexes and more
  than 10,000 nested calls are runtime errors. the report shows the source
  line and the calls that led there.
- '[builtin]' functions: alloc, realloc, free, copy, print, println.
  '[extern = "name"]' functions come from a registry of natives (mc/vm.py,
  NATIVES; Machine(natives=...) adds more): mc_write, mc_write_line.
- print and println replace each '{}' of the format with the next value;
  any value can be printed, arrays and slices of u8 as text.


diagnostics
-----------

codes are E (error) or W (warning), an area and a number: 01 lexing,
02 syntax, 03 modules, 04 declarations and attributes, 05 types, 06 command
line, 07 compile-time evaluation. mc/diag.py lists every code with its name.
