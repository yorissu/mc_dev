
#ifndef __mia__code_h__
#define __mia__code_h__

#include <stdbool.h>
#include <stdint.h>

/// the type codes of typed instructions (see codes.def): scalars, function
/// values, slices (two words, also used for locations) and the bytes of a
/// struct or an array.
typedef enum {
#define mia_code(_name, _text, _size, _kind, _signed) mia_code_##_name,
#include "mia/codes.def"
#undef mia_code
	mia_code_count,
} mia_code_e;

/// the size in memory of a value of 'code' (0 for BYTES).
static inline uint64_t mia_code_size(const mia_code_e code) {
	switch (code) {
#define mia_code(_name, _text, _size, _kind, _signed) case mia_code_##_name: return _size;
#include "mia/codes.def"
#undef mia_code
		default: return 0;
	}
}

/// the kind of 'code': 'i', 'f', 'b', 'n', 's' or 'x' (see codes.def).
static inline char mia_code_kind(const mia_code_e code) {
	switch (code) {
#define mia_code(_name, _text, _size, _kind, _signed) case mia_code_##_name: return _kind;
#include "mia/codes.def"
#undef mia_code
		default: return 'x';
	}
}

/// whether 'code' is a signed integer.
static inline bool mia_code_is_signed(const mia_code_e code) {
	switch (code) {
#define mia_code(_name, _text, _size, _kind, _signed) case mia_code_##_name: return _signed;
#include "mia/codes.def"
#undef mia_code
		default: return false;
	}
}

static inline bool mia_code_is_float(const mia_code_e code) {
	return 'f' == mia_code_kind(code);
}

/// the name of a code: 'U8', 'BYTES'.
const char* mia_code_name(const mia_code_e code);

#endif
