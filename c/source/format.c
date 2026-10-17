
#include "mc/format.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void mc_format_float_repr(mc_string_s* const out, const double value) {
	if (isnan(value)) {
		mc_string_append_cstr(out, "nan");
		return;
	}
	if (isinf(value)) {
		mc_string_append_cstr(out, value > 0 ? "inf" : "-inf");
		return;
	}
	// the shortest digits that read back: "d.ddde+XX".
	char text[64];
	for (int precision = 0; precision <= 17; ++precision) {
		(void)snprintf(text, sizeof(text), "%.*e", precision, value);
		if (strtod(text, NULL) == value) break;
	}
	const char* cursor = text;
	bool negative = false;
	if ('-' == *cursor) {
		negative = true;
		++cursor;
	}
	char digits[32];
	size_t count = 0;
	while (*cursor != 'e' && *cursor != '\0') {
		if (*cursor != '.') digits[count++] = *cursor;
		++cursor;
	}
	digits[count] = '\0';
	const int exponent = ('e' == *cursor) ? atoi(cursor + 1) : 0;
	while (count > 1 && '0' == digits[count - 1]) digits[--count] = '\0';
	if (negative) mc_string_append_char(out, '-');
	if (exponent >= -4 && exponent < 16) {
		if (exponent >= 0) {
			for (int i = 0; i <= exponent; ++i) mc_string_append_char(out, (size_t)i < count ? digits[i] : '0');
			mc_string_append_char(out, '.');
			if ((size_t)(exponent + 1) < count) mc_string_append(out, mc_view_new(digits + exponent + 1, count - (size_t)exponent - 1));
			else mc_string_append_char(out, '0');
		}
		else {
			mc_string_append_cstr(out, "0.");
			mc_string_append_repeat(out, '0', (size_t)(-exponent - 1));
			mc_string_append(out, mc_view_new(digits, count));
		}
		return;
	}
	mc_string_append_char(out, digits[0]);
	if (count > 1) {
		mc_string_append_char(out, '.');
		mc_string_append(out, mc_view_new(digits + 1, count - 1));
	}
	mc_string_appendf(out, "e%c%02d", exponent < 0 ? '-' : '+', abs(exponent));
}

void mc_format_float_g(mc_string_s* const out, const double value, const int precision) {
	mc_string_appendf(out, "%.*g", precision, value);
}

void mc_format_f32(mc_string_s* const out, const double value) {
	char text[64];
	for (int precision = 1; precision < 10; ++precision) {
		(void)snprintf(text, sizeof(text), "%.*g", precision, value);
		if ((double)(float)strtod(text, NULL) == value) break;
	}
	mc_string_append_cstr(out, text);
}

void mc_format_float_value(mc_string_s* const out, const double value, const int bits) {
	if (isnan(value)) {
		mc_string_append_cstr(out, "nan");
		return;
	}
	if (isinf(value)) {
		mc_string_append_cstr(out, value > 0 ? "inf" : "-inf");
		return;
	}
	const size_t start = out->length;
	if (32 == bits) mc_format_f32(out, value);
	else mc_format_float_repr(out, value);
	bool marked = false;
	for (size_t i = start; i < out->length; ++i) marked = marked || 'e' == out->data[i] || '.' == out->data[i] || 'n' == out->data[i];
	if (!marked) mc_string_append_cstr(out, ".0");
}

void mc_format_uint128(mc_string_s* const out, const mc_i128_s value) {
	char text[48];
	mc_string_append_cstr(out, mc_u128_text(text, value));
}

void mc_format_int128(mc_string_s* const out, const mc_i128_s value) {
	char text[48];
	mc_string_append_cstr(out, mc_i128_text(text, value));
}

static void _mc_repr(mc_string_s* const out, const mc_view_s text, const bool bytes) {
	const bool single = memchr(text.data, '\'', text.length) != NULL;
	const bool double_ = memchr(text.data, '"', text.length) != NULL;
	const char quote = (single && !double_) ? '"' : '\'';
	mc_string_append_char(out, quote);
	for (size_t i = 0; i < text.length; ++i) {
		const uint8_t c = (uint8_t)text.data[i];
		if (c == (uint8_t)quote || '\\' == c) {
			mc_string_append_char(out, '\\');
			mc_string_append_char(out, (char)c);
		}
		else if ('\t' == c) mc_string_append_cstr(out, "\\t");
		else if ('\n' == c) mc_string_append_cstr(out, "\\n");
		else if ('\r' == c) mc_string_append_cstr(out, "\\r");
		else if (c < 0x20 || 0x7F == c || (bytes && c >= 0x80)) mc_string_appendf(out, "\\x%02x", c);
		else mc_string_append_char(out, (char)c);
	}
	mc_string_append_char(out, quote);
}

void mc_format_repr_text(mc_string_s* const out, const mc_view_s text) {
	_mc_repr(out, text, false);
}

void mc_format_repr_bytes(mc_string_s* const out, const mc_view_s bytes) {
	_mc_repr(out, bytes, true);
}

void mc_format_utf8(mc_string_s* const out, const mc_view_s bytes) {
	const uint8_t* const data = (const uint8_t*)bytes.data;
	size_t i = 0;
	while (i < bytes.length) {
		const uint8_t first = data[i];
		size_t length = 0;
		uint8_t low = 0x80, high = 0xBF;   // the second byte's range
		if (first < 0x80) length = 1;
		else if (first >= 0xC2 && first <= 0xDF) length = 2;
		else if (0xE0 == first) length = 3, low = 0xA0;
		else if ((first >= 0xE1 && first <= 0xEC) || 0xEE == first || 0xEF == first) length = 3;
		else if (0xED == first) length = 3, high = 0x9F;
		else if (0xF0 == first) length = 4, low = 0x90;
		else if (first >= 0xF1 && first <= 0xF3) length = 4;
		else if (0xF4 == first) length = 4, high = 0x8F;
		size_t taken = 1;
		while (taken < length && i + taken < bytes.length) {
			const uint8_t byte = data[i + taken];
			const bool fits = (1 == taken) ? (byte >= low && byte <= high) : (byte >= 0x80 && byte <= 0xBF);
			if (!fits) break;
			++taken;
		}
		if (length > 0 && taken == length) mc_string_append(out, mc_view_new(bytes.data + i, length));
		else mc_string_append_cstr(out, "\xEF\xBF\xBD");
		i += taken;
	}
}
