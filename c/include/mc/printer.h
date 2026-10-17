
#ifndef __mc__printer_h__
#define __mc__printer_h__

/// prints syntax trees as indented text: one node per line, scalar fields
/// inline, child nodes indented below with their field name. with hooks, the
/// types of expressions and the chosen generic instances are shown too.

#include "mc/ast.h"

typedef struct {
	void (*type)(mc_string_s* const out, const struct mc_type_s* const type);
	void (*instance)(mc_string_s* const out, const struct mc_instance_s* const instance);
} mc_print_hooks_s;

/// appends the tree of 'node'; 'hooks' may be null (syntax only).
void mc_print_tree(const mc_node_s* const node, const mc_print_hooks_s* const hooks, mc_string_s* const out);

#endif
