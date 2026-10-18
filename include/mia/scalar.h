
#ifndef __mia__scalar_h__
#define __mia__scalar_h__

/// the built-in scalar types (s8 ... f64, int, uint, float, bool): their
/// names, kinds and sizes, from tokens.def.

#include "mia/view.h"

/// the built-in scalar types, in the order of tokens.def.
typedef enum {
#define mia_token_scalar(_name, _kind, _bits, _signed) mia_scalar_##_name,
#define mia_token_keyword(_name)
#define mia_token_punctuator(_name, _text)
#include "mia/tokens.def"
#undef mia_token_scalar
#undef mia_token_keyword
#undef mia_token_punctuator
	mia_scalar_count,
	mia_scalar_none = -1,
} mia_scalar_e;

/// the name of a scalar type: "u8", "f64".
const char* mia_scalar_name(const mia_scalar_e scalar);

mia_scalar_e mia_scalar_from_name(const mia_view_s name);

/// the scalar's kind: 'i' integer, 'f' float, 'b' bool.
char mia_scalar_kind(const mia_scalar_e scalar);

/// bits of the scalar on the target (int, uint and float are a word).
uint32_t mia_scalar_bits(const mia_scalar_e scalar);

bool mia_scalar_signed(const mia_scalar_e scalar);

/// bytes in a machine word.
#define mia_scalar_word_size 8

#endif
