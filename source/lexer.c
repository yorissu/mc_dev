
#include "mia/lexer.h"

#include <string.h>
#include <stdlib.h>

typedef struct {
	const mia_source_s* source  ;
	const char*         text    ;
	uint32_t            length  ;
	uint32_t            position;
	mia_arena_s*        arena   ;
	mia_diag_s*         diag    ;
	mia_token_list_s*   tokens  ;
} _mia_lexer_s;

static char _mia_lexer_peek(const _mia_lexer_s* const l, const uint32_t offset) {
	const uint32_t index = l->position + offset;
	return (index < l->length) ? l->text[index] : '\0';
}

static bool _mia_lexer_at_end(const _mia_lexer_s* const l, const uint32_t offset) {
	return l->position + offset >= l->length;
}

static mia_span_s _mia_lexer_span(const _mia_lexer_s* const l, const uint32_t begin, const uint32_t end) {
	return (const mia_span_s) { .file = l->source->id, .begin = begin, .end = end, };
}

static bool _mia_lexer_is_digit(const char c) {
	return c >= '0' && c <= '9';
}

/// letters, '_', and every byte of a multi-byte utf-8 character.
static bool _mia_lexer_is_identifier_start(const char c) {
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || '_' == c || ((uint8_t)c >= 0x80);
}

static bool _mia_lexer_is_identifier_part(const char c) {
	return _mia_lexer_is_identifier_start(c) || _mia_lexer_is_digit(c);
}

/// whether 'text' is valid utf-8: no stray continuation bytes, no
/// truncated, overlong or surrogate sequences, nothing above U+10FFFF.
static bool _mia_lexer_valid_utf8(const mia_view_s text) {
	const uint8_t* const bytes = (const uint8_t*)text.data;
	size_t at = 0;
	while (at < text.length) {
		const uint8_t first = bytes[at];
		size_t count = 0;
		uint32_t point = 0, minimum = 0;
		if (first < 0x80) { at += 1; continue; }
		else if (0xC0 == (first & 0xE0)) { count = 1; point = first & 0x1F; minimum = 0x80; }
		else if (0xE0 == (first & 0xF0)) { count = 2; point = first & 0x0F; minimum = 0x800; }
		else if (0xF0 == (first & 0xF8)) { count = 3; point = first & 0x07; minimum = 0x10000; }
		else return false;
		if (count >= text.length - at) return false;   // truncated at the end
		for (size_t i = 1; i <= count; ++i) {
			if (0x80 != (bytes[at + i] & 0xC0)) return false;
			point = (point << 6) | (bytes[at + i] & 0x3F);
		}
		if (point < minimum || point > 0x10FFFF || (point >= 0xD800 && point <= 0xDFFF)) return false;
		at += count + 1;
	}
	return true;
}

/// the bytes of the utf-8 character starting at 'position'.
static uint32_t _mia_lexer_character_length(const _mia_lexer_s* const l, const uint32_t position) {
	const uint8_t first = (uint8_t)l->text[position];
	uint32_t length = (first < 0x80) ? 1 : (first >> 5) == 0x6 ? 2 : (first >> 4) == 0xE ? 3 : (first >> 3) == 0x1E ? 4 : 1;
	if (position + length > l->length) length = l->length - position;
	return length;
}

static void _mia_lexer_push(_mia_lexer_s* const l, const mia_token_s given) {
	mia_token_s token = given;
	token.text = mia_view_slice(l->source->text, token.span.begin, token.span.end);
	mia_vector_push(l->tokens, token);
}

static void _mia_lexer_block_comment(_mia_lexer_s* const l) {
	mia_vector_of(uint32_t) starts = {0};
	mia_vector_push(&starts, l->position);
	l->position += 2;
	while (starts.count > 0) {
		if (_mia_lexer_at_end(l, 0)) {
			const uint32_t start = mia_vector_last(&starts);
			mia_diag_help(mia_diag_label_main(mia_diag_error(l->diag, mia_diag_E0103, _mia_lexer_span(l, start, start + 2), "unterminated block comment"),
				"this comment is never closed"), "block comments nest: every `/*` needs its own `*/`");
			break;
		}
		if ('/' == _mia_lexer_peek(l, 0) && '*' == _mia_lexer_peek(l, 1)) {
			mia_vector_push(&starts, l->position);
			l->position += 2;
		}
		else if ('*' == _mia_lexer_peek(l, 0) && '/' == _mia_lexer_peek(l, 1)) {
			(void)mia_vector_pop(&starts);
			l->position += 2;
		}
		else {
			l->position += 1;
		}
	}
	mia_vector_free(&starts);
}

