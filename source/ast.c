
#include "mia/ast.h"

#include <string.h>

static const char* const _mia_ast_node_names[] = {
#define mia_ast_node(_kind, _name) _name,
#include "mia/ast.def"
#undef mia_ast_node
};

static const char* const _mia_ast_operator_texts[] = {
	"", "+", "-", "*", "/", "%", "&", "|", "^", "<<", ">>", "==", "!=", "<", ">", "<=", ">=", "&&", "||",
	"-", "!", "~", "=",
};

const char* mia_ast_operator_text(const mia_ast_operator_e operation) {
	mia_assert(operation < mia_ast_operator_count);
	return _mia_ast_operator_texts[operation];
}

mia_ast_node_s* mia_ast_node_new(mia_arena_s* const arena, const mia_ast_node_kind_e kind, const mia_span_s span) {
	mia_ast_node_s* const node = mia_arena_new(arena, mia_ast_node_s);
	node->kind = kind;
	node->span = span;
	return node;
}

const char* mia_ast_node_kind_name(const mia_ast_node_kind_e kind) {
	mia_assert(kind < mia_ast_node_count);
	return _mia_ast_node_names[kind];
}

static mia_ast_node_s* _mia_ast_clone(mia_arena_s* const arena, const mia_ast_node_s* const node);

static mia_ast_nodes_s _mia_ast_clone_all(mia_arena_s* const arena, const mia_ast_nodes_s* const nodes) {
	mia_ast_nodes_s copy = {0};
	for (size_t i = 0; i < nodes->count; ++i) mia_arena_push(arena, &copy, _mia_ast_clone(arena, nodes->data[i]));
	return copy;
}

static mia_view_list_s _mia_ast_clone_views(mia_arena_s* const arena, const mia_view_list_s* const views) {
	mia_view_list_s copy = {0};
	for (size_t i = 0; i < views->count; ++i) mia_arena_push(arena, &copy, views->data[i]);
	return copy;
}

static mia_ast_node_s* _mia_ast_clone(mia_arena_s* const arena, const mia_ast_node_s* const node) {
	if (NULL == node) return NULL;
	mia_ast_node_s* const copy = mia_ast_node_new(arena, node->kind, node->span);
	copy->as = node->as;
#define _mia_ast_child(_field) copy->as._field = _mia_ast_clone(arena, node->as._field)
#define _mia_ast_children(_field) copy->as._field = _mia_ast_clone_all(arena, &node->as._field)
	switch (node->kind) {
		case mia_ast_node_type_name:      copy->as.type_name.path = _mia_ast_clone_views(arena, &node->as.type_name.path); _mia_ast_children(type_name.arguments); break;
		case mia_ast_node_type_slice:     _mia_ast_child(type_slice.element); break;
		case mia_ast_node_type_array:     _mia_ast_child(type_array.length); _mia_ast_child(type_array.element); break;
		case mia_ast_node_type_reference: _mia_ast_child(type_reference.element); break;
		case mia_ast_node_type_function:  _mia_ast_children(type_function.parameters); _mia_ast_child(type_function.result); break;
		case mia_ast_node_member:         _mia_ast_child(member.target); break;
		case mia_ast_node_call:           _mia_ast_child(call.callee); _mia_ast_children(call.type_arguments); _mia_ast_children(call.arguments); break;
		case mia_ast_node_specialize:     _mia_ast_child(specialize.target); _mia_ast_children(specialize.type_arguments); break;
		case mia_ast_node_index:          _mia_ast_child(index.target); _mia_ast_child(index.index); break;
		case mia_ast_node_slice_of:       _mia_ast_child(slice_of.target); _mia_ast_child(slice_of.low); _mia_ast_child(slice_of.high); break;
		case mia_ast_node_unary:          _mia_ast_child(unary.operand); break;
		case mia_ast_node_binary:         _mia_ast_child(binary.left); _mia_ast_child(binary.right); break;
		case mia_ast_node_paren:          _mia_ast_child(paren.inner); break;
		case mia_ast_node_cast:           _mia_ast_child(cast.type); _mia_ast_child(cast.operand); break;
		case mia_ast_node_reference:      _mia_ast_child(reference.operand); break;
		case mia_ast_node_entry:          _mia_ast_child(entry.value); break;
		case mia_ast_node_composite:      _mia_ast_child(composite.type); _mia_ast_children(composite.entries); break;
		case mia_ast_node_lambda:         _mia_ast_children(lambda.parameters); _mia_ast_child(lambda.result); _mia_ast_child(lambda.body); break;
		case mia_ast_node_sizeof:         _mia_ast_child(size_of.type); break;
		case mia_ast_node_lenof:          _mia_ast_child(lenof.operand); break;
		case mia_ast_node_assert:         _mia_ast_child(assert_statement.condition); _mia_ast_child(assert_statement.message); break;
		case mia_ast_node_let:            _mia_ast_child(let.type); _mia_ast_child(let.value); _mia_ast_children(let.attributes); break;
		case mia_ast_node_declaration_statement: _mia_ast_child(declaration_statement.declaration); break;
		case mia_ast_node_if:             _mia_ast_child(if_statement.condition); _mia_ast_child(if_statement.then); _mia_ast_child(if_statement.otherwise); _mia_ast_children(if_statement.attributes); break;
		case mia_ast_node_while:          _mia_ast_child(while_statement.condition); _mia_ast_child(while_statement.body); _mia_ast_children(while_statement.attributes); break;
		case mia_ast_node_for:            _mia_ast_child(for_statement.start); _mia_ast_child(for_statement.end); _mia_ast_child(for_statement.body); _mia_ast_children(for_statement.attributes); break;
		case mia_ast_node_return:         _mia_ast_child(return_statement.value); _mia_ast_children(return_statement.attributes); break;
		case mia_ast_node_break:
		case mia_ast_node_continue:       _mia_ast_children(jump.attributes); break;
		case mia_ast_node_block:          _mia_ast_children(block.statements); _mia_ast_children(block.attributes); break;
		case mia_ast_node_expression_statement: _mia_ast_child(expression_statement.expression); _mia_ast_children(expression_statement.attributes); break;
		case mia_ast_node_assign:         _mia_ast_child(assign.target); _mia_ast_child(assign.value); _mia_ast_children(assign.attributes); break;
		case mia_ast_node_param:          _mia_ast_child(param.type); _mia_ast_child(param.default_value); break;
		case mia_ast_node_field:          _mia_ast_child(field.type); _mia_ast_child(field.default_value); _mia_ast_children(field.attributes); break;
		case mia_ast_node_function:
			_mia_ast_children(function.generics); _mia_ast_children(function.parameters); _mia_ast_child(function.result);
			_mia_ast_child(function.body); _mia_ast_children(function.attributes);
			break;
		case mia_ast_node_struct:         _mia_ast_children(structure.generics); _mia_ast_children(structure.fields); _mia_ast_children(structure.attributes); break;
		case mia_ast_node_constant:       _mia_ast_child(constant.value); _mia_ast_children(constant.attributes); break;
		case mia_ast_node_import:         copy->as.import.path = _mia_ast_clone_views(arena, &node->as.import.path); break;
		case mia_ast_node_from_import:    copy->as.from_import.path = _mia_ast_clone_views(arena, &node->as.from_import.path); _mia_ast_children(from_import.names); break;
		case mia_ast_node_module:         _mia_ast_children(module.imports); _mia_ast_children(module.declarations); break;
		default: break;
	}
#undef _mia_ast_child
#undef _mia_ast_children
	return copy;
}

