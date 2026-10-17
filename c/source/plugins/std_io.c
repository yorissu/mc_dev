
/// the 'std' plugin: io.print and println (values in a format string),
/// write and write_line (raw bytes). text is utf-8; invalid sequences are
/// written as U+FFFD.

#include "std.h"
#include "mc/format.h"
#include "mc/string.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define _mc_nesting_limit 256

/// the text being written. it lives as long as mc (it is reused): when a
/// value stops the program halfway, nothing is left to free.
static mc_string_s _mc_out;
static mc_string_s _mc_text;

// --- the check of a format ---------------------------------------------------------------

/// the number of '{}' in a format string; false (with 'problem') when it is invalid.
static bool _mc_placeholders(const uint8_t* const data, const size_t length, size_t* const count, const char** const problem) {
	*count = 0;
	for (size_t index = 0; index < length;) {
		const uint8_t character = data[index];
		const bool has_following = index + 1 < length;
		const uint8_t following = has_following ? data[index + 1] : 0;
		if ('{' == character) {
			if (has_following && '{' == following) { index += 2; continue; }
			if (has_following && '}' == following) { *count += 1; index += 2; continue; }
			*problem = "`{` must be followed by `}` (format options are not supported yet)";
			return false;
		}
		if ('}' == character) {
			if (has_following && '}' == following) { index += 2; continue; }
			*problem = "a single `}` must be written `}}`";
			return false;
		}
		index += 1;
	}
	return true;
}

void mc_std_check_format(mc_plugin_check_s* const check) {
	const mc_plugin_constant_s format = mc_plugin_check_constant(check, 0);
	// a format that is not known is reported by the compiler already
	if (format.kind != mc_plugin_constant_bytes) return;
	const size_t values = mc_plugin_check_count(check) - mc_plugin_check_fixed_count(check);
	size_t placeholders = 0;
	const char* problem = NULL;
	if (!_mc_placeholders(format.bytes, format.length, &placeholders, &problem)) {
		mc_plugin_diagnostic_s* const diagnostic = mc_plugin_error(check, "E0530", 0, "invalid format string");
		mc_plugin_label(check, diagnostic, 0, problem);
		mc_plugin_help(check, diagnostic, "`{}` is replaced by the next value; write `{{` and `}}` for braces");
		return;
	}
	if (placeholders != values) {
		char message[128], count[64];
		(void)snprintf(message, sizeof(message), "the format string has %zu placeholder(s), but %zu value(s) are given", placeholders, values);
		(void)snprintf(count, sizeof(count), "%zu `{}`", placeholders);
		mc_plugin_diagnostic_s* const diagnostic = mc_plugin_error(check, "E0530", MC_PLUGIN_CALL, message);
		mc_plugin_label(check, diagnostic, MC_PLUGIN_CALL, "they must match");
		mc_plugin_label(check, diagnostic, 0, count);
	}
}

// --- values as text ----------------------------------------------------------------------

/// bytes as text; one trailing NUL is left out when 'strip_nul'.
static void _mc_bytes_text(mc_string_s* const out, const uint8_t* const bytes, uint64_t count, const bool strip_nul) {
	if (strip_nul && count > 0 && 0 == bytes[count - 1]) --count;
	if (count > 0) mc_format_utf8(out, mc_view_new((const char*)bytes, (size_t)count));
}