static void _mia_lexer_skip_trivia(_mia_lexer_s* const l) {
	while (!_mia_lexer_at_end(l, 0)) {
		const char c = _mia_lexer_peek(l, 0);
		if (' ' == c || '\t' == c || '\r' == c || '\n' == c) {
			l->position += 1;
		}
		else if ('#' == c || ('/' == c && '/' == _mia_lexer_peek(l, 1))) {
			while (!_mia_lexer_at_end(l, 0) && '\n' != _mia_lexer_peek(l, 0)) l->position += 1;
		}
		else if ('/' == c && '*' == _mia_lexer_peek(l, 1)) {
			_mia_lexer_block_comment(l);
		}
		else {
			return;
		}
	}
}

/// digits of 'valid' (lower case) and '_' separators.
static void _mia_lexer_digits(_mia_lexer_s* const l, const char* const valid) {
	while (!_mia_lexer_at_end(l, 0)) {
		const char c = _mia_lexer_peek(l, 0);
		const char lower = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
		if ('_' != c && NULL == strchr(valid, lower)) break;
		if ('\0' == lower) break;
		l->position += 1;
	}
}

static void _mia_lexer_number(_mia_lexer_s* const l) {
	const uint32_t start = l->position;
	uint32_t base = 10;
	bool is_float = false;
	const char second = _mia_lexer_peek(l, 1);
	uint32_t body_start = start;
	if ('0' == _mia_lexer_peek(l, 0) && ('x' == second || 'X' == second || 'o' == second || 'O' == second || 'b' == second || 'B' == second)) {
		base = ('x' == second || 'X' == second) ? 16 : ('o' == second || 'O' == second) ? 8 : 2;
		l->position += 2;
		body_start = l->position;
		_mia_lexer_digits(l, (16 == base) ? "0123456789abcdef" : (8 == base) ? "01234567" : "01");
	}
	else {
		_mia_lexer_digits(l, "0123456789");
		if ('.' == _mia_lexer_peek(l, 0) && _mia_lexer_is_digit(_mia_lexer_peek(l, 1))) {
			l->position += 1;
			_mia_lexer_digits(l, "0123456789");
			is_float = true;
		}
		const char e = _mia_lexer_peek(l, 0);
		if (('e' == e || 'E' == e) && (_mia_lexer_is_digit(_mia_lexer_peek(l, 1))
				|| (('+' == _mia_lexer_peek(l, 1) || '-' == _mia_lexer_peek(l, 1)) && _mia_lexer_is_digit(_mia_lexer_peek(l, 2))))) {
			l->position += ('+' == _mia_lexer_peek(l, 1) || '-' == _mia_lexer_peek(l, 1)) ? 2 : 1;
			_mia_lexer_digits(l, "0123456789");
			is_float = true;
		}
	}
	const uint32_t body_end = l->position;
	while (!_mia_lexer_at_end(l, 0) && _mia_lexer_is_identifier_part(_mia_lexer_peek(l, 0))) l->position += 1;
	const mia_view_s body = mia_view_new(l->text + body_start, body_end - body_start);
	const mia_view_s suffix_text = mia_view_new(l->text + body_end, l->position - body_end);
	const mia_span_s span = _mia_lexer_span(l, start, l->position);

	// the body without separators.
	char cleaned[512];
	size_t cleaned_length = 0;
	for (size_t i = 0; i < body.length && cleaned_length + 1 < sizeof(cleaned); ++i) {
		if ('_' != body.data[i]) cleaned[cleaned_length++] = body.data[i];
	}
	cleaned[cleaned_length] = '\0';

	const mia_scalar_e suffix = (suffix_text.length > 0) ? mia_scalar_from_name(suffix_text) : mia_scalar_none;
	char* problem = NULL;
	bool bad_separator = body.length > 0 && ('_' == body.data[0] || '_' == body.data[body.length - 1]);
	for (size_t i = 0; i + 1 < body.length; ++i) {
		const char a = body.data[i], b = body.data[i + 1];
		if (('_' == a && ('.' == b || '_' == b)) || ('.' == a && '_' == b)) bad_separator = true;
	}
	if (0 == cleaned_length) {
		problem = mia_strdup("a number needs at least one digit");
	}
	else if (cleaned_length + 1 >= sizeof(cleaned)) {
		problem = mia_strdup("the number is too long");
	}
	else if (bad_separator) {
		problem = mia_strdup("`_` may only separate digits");
	}
	else if (suffix_text.length > 0 && (mia_scalar_none == suffix || 'b' == mia_scalar_kind(suffix))) {
		problem = mia_string_format("unknown number suffix `" mia_view_fmt "`", mia_view_arg(suffix_text));
	}
	else if (suffix != mia_scalar_none && 'f' == mia_scalar_kind(suffix) && base != 10) {
		problem = mia_strdup("only decimal numbers can have a float suffix");
	}
	else if (is_float && suffix != mia_scalar_none && 'i' == mia_scalar_kind(suffix)) {
		problem = mia_string_format("a float cannot have the integer suffix `%s`", mia_scalar_name(suffix));
	}
	mia_token_s token = { .span = span, .suffix = mia_scalar_none, };
	if (problem != NULL) {
		mia_diag_label_main(mia_diag_error(l->diag, mia_diag_E0105, span, "invalid number literal"), "%s", problem);
		mia_free(problem);
		token.kind = mia_token_integer;
	}
	else if (is_float || (suffix != mia_scalar_none && 'f' == mia_scalar_kind(suffix))) {
		token.kind = mia_token_float;
		token.real = strtod(cleaned, NULL);
		token.suffix = suffix;
	}
	else {
		token.kind = mia_token_integer;
		// the value as an unsigned 128-bit integer; more digits than fit are an error
		// constants are signed 128-bit integers: 2^127 - 1 at most
		const mia_int128_s largest = mia_int128_from_halves((uint64_t)INT64_MAX, UINT64_MAX);
		mia_int128_s value = mia_int128_zero();
		bool overflow = false;
		for (size_t i = 0; i < cleaned_length && !overflow; ++i) {
			const char c = cleaned[i];
			const mia_int128_s digit = mia_int128_from_uint((c >= '0' && c <= '9') ? (uint32_t)(c - '0') : (uint32_t)((c | 0x20) - 'a' + 10));
			mia_int128_s limit;
			mia_int128_divide_unsigned(mia_int128_sub(largest, digit), mia_int128_from_uint(base), &limit, NULL);
			overflow = mia_int128_compare_unsigned(value, limit) > 0;
			value = mia_int128_add(mia_int128_mul(value, mia_int128_from_uint(base)), digit);
		}
		if (overflow) {
			mia_diag_label_main(mia_diag_error(l->diag, mia_diag_E0105, span, "invalid number literal"), "the number does not fit in 128 bits (2^127 - 1 at most)");
			value = mia_int128_zero();
		}
		token.integer = value;
		token.suffix = suffix;
	}
	_mia_lexer_push(l, token);
}

