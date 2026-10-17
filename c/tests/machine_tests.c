
/// unit tests of the back end: instructions, the bytecode file, the compiler
/// and the virtual machine (whole programs, from source to output).

#include "cairo.h"
#include "helpers.h"

#define PRELUDE "from io import print, println\nfrom memory import alloc, realloc, free, copy\n"

cairo_test_new(bytecode, instruction_names_round_trip) {
	const char* const names[] = { "CONST", "ADD_S32", "LT_F64", "EQ_FN", "CAST_INT_F32", "CAST_U8_FLOAT", "RANGE_TEST", "BITNOT_U64", };
	for (size_t i = 0; i < mc_count_of(names); ++i) {
		mc_instruction_s instruction = {0};
		cairo_assert_true(mc_instruction_parse(names[i], &instruction));
		mc_string_s text = {0};
		mc_instruction_name(&instruction, &text);
		cairo_assert_streq(mc_string_cstr(&text), names[i]);
		mc_string_free(&text);
	}
	mc_instruction_s instruction = {0};
	cairo_assert_false(mc_instruction_parse("FROB", &instruction));
	cairo_assert_false(mc_instruction_parse("ADD_X9", &instruction));
	cairo_assert_false(mc_instruction_parse("CONST_INT", &instruction));
}

cairo_test_new(bytecode, file_round_trip) {
	mc_test_run_s run = mc_test_run(PRELUDE
		"Point :: { let x: int; let y: f32 = 1.5; }\n"
		"NAMES :: \"ab\";\n"
		"pub main :: () -> int { let p: Point = { x: 2, }; println(\"{} {}\", p, NAMES[..]); return p.x; }\n");
	cairo_assert_true(run.compiled);
	const char* const path = "/tmp/mc_tests_round_trip.mcb";
	cairo_assert_true(mc_module_bc_save(&run.module, path));
	mc_module_bc_s loaded = {0};
	char* error = NULL;
	cairo_assert_true(mc_module_bc_load(path, &loaded, &error));
	mc_string_s before = {0}, after = {0};
	mc_disassemble(&run.module, &before);
	mc_disassemble(&loaded, &after);
	cairo_assert_streq(mc_string_cstr(&before), mc_string_cstr(&after));
	cairo_assert_true(strstr(mc_string_cstr(&before), "(entry)") != NULL);
	mc_string_free(&before);
	mc_string_free(&after);
	mc_module_bc_free(&loaded);
	(void)unlink(path);
	mc_test_run_free(&run);
}

cairo_test_new(bytecode, loading_rejects_other_files) {
	const char* const path = "/tmp/mc_tests_bad.mcb";
	FILE* const file = fopen(path, "wb");
	(void)fputs("not bytecode\n", file);
	(void)fclose(file);
	mc_module_bc_s module = {0};
	char* error = NULL;
	cairo_assert_false(mc_module_bc_load(path, &module, &error));
	cairo_assert_true(error != NULL);
	mc_free(error);
	mc_module_bc_free(&module);
	(void)unlink(path);
}

/// runs 'source' and checks its output, exit status and runtime error (null: none).
static bool _runs(const char* const source, const char* const out, const int status, const char* const message) {
	mc_test_run_s run = mc_test_run(source);
	bool good = run.compiled;
	if (!good) (void)fprintf(stderr, "does not compile:\n%s", run.diagnostics);
	if (good && 0 != strcmp(run.out, out)) {
		(void)fprintf(stderr, "printed `%s`, expected `%s`\n", run.out, out);
		good = false;
	}
	if (good && run.status != status) {
		(void)fprintf(stderr, "exit status %d, expected %d\n", run.status, status);
		good = false;
	}
	if (good && (NULL == message) != (NULL == run.message)) {
		(void)fprintf(stderr, "runtime error `%s`, expected `%s`\n", run.message ? run.message : "none", message ? message : "none");
		good = false;
	}
	if (good && message != NULL && 0 != strcmp(message, run.message)) {
		(void)fprintf(stderr, "runtime error `%s`, expected `%s`\n", run.message, message);
		good = false;
	}
	mc_test_run_free(&run);
	return good;
}

cairo_test_new(machine, formatted_printing) {
	cairo_assert_true(_runs(PRELUDE
		"Point :: { let x: int; let y: int; }\n"
		"pub main :: () {\n"
		"	let p: Point = { x: 1, y: -2, };\n"
		"	let items: [3]int = { 1, 2, 3, };\n"
		"	println(\"{} {} {{literal}} {}\", p, items, true);\n"
		"	print(\"{} {}\", 0.1f32, 1.0 / 3.0);\n"
		"	println(\" {}\", \"text\"c);\n"
		"}\n",
		"{x: 1, y: -2} {1, 2, 3} {literal} true\n0.1 0.3333333333333333 text\n", 0, NULL));
}

cairo_test_new(machine, integers_wrap_to_their_type) {
	cairo_assert_true(_runs(PRELUDE
		"add :: (a: u8, b: u8) -> u8 { return a + b; }\n"
		"negate :: (a: s8) -> s8 { return -a; }\n"
		"shift :: (a: u64, b: u64) -> u64 { return a << b; }\n"
		"pub main :: () { println(\"{} {} {} {}\", add(255, 1), negate(-128), shift(1, 63), (u8)(-1 + 0)); }\n",
		"0 -128 9223372036854775808 255\n", 0, NULL));
}

