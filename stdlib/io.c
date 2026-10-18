
/// the plugin 'io': the functions of the module io (io.mia). print and
/// println write values in a format string (whose check at compile time is
/// here too). text is utf-8; invalid sequences are written as U+FFFD.
///
/// a format is text with placeholders: '{}' or '{:spec}' takes the next
/// value, '{{' and '}}' write braces. a spec is
///
///     [[fill]align][sign][#][0][width][.precision][type]
///
/// align '<' left, '>' right, '^' center (fill: any printable ascii but '{'
/// and '}'); sign '+' (always) or '-' (the default); '#' writes '0x', '0o' or
/// '0b' before integers in those bases; '0' pads numbers with zeros after
/// the sign; type 'd' (the default), 'x', 'X', 'o' or 'b' for integers, 'f',
/// 'e', 'E' or 'g' (the default: the shortest text that reads back) for
/// floats. a precision is the digits after the point ('f', 'e') or the
/// significant digits ('g'). width and fill apply to every value: numbers
/// are right-aligned, everything else left-aligned unless 'align' says.

#include "mia/format.h"
#include "mia/plugin.h"
#include "mia/string.h"

#include <ctype.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

typedef struct {
	char   fill         ;
	char   align        ;   // '<', '>', '^', or 0
	char   sign         ;   // '+', '-', or 0
	bool   alternate    ;   // '#'
	bool   zero         ;   // '0'
	size_t width        ;
	bool   has_precision;
	size_t precision    ;
	char   type         ;   // one of "dxXobfeEg", or 0
} _mia_io_spec_s;

/// a piece of a format: text, then a placeholder (or a problem).
typedef struct {
	size_t         text_start ;
	size_t         text_end   ;
	bool           placeholder;
	size_t         spec_start ;   // the text between the braces
	size_t         spec_end   ;
	_mia_io_spec_s spec       ;
	const char*    problem    ;   // the format is invalid here
} _mia_io_piece_s;

/// what a spec is applied to.
typedef enum {
	_mia_io_value_int  ,
	_mia_io_value_float,
	_mia_io_value_other,  // bools, text, arrays, structs...
} _mia_io_value_e;

/// io.print and println mark the values that are c strings ("..."c): only
/// they lose their trailing NUL when printed.
#define _mia_io_flag_c_string 1u

/// parses the text after the ':' of a placeholder; the problem, or null when it is valid.
static const char* _mia_io_parse_spec(const uint8_t* const text, const size_t length, _mia_io_spec_s* const spec);

// --- the format string -------------------------------------------------------------------

static bool _mia_io_next_placeholder(const uint8_t* const data, const size_t length, size_t* const at, _mia_io_piece_s* const piece) {
	size_t index = *at;
	*piece = (_mia_io_piece_s) { .text_start = index, };
	while (index < length) {
		const uint8_t character = data[index];
		const bool has_following = index + 1 < length;
		const uint8_t following = has_following ? data[index + 1] : 0;
		if (('{' == character || '}' == character) && has_following && following == character) {
			// a brace written twice is one brace of text
			index += 2;
			continue;
		}
		if ('}' == character) {
			piece->text_end = index;
			piece->problem = "a single `}` must be written `}}`";
			*at = length;
			return true;
		}
		if ('{' == character) {
			piece->text_end = index;
			piece->placeholder = true;
			size_t close = index + 1;
			while (close < length && data[close] != '}' && data[close] != '{') ++close;
			if (close >= length || data[close] != '}') {
				piece->problem = "`{` is not closed by `}` (write `{{` for a brace)";
				*at = length;
				return true;
			}
			piece->spec_start = index + 1;
			piece->spec_end = close;
			if (piece->spec_end > piece->spec_start) {
				if (data[piece->spec_start] != ':') piece->problem = "a placeholder is `{}` or `{:spec}`, like `{:>8}` or `{:.3f}`";
				else piece->problem = _mia_io_parse_spec(data + piece->spec_start + 1, piece->spec_end - piece->spec_start - 1, &piece->spec);
			}
			*at = close + 1;
			return true;
		}
		index += 1;
	}
	piece->text_end = length;
	*at = length;
	return piece->text_end > piece->text_start;
}

