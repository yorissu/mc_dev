
#ifndef __mc__lexer_h__
#define __mc__lexer_h__

/// the lexer: source text -> tokens.
///
/// comments: '#' and '//' to the end of the line, '/* */' blocks that nest.
/// numbers: decimal, '0x' hexadecimal, '0o' octal, '0b' binary, decimal
/// floats with an optional exponent, '_' separators between digits, and a
/// type suffix ('255u8', '1.5f32'); hexadecimal numbers take integer suffixes
/// only, because 'f' is a hexadecimal digit. characters: 'a' (a u8).
/// strings: "text", or "text"c with a terminating NUL.

#include "mc/diag.h"
#include "mc/source.h"

typedef enum {
	mc_token_eof       ,
	mc_token_identifier,
	mc_token_type      ,   // a built-in type name: 'scalar' says which
	mc_token_integer   ,
	mc_token_float     ,
	mc_token_char      ,
	mc_token_string    ,
#define mc_keyword(_name) mc_token_##_name,
#define mc_punctuator(_name, _text) mc_token_##_name,
#define mc_scalar(_name, _kind, _bits, _signed)
#include "mc/tokens.def"
#undef mc_keyword
#undef mc_punctuator
#undef mc_scalar
	mc_token_count_    ,
} mc_token_kind_e;

/// the built-in scalar types, in the order of tokens.def.
typedef enum {
#define mc_scalar(_name, _kind, _bits, _signed) mc_scalar_##_name,
#define mc_keyword(_name)
#define mc_punctuator(_name, _text)
#include "mc/tokens.def"
#undef mc_scalar
#undef mc_keyword
#undef mc_punctuator
	mc_scalar_count_,
	mc_scalar_none_ = -1,
} mc_scalar_e;

typedef struct {
	mc_token_kind_e   kind   ;
	mc_span_s         span   ;
	mc_view_s         text   ;
	unsigned __int128 integer;   // integers and characters
	double            real   ;   // floats
	mc_view_s         bytes  ;   // strings: the bytes after escapes (arena owned)
	mc_scalar_e       suffix ;   // numbers: the type suffix, or none
	bool              c      ;   // strings: written "..."c
} mc_token_s;

typedef mc_vector_of(mc_token_s) mc_tokens_s;

/// the tokens of 'source' (ending with an eof token); errors go to 'diag'.
void mc_lex(const mc_source_s* const source, mc_arena_s* const arena, mc_diag_s* const diag, mc_tokens_s* const tokens);

/// the text of a keyword or punctuator kind ("::", "let"), or a description.
const char* mc_token_kind_text(const mc_token_kind_e kind);

const char* mc_scalar_name(const mc_scalar_e scalar);

mc_scalar_e mc_scalar_from_name(const mc_view_s name);

/// the scalar's kind: 'i' integer, 'f' float, 'b' bool.
char mc_scalar_kind(const mc_scalar_e scalar);

/// bits of the scalar on the target (int, uint and float are a word).
uint32_t mc_scalar_bits(const mc_scalar_e scalar);

bool mc_scalar_signed(const mc_scalar_e scalar);

/// bytes in a machine word.
#define mc_word_size 8

#endif
