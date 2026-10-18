
#ifndef __mia__token_h__
#define __mia__token_h__

/// tokens: their kinds (keywords and punctuators from tokens.def) and spellings.

#include "mia/int128.h"
#include "mia/scalar.h"
#include "mia/span.h"
#include "mia/vector.h"
#include "mia/view.h"

typedef enum {
	mia_token_eof       ,
	mia_token_identifier,
	mia_token_type      ,   // a built-in type name: 'scalar' says which
	mia_token_integer   ,
	mia_token_float     ,
	mia_token_char      ,
	mia_token_string    ,
#define mia_token_keyword(_name) mia_token_##_name,
#define mia_token_punctuator(_name, _text) mia_token_##_name,
#define mia_token_scalar(_name, _kind, _bits, _signed)
#include "mia/tokens.def"
#undef mia_token_keyword
#undef mia_token_punctuator
#undef mia_token_scalar
	mia_token_count    ,
} mia_token_kind_e;

typedef struct {
	mia_token_kind_e kind   ;
	mia_span_s       span   ;
	mia_view_s       text   ;
	mia_int128_s     integer;   // integers and characters
	double           real   ;   // floats
	mia_view_s       bytes  ;   // strings: the bytes after escapes (arena owned)
	mia_scalar_e     suffix ;   // numbers: the type suffix, or none
	bool             c      ;   // strings: written "..."c
} mia_token_s;

typedef mia_vector_of(mia_token_s) mia_token_list_s;

/// the text of a keyword or punctuator kind ("::", "let"), or a description.
const char* mia_token_kind_text(const mia_token_kind_e kind);

/// the keyword spelled 'text', or mia_token_identifier.
mia_token_kind_e mia_token_find_keyword(const mia_view_s text);

/// the punctuator 'text' starts with: its length, and its kind in 'kind' (0: none).
size_t mia_token_find_punctuator(const mia_view_s text, mia_token_kind_e* const kind);

#endif
