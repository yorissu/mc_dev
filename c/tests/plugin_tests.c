
/// unit tests of plugins: the registry, versions, the plugin api at
/// compile time and at run time (through a plugin compiled into the
/// tests), and loading the example plugin 'math' as a shared library.

#include "cairo.h"
#include "helpers.h"

#include <string.h>

// --- a plugin compiled into the tests -------------------------------------------------------

static bool _old_init(mc_plugin_api_s* const api) {
	return mc_plugin_compatible(api, MC_PLUGIN_API_VERSION + 1);
}

static bool _careless_init(mc_plugin_api_s* const api) {
	mc_unused(api);
	return true;
}

static mc_plugin_value_s _nothing(mc_plugin_call_s* const call) {
	return mc_plugin_make_none(call);
}

static bool _duplicate_init(mc_plugin_api_s* const api) {
	return mc_plugin_compatible(api, MC_PLUGIN_API_VERSION) && mc_plugin_register(api, "dup", "memory.alloc", _nothing, NULL);
}

/// main.describe(p: Point, s: []u16, values: ...) -> uint: a number made of what it sees.
static mc_plugin_value_s _describe(mc_plugin_call_s* const call) {
	uint64_t seen = 0;
	const mc_plugin_type_s* const point = mc_plugin_argument_type(call, 0);
	if (mc_plugin_kind_struct == mc_plugin_type_kind(call, point) && 2 == mc_plugin_type_field_count(call, point)
			&& 0 == strcmp("y", mc_plugin_type_field_name(call, point, 1)) && 8 == mc_plugin_type_field_offset(call, point, 1)
			&& 16 == mc_plugin_type_size(call, point) && 8 == mc_plugin_type_alignment(call, point)) {
		seen += 1;
	}
	const unsigned char* const memory = mc_plugin_get_memory(call, 0);
	int16_t x = 0;
	memcpy(&x, memory, sizeof(x));
	if (-3 == x) seen += 10;
	const mc_plugin_slice_s slice = mc_plugin_get_slice(call, 1);
	const uint16_t* const items = slice.data;
	if (3 == slice.length && 16 == mc_plugin_type_bits(call, slice.element) && !mc_plugin_type_signed(call, slice.element) && 500 == items[2] && slice.read_only) {
		seen += 100;
	}
	if (4 == mc_plugin_count(call) && 2 == mc_plugin_fixed_count(call) && mc_plugin_kind_bool == mc_plugin_type_kind(call, mc_plugin_argument_type(call, 2))
			&& mc_plugin_get_bool(call, 2) && -7 == mc_plugin_get_int(call, 3) && 7 == mc_plugin_argument_flags(call, 3) && 0 == mc_plugin_argument_flags(call, 2)) {
		seen += 1000;
	}
	return mc_plugin_make_uint(call, seen);
}

/// marks the last value, and refuses more than two values.
static void _check_describe(mc_plugin_check_s* const check) {
	const size_t count = mc_plugin_check_count(check);
	if (count > 4) {
		mc_plugin_diagnostic_s* const diagnostic = mc_plugin_error(check, NULL, count - 1, "too many values for `describe`");
		mc_plugin_label(check, diagnostic, count - 1, "one too many");
		mc_plugin_label(check, diagnostic, MC_PLUGIN_CALL, "in this call");
		mc_plugin_note(check, diagnostic, "it takes two");
		return;
	}
	const mc_plugin_constant_s last = mc_plugin_check_constant(check, count - 1);
	if (mc_plugin_constant_integer == last.kind && -7 == last.integer && mc_plugin_kind_int == mc_plugin_type_kind(check, mc_plugin_check_type(check, count - 1))) {
		mc_plugin_check_mark(check, count - 1, 7);
	}
}

/// main.wrap(value: int) -> u8: integers wrap to the declared result.
static mc_plugin_value_s _wrap(mc_plugin_call_s* const call) {
	return mc_plugin_make_int(call, mc_plugin_get_int(call, 0));
}

/// main.oops(): fails, and reads an argument that is not there.
static mc_plugin_value_s _oops(mc_plugin_call_s* const call) {
	if (mc_plugin_count(call) == 0) (void)mc_plugin_get_int(call, 3);
	return mc_plugin_make_none(call);
}