typedef struct {
	const mia_ast_node_s* node ;
	size_t                depth;
} _mia_ast_visit_s;

typedef mia_vector_of(_mia_ast_visit_s) _mia_ast_visits_s;

static void _mia_ast_push(_mia_ast_visits_s* const visits, const mia_ast_node_s* const node, const size_t depth) {
	if (node != NULL) mia_vector_push(visits, ((_mia_ast_visit_s) { .node = node, .depth = depth, }));
}

static void _mia_ast_push_all(_mia_ast_visits_s* const visits, const mia_ast_nodes_s* const nodes, const size_t depth) {
	for (size_t i = 0; i < nodes->count; ++i) _mia_ast_push(visits, nodes->data[i], depth);
}

/// pushes the children of 'node', one level deeper.
static void _mia_ast_push_children(_mia_ast_visits_s* const visits, const mia_ast_node_s* const node, const size_t depth) {
#define _mia_ast_child(_field) _mia_ast_push(visits, node->as._field, depth)
#define _mia_ast_children(_field) _mia_ast_push_all(visits, &node->as._field, depth)
	switch (node->kind) {
		case mia_ast_node_type_name:      _mia_ast_children(type_name.arguments); break;
		case mia_ast_node_type_slice:     _mia_ast_child(type_slice.element); break;
		case mia_ast_node_type_array:     _mia_ast_child(type_array.length); _mia_ast_child(type_array.element); break;
		case mia_ast_node_type_reference: _mia_ast_child(type_reference.element); break;
		case mia_ast_node_type_function:  _mia_ast_children(type_function.parameters); _mia_ast_child(type_function.result); break;
		case mia_ast_node_member:         _mia_ast_child(member.target); break;
		case mia_ast_node_call:           _mia_ast_child(call.callee); _mia_ast_children(call.type_arguments); _mia_ast_children(call.arguments); break;
		case mia_ast_node_specialize:     _mia_ast_child(specialize.target); _mia_ast_children(specialize.type_arguments); break;
		case mia_ast_node_index:          _mia_ast_child(index.target); _mia_ast_child(index.index); break;
		case mia_ast_node_slice_of:       _mia_ast_child(slice_of.target); _mia_ast_child(slice_of.low); _mia_ast_child(slice_of.high); break;
		case mia_ast_node_unary:          _mia_ast_child(unary.operand); break;
		case mia_ast_node_binary:         _mia_ast_child(binary.left); _mia_ast_child(binary.right); break;
		case mia_ast_node_paren:          _mia_ast_child(paren.inner); break;
		case mia_ast_node_cast:           _mia_ast_child(cast.type); _mia_ast_child(cast.operand); break;
		case mia_ast_node_reference:      _mia_ast_child(reference.operand); break;
		case mia_ast_node_entry:          _mia_ast_child(entry.value); break;
		case mia_ast_node_composite:      _mia_ast_child(composite.type); _mia_ast_children(composite.entries); break;
		case mia_ast_node_lambda:         _mia_ast_children(lambda.parameters); _mia_ast_child(lambda.result); _mia_ast_child(lambda.body); break;
		case mia_ast_node_sizeof:         _mia_ast_child(size_of.type); break;
		case mia_ast_node_lenof:          _mia_ast_child(lenof.operand); break;
		case mia_ast_node_assert:         _mia_ast_child(assert_statement.condition); _mia_ast_child(assert_statement.message); break;
		case mia_ast_node_let:            _mia_ast_child(let.type); _mia_ast_child(let.value); break;
		case mia_ast_node_declaration_statement: _mia_ast_child(declaration_statement.declaration); break;
		case mia_ast_node_if:             _mia_ast_child(if_statement.condition); _mia_ast_child(if_statement.then); _mia_ast_child(if_statement.otherwise); break;
		case mia_ast_node_while:          _mia_ast_child(while_statement.condition); _mia_ast_child(while_statement.body); break;
		case mia_ast_node_for:            _mia_ast_child(for_statement.start); _mia_ast_child(for_statement.end); _mia_ast_child(for_statement.body); break;
		case mia_ast_node_return:         _mia_ast_child(return_statement.value); break;
		case mia_ast_node_block:          _mia_ast_children(block.statements); break;
		case mia_ast_node_expression_statement: _mia_ast_child(expression_statement.expression); break;
		case mia_ast_node_assign:         _mia_ast_child(assign.target); _mia_ast_child(assign.value); break;
		case mia_ast_node_param:          _mia_ast_child(param.type); _mia_ast_child(param.default_value); break;
		case mia_ast_node_field:          _mia_ast_child(field.type); _mia_ast_child(field.default_value); break;
		case mia_ast_node_function:       _mia_ast_children(function.parameters); _mia_ast_child(function.result); _mia_ast_child(function.body); break;
		case mia_ast_node_struct:         _mia_ast_children(structure.fields); break;
		case mia_ast_node_constant:       _mia_ast_child(constant.value); break;
		case mia_ast_node_module:         _mia_ast_children(module.declarations); break;
		default: break;
	}
#undef _mia_ast_child
#undef _mia_ast_children
}

