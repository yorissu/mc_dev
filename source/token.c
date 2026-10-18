
#include "mia/token.h"

#include <string.h>

typedef struct {
	const char*      text;
	mia_token_kind_e kind;
} _mia_token_spelling_s;

static const _mia_token_spelling_s _mia_token_keywords[] = {
#define mia_token_keyword(_name) { #_name, mia_token_##_name, },
#define mia_token_punctuator(_name, _text)
#define mia_token_scalar(_name, _kind, _bits, _signed)
#include "mia/tokens.def"
#undef mia_token_keyword
#undef mia_token_punctuator
#undef mia_token_scalar
};

static const _mia_token_spelling_s _mia_token_punctuators[] = {
#define mia_token_keyword(_name)
#define mia_token_punctuator(_name, _text) { _text, mia_token_##_name, },
#define mia_token_scalar(_name, _kind, _bits, _signed)
#include "mia/tokens.def"
#undef mia_token_keyword
#undef mia_token_punctuator
#undef mia_token_scalar
};

const char* mia_token_kind_text(const mia_token_kind_e kind) {
	switch (kind) {
		case mia_token_eof:        return "end of file";
		case mia_token_identifier: return "a name";
		case mia_token_type:       return "a type";
		case mia_token_integer:    return "an integer";
		case mia_token_float:      return "a float";
		case mia_token_char:       return "a character";
		case mia_token_string:     return "a string";
		default: break;
	}
	for (size_t i = 0; i < mia_count_of(_mia_token_keywords); ++i) if (_mia_token_keywords[i].kind == kind) return _mia_token_keywords[i].text;
	for (size_t i = 0; i < mia_count_of(_mia_token_punctuators); ++i) if (_mia_token_punctuators[i].kind == kind) return _mia_token_punctuators[i].text;
	return "?";
}

mia_token_kind_e mia_token_find_keyword(const mia_view_s text) {
	for (size_t i = 0; i < mia_count_of(_mia_token_keywords); ++i) {
		if (mia_view_eq_cstr(text, _mia_token_keywords[i].text)) return _mia_token_keywords[i].kind;
	}
	return mia_token_identifier;
}

size_t mia_token_find_punctuator(const mia_view_s text, mia_token_kind_e* const kind) {
	for (size_t i = 0; i < mia_count_of(_mia_token_punctuators); ++i) {
		const size_t length = strlen(_mia_token_punctuators[i].text);
		if (length <= text.length && 0 == memcmp(text.data, _mia_token_punctuators[i].text, length)) {
			*kind = _mia_token_punctuators[i].kind;
			return length;
		}
	}
	return 0;
}
