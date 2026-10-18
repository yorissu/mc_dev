
#ifndef __mia__lexer_h__
#define __mia__lexer_h__

/// the lexer: source text -> tokens.
///
/// comments: '#' and '//' to the end of the line, '/* */' blocks that nest.
/// numbers: decimal, '0x' hexadecimal, '0o' octal, '0b' binary, decimal
/// floats with an optional exponent, '_' separators between digits, and a
/// type suffix ('255u8', '1.5f32'); hexadecimal numbers take integer suffixes
/// only, because 'f' is a hexadecimal digit. characters: 'a' (a u8).
/// strings: "text", or "text"c with a terminating NUL.

#include "mia/diag.h"
#include "mia/source.h"
#include "mia/token.h"

/// the tokens of 'source' (ending with an eof token); errors go to 'diag'.
void mia_lexer_lex(const mia_source_s* const source, mia_arena_s* const arena, mia_diag_s* const diag, mia_token_list_s* const tokens);

#endif