cairo_test_new(machine, division_truncates_and_checks_zero) {
	cairo_assert_true(_runs(PRELUDE
		"divide :: (a: int, b: int) -> int { return a / b; }\n"
		"remainder :: (a: int, b: int) -> int { return a % b; }\n"
		"pub main :: () -> int { println(\"{} {}\", divide(-7, 2), remainder(-7, 2)); return divide(1, 0); }\n",
		"-3 -1\n", 101, "division by zero"));
}

cairo_test_new(machine, recursion_loops_and_exit_status) {
	cairo_assert_true(_runs(PRELUDE
		"fib :: (n: int) -> int { if n < 2 { return n; } return fib(n - 1) + fib(n - 2); }\n"
		"pub main :: () -> int {\n"
		"	let mut total = 0;\n"
		"	for i in 10..0 { total += i; }\n"
		"	for i in 0..=3 { if i == 1 { continue; } total += 100; }\n"
		"	let mut n = 0;\n"
		"	while true { n += 1; if n == 5 { break; } }\n"
		"	println(\"{} {} {}\", fib(20), total, n);\n"
		"	return 300;\n"
		"}\n",
		"6765 355 5\n", 44, NULL));
}

cairo_test_new(machine, function_values_and_lambdas) {
	cairo_assert_true(_runs(PRELUDE
		"twice :: (f: (int) -> int, x: int) -> int { return f(f(x)); }\n"
		"double :: (x: int) -> int { return 2 * x; }\n"
		"pub main :: () {\n"
		"	let g = double;\n"
		"	println(\"{} {} {}\", twice(double, 3), twice((x: int) -> int { return x + 1; }, 3), g == double);\n"
		"}\n",
		"12 5 true\n", 0, NULL));
}

cairo_test_new(machine, references_write_through) {
	cairo_assert_true(_runs(PRELUDE
		"Pair :: { let mut a: int; let mut b: [2]int; }\n"
		"bump :: (mut value: &int) { value += 1; }\n"
		"pub main :: () {\n"
		"	let mut pair: Pair = { a: 1, b: { 5, 6, }, };\n"
		"	bump(mut &pair.a);\n"
		"	bump(mut &pair.b[1]);\n"
		"	println(\"{}\", pair);\n"
		"}\n",
		"{a: 2, b: {5, 7}}\n", 0, NULL));
}

cairo_test_new(machine, heap_memory_is_checked) {
	cairo_assert_true(_runs(PRELUDE
		"pub main :: () -> int {\n"
		"	let mut data: []int = alloc<int>(4);\n"
		"	data[3] = 7;\n"
		"	data = realloc(data, 6);\n"
		"	let other: []int = alloc<int>(2);\n"
		"	println(\"{} {} {}\", data, copy(other, data[3..]), other);\n"
		"	_ = free(data);\n"
		"	return data[0];\n"
		"}\n",
		"{0, 0, 0, 7, 0, 0} 2 {7, 0}\n", 101, "use of freed heap memory"));
}

cairo_test_new(machine, bounds_are_checked) {
	cairo_assert_true(_runs(PRELUDE
		"get :: (items: []int, index: int) -> int { return items[index]; }\n"
		"pub main :: () -> int { let items: [3]int = { 1, 2, 3, }; return get(items[..], 3); }\n",
		"", 101, "index 3 is out of bounds (length 3)"));
	cairo_assert_true(_runs(PRELUDE
		"part :: (items: []int, low: int) -> []int { return items[low..]; }\n"
		"pub main :: () -> int { let items: [3]int = { 1, 2, 3, }; return (int)lenof(part(items[..], 4)); }\n",
		"", 101, "slice 4..3 is out of bounds (length 3)"));
}

cairo_test_new(machine, slices_of_returned_locals_are_caught) {
	cairo_assert_true(_runs(PRELUDE
		"leak :: () -> []int { let local: [2]int = { 1, 2, }; return local[..]; }\n"
		"pub main :: () -> int { return leak()[0]; }\n",
		"", 101, "use of a local array of a function that has returned"));
}

cairo_test_new(machine, assertions_and_limits) {
	cairo_assert_true(_runs(PRELUDE "pub main :: () { let x = 1; assert(x == 2, \"x is two\"); }\n", "", 101, "x is two"));
	mc_test_run_s run = mc_test_run_limited(PRELUDE "pub main :: () { while true {} }\n", 1000);
	cairo_assert_true(run.compiled);
	cairo_assert_false(run.finished);
	cairo_assert_streq(run.message, "stopped after 1000 instructions");
	mc_test_run_free(&run);
}

cairo_test_new(machine, garbage_is_collected) {
	mc_test_run_s run = mc_test_run(PRELUDE
		"make :: (n: int) -> [16]int { let mut items: [16]int = {}; items[0] = n; return items; }\n"
		"pub main :: () -> int {\n"
		"	let mut total = 0;\n"
		"	for i in 0..20000 { let items = make(i); let view = items[..]; total = (total + view[0]) % 1000; }\n"
		"	return total;\n"
		"}\n");
	cairo_assert_true(run.finished);
	cairo_assert_eq(run.status, 0);
	cairo_assert_gt(run.collections, (uint64_t)0);
	mc_test_run_free(&run);
}

cairo_test_new(compiler, constants_are_evaluated_at_compile_time) {
	cairo_assert_true(_runs(PRELUDE
		"square :: (n: int) -> int { return n * n; }\n"
		"TABLE :: table();\n"
		"table :: () -> [4]int { let mut t: [4]int = {}; for i in 0..4 { t[i] = square(i); } return t; }\n"
		"pub main :: () { println(\"{}\", TABLE); }\n",
		"{0, 1, 4, 9}\n", 0, NULL));
}