static bool _test_init(mc_plugin_api_s* const api) {
	return mc_plugin_compatible(api, MC_PLUGIN_API_VERSION)
		&& mc_plugin_register(api, "test", "main.describe", _describe, _check_describe)
		&& mc_plugin_register(api, "test", "main.wrap", _wrap, NULL)
		&& mc_plugin_register(api, "test", "main.oops", _oops, NULL);
}

#define PROGRAM_HEAD \
	"from io import println\n" \
	"Point :: { let x: s16; let y: f64; }\n" \
	"[plugin = \"test\"] describe :: (p: Point, s: []u16, values: ...) -> uint;\n" \
	"[plugin] wrap :: (value: int) -> u8;\n" \
	"[plugin] oops :: ();\n"

// --- tests ------------------------------------------------------------------------------------

cairo_test_new(plugins, the_registry) {
	mc_plugins_s plugins;
	mc_plugins_init(&plugins);
	cairo_assert_true(mc_plugins_loaded(&plugins, "std"));
	cairo_assert_true(mc_plugins_find(&plugins, "io.println") != NULL);
	cairo_assert_true(NULL == mc_plugins_find(&plugins, "io.nothing"));
	char* error = NULL;
	cairo_assert_false(mc_plugins_add(&plugins, _old_init, &error));
	cairo_assert_true(strstr(error, "needs plugin api version") != NULL);
	mc_free(error);
	cairo_assert_false(mc_plugins_add(&plugins, _careless_init, &error));
	cairo_assert_true(strstr(error, "mc_plugin_compatible") != NULL);
	mc_free(error);
	cairo_assert_false(mc_plugins_add(&plugins, _duplicate_init, &error));
	cairo_assert_true(strstr(error, "provided by the plugins `std` and `dup`") != NULL);
	mc_free(error);
	cairo_assert_false(mc_plugins_loaded(&plugins, "dup"));
	cairo_assert_false(mc_plugins_load(&plugins, "/no/such/plugin.so", &error));
	cairo_assert_true(strstr(error, "no such file") != NULL);
	mc_free(error);
	mc_plugins_free(&plugins);
}

cairo_test_new(plugins, the_api_at_compile_time_and_run_time) {
	mc_plugins_s plugins;
	mc_plugins_init(&plugins);
	char* error = NULL;
	cairo_assert_true(mc_plugins_add(&plugins, _test_init, &error));
	mc_test_run_s run = mc_test_run_with(PROGRAM_HEAD
		"pub main :: () -> int {\n"
		"	let p: Point = { x: -3, y: 2.5, };\n"
		"	let numbers: [3]u16 = { 1, 2, 500, };\n"
		"	println(\"{} {}\", describe(p, numbers[..], true, -7), wrap(300));\n"
		"	return (int)wrap(258);\n"
		"}\n", 0, &plugins, NULL);
	cairo_assert_streq(run.diagnostics, "");
	cairo_assert_true(run.finished);
	cairo_assert_streq(run.out, "1111 44\n");
	cairo_assert_eq(run.status, 2);
	cairo_assert_eq(run.module.intrinsics.data[0].flags.count, (size_t)4);
	mc_test_run_free(&run);
	// the check reports through the normal diagnostics
	run = mc_test_run_with(PROGRAM_HEAD
		"pub main :: () { let p: Point = { x: 1, y: 1.0, }; _ = describe(p, null, 1, 2, 3); }\n", 0, &plugins, NULL);
	cairo_assert_false(run.compiled);
	cairo_assert_true(strstr(run.diagnostics, "error[E0531]: too many values for `describe`") != NULL);
	mc_test_run_free(&run);
	// a plugin that reads an argument it does not have stops the program, it does not crash
	run = mc_test_run_with(PROGRAM_HEAD "pub main :: () { oops(); }\n", 0, &plugins, NULL);
	cairo_assert_true(run.compiled);
	cairo_assert_false(run.finished);
	cairo_assert_streq(run.message, "the plugin function `main.oops` reads argument 3, but it has 0");
	mc_test_run_free(&run);
	mc_plugins_free(&plugins);
}