// --- specifiers --------------------------------------------------------------------------

static bool _mia_io_is_align(const uint8_t character) {
	return '<' == character || '>' == character || '^' == character;
}

/// a decimal number of at most 9 digits; false when there is none.
static bool _mia_io_number(const uint8_t* const text, const size_t length, size_t* const at, size_t* const value) {
	size_t digits = 0;
	*value = 0;
	while (*at < length && text[*at] >= '0' && text[*at] <= '9') {
		if (digits >= 9) return false;
		*value = *value * 10 + (size_t)(text[*at] - '0');
		++digits;
		++*at;
	}
	return digits > 0;
}

static const char* _mia_io_parse_spec(const uint8_t* const text, const size_t length, _mia_io_spec_s* const spec) {
	*spec = (_mia_io_spec_s) { .fill = ' ', };
	static char problem[96];
	size_t at = 0;
	// [[fill]align]
	if (length >= 2 && _mia_io_is_align(text[1]) && text[0] != '{' && text[0] != '}') {
		if (text[0] < 0x20 || text[0] >= 0x7F) return "the fill character must be printable ascii";
		spec->fill = (char)text[0];
		spec->align = (char)text[1];
		at = 2;
	}
	else if (length >= 1 && _mia_io_is_align(text[0])) {
		spec->align = (char)text[0];
		at = 1;
	}
	// [sign][#][0][width][.precision][type]
	if (at < length && ('+' == text[at] || '-' == text[at])) spec->sign = (char)text[at++];
	if (at < length && '#' == text[at]) {
		spec->alternate = true;
		++at;
	}
	if (at < length && '0' == text[at]) {
		spec->zero = true;
		++at;
	}
	if (at < length && text[at] >= '0' && text[at] <= '9' && !_mia_io_number(text, length, &at, &spec->width)) return "the width is too large";
	if (at < length && '.' == text[at]) {
		++at;
		if (!_mia_io_number(text, length, &at, &spec->precision)) return "`.` must be followed by the precision, a number";
		if (spec->precision > 100) return "the precision is too large (at most 100)";
		spec->has_precision = true;
	}
	if (spec->width > 10000) return "the width is too large (at most 10000)";
	if (at < length && NULL != strchr("dxXobfeEg", (char)text[at])) spec->type = (char)text[at++];
	if (at < length) {
		const uint8_t character = text[at];
		if (character >= 0x20 && character < 0x7F) (void)snprintf(problem, sizeof(problem), "unknown character `%c` in the format specifier", character);
		else (void)snprintf(problem, sizeof(problem), "unknown byte 0x%02X in the format specifier", character);
		return problem;
	}
	return NULL;
}

static const char* _mia_io_spec_fits(const _mia_io_spec_s* const spec, const _mia_io_value_e kind) {
	static char problem[96];
	const bool number = _mia_io_value_int == kind || _mia_io_value_float == kind;
	if (spec->type != 0) {
		const bool integer_type = NULL != strchr("dxXob", spec->type);
		if (_mia_io_value_int == kind && !integer_type) {
			(void)snprintf(problem, sizeof(problem), "`%c` is for floats, but the value is an integer", spec->type);
			return problem;
		}
		if (_mia_io_value_float == kind && integer_type) {
			(void)snprintf(problem, sizeof(problem), "`%c` is for integers, but the value is a float", spec->type);
			return problem;
		}
		if (!number) {
			(void)snprintf(problem, sizeof(problem), "`%c` is for numbers, but the value is not one", spec->type);
			return problem;
		}
	}
	if (spec->has_precision && kind != _mia_io_value_float) return "a precision (`.n`) is for floats only";
	if (spec->sign != 0 && !number) return "a sign (`+` or `-`) is for numbers only";
	if (spec->zero && !number) return "`0` (padding with zeros) is for numbers only";
	if (spec->alternate && (kind != _mia_io_value_int || NULL == strchr("xXob", spec->type) || 0 == spec->type)) {
		return "`#` is for integers written with `x`, `X`, `o` or `b`";
	}
	return NULL;
}

// --- formatting --------------------------------------------------------------------------