/// one escape after its backslash; false when invalid (reported).
static bool _mia_lexer_escape(_mia_lexer_s* const l, uint8_t* const value) {
	const uint32_t start = l->position - 1;
	const char c = _mia_lexer_peek(l, 0);
	const char* const simple = "nt0r\\\"'";
	const uint8_t values[] = { 10, 9, 0, 13, 92, 34, 39, };
	if (!_mia_lexer_at_end(l, 0) && strchr(simple, c) != NULL) {
		*value = values[strchr(simple, c) - simple];
		l->position += 1;
		return true;
	}
	if ('x' == c) {
		const char h1 = _mia_lexer_peek(l, 1), h2 = _mia_lexer_peek(l, 2);
		const char* const hex = "0123456789abcdefABCDEF";
		if (h1 != '\0' && h2 != '\0' && strchr(hex, h1) != NULL && strchr(hex, h2) != NULL) {
			const char digits[3] = { h1, h2, '\0', };
			*value = (uint8_t)strtoul(digits, NULL, 16);
			l->position += 3;
			return true;
		}
	}
	if (!_mia_lexer_at_end(l, 0) && '\n' != c) l->position += _mia_lexer_character_length(l, l->position);
	mia_diag_help(mia_diag_label_main(mia_diag_error(l->diag, mia_diag_E0104, _mia_lexer_span(l, start, l->position), "invalid escape sequence"),
		"unknown escape"), "valid escapes: \\n \\t \\r \\0 \\\\ \\\" \\' \\xHH");
	return false;
}

