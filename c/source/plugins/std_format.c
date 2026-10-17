
/// the 'std' plugin: format specifiers, '{:spec}' in the formats of
/// io.print and io.println (see std.h for the grammar). the same parser
/// serves the check at compile time and the formatting at run time.

#include "std.h"

#include <ctype.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

// --- the format string -------------------------------------------------------------------

bool mc_std_next_placeholder(const uint8_t* const data, const size_t length, size_t* const at, mc_std_piece_s* const piece) {
	size_t index = *at;
	*piece = (mc_std_piece_s) { .text_start = index, };
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
				else piece->problem = mc_std_parse_spec(data + piece->spec_start + 1, piece->spec_end - piece->spec_start - 1, &piece->spec);
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

static bool _mc_is_align(const uint8_t character) {
	return '<' == character || '>' == character || '^' == character;
}

/// a decimal number of at most 9 digits; false when there is none.
static bool _mc_number(const uint8_t* const text, const size_t length, size_t* const at, size_t* const value) {
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

const char* mc_std_parse_spec(const uint8_t* const text, const size_t length, mc_std_spec_s* const spec) {
	*spec = (mc_std_spec_s) { .fill = ' ', };
	static char problem[96];
	size_t at = 0;
	// [[fill]align]
	if (length >= 2 && _mc_is_align(text[1]) && text[0] != '{' && text[0] != '}') {
		if (text[0] < 0x20 || text[0] >= 0x7F) return "the fill character must be printable ascii";
		spec->fill = (char)text[0];
		spec->align = (char)text[1];
		at = 2;
	}
	else if (length >= 1 && _mc_is_align(text[0])) {
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
	if (at < length && text[at] >= '0' && text[at] <= '9' && !_mc_number(text, length, &at, &spec->width)) return "the width is too large";
	if (at < length && '.' == text[at]) {
		++at;
		if (!_mc_number(text, length, &at, &spec->precision)) return "`.` must be followed by the precision, a number";
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

const char* mc_std_spec_fits(const mc_std_spec_s* const spec, const mc_std_value_e kind) {
	static char problem[96];
	const bool number = mc_std_value_int == kind || mc_std_value_float == kind;
	if (spec->type != 0) {
		const bool integer_type = NULL != strchr("dxXob", spec->type);
		if (mc_std_value_int == kind && !integer_type) {
			(void)snprintf(problem, sizeof(problem), "`%c` is for floats, but the value is an integer", spec->type);
			return problem;
		}
		if (mc_std_value_float == kind && integer_type) {
			(void)snprintf(problem, sizeof(problem), "`%c` is for integers, but the value is a float", spec->type);
			return problem;
		}
		if (!number) {
			(void)snprintf(problem, sizeof(problem), "`%c` is for numbers, but the value is not one", spec->type);
			return problem;
		}
	}
	if (spec->has_precision && kind != mc_std_value_float) return "a precision (`.n`) is for floats only";
	if (spec->sign != 0 && !number) return "a sign (`+` or `-`) is for numbers only";
	if (spec->zero && !number) return "`0` (padding with zeros) is for numbers only";
	if (spec->alternate && (kind != mc_std_value_int || NULL == strchr("xXob", spec->type) || 0 == spec->type)) {
		return "`#` is for integers written with `x`, `X`, `o` or `b`";
	}
	return NULL;
}

// --- formatting --------------------------------------------------------------------------

/// the code points in utf-8 text (bytes that do not continue a sequence).
static size_t _mc_width(const char* const text, const size_t length) {
	size_t points = 0;
	for (size_t i = 0; i < length; ++i) if (0x80 != ((uint8_t)text[i] & 0xC0)) ++points;
	return points;
}

/// appends 'body' to 'out', padded to the width of the spec. numbers are
/// right-aligned by default, everything else left-aligned; with '0', a
/// number is padded with zeros after its sign and prefix ('head' bytes).
static void _mc_pad(mc_string_s* const out, const mc_std_spec_s* const spec, const char* const body, const size_t length, const size_t head,
					const bool number) {
	const size_t width = _mc_width(body, length);
	if (width >= spec->width) {
		mc_string_append(out, mc_view_new(body, length));
		return;
	}
	const size_t missing = spec->width - width;
	if (number && spec->zero && 0 == spec->align) {
		mc_string_append(out, mc_view_new(body, head));
		mc_string_append_repeat(out, '0', missing);
		mc_string_append(out, mc_view_new(body + head, length - head));
		return;
	}
	const char align = (spec->align != 0) ? spec->align : (number ? '>' : '<');
	const size_t before = ('>' == align) ? missing : ('^' == align) ? missing / 2 : 0;
	mc_string_append_repeat(out, spec->fill, before);
	mc_string_append(out, mc_view_new(body, length));
	mc_string_append_repeat(out, spec->fill, missing - before);
}

void mc_std_format_integer(mc_string_s* const out, const mc_std_spec_s* const spec, const bool negative, const uint64_t magnitude) {
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
	_mc_pad(out, spec, body, length, head, true);
}

void mc_std_format_float(mc_string_s* const out, const mc_std_spec_s* const spec, const double value, const int bits) {
	mc_string_s body = {0};
	if (!isfinite(value)) {
		// spelled here: c libraries differ ('-nan', 'nan(ind)'); 'E' writes them in capitals
		const char* const text = isnan(value) ? "nan" : (value > 0) ? "inf" : "-inf";
		for (const char* at = text; *at != '\0'; ++at) mc_string_append_char(&body, ('E' == spec->type) ? (char)toupper((unsigned char)*at) : *at);
	}
	else if ((0 == spec->type || 'g' == spec->type) && !spec->has_precision) {
		mc_format_float_value(&body, value, bits);
	}
	else {
		const char type = (spec->type != 0) ? spec->type : 'g';
		const int precision = spec->has_precision ? (int)spec->precision : 6;
		char format[16];
		(void)snprintf(format, sizeof(format), "%%.%d%c", precision, type);
		char text[160];
#if defined(__GNUC__)
#	pragma GCC diagnostic push
#	pragma GCC diagnostic ignored "-Wformat-nonliteral"
#endif
		(void)snprintf(text, sizeof(text), format, value);
#if defined(__GNUC__)
#	pragma GCC diagnostic pop
#endif
		mc_string_append_cstr(&body, text);
	}
	if ('+' == spec->sign && body.length > 0 && body.data[0] != '-') {
		mc_string_s signed_ = {0};
		mc_string_append_char(&signed_, '+');
		mc_string_append(&signed_, mc_string_view(&body));
		mc_string_free(&body);
		body = signed_;
	}
	const size_t head = (body.length > 0 && ('-' == body.data[0] || '+' == body.data[0])) ? 1 : 0;
	// zeros do not pad infinities and nan, spaces do
	mc_std_spec_s padding = *spec;
	padding.zero = spec->zero && isfinite(value);
	_mc_pad(out, &padding, (body.data != NULL) ? body.data : "", body.length, head, true);
	mc_string_free(&body);
}

void mc_std_format_text(mc_string_s* const out, const mc_std_spec_s* const spec, const mc_string_s* const text) {
	_mc_pad(out, spec, (text->data != NULL) ? text->data : "", text->length, 0, false);
}
