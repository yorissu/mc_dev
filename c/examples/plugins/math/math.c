
/// an example plugin: 'math' for mc programs. it shows the parts of the
/// plugin api (mc/plugin.h): numbers in and out, slices, heap memory, a
/// struct argument, failing with a runtime error and a compile-time check.
///
/// build it (from c/): make plugins, then
///
///     mc run --plugin build/debug/plugins/math.so -I examples/plugins/math main.mc
///
/// the mc side of it is math.mc, beside this file.

#include "mc/plugin.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static mc_plugin_value_s _sqrt(mc_plugin_call_s* const call) {
	return mc_plugin_make_f64(call, sqrt(mc_plugin_get_f64(call, 0)));
}

static mc_plugin_value_s _sin(mc_plugin_call_s* const call) {
	return mc_plugin_make_f64(call, sin(mc_plugin_get_f64(call, 0)));
}

static mc_plugin_value_s _cos(mc_plugin_call_s* const call) {
	return mc_plugin_make_f64(call, cos(mc_plugin_get_f64(call, 0)));
}

static mc_plugin_value_s _pow(mc_plugin_call_s* const call) {
	return mc_plugin_make_f64(call, pow(mc_plugin_get_f64(call, 0), mc_plugin_get_f64(call, 1)));
}

static mc_plugin_value_s _floor(mc_plugin_call_s* const call) {
	return mc_plugin_make_int(call, (int64_t)floor(mc_plugin_get_f64(call, 0)));
}

/// the sum of a []f64.
static mc_plugin_value_s _sum(mc_plugin_call_s* const call) {
	const mc_plugin_slice_s values = mc_plugin_get_slice(call, 0);
	const double* const items = values.data;
	double total = 0.0;
	for (uint64_t i = 0; i < values.length; ++i) total += items[i];
	return mc_plugin_make_f64(call, total);
}

/// scales a []f64 in place; a read-only slice is a runtime error.
static mc_plugin_value_s _scale(mc_plugin_call_s* const call) {
	const mc_plugin_slice_s values = mc_plugin_get_slice(call, 0);
	if (values.read_only) mc_plugin_fail(call, "`math.scale` of a read-only slice");
	double* const items = values.data;
	const double factor = mc_plugin_get_f64(call, 1);
	for (uint64_t i = 0; i < values.length; ++i) items[i] *= factor;
	return mc_plugin_make_none(call);
}

/// 'count' numbers from 'start' by 'step', on the heap (free them with memory.free).
static mc_plugin_value_s _steps(mc_plugin_call_s* const call) {
	const double start = mc_plugin_get_f64(call, 0), step = mc_plugin_get_f64(call, 1);
	const uint64_t count = mc_plugin_get_uint(call, 2);
	// the element type comes from the result: the type of 'start' is f64 too
	void* data = NULL;
	const mc_plugin_value_s result = mc_plugin_heap_alloc(call, mc_plugin_argument_type(call, 0), count, &data);
	double* const items = data;
	for (uint64_t i = 0; i < count; ++i) items[i] = start + step * (double)i;
	return result;
}

/// the length of a 'Point' (a struct with the fields x and y, f64): read through its layout.
static mc_plugin_value_s _length(mc_plugin_call_s* const call) {
	const mc_plugin_type_s* const type = mc_plugin_argument_type(call, 0);
	const unsigned char* const memory = mc_plugin_get_memory(call, 0);
	double x = 0.0, y = 0.0;
	for (size_t i = 0; i < mc_plugin_type_field_count(call, type); ++i) {
		const char* const name = mc_plugin_type_field_name(call, type, i);
		double* const target = (0 == strcmp(name, "x")) ? &x : (0 == strcmp(name, "y")) ? &y : NULL;
		if (target != NULL) memcpy(target, memory + mc_plugin_type_field_offset(call, type, i), sizeof(double));
	}
	return mc_plugin_make_f64(call, sqrt(x * x + y * y));
}

/// at compile time: the square root of a negative constant is a mistake.
static void _check_sqrt(mc_plugin_check_s* const check) {
	const mc_plugin_constant_s value = mc_plugin_check_constant(check, 0);
	if (value.kind == mc_plugin_constant_float && value.real < 0.0) {
		mc_plugin_diagnostic_s* const diagnostic = mc_plugin_warning(check, NULL, 0, "the square root of a negative number is nan");
		char text[64];
		(void)snprintf(text, sizeof(text), "this is %g", value.real);
		mc_plugin_label(check, diagnostic, 0, text);
		mc_plugin_help(check, diagnostic, "take the square root of its absolute value");
	}
}

MC_PLUGIN_EXPORT bool mc_plugin_init(mc_plugin_api_s* const api) {
	if (!mc_plugin_compatible(api, MC_PLUGIN_API_VERSION)) return false;
	return mc_plugin_register(api, "math", "math.sqrt", _sqrt, _check_sqrt)
		&& mc_plugin_register(api, "math", "math.sin", _sin, NULL)
		&& mc_plugin_register(api, "math", "math.cos", _cos, NULL)
		&& mc_plugin_register(api, "math", "math.pow", _pow, NULL)
		&& mc_plugin_register(api, "math", "math.floor", _floor, NULL)
		&& mc_plugin_register(api, "math", "math.sum", _sum, NULL)
		&& mc_plugin_register(api, "math", "math.scale", _scale, NULL)
		&& mc_plugin_register(api, "math", "math.steps", _steps, NULL)
		&& mc_plugin_register(api, "math", "math.length", _length, NULL);
}