/// the end of a string literal after its closing quotes: the 'c' suffix
/// (a NUL-terminated string), then the token.
static void _mia_lexer_string_end(_mia_lexer_s* const l, const uint32_t start, mia_string_s* const data) {
	const uint32_t suffix_start = l->position;
	while (!_mia_lexer_at_end(l, 0) && _mia_lexer_is_identifier_part(_mia_lexer_peek(l, 0))) l->position += 1;
	const mia_view_s suffix = mia_view_new(l->text + suffix_start, l->position - suffix_start);
	bool c_string = false;
	if (suffix.length > 0) {
		if (mia_view_eq_cstr(suffix, "c")) {
			c_string = true;
		}
		else {
			mia_diag_help(mia_diag_label_main(mia_diag_error(l->diag, mia_diag_E0107, _mia_lexer_span(l, suffix_start, l->position),
				"unknown string suffix `" mia_view_fmt "`", mia_view_arg(suffix)), "only `c` is allowed"),
				"`\"text\"c` is a NUL-terminated string");
		}
	}
	if (c_string) {
		if (memchr(mia_string_cstr(data), 0, data->length) != NULL) {
			mia_diag_label_main(mia_diag_error(l->diag, mia_diag_E0108, _mia_lexer_span(l, start, l->position), "C string contains a NUL byte"),
				"C would stop reading at the first NUL");
		}
		mia_string_append_char(data, '\0');
	}
	mia_token_s token = { .kind = mia_token_string, .span = _mia_lexer_span(l, start, l->position), .suffix = mia_scalar_none, .c = c_string, };
	token.bytes = mia_view_new(mia_arena_strndup(l->arena, mia_string_cstr(data), data->length), data->length);
	mia_string_free(data);
	_mia_lexer_push(l, token);
}

/// the end of the line that starts at 'at' (its '\n', or the end of the text).
static uint32_t _mia_lexer_line_end(const _mia_lexer_s* const l, const uint32_t start) {
	uint32_t at = start;
	while (at < l->length && l->text[at] != '\n') ++at;
	return at;
}

/// a multiline string, '"""' to '"""'. its text is the lines between them,
/// without the indentation of the closing '"""' (swift's rule), so it can
/// be indented with the code around it; escapes work as in '"..."'.
static void _mia_lexer_multiline_string(_mia_lexer_s* const l) {
	const uint32_t start = l->position;
	l->position += 3;
	// the opening '"""' ends its line
	uint32_t at = l->position;
	while (at < l->length && (' ' == l->text[at] || '\t' == l->text[at] || '\r' == l->text[at])) ++at;
	if (at < l->length && l->text[at] != '\n') {
		mia_diag_help(mia_diag_label_main(mia_diag_error(l->diag, mia_diag_E0109, _mia_lexer_span(l, start, start + 3), "a multiline string starts on the next line"),
			"text follows the opening `\"\"\"`"), "write `\"...\"` for a string on one line");
	}
	// the closing '"""': the first line that holds nothing else before it
	const uint32_t opening_end = _mia_lexer_line_end(l, at);
	const uint32_t first = (opening_end < l->length) ? opening_end + 1 : opening_end;
	uint32_t line = first, close = 0, indent = 0;
	bool closed = false;
	while (line < l->length && !closed) {
		uint32_t text = line;
		while (text < l->length && (' ' == l->text[text] || '\t' == l->text[text])) ++text;
		if (text + 3 <= l->length && 0 == strncmp(l->text + text, "\"\"\"", 3)) {
			closed = true;
			close = text;
			indent = text - line;
		}
		else {
			line = _mia_lexer_line_end(l, line) + 1;
		}
	}
	mia_string_s data = {0};
	if (!closed) {
		mia_diag_label_main(mia_diag_error(l->diag, mia_diag_E0102, _mia_lexer_span(l, start, start + 3), "unterminated multiline string"),
			"this string is never closed");
		l->position = l->length;
		_mia_lexer_string_end(l, start, &data);
		return;
	}
	// every line between, without the indentation; the last line break is not part of the text
	for (uint32_t row = first; row < line; row = _mia_lexer_line_end(l, row) + 1) {
		const uint32_t end = _mia_lexer_line_end(l, row);
		uint32_t content_end = end;
		if (content_end > row && '\r' == l->text[content_end - 1]) --content_end;
		uint32_t blank = row;
		while (blank < content_end && (' ' == l->text[blank] || '\t' == l->text[blank])) ++blank;
		if (row > first) mia_string_append_char(&data, '\n');
		if (blank == content_end) continue;   // a blank line, whatever its indentation
		if (content_end - row < indent || strncmp(l->text + row, l->text + line, indent) != 0) {
			mia_diag_help(mia_diag_label_main(mia_diag_error(l->diag, mia_diag_E0109, _mia_lexer_span(l, row, blank), "a line of a multiline string is indented less than its end"),
				"indented less than the closing `\"\"\"`"), "indent every line at least as much as the closing `\"\"\"`, with the same characters");
			continue;
		}
		l->position = row + indent;
		while (l->position < content_end) {
			const char c = _mia_lexer_peek(l, 0);
			l->position += 1;
			if ('\\' == c) {
				uint8_t value = 0;
				if (_mia_lexer_escape(l, &value)) mia_string_append_char(&data, (char)value);
			}
			else {
				mia_string_append_char(&data, c);
			}
		}
	}
	l->position = close + 3;
	_mia_lexer_string_end(l, start, &data);
}