/// the code points in utf-8 text (bytes that do not continue a sequence).
static size_t _mia_io_width(const char* const text, const size_t length) {
	size_t points = 0;
	for (size_t i = 0; i < length; ++i) if (0x80 != ((uint8_t)text[i] & 0xC0)) ++points;
	return points;
}

/// appends 'body' to 'out', padded to the width of the spec. numbers are
/// right-aligned by default, everything else left-aligned; with '0', a
/// number is padded with zeros after its sign and prefix ('head' bytes).
static void _mia_io_pad(mia_string_s* const out, const _mia_io_spec_s* const spec, const char* const body, const size_t length, const size_t head,
						const bool number) {
	const size_t width = _mia_io_width(body, length);
	if (width >= spec->width) {
		mia_string_append(out, mia_view_new(body, length));
		return;
	}
	const size_t missing = spec->width - width;
	if (number && spec->zero && 0 == spec->align) {
		mia_string_append(out, mia_view_new(body, head));
		mia_string_append_repeat(out, '0', missing);
		mia_string_append(out, mia_view_new(body + head, length - head));
		return;
	}
	const char align = (spec->align != 0) ? spec->align : (number ? '>' : '<');
	const size_t before = ('>' == align) ? missing : ('^' == align) ? missing / 2 : 0;
	mia_string_append_repeat(out, spec->fill, before);
	mia_string_append(out, mia_view_new(body, length));
	mia_string_append_repeat(out, spec->fill, missing - before);
}

static void _mia_io_format_integer(mia_string_s* const out, const _mia_io_spec_s* const spec, const bool negative, const uint64_t magnitude) {
	char digits[80];
	size_t count = 0;
	const char type = (spec->type != 0) ? spec->type : 'd';
	const unsigned base = ('x' == type || 'X' == type) ? 16 : ('o' == type) ? 8 : ('b' == type) ? 2 : 10;
	const char* const symbols = ('X' == type) ? "0123456789ABCDEF" : "0123456789abcdef";
	uint64_t rest = magnitude;
	do {
		digits[count++] = symbols[rest % base];
		rest /= base;
	} while (rest > 0);
	char body[96];
	size_t length = 0;
	if (negative) body[length++] = '-';
	else if ('+' == spec->sign) body[length++] = '+';
	if (spec->alternate) {
		body[length++] = '0';
		body[length++] = ('X' == type) ? 'X' : (16 == base) ? 'x' : (8 == base) ? 'o' : 'b';
	}
	const size_t head = length;
	while (count > 0) body[length++] = digits[--count];
	_mia_io_pad(out, spec, body, length, head, true);
}

static void _mia_io_format_float(mia_string_s* const out, const _mia_io_spec_s* const spec, const double value, const int bits) {
	mia_string_s body = {0};
	if (!isfinite(value)) {
		// spelled here: c libraries differ ('-nan', 'nan(ind)'); 'E' writes them in capitals
		const char* const text = isnan(value) ? "nan" : (value > 0) ? "inf" : "-inf";
		for (const char* at = text; *at != '\0'; ++at) mia_string_append_char(&body, ('E' == spec->type) ? (char)toupper((unsigned char)*at) : *at);
	}
	else if ((0 == spec->type || 'g' == spec->type) && !spec->has_precision) {
		mia_format_float_value(&body, value, bits);
	}
	else {
		const char type = (spec->type != 0) ? spec->type : 'g';
		const int precision = spec->has_precision ? (int)spec->precision : 6;
		mia_format_float_c(&body, value, precision, type);
	}
	if ('+' == spec->sign && body.length > 0 && body.data[0] != '-') {
		mia_string_s is_signed = {0};
		mia_string_append_char(&is_signed, '+');
		mia_string_append(&is_signed, mia_string_view(&body));
		mia_string_free(&body);
		body = is_signed;
	}
	const size_t head = (body.length > 0 && ('-' == body.data[0] || '+' == body.data[0])) ? 1 : 0;
	// zeros do not pad infinities and nan, spaces do
	_mia_io_spec_s padding = *spec;
	padding.zero = spec->zero && isfinite(value);
	_mia_io_pad(out, &padding, (body.data != NULL) ? body.data : "", body.length, head, true);
	mia_string_free(&body);
}

