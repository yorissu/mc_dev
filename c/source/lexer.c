
#include "mc/lexer.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
	const char*     text;
	mc_token_kind_e kind;
} _mc_spelling_s;

static const _mc_spelling_s _mc_keywords[] = {
#define mc_keyword(_name) { #_name, mc_token_##_name, },
#define mc_punctuator(_name, _text)
#define mc_scalar(_name, _kind, _bits, _signed)
#include "mc/tokens.def"
#undef mc_keyword
#undef mc_punctuator
#undef mc_scalar
};

static const _mc_spelling_s _mc_punctuators[] = {
#define mc_keyword(_name)
#define mc_punctuator(_name, _text) { _text, mc_token_##_name, },
#define mc_scalar(_name, _kind, _bits, _signed)
#include "mc/tokens.def"
#undef mc_keyword
#undef mc_punctuator
#undef mc_scalar
};

typedef struct {
	const char* name   ;
	char        kind   ;
	uint32_t    bits   ;
	bool        is_signed;
} _mc_scalar_info_s;

static const _mc_scalar_info_s _mc_scalars[] = {
#define mc_keyword(_name)
#define mc_punctuator(_name, _text)
#define mc_scalar(_name, _kind, _bits, _signed) { #_name, _kind, _bits, _signed, },
#include "mc/tokens.def"
#undef mc_keyword
#undef mc_punctuator
#undef mc_scalar
};

const char* mc_scalar_name(const mc_scalar_e scalar) {
	mc_assert(scalar >= 0 && scalar < mc_scalar_count_);
	return _mc_scalars[scalar].name;
}

mc_scalar_e mc_scalar_from_name(const mc_view_s name) {
	for (size_t i = 0; i < mc_count_of(_mc_scalars); ++i) {
		if (mc_view_eq_cstr(name, _mc_scalars[i].name)) return (mc_scalar_e)i;
	}
	return mc_scalar_none_;
}

char mc_scalar_kind(const mc_scalar_e scalar) {
	return _mc_scalars[scalar].kind;
}

uint32_t mc_scalar_bits(const mc_scalar_e scalar) {
	const uint32_t bits = _mc_scalars[scalar].bits;
	return (0 == bits) ? mc_word_size * 8 : bits;
}

bool mc_scalar_signed(const mc_scalar_e scalar) {
	return _mc_scalars[scalar].is_signed;
}

const char* mc_token_kind_text(const mc_token_kind_e kind) {
	switch (kind) {
		case mc_token_eof:        return "end of file";
		case mc_token_identifier: return "a name";
		case mc_token_type:       return "a type";
		case mc_token_integer:    return "an integer";
		case mc_token_float:      return "a float";
		case mc_token_char:       return "a character";
		case mc_token_string:     return "a string";
		default: break;
	}
	for (size_t i = 0; i < mc_count_of(_mc_keywords); ++i) if (_mc_keywords[i].kind == kind) return _mc_keywords[i].text;
	for (size_t i = 0; i < mc_count_of(_mc_punctuators); ++i) if (_mc_punctuators[i].kind == kind) return _mc_punctuators[i].text;
	return "?";
}

// --- the lexer ---------------------------------------------------------------

typedef struct {
	const mc_source_s* source  ;
	const char*        text    ;
	uint32_t           length  ;
	uint32_t           position;
	mc_arena_s*        arena   ;
	mc_diag_s*         diag    ;
	mc_tokens_s*       tokens  ;
} _mc_lexer_s;

static char _mc_peek(const _mc_lexer_s* const l, const uint32_t offset) {
	const uint32_t index = l->position + offset;
	return (index < l->length) ? l->text[index] : '\0';
}

static bool _mc_at_end(const _mc_lexer_s* const l, const uint32_t offset) {
	return l->position + offset >= l->length;
}

static mc_span_s _mc_span(const _mc_lexer_s* const l, const uint32_t begin, const uint32_t end) {
	return (const mc_span_s) { .file = l->source->id, .begin = begin, .end = end, };
}

static bool _mc_is_digit(const char c) {
	return c >= '0' && c <= '9';
}

