
#ifndef __mc__parser_h__
#define __mc__parser_h__

/// the parser: tokens -> syntax tree (recursive descent).
///
/// declarations are 'name :: ...': a struct when the braces start with 'let',
/// a function when the parentheses hold parameters ('()', '(name:' or
/// '(mut'), and a constant otherwise. operators follow c's order but are
/// strict: where the order is not obvious (bit operators, shifts, mixing
/// '&&' with '||', chained comparisons) parentheses are required. because
/// comparisons cannot be chained, 'name<T>(' is always a generic call.
/// after an error the parser skips to the next statement or declaration.

#include "mc/ast.h"
#include "mc/diag.h"

/// the syntax tree of 'source' (never null); nodes live in 'arena'.
mc_node_s* mc_parse(const mc_source_s* const source, mc_arena_s* const arena, mc_diag_s* const diag);

#endif
