
#ifndef __mia__printer_h__
#define __mia__printer_h__

/// prints syntax trees as indented text: one node per line, scalar fields
/// inline, child nodes indented below with their field name. with hooks, the
/// types of expressions and the chosen generic instances are shown too.

#include "mia/ast.h"

typedef struct {
	void (*type)(mia_string_s* const out, const struct mia_type_s* const type);
	void (*instance)(mia_string_s* const out, const struct mia_program_instance_s* const instance);
} mia_printer_hooks_s;

/// appends the tree of 'node'; 'hooks' may be null (syntax only).
void mia_printer_print(const mia_ast_node_s* const node, const mia_printer_hooks_s* const hooks, mia_string_s* const out);

#endif