/// letters, '_', and every byte of a multi-byte utf-8 character.
static bool _mc_is_identifier_start(const char c) {
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || '_' == c || ((uint8_t)c >= 0x80);
}

static bool _mc_is_identifier_part(const char c) {
	return _mc_is_identifier_start(c) || _mc_is_digit(c);
}

/// the bytes of the utf-8 character starting at 'position'.
static uint32_t _mc_character_length(const _mc_lexer_s* const l, const uint32_t position) {
	const uint8_t first = (uint8_t)l->text[position];
	uint32_t length = (first < 0x80) ? 1 : (first >> 5) == 0x6 ? 2 : (first >> 4) == 0xE ? 3 : (first >> 3) == 0x1E ? 4 : 1;
	if (position + length > l->length) length = l->length - position;
	return length;
}

static void _mc_push(_mc_lexer_s* const l, mc_token_s token) {
	token.text = mc_view_slice(l->source->text, token.span.begin, token.span.end);
	mc_vector_push(l->tokens, token);
}

static void _mc_block_comment(_mc_lexer_s* const l) {
	mc_vector_of(uint32_t) starts = {0};
	mc_vector_push(&starts, l->position);
	l->position += 2;
	while (starts.count > 0) {
		if (_mc_at_end(l, 0)) {
			const uint32_t start = mc_vector_last(&starts);
			mc_help(mc_label_main(mc_error(l->diag, mc_E0103, _mc_span(l, start, start + 2), "unterminated block comment"),
				"this comment is never closed"), "block comments nest: every `/*` needs its own `*/`");
			break;
		}
		if ('/' == _mc_peek(l, 0) && '*' == _mc_peek(l, 1)) {
			mc_vector_push(&starts, l->position);
			l->position += 2;
		}
		else if ('*' == _mc_peek(l, 0) && '/' == _mc_peek(l, 1)) {
			(void)mc_vector_pop(&starts);
			l->position += 2;
		}
		else {
			l->position += 1;
		}
	}
	mc_vector_free(&starts);
}

static void _mc_skip_trivia(_mc_lexer_s* const l) {
	while (!_mc_at_end(l, 0)) {
		const char c = _mc_peek(l, 0);
		if (' ' == c || '\t' == c || '\r' == c || '\n' == c) {
			l->position += 1;
		}
		else if ('#' == c || ('/' == c && '/' == _mc_peek(l, 1))) {
			while (!_mc_at_end(l, 0) && '\n' != _mc_peek(l, 0)) l->position += 1;
		}
		else if ('/' == c && '*' == _mc_peek(l, 1)) {
			_mc_block_comment(l);
		}
		else {
			return;
		}
	}
}

/// digits of 'valid' (lower case) and '_' separators.
static void _mc_digits(_mc_lexer_s* const l, const char* const valid) {
	while (!_mc_at_end(l, 0)) {
		const char c = _mc_peek(l, 0);
		const char lower = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
		if ('_' != c && NULL == strchr(valid, lower)) break;
		if ('\0' == lower) break;
		l->position += 1;
	}
}

