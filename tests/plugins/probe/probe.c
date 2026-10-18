
/// a plugin for the tests: it looks at its arguments through the plugin api
/// and answers with a number made of what it saw, checks its calls at
/// compile time, and misbehaves on purpose.

#include "mia/plugin.h"

#include <string.h>

/// main.describe(p: Point, s: []u16, values: ...) -> uint
static mia_plugin_value_s _probe_describe(mia_plugin_call_s* const call) {
	uint64_t seen = 0;
	const mia_plugin_type_s* const point = mia_plugin_argument_type(call, 0);
	if (mia_plugin_kind_struct == mia_plugin_type_kind(call, point) && 2 == mia_plugin_type_field_count(call, point)
			&& 0 == strcmp("y", mia_plugin_type_field_name(call, point, 1)) && 8 == mia_plugin_type_field_offset(call, point, 1)
			&& 16 == mia_plugin_type_size(call, point) && 8 == mia_plugin_type_alignment(call, point)) {
		seen += 1;
	}
	const unsigned char* const memory = mia_plugin_get_memory(call, 0);
	int16_t x = 0;
	memcpy(&x, memory, sizeof(x));
	if (-3 == x) seen += 10;
	const mia_plugin_slice_s slice = mia_plugin_get_slice(call, 1);
	const uint16_t* const items = slice.data;
	if (3 == slice.length && 16 == mia_plugin_type_bits(call, slice.element) && !mia_plugin_type_signed(call, slice.element) && 500 == items[2]
			&& slice.read_only) {
		seen += 100;
	}
	if (4 == mia_plugin_count(call) && 2 == mia_plugin_fixed_count(call) && mia_plugin_kind_bool == mia_plugin_type_kind(call, mia_plugin_argument_type(call, 2))
			&& mia_plugin_get_bool(call, 2) && -7 == mia_plugin_get_int(call, 3) && 7 == mia_plugin_argument_flags(call, 3)
			&& 0 == mia_plugin_argument_flags(call, 2)) {
		seen += 1000;
	}
	return mia_plugin_make_uint(call, seen);
}

/// marks the last value; refuses more than two values.
static void _probe_check_describe(mia_plugin_check_s* const check) {
	const size_t count = mia_plugin_check_count(check);
	if (count > 4) {
		mia_plugin_diagnostic_s* const diagnostic = mia_plugin_error(check, NULL, count - 1, "too many values for `describe`");
		mia_plugin_label(check, diagnostic, count - 1, "one too many");
		mia_plugin_label(check, diagnostic, mia_plugin_call, "in this call");
		mia_plugin_note(check, diagnostic, "it takes two");
		return;
	}
	const mia_plugin_constant_s last = mia_plugin_check_constant(check, count - 1);
	if (mia_plugin_constant_integer == last.kind && -7 == last.integer && mia_plugin_kind_int == mia_plugin_type_kind(check, mia_plugin_check_type(check, count - 1))) {
		mia_plugin_check_mark(check, count - 1, 7);
	}
}

/// main.wrap(value: int) -> u8: results wrap to the declared type.
static mia_plugin_value_s _probe_wrap(mia_plugin_call_s* const call) {
	return mia_plugin_make_int(call, mia_plugin_get_int(call, 0));
}

/// main.oops(): reads an argument that is not there.
static mia_plugin_value_s _probe_oops(mia_plugin_call_s* const call) {
	if (0 == mia_plugin_count(call)) (void)mia_plugin_get_int(call, 3);
	return mia_plugin_make_none(call);
}

mia_plugin_export bool mia_plugin_init(mia_plugin_api_s* const api) {
	return mia_plugin_compatible(api, mia_plugin_api_version)
		&& mia_plugin_register(api, "probe", "main.describe", _probe_describe, _probe_check_describe)
		&& mia_plugin_register(api, "probe", "main.wrap", _probe_wrap, NULL)
		&& mia_plugin_register(api, "probe", "main.oops", _probe_oops, NULL);
}