static void _mia_lexer_string(_mia_lexer_s* const l) {
	if (l->position + 3 <= l->length && 0 == strncmp(l->text + l->position, "\"\"\"", 3)) {
		_mia_lexer_multiline_string(l);
		return;
	}
	const uint32_t start = l->position;
	l->position += 1;
	mia_string_s data = {0};
	for (;;) {
		if (_mia_lexer_at_end(l, 0) || '\n' == _mia_lexer_peek(l, 0)) {
			mia_diag_label_main(mia_diag_error(l->diag, mia_diag_E0102, _mia_lexer_span(l, start, start + 1), "unterminated string literal"),
				"this string is never closed");
			break;
		}
		const char c = _mia_lexer_peek(l, 0);
		l->position += 1;
		if ('"' == c) break;
		if ('\\' == c) {
			uint8_t value = 0;
			if (_mia_lexer_escape(l, &value)) mia_string_append_char(&data, (char)value);
		}
		else {
			mia_string_append_char(&data, c);
		}
	}
	_mia_lexer_string_end(l, start, &data);
}

static void _mia_lexer_character(_mia_lexer_s* const l) {
	const uint32_t start = l->position;
	l->position += 1;
	bool has_value = false;
	uint8_t value = 0;
	const char c = _mia_lexer_peek(l, 0);
	if ('\\' == c) {
		l->position += 1;
		has_value = _mia_lexer_escape(l, &value);
	}
	else if (!_mia_lexer_at_end(l, 0) && '\'' != c && '\n' != c) {
		const uint32_t length = _mia_lexer_character_length(l, l->position);
		l->position += length;
		has_value = true;
		if (1 == length) {
			value = (uint8_t)c;
		}
		else {
			mia_diag_label_main(mia_diag_error(l->diag, mia_diag_E0106, _mia_lexer_span(l, start, l->position), "character literal is not a single byte"),
				"character literals are u8 values");
			value = 0;
		}
	}
	if ('\'' == _mia_lexer_peek(l, 0) && !_mia_lexer_at_end(l, 0) && has_value) {
		l->position += 1;
	}
	else {
		while (!_mia_lexer_at_end(l, 0) && '\'' != _mia_lexer_peek(l, 0) && '\n' != _mia_lexer_peek(l, 0)) l->position += 1;
		if (!_mia_lexer_at_end(l, 0) && '\'' == _mia_lexer_peek(l, 0)) l->position += 1;
		mia_diag_label_main(mia_diag_error(l->diag, mia_diag_E0106, _mia_lexer_span(l, start, l->position), "invalid character literal"),
			"expected exactly one character");
		value = 0;
	}
	const mia_token_s token = { .kind = mia_token_char, .span = _mia_lexer_span(l, start, l->position), .integer = mia_int128_from_uint(value), .suffix = mia_scalar_none, };
	_mia_lexer_push(l, token);
}