static void _mc_number(_mc_lexer_s* const l) {
	const uint32_t start = l->position;
	uint32_t base = 10;
	bool is_float = false;
	const char second = _mc_peek(l, 1);
	uint32_t body_start = start;
	if ('0' == _mc_peek(l, 0) && ('x' == second || 'X' == second || 'o' == second || 'O' == second || 'b' == second || 'B' == second)) {
		base = ('x' == second || 'X' == second) ? 16 : ('o' == second || 'O' == second) ? 8 : 2;
		l->position += 2;
		body_start = l->position;
		_mc_digits(l, (16 == base) ? "0123456789abcdef" : (8 == base) ? "01234567" : "01");
	}
	else {
		_mc_digits(l, "0123456789");
		if ('.' == _mc_peek(l, 0) && _mc_is_digit(_mc_peek(l, 1))) {
			l->position += 1;
			_mc_digits(l, "0123456789");
			is_float = true;
		}
		const char e = _mc_peek(l, 0);
		if (('e' == e || 'E' == e) && (_mc_is_digit(_mc_peek(l, 1))
				|| (('+' == _mc_peek(l, 1) || '-' == _mc_peek(l, 1)) && _mc_is_digit(_mc_peek(l, 2))))) {
			l->position += ('+' == _mc_peek(l, 1) || '-' == _mc_peek(l, 1)) ? 2 : 1;
			_mc_digits(l, "0123456789");
			is_float = true;
		}
	}
	const uint32_t body_end = l->position;
	while (!_mc_at_end(l, 0) && _mc_is_identifier_part(_mc_peek(l, 0))) l->position += 1;
	const mc_view_s body = mc_view_new(l->text + body_start, body_end - body_start);
	const mc_view_s suffix_text = mc_view_new(l->text + body_end, l->position - body_end);
	const mc_span_s span = _mc_span(l, start, l->position);

	// the body without separators.
	char cleaned[512];
	size_t cleaned_length = 0;
	for (size_t i = 0; i < body.length && cleaned_length + 1 < sizeof(cleaned); ++i) {
		if ('_' != body.data[i]) cleaned[cleaned_length++] = body.data[i];
	}
	cleaned[cleaned_length] = '\0';

	const mc_scalar_e suffix = (suffix_text.length > 0) ? mc_scalar_from_name(suffix_text) : mc_scalar_none_;
	char* problem = NULL;
	bool bad_separator = body.length > 0 && ('_' == body.data[0] || '_' == body.data[body.length - 1]);
	for (size_t i = 0; i + 1 < body.length; ++i) {
		const char a = body.data[i], b = body.data[i + 1];
		if (('_' == a && ('.' == b || '_' == b)) || ('.' == a && '_' == b)) bad_separator = true;
	}
	if (0 == cleaned_length) {
		problem = mc_strdup("a number needs at least one digit");
	}
	else if (bad_separator) {
		problem = mc_strdup("`_` may only separate digits");
	}
	else if (suffix_text.length > 0 && (mc_scalar_none_ == suffix || 'b' == mc_scalar_kind(suffix))) {
		problem = mc_format("unknown number suffix `" mc_view_fmt "`", mc_view_arg(suffix_text));
	}
	else if (suffix != mc_scalar_none_ && 'f' == mc_scalar_kind(suffix) && base != 10) {
		problem = mc_strdup("only decimal numbers can have a float suffix");
	}
	else if (is_float && suffix != mc_scalar_none_ && 'i' == mc_scalar_kind(suffix)) {
		problem = mc_format("a float cannot have the integer suffix `%s`", mc_scalar_name(suffix));
	}
	mc_token_s token = { .span = span, .suffix = mc_scalar_none_, };
	if (problem != NULL) {
		mc_label_main(mc_error(l->diag, mc_E0105, span, "invalid number literal"), "%s", problem);
		mc_free(problem);
		token.kind = mc_token_integer;
	}
	else if (is_float || (suffix != mc_scalar_none_ && 'f' == mc_scalar_kind(suffix))) {
		token.kind = mc_token_float;
		token.real = strtod(cleaned, NULL);
		token.suffix = suffix;
	}
	else {
		token.kind = mc_token_integer;
		unsigned __int128 value = 0;
		for (size_t i = 0; i < cleaned_length; ++i) {
			const char c = cleaned[i];
			const uint32_t digit = (c >= '0' && c <= '9') ? (uint32_t)(c - '0') : (uint32_t)((c | 0x20) - 'a' + 10);
			value = value * base + digit;
		}
		token.integer = value;
		token.suffix = suffix;
	}
	_mc_push(l, token);
}