static void _mia_io_format_text(mia_string_s* const out, const _mia_io_spec_s* const spec, const mia_string_s* const text) {
	_mia_io_pad(out, spec, (text->data != NULL) ? text->data : "", text->length, 0, false);
}

// --- printing ------------------------------------------------------------------

/// how deep a value may nest when it is written (writing recurses): structs
/// nest 1000 deep (mia_ast_depth_limit), arrays in them add levels
#define _mia_io_nesting_limit 4000

/// the text being written. it lives as long as the plugin (it is reused):
/// when a value stops the program halfway, nothing is left to free; it is
/// given back when mia unloads the plugin (_mia_io_exit).
static mia_string_s _mia_io_out;
static mia_string_s _mia_io_text;
static mia_string_s _mia_io_piece;

static void _mia_io_exit(void) {
	mia_string_free(&_mia_io_out);
	mia_string_free(&_mia_io_text);
	mia_string_free(&_mia_io_piece);
}

// --- the check of a format ---------------------------------------------------------------

/// what a spec is applied to, from the type of the value.
static _mia_io_value_e _mia_io_value_kind(mia_plugin_check_s* const check, const mia_plugin_type_s* const type) {
	switch (mia_plugin_type_kind(check, type)) {
		case mia_plugin_kind_int:   return _mia_io_value_int;
		case mia_plugin_kind_float: return _mia_io_value_float;
		default:                   return _mia_io_value_other;
	}
}

static void _mia_io_check_format(mia_plugin_check_s* const check) {
	const size_t fixed = mia_plugin_check_fixed_count(check), count = mia_plugin_check_count(check);
	// values from "..."c literals lose their NUL when printed
	for (size_t i = fixed; i < count; ++i) {
		if (mia_plugin_check_constant(check, i).c_string) mia_plugin_check_mark(check, i, _mia_io_flag_c_string);
	}
	const mia_plugin_constant_s format = mia_plugin_check_constant(check, 0);
	// a format that is not known is reported by the compiler already
	if (format.kind != mia_plugin_constant_bytes) return;
	size_t placeholders = 0;
	size_t at = 0;
	_mia_io_piece_s piece;
	while (_mia_io_next_placeholder(format.bytes, format.length, &at, &piece)) {
		if (piece.problem != NULL) {
			mia_plugin_diagnostic_s* const diagnostic = mia_plugin_error(check, "E0530", 0, "invalid format string");
			mia_plugin_label(check, diagnostic, 0, piece.problem);
			mia_plugin_help(check, diagnostic, "`{}` and `{:spec}` are replaced by the next value; write `{{` and `}}` for braces");
			return;
		}
		if (!piece.placeholder) continue;
		const size_t value = fixed + placeholders;
		placeholders += 1;
		if (value >= count) continue;
		const mia_plugin_type_s* const type = mia_plugin_check_type(check, value);
		const char* const problem = _mia_io_spec_fits(&piece.spec, _mia_io_value_kind(check, type));
		if (problem != NULL) {
			char found[160];
			(void)snprintf(found, sizeof(found), "this is a `%s`", mia_plugin_type_name(check, type));
			mia_plugin_diagnostic_s* const diagnostic = mia_plugin_error(check, "E0530", 0, "the format specifier does not fit its value");
			mia_plugin_label(check, diagnostic, 0, problem);
			mia_plugin_label(check, diagnostic, value, found);
			return;
		}
	}
	const size_t values = count - fixed;
	if (placeholders != values) {
		char message[128], placeholder_count[64];
		(void)snprintf(message, sizeof(message), "the format string has %zu placeholder(s), but %zu value(s) are given", placeholders, values);
		(void)snprintf(placeholder_count, sizeof(placeholder_count), "%zu placeholder(s)", placeholders);
		mia_plugin_diagnostic_s* const diagnostic = mia_plugin_error(check, "E0530", mia_plugin_call, message);
		mia_plugin_label(check, diagnostic, mia_plugin_call, "they must match");
		mia_plugin_label(check, diagnostic, 0, placeholder_count);
	}
}

// --- values as text ----------------------------------------------------------------------

