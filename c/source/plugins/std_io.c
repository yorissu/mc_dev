
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
static mc_string_s _mc_piece;

// --- the check of a format ---------------------------------------------------------------

/// what a spec is applied to, from the type of the value.
static mc_std_value_e _mc_value_kind(mc_plugin_check_s* const check, const mc_plugin_type_s* const type) {
	switch (mc_plugin_type_kind(check, type)) {
		case mc_plugin_kind_int:   return mc_std_value_int;
		case mc_plugin_kind_float: return mc_std_value_float;
		default:                   return mc_std_value_other;
	}
}

void mc_std_check_format(mc_plugin_check_s* const check) {
	const size_t fixed = mc_plugin_check_fixed_count(check), count = mc_plugin_check_count(check);
	// values from "..."c literals lose their NUL when printed
	for (size_t i = fixed; i < count; ++i) {
		if (mc_plugin_check_constant(check, i).c_string) mc_plugin_check_mark(check, i, mc_std_flag_c_string);
	}
	const mc_plugin_constant_s format = mc_plugin_check_constant(check, 0);
	// a format that is not known is reported by the compiler already
	if (format.kind != mc_plugin_constant_bytes) return;
	size_t placeholders = 0;
	size_t at = 0;
	mc_std_piece_s piece;
	while (mc_std_next_placeholder(format.bytes, format.length, &at, &piece)) {
		if (piece.problem != NULL) {
			mc_plugin_diagnostic_s* const diagnostic = mc_plugin_error(check, "E0530", 0, "invalid format string");
			mc_plugin_label(check, diagnostic, 0, piece.problem);
			mc_plugin_help(check, diagnostic, "`{}` and `{:spec}` are replaced by the next value; write `{{` and `}}` for braces");
			return;
		}
		if (!piece.placeholder) continue;
		const size_t value = fixed + placeholders;
		placeholders += 1;
		if (value >= count) continue;
		const mc_plugin_type_s* const type = mc_plugin_check_type(check, value);
		const char* const problem = mc_std_spec_fits(&piece.spec, _mc_value_kind(check, type));
		if (problem != NULL) {
			char found[160];
			(void)snprintf(found, sizeof(found), "this is a `%s`", mc_plugin_type_name(check, type));
			mc_plugin_diagnostic_s* const diagnostic = mc_plugin_error(check, "E0530", 0, "the format specifier does not fit its value");
			mc_plugin_label(check, diagnostic, 0, problem);
			mc_plugin_label(check, diagnostic, value, found);
			return;
		}
	}
	const size_t values = count - fixed;
	if (placeholders != values) {
		char message[128], placeholder_count[64];
		(void)snprintf(message, sizeof(message), "the format string has %zu placeholder(s), but %zu value(s) are given", placeholders, values);
		(void)snprintf(placeholder_count, sizeof(placeholder_count), "%zu placeholder(s)", placeholders);
		mc_plugin_diagnostic_s* const diagnostic = mc_plugin_error(check, "E0530", MC_PLUGIN_CALL, message);
		mc_plugin_label(check, diagnostic, MC_PLUGIN_CALL, "they must match");
		mc_plugin_label(check, diagnostic, 0, placeholder_count);
	}
}

// --- values as text ----------------------------------------------------------------------

/// bytes as text; one trailing NUL is left out when 'strip_nul'.
static void _mc_bytes_text(mc_string_s* const out, const uint8_t* const bytes, uint64_t count, const bool strip_nul) {
	if (strip_nul && count > 0 && 0 == bytes[count - 1]) --count;
	if (count > 0) mc_format_utf8(out, mc_view_new((const char*)bytes, (size_t)count));
}

/// the value of 'type' at 'memory' as print shows it; arrays and slices of
/// u8 are text, without their trailing NUL when they are a c string.
static void _mc_value(mc_plugin_call_s* const call, mc_string_s* const out, const uint8_t* const memory, const mc_plugin_type_s* const type,
					  const int depth, const bool c_string) {
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
				_mc_bytes_text(out, items, count, c_string);
				return;
			}
			const uint64_t size = mc_plugin_type_size(call, element);
			mc_string_append_char(out, '{');
			for (uint64_t i = 0; i < count; ++i) {
				if (i > 0) mc_string_append_cstr(out, ", ");
				_mc_value(call, out, items + i * size, element, depth + 1, false);
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
				_mc_value(call, out, memory + mc_plugin_type_field_offset(call, type, i), mc_plugin_type_field_type(call, type, i), depth + 1, false);
			}
			mc_string_append_char(out, '}');
			return;
		}
	}
}

/// a value by a spec: numbers through the spec, everything else as its text, padded.
static void _mc_value_spec(mc_plugin_call_s* const call, mc_string_s* const out, const uint8_t* const memory, const mc_plugin_type_s* const type,
						   const mc_std_spec_s* const spec, const bool c_string) {
	const mc_plugin_kind_e kind = mc_plugin_type_kind(call, type);
	if (mc_plugin_kind_int == kind) {
		const uint32_t bits = mc_plugin_type_bits(call, type);
		uint64_t value = 0;
		memcpy(&value, memory, bits / 8);
		bool negative = false;
		if (mc_plugin_type_signed(call, type) && (value >> (bits - 1)) != 0) {
			// the magnitude of a negative value of the type's width
			const uint64_t extended = (bits < 64) ? (value | ~(((uint64_t)1 << bits) - 1)) : value;
			negative = true;
			value = (uint64_t)0 - extended;
		}
		mc_std_format_integer(out, spec, negative, value);
		return;
	}
	if (mc_plugin_kind_float == kind) {
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
		mc_std_format_float(out, spec, value, (int)bits);
		return;
	}
	mc_string_s* const text = &_mc_piece;
	mc_string_clear(text);
	_mc_value(call, text, memory, type, 0, c_string);
	mc_std_format_text(out, spec, text);
}

/// the format (argument 0) with each placeholder replaced by the next
/// value; '{{' and '}}' are braces. the check matched them already.
static void _mc_formatted(mc_plugin_call_s* const call, mc_string_s* const out) {
	const mc_plugin_slice_s format = mc_plugin_get_slice(call, 0);
	const uint8_t* const data = format.data;
	const size_t count = mc_plugin_count(call);
	size_t next = mc_plugin_fixed_count(call);
	mc_string_s* const text = &_mc_text;
	size_t at = 0;
	mc_std_piece_s piece;
	while (mc_std_next_placeholder(data, (size_t)format.length, &at, &piece)) {
		// the text, with doubled braces written once
		mc_string_clear(text);
		for (size_t i = piece.text_start; i < piece.text_end; ++i) {
			mc_string_append_char(text, (char)data[i]);
			if (('{' == data[i] || '}' == data[i]) && i + 1 < piece.text_end && data[i + 1] == data[i]) ++i;
		}
		mc_format_utf8(out, mc_string_view(text));
		if (!piece.placeholder || piece.problem != NULL || next >= count) continue;
		const mc_plugin_type_s* const type = mc_plugin_argument_type(call, next);
		const uint8_t* const memory = mc_plugin_get_memory(call, next);
		const bool c_string = 0 != (mc_plugin_argument_flags(call, next) & mc_std_flag_c_string);
		if (piece.spec_end > piece.spec_start) _mc_value_spec(call, out, memory, type, &piece.spec, c_string);
		else _mc_value(call, out, memory, type, 0, c_string);
		++next;
	}
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
