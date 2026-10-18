
#include "mia/format.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

void mia_format_float_repr(mia_string_s* const out, const double value) {
	if (isnan(value)) {
		mia_string_append_cstr(out, "nan");
		return;
	}
	if (isinf(value)) {
		mia_string_append_cstr(out, value > 0 ? "inf" : "-inf");
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
	if (negative) mia_string_append_char(out, '-');
	if (exponent >= -4 && exponent < 16) {
		if (exponent >= 0) {
			for (int i = 0; i <= exponent; ++i) mia_string_append_char(out, (size_t)i < count ? digits[i] : '0');
			mia_string_append_char(out, '.');
			if ((size_t)(exponent + 1) < count) mia_string_append(out, mia_view_new(digits + exponent + 1, count - (size_t)exponent - 1));
			else mia_string_append_char(out, '0');
		}
		else {
			mia_string_append_cstr(out, "0.");
			mia_string_append_repeat(out, '0', (size_t)(-exponent - 1));
			mia_string_append(out, mia_view_new(digits, count));
		}
		return;
	}
	mia_string_append_char(out, digits[0]);
	if (count > 1) {
		mia_string_append_char(out, '.');
		mia_string_append(out, mia_view_new(digits + 1, count - 1));
	}
	mia_string_appendf(out, "e%c%02d", exponent < 0 ? '-' : '+', abs(exponent));
}

void mia_format_float_c(mia_string_s* const out, const double value, const int precision, const char type) {
	const size_t start = out->length;
	switch (type) {
		case 'e': mia_string_appendf(out, "%.*e", precision, value); break;
		case 'E': mia_string_appendf(out, "%.*E", precision, value); break;
		case 'f': mia_string_appendf(out, "%.*f", precision, value); break;
		default:  mia_string_appendf(out, "%.*g", precision, value); break;
	}
	// the exponent's digits after its sign: leading zeros beyond two go
	char* const text = out->data + start;
	char* const mark = strpbrk(text, "eE");
	if (NULL == mark || ('+' != mark[1] && '-' != mark[1])) return;
	char* const digits = mark + 2;
	size_t count = strlen(digits), zeros = 0;
	while (count - zeros > 2 && '0' == digits[zeros]) ++zeros;
	if (0 == zeros) return;
	memmove(digits, digits + zeros, count - zeros + 1);
	out->length -= zeros;
}

void mia_format_f32(mia_string_s* const out, const double value) {
	char text[64];
	int precision = 1;
	for (; precision < 10; ++precision) {
		(void)snprintf(text, sizeof(text), "%.*g", precision, value);
		if ((double)(float)strtod(text, NULL) == value) break;
	}
	mia_format_float_c(out, value, (precision < 10) ? precision : 9, 'g');
}

void mia_format_float_value(mia_string_s* const out, const double value, const int bits) {
	if (isnan(value)) {
		mia_string_append_cstr(out, "nan");
		return;
	}
	if (isinf(value)) {
		mia_string_append_cstr(out, value > 0 ? "inf" : "-inf");
		return;
	}
	const size_t start = out->length;
	if (32 == bits) mia_format_f32(out, value);
	else mia_format_float_repr(out, value);
	bool marked = false;
	for (size_t i = start; i < out->length; ++i) marked = marked || 'e' == out->data[i] || '.' == out->data[i] || 'n' == out->data[i];
	if (!marked) mia_string_append_cstr(out, ".0");
}

void mia_format_uint128(mia_string_s* const out, const mia_int128_s value) {
	char text[48];
	mia_string_append_cstr(out, mia_int128_text_unsigned(text, value));
}

void mia_format_int128(mia_string_s* const out, const mia_int128_s value) {
	char text[48];
	mia_string_append_cstr(out, mia_int128_text(text, value));
}

static void _mia_format_repr(mia_string_s* const out, const mia_view_s text, const bool bytes) {
	const bool single = memchr(text.data, '\'', text.length) != NULL;
	const bool has_double_quote = memchr(text.data, '"', text.length) != NULL;
	const char quote = (single && !has_double_quote) ? '"' : '\'';
	mia_string_append_char(out, quote);
	for (size_t i = 0; i < text.length; ++i) {
		const uint8_t c = (uint8_t)text.data[i];
		if (c == (uint8_t)quote || '\\' == c) {
			mia_string_append_char(out, '\\');
			mia_string_append_char(out, (char)c);
		}
		else if ('\t' == c) mia_string_append_cstr(out, "\\t");
		else if ('\n' == c) mia_string_append_cstr(out, "\\n");
		else if ('\r' == c) mia_string_append_cstr(out, "\\r");
		else if (c < 0x20 || 0x7F == c || (bytes && c >= 0x80)) mia_string_appendf(out, "\\x%02x", c);
		else mia_string_append_char(out, (char)c);
	}
	mia_string_append_char(out, quote);
}

void mia_format_repr_text(mia_string_s* const out, const mia_view_s text) {
	_mia_format_repr(out, text, false);
}

void mia_format_repr_bytes(mia_string_s* const out, const mia_view_s bytes) {
	_mia_format_repr(out, bytes, true);
}

void mia_format_utf8(mia_string_s* const out, const mia_view_s bytes) {
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
		if (length > 0 && taken == length) mia_string_append(out, mia_view_new(bytes.data + i, length));
		else mia_string_append_cstr(out, "\xEF\xBF\xBD");
		i += taken;
	}
}
