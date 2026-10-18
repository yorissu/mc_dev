
/// an example plugin: 'math' for mia programs. it shows the parts of the
/// plugin api (mia/plugin.h): numbers in and out, slices, heap memory, a
/// struct argument, failing with a runtime error and a compile-time check.
///
/// build it with 'make plugin-math', then
///
///     build/release/mia run -P build/release/examples/plugins examples/plugins/math/main.mia
///
/// the mia side of it is math.mia, beside this file.

#include "mia/plugin.h"

#include <string.h>
#include <stdio.h>
#include <math.h>

static mia_plugin_value_s _math_sqrt(mia_plugin_call_s* const call) {
	return mia_plugin_make_f64(call, sqrt(mia_plugin_get_f64(call, 0)));
}

static mia_plugin_value_s _math_sin(mia_plugin_call_s* const call) {
	return mia_plugin_make_f64(call, sin(mia_plugin_get_f64(call, 0)));
}

static mia_plugin_value_s _math_cos(mia_plugin_call_s* const call) {
	return mia_plugin_make_f64(call, cos(mia_plugin_get_f64(call, 0)));
}

static mia_plugin_value_s _math_pow(mia_plugin_call_s* const call) {
	return mia_plugin_make_f64(call, pow(mia_plugin_get_f64(call, 0), mia_plugin_get_f64(call, 1)));
}

static mia_plugin_value_s _math_floor(mia_plugin_call_s* const call) {
	return mia_plugin_make_int(call, (int64_t)floor(mia_plugin_get_f64(call, 0)));
}

/// the sum of a []f64.
static mia_plugin_value_s _math_sum(mia_plugin_call_s* const call) {
	const mia_plugin_slice_s values = mia_plugin_get_slice(call, 0);
	const double* const items = values.data;
	double total = 0.0;
	for (uint64_t i = 0; i < values.length; ++i) total += items[i];
	return mia_plugin_make_f64(call, total);
}

/// scales a []f64 in place; a read-only slice is a runtime error.
static mia_plugin_value_s _math_scale(mia_plugin_call_s* const call) {
	const mia_plugin_slice_s values = mia_plugin_get_slice(call, 0);
	if (values.read_only) mia_plugin_fail(call, "`math.scale` of a read-only slice");
	double* const items = values.data;
	const double factor = mia_plugin_get_f64(call, 1);
	for (uint64_t i = 0; i < values.length; ++i) items[i] *= factor;
	return mia_plugin_make_none(call);
}

/// 'count' numbers from 'start' by 'step', on the heap (free them with memory.free).
static mia_plugin_value_s _math_steps(mia_plugin_call_s* const call) {
	const double start = mia_plugin_get_f64(call, 0), step = mia_plugin_get_f64(call, 1);
	const uint64_t count = mia_plugin_get_uint(call, 2);
	// the element type comes from the result: the type of 'start' is f64 too
	void* data = NULL;
	const mia_plugin_value_s result = mia_plugin_heap_alloc(call, mia_plugin_argument_type(call, 0), count, &data);
	double* const items = data;
	for (uint64_t i = 0; i < count; ++i) items[i] = start + step * (double)i;
	return result;
}

/// the length of a 'Point' (a struct with the fields x and y, f64): read through its layout.
static mia_plugin_value_s _math_length(mia_plugin_call_s* const call) {
	const mia_plugin_type_s* const type = mia_plugin_argument_type(call, 0);
	const unsigned char* const memory = mia_plugin_get_memory(call, 0);
	double x = 0.0, y = 0.0;
	for (size_t i = 0; i < mia_plugin_type_field_count(call, type); ++i) {
		const char* const name = mia_plugin_type_field_name(call, type, i);
		double* const target = (0 == strcmp(name, "x")) ? &x : (0 == strcmp(name, "y")) ? &y : NULL;
		if (target != NULL) memcpy(target, memory + mia_plugin_type_field_offset(call, type, i), sizeof(double));
	}
	return mia_plugin_make_f64(call, sqrt(x * x + y * y));
}

/// at compile time: the square root of a negative constant is a mistake.
static void _math_check_sqrt(mia_plugin_check_s* const check) {
	const mia_plugin_constant_s value = mia_plugin_check_constant(check, 0);
	if (value.kind == mia_plugin_constant_float && value.real < 0.0) {
		mia_plugin_diagnostic_s* const diagnostic = mia_plugin_warning(check, NULL, 0, "the square root of a negative number is nan");
		char text[64];
		(void)snprintf(text, sizeof(text), "this is %g", value.real);
		mia_plugin_label(check, diagnostic, 0, text);
		mia_plugin_help(check, diagnostic, "take the square root of its absolute value");
	}
}

mia_plugin_export bool mia_plugin_init(mia_plugin_api_s* const api) {
	if (!mia_plugin_compatible(api, mia_plugin_api_version)) return false;
	return mia_plugin_register(api, "math", "math.sqrt", _math_sqrt, _math_check_sqrt)
		&& mia_plugin_register(api, "math", "math.sin", _math_sin, NULL)
		&& mia_plugin_register(api, "math", "math.cos", _math_cos, NULL)
		&& mia_plugin_register(api, "math", "math.pow", _math_pow, NULL)
		&& mia_plugin_register(api, "math", "math.floor", _math_floor, NULL)
		&& mia_plugin_register(api, "math", "math.sum", _math_sum, NULL)
		&& mia_plugin_register(api, "math", "math.scale", _math_scale, NULL)
		&& mia_plugin_register(api, "math", "math.steps", _math_steps, NULL)
		&& mia_plugin_register(api, "math", "math.length", _math_length, NULL);
}