/// a character as python's repr() shows it, for messages.
static void _mia_lexer_repr_character(mia_string_s* const out, const mia_view_s character) {
	if (1 == character.length) {
		const uint8_t c = (uint8_t)character.data[0];
		if ('\\' == c) { mia_string_append_cstr(out, "'\\\\'"); return; }
		if ('\'' == c) { mia_string_append_cstr(out, "\"'\""); return; }
		if (c < 0x20 || 0x7F == c) {
			if ('\t' == c) mia_string_append_cstr(out, "'\\t'");
			else if ('\n' == c) mia_string_append_cstr(out, "'\\n'");
			else if ('\r' == c) mia_string_append_cstr(out, "'\\r'");
			else mia_string_appendf(out, "'\\x%02x'", c);
			return;
		}
	}
	mia_string_appendf(out, "'" mia_view_fmt "'", mia_view_arg(character));
}

static void _mia_lexer_token(_mia_lexer_s* const l) {
	const uint32_t start = l->position;
	const char c = _mia_lexer_peek(l, 0);
	if (_mia_lexer_is_digit(c)) {
		_mia_lexer_number(l);
		return;
	}
	if (_mia_lexer_is_identifier_start(c)) {
		while (!_mia_lexer_at_end(l, 0) && _mia_lexer_is_identifier_part(_mia_lexer_peek(l, 0))) l->position += 1;
		const mia_view_s text = mia_view_new(l->text + start, l->position - start);
		if (!_mia_lexer_valid_utf8(text)) {
			mia_diag_label_main(mia_diag_error(l->diag, mia_diag_E0101, _mia_lexer_span(l, start, l->position), "a name that is not valid utf-8"),
				"the file's bytes are not utf-8 text here");
		}
		mia_token_s token = { .kind = mia_token_identifier, .span = _mia_lexer_span(l, start, l->position), .suffix = mia_scalar_none, };
		token.kind = mia_token_find_keyword(text);
		const mia_scalar_e scalar = mia_scalar_from_name(text);
		if (scalar != mia_scalar_none) {
			token.kind = mia_token_type;
			token.suffix = scalar;
		}
		_mia_lexer_push(l, token);
		return;
	}
	if ('"' == c) {
		_mia_lexer_string(l);
		return;
	}
	if ('\'' == c) {
		_mia_lexer_character(l);
		return;
	}
	mia_token_kind_e kind = mia_token_eof;
	const size_t length = mia_token_find_punctuator(mia_view_new(l->text + l->position, l->length - l->position), &kind);
	if (length > 0) {
		l->position += (uint32_t)length;
		const mia_token_s token = { .kind = kind, .span = _mia_lexer_span(l, start, l->position), .suffix = mia_scalar_none, };
		_mia_lexer_push(l, token);
		return;
	}
	const uint32_t unknown = _mia_lexer_character_length(l, start);
	l->position += unknown;
	mia_string_s shown = {0};
	_mia_lexer_repr_character(&shown, mia_view_new(l->text + start, unknown));
	mia_diag_label_main(mia_diag_error(l->diag, mia_diag_E0101, _mia_lexer_span(l, start, l->position), "unknown character %s", mia_string_cstr(&shown)),
		"not part of the language");
	mia_string_free(&shown);
}

void mia_lexer_lex(const mia_source_s* const source, mia_arena_s* const arena, mia_diag_s* const diag, mia_token_list_s* const tokens) {
	_mia_lexer_s lexer = {
		.source = source, .text = source->text.data, .length = (uint32_t)source->text.length,
		.arena = arena, .diag = diag, .tokens = tokens,
	};
	// a byte order mark (some editors write one) is not part of the program
	if (lexer.length >= 3 && 0 == memcmp(lexer.text, "\xEF\xBB\xBF", 3)) lexer.position = 3;
	for (;;) {
		_mia_lexer_skip_trivia(&lexer);
		if (_mia_lexer_at_end(&lexer, 0)) {
			const mia_token_s eof = { .kind = mia_token_eof, .span = _mia_lexer_span(&lexer, lexer.length, lexer.length), .suffix = mia_scalar_none, };
			_mia_lexer_push(&lexer, eof);
			return;
		}
		_mia_lexer_token(&lexer);
	}
}
