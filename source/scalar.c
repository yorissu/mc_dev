
#include "mia/scalar.h"

typedef struct {
	const char* name     ;
	char        kind     ;
	uint32_t    bits     ;
	bool        is_signed;
} _mia_scalar_info_s;

static const _mia_scalar_info_s _mia_scalar_scalars[] = {
#define mia_token_keyword(_name)
#define mia_token_punctuator(_name, _text)
#define mia_token_scalar(_name, _kind, _bits, _signed) { #_name, _kind, _bits, _signed, },
#include "mia/tokens.def"
#undef mia_token_keyword
#undef mia_token_punctuator
#undef mia_token_scalar
};

const char* mia_scalar_name(const mia_scalar_e scalar) {
	mia_assert(scalar >= 0 && scalar < mia_scalar_count);
	return _mia_scalar_scalars[scalar].name;
}

mia_scalar_e mia_scalar_from_name(const mia_view_s name) {
	for (size_t i = 0; i < mia_count_of(_mia_scalar_scalars); ++i) {
		if (mia_view_eq_cstr(name, _mia_scalar_scalars[i].name)) return (mia_scalar_e)i;
	}
	return mia_scalar_none;
}

char mia_scalar_kind(const mia_scalar_e scalar) {
	return _mia_scalar_scalars[scalar].kind;
}

uint32_t mia_scalar_bits(const mia_scalar_e scalar) {
	const uint32_t bits = _mia_scalar_scalars[scalar].bits;
	return (0 == bits) ? mia_scalar_word_size * 8 : bits;
}

bool mia_scalar_signed(const mia_scalar_e scalar) {
	return _mia_scalar_scalars[scalar].is_signed;
}