/// one escape after its backslash; false when invalid (reported).
static bool _mc_escape(_mc_lexer_s* const l, uint8_t* const value) {
	const uint32_t start = l->position - 1;
	const char c = _mc_peek(l, 0);
	const char* const simple = "nt0r\\\"'";
	const uint8_t values[] = { 10, 9, 0, 13, 92, 34, 39, };
	if (!_mc_at_end(l, 0) && strchr(simple, c) != NULL) {
		*value = values[strchr(simple, c) - simple];
		l->position += 1;
		return true;
	}
	if ('x' == c) {
		const char h1 = _mc_peek(l, 1), h2 = _mc_peek(l, 2);
		const char* const hex = "0123456789abcdefABCDEF";
		if (h1 != '\0' && h2 != '\0' && strchr(hex, h1) != NULL && strchr(hex, h2) != NULL) {
			const char digits[3] = { h1, h2, '\0', };
			*value = (uint8_t)strtoul(digits, NULL, 16);
			l->position += 3;
			return true;
		}
	}
	if (!_mc_at_end(l, 0) && '\n' != c) l->position += _mc_character_length(l, l->position);
	mc_help(mc_label_main(mc_error(l->diag, mc_E0104, _mc_span(l, start, l->position), "invalid escape sequence"),
		"unknown escape"), "valid escapes: \\n \\t \\r \\0 \\\\ \\\" \\' \\xHH");
	return false;
}

static void _mc_string(_mc_lexer_s* const l) {
	const uint32_t start = l->position;
	l->position += 1;
	mc_string_s data = {0};
	for (;;) {
		if (_mc_at_end(l, 0) || '\n' == _mc_peek(l, 0)) {
			mc_label_main(mc_error(l->diag, mc_E0102, _mc_span(l, start, start + 1), "unterminated string literal"),
				"this string is never closed");
			break;
		}
		const char c = _mc_peek(l, 0);
		l->position += 1;
		if ('"' == c) break;
		if ('\\' == c) {
			uint8_t value = 0;
			if (_mc_escape(l, &value)) mc_string_append_char(&data, (char)value);
		}
		else {
			mc_string_append_char(&data, c);
		}
	}
	const uint32_t suffix_start = l->position;
	while (!_mc_at_end(l, 0) && _mc_is_identifier_part(_mc_peek(l, 0))) l->position += 1;
	const mc_view_s suffix = mc_view_new(l->text + suffix_start, l->position - suffix_start);
	bool c_string = false;
	if (suffix.length > 0) {
		if (mc_view_eq_cstr(suffix, "c")) {
			c_string = true;
		}
		else {
			mc_help(mc_label_main(mc_error(l->diag, mc_E0107, _mc_span(l, suffix_start, l->position),
				"unknown string suffix `" mc_view_fmt "`", mc_view_arg(suffix)), "only `c` is allowed"),
				"`\"text\"c` is a NUL-terminated string");
		}
	}
	if (c_string) {
		if (memchr(mc_string_cstr(&data), 0, data.length) != NULL) {
			mc_label_main(mc_error(l->diag, mc_E0108, _mc_span(l, start, l->position), "C string contains a NUL byte"),
				"C would stop reading at the first NUL");
		}
		mc_string_append_char(&data, '\0');
	}
	mc_token_s token = { .kind = mc_token_string, .span = _mc_span(l, start, l->position), .suffix = mc_scalar_none_, .c = c_string, };
	token.bytes = mc_view_new(mc_arena_strndup(l->arena, mc_string_cstr(&data), data.length), data.length);
	mc_string_free(&data);
	_mc_push(l, token);
}

static void _mc_character(_mc_lexer_s* const l) {
	const uint32_t start = l->position;
	l->position += 1;
	bool has_value = false;
	uint8_t value = 0;
	const char c = _mc_peek(l, 0);
	if ('\\' == c) {
		l->position += 1;
		has_value = _mc_escape(l, &value);
	}
	else if (!_mc_at_end(l, 0) && '\'' != c && '\n' != c) {
		const uint32_t length = _mc_character_length(l, l->position);
		l->position += length;
		has_value = true;
		if (1 == length) {
			value = (uint8_t)c;
		}
		else {
			mc_label_main(mc_error(l->diag, mc_E0106, _mc_span(l, start, l->position), "character literal is not a single byte"),
				"character literals are u8 values");
			value = 0;
		}
	}
	if ('\'' == _mc_peek(l, 0) && !_mc_at_end(l, 0) && has_value) {
		l->position += 1;
	}
	else {
		while (!_mc_at_end(l, 0) && '\'' != _mc_peek(l, 0) && '\n' != _mc_peek(l, 0)) l->position += 1;
		if (!_mc_at_end(l, 0) && '\'' == _mc_peek(l, 0)) l->position += 1;
		mc_label_main(mc_error(l->diag, mc_E0106, _mc_span(l, start, l->position), "invalid character literal"),
			"expected exactly one character");
		value = 0;
	}
	const mc_token_s token = { .kind = mc_token_char, .span = _mc_span(l, start, l->position), .integer = value, .suffix = mc_scalar_none_, };
	_mc_push(l, token);
}

