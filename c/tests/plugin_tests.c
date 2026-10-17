
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