/// bytes as text; one trailing NUL is left out when 'strip_nul'.
static void _mia_io_bytes_text(mia_string_s* const out, const uint8_t* const bytes, const uint64_t length, const bool strip_nul) {
	const uint64_t count = (strip_nul && length > 0 && 0 == bytes[length - 1]) ? length - 1 : length;
	if (count > 0) mia_format_utf8(out, mia_view_new((const char*)bytes, (size_t)count));
}

/// the value of 'type' at 'memory' as print shows it; arrays and slices of
/// u8 are text, without their trailing NUL when they are a c string.
static void _mia_io_value(mia_plugin_call_s* const call, mia_string_s* const out, const uint8_t* const memory, const mia_plugin_type_s* const type,
						  const int depth, const bool c_string) {
	if (depth > _mia_io_nesting_limit) mia_plugin_fail(call, "values nest too deeply");
	switch (mia_plugin_type_kind(call, type)) {
		case mia_plugin_kind_int: {
			const uint32_t bits = mia_plugin_type_bits(call, type);
			uint64_t value = 0;
			memcpy(&value, memory, bits / 8);
			if (mia_plugin_type_signed(call, type)) {
				// sign-extend from the type's width
				const int64_t extended = (bits < 64 && (value >> (bits - 1)) != 0) ? (int64_t)(value | ~(((uint64_t)1 << bits) - 1)) : (int64_t)value;
				mia_string_appendf(out, "%" PRId64, extended);
			}
			else {
				mia_string_appendf(out, "%" PRIu64, value);
			}
			return;
		}
		case mia_plugin_kind_float: {
			const uint32_t bits = mia_plugin_type_bits(call, type);
			double value = 0.0;
			if (32 == bits) {
				float single = 0.0f;
				memcpy(&single, memory, sizeof(single));
				value = (double)single;
			}
			else {
				memcpy(&value, memory, sizeof(value));
			}
			mia_format_float_value(out, value, (int)bits);
			return;
		}
		case mia_plugin_kind_bool:
			mia_string_append_cstr(out, memory[0] != 0 ? "true" : "false");
			return;
		case mia_plugin_kind_function:
			mia_string_appendf(out, "<function %s>", mia_plugin_function_name_at(call, memory));
			return;
		case mia_plugin_kind_array:
		case mia_plugin_kind_slice: {
			const mia_plugin_type_s* const element = mia_plugin_type_element(call, type);
			const uint8_t* items = memory;
			uint64_t count = mia_plugin_type_length(call, type);
			if (mia_plugin_kind_slice == mia_plugin_type_kind(call, type)) {
				const mia_plugin_slice_s slice = mia_plugin_slice_at(call, memory, type);
				items = slice.data;
				count = slice.length;
			}
			if (0 == strcmp(mia_plugin_type_name(call, element), "u8")) {
				_mia_io_bytes_text(out, items, count, c_string);
				return;
			}
			const uint64_t size = mia_plugin_type_size(call, element);
			mia_string_append_char(out, '{');
			for (uint64_t i = 0; i < count; ++i) {
				if (i > 0) mia_string_append_cstr(out, ", ");
				_mia_io_value(call, out, items + i * size, element, depth + 1, false);
			}
			mia_string_append_char(out, '}');
			return;
		}
		case mia_plugin_kind_struct: {
			const size_t fields = mia_plugin_type_field_count(call, type);
			mia_string_append_char(out, '{');
			for (size_t i = 0; i < fields; ++i) {
				if (i > 0) mia_string_append_cstr(out, ", ");
				mia_string_appendf(out, "%s: ", mia_plugin_type_field_name(call, type, i));
				_mia_io_value(call, out, memory + mia_plugin_type_field_offset(call, type, i), mia_plugin_type_field_type(call, type, i), depth + 1, false);
			}
			mia_string_append_char(out, '}');
			return;
		}
	}
}