/// a character as python's repr() shows it, for messages.
static void _mc_repr_character(mc_string_s* const out, const mc_view_s character) {
	if (1 == character.length) {
		const uint8_t c = (uint8_t)character.data[0];
		if ('\\' == c) { mc_string_append_cstr(out, "'\\\\'"); return; }
		if ('\'' == c) { mc_string_append_cstr(out, "\"'\""); return; }
		if (c < 0x20 || 0x7F == c) {
			if ('\t' == c) mc_string_append_cstr(out, "'\\t'");
			else if ('\n' == c) mc_string_append_cstr(out, "'\\n'");
			else if ('\r' == c) mc_string_append_cstr(out, "'\\r'");
			else mc_string_appendf(out, "'\\x%02x'", c);
			return;
		}
	}
	mc_string_appendf(out, "'" mc_view_fmt "'", mc_view_arg(character));
}

static void _mc_token(_mc_lexer_s* const l) {
	const uint32_t start = l->position;
	const char c = _mc_peek(l, 0);
	if (_mc_is_digit(c)) {
		_mc_number(l);
		return;
	}
	if (_mc_is_identifier_start(c)) {
		while (!_mc_at_end(l, 0) && _mc_is_identifier_part(_mc_peek(l, 0))) l->position += 1;
		const mc_view_s text = mc_view_new(l->text + start, l->position - start);
		mc_token_s token = { .kind = mc_token_identifier, .span = _mc_span(l, start, l->position), .suffix = mc_scalar_none_, };
		for (size_t i = 0; i < mc_count_of(_mc_keywords); ++i) {
			if (mc_view_eq_cstr(text, _mc_keywords[i].text)) token.kind = _mc_keywords[i].kind;
		}
		const mc_scalar_e scalar = mc_scalar_from_name(text);
		if (scalar != mc_scalar_none_) {
			token.kind = mc_token_type;
			token.suffix = scalar;
		}
		_mc_push(l, token);
		return;
	}
	if ('"' == c) {
		_mc_string(l);
		return;
	}
	if ('\'' == c) {
		_mc_character(l);
		return;
	}
	for (size_t i = 0; i < mc_count_of(_mc_punctuators); ++i) {
		const size_t length = strlen(_mc_punctuators[i].text);
		if (l->position + length <= l->length && 0 == memcmp(l->text + l->position, _mc_punctuators[i].text, length)) {
			l->position += (uint32_t)length;
			const mc_token_s token = { .kind = _mc_punctuators[i].kind, .span = _mc_span(l, start, l->position), .suffix = mc_scalar_none_, };
			_mc_push(l, token);
			return;
		}
	}
	const uint32_t length = _mc_character_length(l, start);
	l->position += length;
	mc_string_s shown = {0};
	_mc_repr_character(&shown, mc_view_new(l->text + start, length));
	mc_label_main(mc_error(l->diag, mc_E0101, _mc_span(l, start, l->position), "unknown character %s", mc_string_cstr(&shown)),
		"not part of the language");
	mc_string_free(&shown);
}

void mc_lex(const mc_source_s* const source, mc_arena_s* const arena, mc_diag_s* const diag, mc_tokens_s* const tokens) {
	_mc_lexer_s lexer = {
		.source = source, .text = source->text.data, .length = (uint32_t)source->text.length,
		.arena = arena, .diag = diag, .tokens = tokens,
	};
	for (;;) {
		_mc_skip_trivia(&lexer);
		if (_mc_at_end(&lexer, 0)) {
			const mc_token_s eof = { .kind = mc_token_eof, .span = _mc_span(&lexer, lexer.length, lexer.length), .suffix = mc_scalar_none_, };
			_mc_push(&lexer, eof);
			return;
		}
		_mc_token(&lexer);
	}
}