cairo_test_new(plugins, a_missing_plugin_is_an_error) {
	mc_test_run_s run = mc_test_run_with("[plugin = \"elsewhere\"] f :: () -> int;\npub main :: () -> int { return f(); }\n", 0, NULL, NULL);
	cairo_assert_false(run.compiled);
	cairo_assert_true(strstr(run.diagnostics, "error[E0416]: no plugin provides `main.f`") != NULL);
	mc_test_run_free(&run);
}

cairo_test_new(plugins, the_example_math_plugin) {
	mc_plugins_s plugins;
	mc_plugins_init(&plugins);
	char* const path = mc_format("%s/math%s", MC_TEST_PLUGINS, mc_plugins_extension());
	char* error = NULL;
	const bool loaded = mc_plugins_load(&plugins, path, &error);
	if (!loaded) (void)fprintf(stderr, "%s (build it with 'make plugins')\n", error);
	cairo_assert_true(loaded);
	mc_free(error);
	mc_free(path);
	cairo_assert_true(mc_plugins_loaded(&plugins, "math"));
	mc_test_run_s run = mc_test_run_with(
		"import math\nfrom io import println\nfrom memory import free\n"
		"pub main :: () -> int {\n"
		"	let values = math.steps(1.0, 1.0, 3);\n"
		"	println(\"{} {} {}\", math.sqrt(9.0), math.sum(values), math.length({ x: 6, y: 8, }));\n"
		"	_ = free(values);\n"
		"	return math.floor(values[0]);\n"
		"}\n", 0, &plugins, "examples/plugins/math");
	cairo_assert_streq(run.diagnostics, "");
	cairo_assert_streq(run.out, "3.0 6.0 10.0\n");
	cairo_assert_streq(run.message, "use of freed heap memory");
	mc_test_run_free(&run);
	mc_plugins_free(&plugins);
}

// --- the format specifiers of the std plugin ---------------------------------------------------

#include "../source/plugins/std.h"

/// a spec parsed from 'text' (the part after ':'), or its problem.
static const char* _spec(const char* const text, mc_std_spec_s* const spec) {
	return mc_std_parse_spec((const uint8_t*)text, strlen(text), spec);
}

cairo_test_new(std_format, specs_are_parsed) {
	mc_std_spec_s spec;
	cairo_assert_true(NULL == _spec("*^+#012.3x", &spec));
	cairo_assert_eq(spec.fill, '*');
	cairo_assert_eq(spec.align, '^');
	cairo_assert_eq(spec.sign, '+');
	cairo_assert_true(spec.alternate);
	cairo_assert_true(spec.zero);
	cairo_assert_eq(spec.width, (size_t)12);
	cairo_assert_true(spec.has_precision);
	cairo_assert_eq(spec.precision, (size_t)3);
	cairo_assert_eq(spec.type, 'x');
	cairo_assert_true(NULL == _spec("<", &spec));
	cairo_assert_eq(spec.align, '<');
	cairo_assert_eq(spec.fill, ' ');
	cairo_assert_true(NULL == _spec("<<5", &spec));
	cairo_assert_eq(spec.fill, '<');
	cairo_assert_eq(spec.width, (size_t)5);
	cairo_assert_true(NULL == _spec("", &spec));
	cairo_assert_streq(_spec("q", &spec), "unknown character `q` in the format specifier");
	cairo_assert_streq(_spec("5.", &spec), "`.` must be followed by the precision, a number");
	cairo_assert_streq(_spec(".101f", &spec), "the precision is too large (at most 100)");
	cairo_assert_streq(_spec("x5", &spec), "unknown character `5` in the format specifier");
	cairo_assert_true(NULL == _spec(".2f", &spec));
	cairo_assert_true(NULL == mc_std_spec_fits(&spec, mc_std_value_float));
	cairo_assert_streq(mc_std_spec_fits(&spec, mc_std_value_int), "`f` is for floats, but the value is an integer");
	cairo_assert_true(NULL == _spec("#b", &spec));
	cairo_assert_true(NULL == mc_std_spec_fits(&spec, mc_std_value_int));
	cairo_assert_true(NULL == _spec("#", &spec));
	cairo_assert_streq(mc_std_spec_fits(&spec, mc_std_value_int), "`#` is for integers written with `x`, `X`, `o` or `b`");
	cairo_assert_true(NULL == _spec(">9", &spec));
	cairo_assert_true(NULL == mc_std_spec_fits(&spec, mc_std_value_other));
}