/// the value of 'type' at 'memory' as print shows it.
static void _mc_value(mc_plugin_call_s* const call, mc_string_s* const out, const uint8_t* const memory, const mc_plugin_type_s* const type,
					  const int depth) {
	if (depth > _mc_nesting_limit) mc_plugin_fail(call, "values nest too deeply");
	switch (mc_plugin_type_kind(call, type)) {
		case mc_plugin_kind_int: {
			const uint32_t bits = mc_plugin_type_bits(call, type);
			uint64_t value = 0;
			memcpy(&value, memory, bits / 8);
			if (mc_plugin_type_signed(call, type)) {
				// sign-extend from the type's width
				const int64_t extended = (bits < 64 && (value >> (bits - 1)) != 0) ? (int64_t)(value | ~(((uint64_t)1 << bits) - 1)) : (int64_t)value;
				mc_string_appendf(out, "%" PRId64, extended);
			}
			else {
				mc_string_appendf(out, "%" PRIu64, value);
			}
			return;
		}
		case mc_plugin_kind_float: {
			const uint32_t bits = mc_plugin_type_bits(call, type);
			double value = 0.0;
			if (32 == bits) {
				float single = 0.0f;
				memcpy(&single, memory, sizeof(single));
				value = (double)single;
			}
			else {
				memcpy(&value, memory, sizeof(value));
			}
			mc_format_float_value(out, value, (int)bits);
			return;
		}
		case mc_plugin_kind_bool:
			mc_string_append_cstr(out, memory[0] != 0 ? "true" : "false");
			return;
		case mc_plugin_kind_function:
			mc_string_appendf(out, "<function %s>", mc_plugin_function_name_at(call, memory));
			return;
		case mc_plugin_kind_array:
		case mc_plugin_kind_slice: {
			const mc_plugin_type_s* const element = mc_plugin_type_element(call, type);
			const uint8_t* items = memory;
			uint64_t count = mc_plugin_type_length(call, type);
			if (mc_plugin_kind_slice == mc_plugin_type_kind(call, type)) {
				const mc_plugin_slice_s slice = mc_plugin_slice_at(call, memory, type);
				items = slice.data;
				count = slice.length;
			}
			if (0 == strcmp(mc_plugin_type_name(call, element), "u8")) {
				_mc_bytes_text(out, items, count, true);
				return;
			}
			const uint64_t size = mc_plugin_type_size(call, element);
			mc_string_append_char(out, '{');
			for (uint64_t i = 0; i < count; ++i) {
				if (i > 0) mc_string_append_cstr(out, ", ");
				_mc_value(call, out, items + i * size, element, depth + 1);
			}
			mc_string_append_char(out, '}');
			return;
		}
		case mc_plugin_kind_struct: {
			const size_t fields = mc_plugin_type_field_count(call, type);
			mc_string_append_char(out, '{');
			for (size_t i = 0; i < fields; ++i) {
				if (i > 0) mc_string_append_cstr(out, ", ");
				mc_string_appendf(out, "%s: ", mc_plugin_type_field_name(call, type, i));
				_mc_value(call, out, memory + mc_plugin_type_field_offset(call, type, i), mc_plugin_type_field_type(call, type, i), depth + 1);
			}
			mc_string_append_char(out, '}');
			return;
		}
	}
}

/// the format (argument 0) with each '{}' replaced by the next value;
/// '{{' and '}}' are braces. the check matched them already.
static void _mc_formatted(mc_plugin_call_s* const call, mc_string_s* const out) {
	const mc_plugin_slice_s format = mc_plugin_get_slice(call, 0);
	const uint8_t* const data = format.data;
	const size_t count = mc_plugin_count(call);
	size_t next = mc_plugin_fixed_count(call);
	mc_string_s* const text = &_mc_text;
	mc_string_clear(text);
	for (uint64_t index = 0; index < format.length;) {
		const uint8_t character = data[index];
		const int following = (index + 1 < format.length) ? (int)data[index + 1] : -1;
		if ('{' == character && '}' == following) {
			mc_format_utf8(out, mc_string_view(text));
			mc_string_clear(text);
			if (next < count) {
				_mc_value(call, out, mc_plugin_get_memory(call, next), mc_plugin_argument_type(call, next), 0);
				++next;
			}
			index += 2;
			continue;
		}
		if (('{' == character || '}' == character) && following == character) ++index;
		mc_string_append_char(text, (char)character);
		++index;
	}
	mc_format_utf8(out, mc_string_view(text));
}

static void _mc_print(mc_plugin_call_s* const call, const bool line) {
	mc_string_s* const out = &_mc_out;
	mc_string_clear(out);
	_mc_formatted(call, out);
	if (line) mc_string_append_char(out, '\n');
	mc_plugin_write(call, out->data, out->length);
}

mc_plugin_value_s mc_std_print(mc_plugin_call_s* const call) {
	_mc_print(call, false);
	return mc_plugin_make_none(call);
}

mc_plugin_value_s mc_std_println(mc_plugin_call_s* const call) {
	_mc_print(call, true);
	return mc_plugin_make_none(call);
}

static void _mc_write_bytes(mc_plugin_call_s* const call, const bool line) {
	const mc_plugin_slice_s bytes = mc_plugin_get_slice(call, 0);
	mc_string_s* const out = &_mc_out;
	mc_string_clear(out);
	_mc_bytes_text(out, bytes.data, bytes.length, false);
	if (line) mc_string_append_char(out, '\n');
	mc_plugin_write(call, out->data, out->length);
}

mc_plugin_value_s mc_std_write(mc_plugin_call_s* const call) {
	_mc_write_bytes(call, false);
	return mc_plugin_make_none(call);
}

mc_plugin_value_s mc_std_write_line(mc_plugin_call_s* const call) {
	_mc_write_bytes(call, true);
	return mc_plugin_make_none(call);
}