const mia_ast_node_s* mia_ast_node_too_deep(const mia_ast_node_s* const root, const size_t limit) {
	_mia_ast_visits_s visits = {0};
	_mia_ast_push(&visits, root, 0);
	const mia_ast_node_s* deep = NULL;
	while (visits.count > 0 && NULL == deep) {
		const _mia_ast_visit_s visit = visits.data[--visits.count];
		if (visit.depth > limit) deep = visit.node;
		else _mia_ast_push_children(&visits, visit.node, visit.depth + 1);
	}
	mia_vector_free(&visits);
	return deep;
}

mia_ast_node_s* mia_ast_node_clone(mia_arena_s* const arena, const mia_ast_node_s* const node) {
	return _mia_ast_clone(arena, node);
}

mia_view_s mia_ast_declaration_name(const mia_ast_node_s* const node) {
	switch (node->kind) {
		case mia_ast_node_function: return node->as.function.name;
		case mia_ast_node_struct:   return node->as.structure.name;
		case mia_ast_node_constant: return node->as.constant.name;
		default: mia_unreachable();
	}
}

mia_span_s mia_ast_declaration_name_span(const mia_ast_node_s* const node) {
	switch (node->kind) {
		case mia_ast_node_function: return node->as.function.name_span;
		case mia_ast_node_struct:   return node->as.structure.name_span;
		case mia_ast_node_constant: return node->as.constant.name_span;
		default: mia_unreachable();
	}
}

const mia_ast_node_s* mia_ast_attribute_find(const mia_ast_nodes_s* const attributes, const char* const name) {
	for (size_t i = 0; i < attributes->count; ++i) {
		if (mia_view_eq_cstr(attributes->data[i]->as.attribute.name, name)) return attributes->data[i];
	}
	return NULL;
}

const mia_ast_nodes_s* mia_ast_statement_attributes(const mia_ast_node_s* const node) {
	static const mia_ast_nodes_s none = {0};
	switch (node->kind) {
		case mia_ast_node_let:                  return &node->as.let.attributes;
		case mia_ast_node_expression_statement: return &node->as.expression_statement.attributes;
		case mia_ast_node_assign:               return &node->as.assign.attributes;
		case mia_ast_node_if:                   return &node->as.if_statement.attributes;
		case mia_ast_node_while:                return &node->as.while_statement.attributes;
		case mia_ast_node_for:                  return &node->as.for_statement.attributes;
		case mia_ast_node_return:               return &node->as.return_statement.attributes;
		case mia_ast_node_break:
		case mia_ast_node_continue:             return &node->as.jump.attributes;
		case mia_ast_node_block:                return &node->as.block.attributes;
		default:                           return &none;
	}
}