cairo_test_new(std_format, placeholders_are_found) {
	const char* const text = "a{{b}} {} {:>3} {x";
	size_t at = 0;
	mc_std_piece_s piece;
	cairo_assert_true(mc_std_next_placeholder((const uint8_t*)text, strlen(text), &at, &piece));
	cairo_assert_true(piece.placeholder);
	cairo_assert_eq(piece.text_end, (size_t)7);
	cairo_assert_true(mc_std_next_placeholder((const uint8_t*)text, strlen(text), &at, &piece));
	cairo_assert_eq(piece.spec.align, '>');
	cairo_assert_eq(piece.spec.width, (size_t)3);
	cairo_assert_true(mc_std_next_placeholder((const uint8_t*)text, strlen(text), &at, &piece));
	cairo_assert_streq(piece.problem, "`{` is not closed by `}` (write `{{` for a brace)");
	cairo_assert_false(mc_std_next_placeholder((const uint8_t*)text, strlen(text), &at, &piece));
}

/// 'value' formatted by the spec 'text'.
static char* _formatted(const char* const text, const int kind, const int64_t integer, const double real) {
	mc_std_spec_s spec;
	mc_string_s out = {0};
	if (_spec(text, &spec) != NULL) return mc_strdup("(invalid)");
	if (0 == kind) mc_std_format_integer(&out, &spec, integer < 0, integer < 0 ? (uint64_t)0 - (uint64_t)integer : (uint64_t)integer);
	else if (1 == kind) mc_std_format_float(&out, &spec, real, 64);
	else {
		mc_string_s value = {0};
		mc_string_append_cstr(&value, "té");
		mc_std_format_text(&out, &spec, &value);
		mc_string_free(&value);
	}
	return mc_string_take(&out);
}

cairo_test_new(std_format, values_are_formatted) {
	static const struct {
		const char* spec  ;
		int         kind  ;   // 0 integer, 1 float, 2 text
		int64_t     integer;
		double      real  ;
		const char* expected;
	} cases[] = {
		{ "", 0, -42, 0, "-42" }, { "05", 0, -42, 0, "-0042" }, { "+", 0, 7, 0, "+7" }, { "#x", 0, 255, 0, "0xff" },
		{ "#X", 0, -255, 0, "-0XFF" }, { "#010b", 0, 5, 0, "0b00000101" }, { "o", 0, 8, 0, "10" }, { "*<6", 0, 1, 0, "1*****" },
		{ "^7", 0, 123, 0, "  123  " }, { ">3", 0, 12345, 0, "12345" },
		{ "", 1, 0, 0.1, "0.1" }, { "", 1, 0, 1.0, "1.0" }, { ".3f", 1, 0, 3.14159, "3.142" }, { "08.2f", 1, 0, -1.5, "-0001.50" },
		{ "e", 1, 0, 1234.5, "1.234500e+03" }, { ".1E", 1, 0, 0.00012, "1.2E-04" }, { ".3g", 1, 0, 1234.5, "1.23e+03" },
		{ "+.1f", 1, 0, 2.0, "+2.0" }, { "08", 1, 0, -1.0 / 0.0, "    -inf" }, { "<6", 1, 0, 0.5, "0.5   " },
		{ "4", 2, 0, 0, "té  " }, { ">4", 2, 0, 0, "  té" }, { "-^5", 2, 0, 0, "-té--" }, { "1", 2, 0, 0, "té" },
	};
	for (size_t i = 0; i < mc_count_of(cases); ++i) {
		char* const result = _formatted(cases[i].spec, cases[i].kind, cases[i].integer, cases[i].real);
		if (0 != strcmp(result, cases[i].expected)) (void)fprintf(stderr, "spec `%s`: `%s`, expected `%s`\n", cases[i].spec, result, cases[i].expected);
		cairo_assert_streq(result, cases[i].expected);
		mc_free(result);
	}
}