/// a value by a spec: numbers through the spec, everything else as its text, padded.
static void _mia_io_value_spec(mia_plugin_call_s* const call, mia_string_s* const out, const uint8_t* const memory, const mia_plugin_type_s* const type,
							   const _mia_io_spec_s* const spec, const bool c_string) {
	const mia_plugin_kind_e kind = mia_plugin_type_kind(call, type);
	if (mia_plugin_kind_int == kind) {
		const uint32_t bits = mia_plugin_type_bits(call, type);
		uint64_t value = 0;
		memcpy(&value, memory, bits / 8);
		bool negative = false;
		if (mia_plugin_type_signed(call, type) && (value >> (bits - 1)) != 0) {
			// the magnitude of a negative value of the type's width
			const uint64_t extended = (bits < 64) ? (value | ~(((uint64_t)1 << bits) - 1)) : value;
			negative = true;
			value = (uint64_t)0 - extended;
		}
		_mia_io_format_integer(out, spec, negative, value);
		return;
	}
	if (mia_plugin_kind_float == kind) {
		const uint32_t bits = mia_plugin_type_bits(call, type);
		double value = 0.0;
		if (32 == bits) {
			float single = 0.0f;
			memcpy(&single, memory, sizeof(single));
			value = (double)single;
		}
		else {
			memcpy(&value, memory, sizeof(value));
		}
		_mia_io_format_float(out, spec, value, (int)bits);
		return;
	}
	mia_string_s* const text = &_mia_io_piece;
	mia_string_clear(text);
	_mia_io_value(call, text, memory, type, 0, c_string);
	_mia_io_format_text(out, spec, text);
}

/// the format (argument 0) with each placeholder replaced by the next
/// value; '{{' and '}}' are braces. the check matched them already.
static void _mia_io_formatted(mia_plugin_call_s* const call, mia_string_s* const out) {
	const mia_plugin_slice_s format = mia_plugin_get_slice(call, 0);
	const uint8_t* const data = format.data;
	const size_t count = mia_plugin_count(call);
	size_t next = mia_plugin_fixed_count(call);
	mia_string_s* const text = &_mia_io_text;
	size_t at = 0;
	_mia_io_piece_s piece;
	while (_mia_io_next_placeholder(data, (size_t)format.length, &at, &piece)) {
		// the text, with doubled braces written once
		mia_string_clear(text);
		for (size_t i = piece.text_start; i < piece.text_end; ++i) {
			mia_string_append_char(text, (char)data[i]);
			if (('{' == data[i] || '}' == data[i]) && i + 1 < piece.text_end && data[i + 1] == data[i]) ++i;
		}
		mia_format_utf8(out, mia_string_view(text));
		if (!piece.placeholder || piece.problem != NULL || next >= count) continue;
		const mia_plugin_type_s* const type = mia_plugin_argument_type(call, next);
		const uint8_t* const memory = mia_plugin_get_memory(call, next);
		const bool c_string = 0 != (mia_plugin_argument_flags(call, next) & _mia_io_flag_c_string);
		if (piece.spec_end > piece.spec_start) _mia_io_value_spec(call, out, memory, type, &piece.spec, c_string);
		else _mia_io_value(call, out, memory, type, 0, c_string);
		++next;
	}
}

static void _mia_io_print(mia_plugin_call_s* const call, const bool line) {
	mia_string_s* const out = &_mia_io_out;
	mia_string_clear(out);
	_mia_io_formatted(call, out);
	if (line) mia_string_append_char(out, '\n');
	mia_plugin_write(call, out->data, out->length);
}

static mia_plugin_value_s _mia_io_run_print(mia_plugin_call_s* const call) {
	_mia_io_print(call, false);
	return mia_plugin_make_none(call);
}

static mia_plugin_value_s _mia_io_run_println(mia_plugin_call_s* const call) {
	_mia_io_print(call, true);
	return mia_plugin_make_none(call);
}

// --- registration ------------------------------------------------------------

mia_plugin_export void mia_plugin_exit(void) {
	_mia_io_exit();
}

mia_plugin_export bool mia_plugin_init(mia_plugin_api_s* const api) {
	if (!mia_plugin_compatible(api, mia_plugin_api_version)) return false;
	return mia_plugin_register(api, "io", "io.print", _mia_io_run_print, _mia_io_check_format)
		&& mia_plugin_register(api, "io", "io.println", _mia_io_run_println, _mia_io_check_format);
}
