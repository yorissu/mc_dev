
#include "mia/code.h"

static const char* const _mia_code_type_codes[] = {
#define mia_code(_name, _text, _size, _kind, _signed) _text,
#include "mia/codes.def"
#undef mia_code
};

const char* mia_code_name(const mia_code_e code) {
	return _mia_code_type_codes[code];
}
