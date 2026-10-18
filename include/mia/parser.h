
#ifndef __mia__parser_h__
#define __mia__parser_h__

/// the parser: tokens -> syntax tree (recursive descent).
///
/// declarations are 'name :: ...': a struct when the braces start with 'let',
/// a function when the parentheses hold parameters ('()' or '(name:'), and
/// a constant otherwise. operators follow c's order but are
/// strict: where the order is not obvious (bit operators, shifts, mixing
/// '&&' with '||', chained comparisons) parentheses are required. because
/// comparisons cannot be chained, 'name<T>(' is always a generic call.
/// after an error the parser skips to the next statement or declaration.

#include "mia/ast.h"
#include "mia/diag.h"

/// the syntax tree of 'source' (never null); nodes live in 'arena'.
mia_ast_node_s* mia_parser_parse(const mia_source_s* const source, mia_arena_s* const arena, mia_diag_s* const diag);

#endif
