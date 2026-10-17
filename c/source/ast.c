
#include "mc/ast.h"

#include <string.h>

static const char* const _mc_node_names[] = {
#define mc_node(_kind, _name) _name,
#include "mc/ast.def"
#undef mc_node
};

static const char* const _mc_operator_texts[] = {
	"", "+", "-", "*", "/", "%", "&", "|", "^", "<<", ">>", "==", "!=", "<", ">", "<=", ">=", "&&", "||",
	"-", "!", "~", "=",
};

const char* mc_operator_text(const mc_operator_e operator_) {
	mc_assert(operator_ < mc_operator_count_);
	return _mc_operator_texts[operator_];
}

mc_node_s* mc_node_new(mc_arena_s* const arena, const mc_node_kind_e kind, const mc_span_s span) {
	mc_node_s* const node = mc_arena_new(arena, mc_node_s);
	node->kind = kind;
	node->span = span;
	return node;
}

const char* mc_node_kind_name(const mc_node_kind_e kind) {
	mc_assert(kind < mc_node_count_);
	return _mc_node_names[kind];
}

static mc_node_s* _mc_clone(mc_arena_s* const arena, const mc_node_s* const node);

static mc_nodes_s _mc_clone_all(mc_arena_s* const arena, const mc_nodes_s* const nodes) {
	mc_nodes_s copy = {0};
	for (size_t i = 0; i < nodes->count; ++i) mc_arena_push(arena, &copy, _mc_clone(arena, nodes->data[i]));
	return copy;
}

static mc_views_s _mc_clone_views(mc_arena_s* const arena, const mc_views_s* const views) {
	mc_views_s copy = {0};
	for (size_t i = 0; i < views->count; ++i) mc_arena_push(arena, &copy, views->data[i]);
	return copy;
}

static mc_node_s* _mc_clone(mc_arena_s* const arena, const mc_node_s* const node) {
	if (NULL == node) return NULL;
	mc_node_s* const copy = mc_node_new(arena, node->kind, node->span);
	copy->as = node->as;
#define _mc_child(_field) copy->as._field = _mc_clone(arena, node->as._field)
#define _mc_children(_field) copy->as._field = _mc_clone_all(arena, &node->as._field)
	switch (node->kind) {
		case mc_node_type_name:      copy->as.type_name.path = _mc_clone_views(arena, &node->as.type_name.path); _mc_children(type_name.arguments); break;
		case mc_node_type_slice:     _mc_child(type_slice.element); break;
		case mc_node_type_array:     _mc_child(type_array.length); _mc_child(type_array.element); break;
		case mc_node_type_reference: _mc_child(type_reference.element); break;
		case mc_node_type_function:  _mc_children(type_function.parameters); _mc_child(type_function.result); break;
		case mc_node_member:         _mc_child(member.target); break;
		case mc_node_call:           _mc_child(call.callee); _mc_children(call.type_arguments); _mc_children(call.arguments); break;
		case mc_node_specialize:     _mc_child(specialize.target); _mc_children(specialize.type_arguments); break;
		case mc_node_index:          _mc_child(index.target); _mc_child(index.index); break;
		case mc_node_slice_of:       _mc_child(slice_of.target); _mc_child(slice_of.low); _mc_child(slice_of.high); break;
		case mc_node_unary:          _mc_child(unary.operand); break;
		case mc_node_binary:         _mc_child(binary.left); _mc_child(binary.right); break;
		case mc_node_paren:          _mc_child(paren.inner); break;
		case mc_node_cast:           _mc_child(cast.type); _mc_child(cast.operand); break;
		case mc_node_reference:      _mc_child(reference.operand); break;
		case mc_node_entry:          _mc_child(entry.value); break;
		case mc_node_composite:      _mc_child(composite.type); _mc_children(composite.entries); break;
		case mc_node_lambda:         _mc_children(lambda.parameters); _mc_child(lambda.result); _mc_child(lambda.body); break;
		case mc_node_sizeof:         _mc_child(sizeof_.type); break;
		case mc_node_lenof:          _mc_child(lenof.operand); break;
		case mc_node_assert:         _mc_child(assert_.condition); _mc_child(assert_.message); break;
		case mc_node_let:            _mc_child(let.type); _mc_child(let.value); _mc_children(let.attributes); break;
		case mc_node_declaration_statement: _mc_child(declaration_statement.declaration); break;
		case mc_node_if:             _mc_child(if_.condition); _mc_child(if_.then); _mc_child(if_.otherwise); _mc_children(if_.attributes); break;
		case mc_node_while:          _mc_child(while_.condition); _mc_child(while_.body); _mc_children(while_.attributes); break;
		case mc_node_for:            _mc_child(for_.start); _mc_child(for_.end); _mc_child(for_.body); _mc_children(for_.attributes); break;
		case mc_node_return:         _mc_child(return_.value); _mc_children(return_.attributes); break;
		case mc_node_break:
		case mc_node_continue:       _mc_children(jump.attributes); break;
		case mc_node_block:          _mc_children(block.statements); _mc_children(block.attributes); break;
		case mc_node_expression_statement: _mc_child(expression_statement.expression); _mc_children(expression_statement.attributes); break;
		case mc_node_assign:         _mc_child(assign.target); _mc_child(assign.value); _mc_children(assign.attributes); break;
		case mc_node_param:          _mc_child(param.type); _mc_child(param.default_); break;
		case mc_node_field:          _mc_child(field.type); _mc_child(field.default_); _mc_children(field.attributes); break;
		case mc_node_function:
			_mc_children(function.generics); _mc_children(function.parameters); _mc_child(function.result);
			_mc_child(function.body); _mc_children(function.attributes);
			break;
		case mc_node_struct:         _mc_children(struct_.generics); _mc_children(struct_.fields); _mc_children(struct_.attributes); break;
		case mc_node_constant:       _mc_child(constant.value); _mc_children(constant.attributes); break;
		case mc_node_import:         copy->as.import.path = _mc_clone_views(arena, &node->as.import.path); break;
		case mc_node_from_import:    copy->as.from_import.path = _mc_clone_views(arena, &node->as.from_import.path); _mc_children(from_import.names); break;
		case mc_node_module:         _mc_children(module.imports); _mc_children(module.declarations); break;
		default: break;
	}
#undef _mc_child
#undef _mc_children
	return copy;
}

mc_node_s* mc_node_clone(mc_arena_s* const arena, const mc_node_s* const node) {
	return _mc_clone(arena, node);
}

bool mc_node_is_declaration(const mc_node_s* const node) {
	return mc_node_function == node->kind || mc_node_struct == node->kind || mc_node_constant == node->kind;
}

mc_view_s mc_declaration_name(const mc_node_s* const node) {
	switch (node->kind) {
		case mc_node_function: return node->as.function.name;
		case mc_node_struct:   return node->as.struct_.name;
		case mc_node_constant: return node->as.constant.name;
		default: mc_unreachable();
	}
}

mc_span_s mc_declaration_name_span(const mc_node_s* const node) {
	switch (node->kind) {
		case mc_node_function: return node->as.function.name_span;
		case mc_node_struct:   return node->as.struct_.name_span;
		case mc_node_constant: return node->as.constant.name_span;
		default: mc_unreachable();
	}
}

const mc_nodes_s* mc_declaration_attributes(const mc_node_s* const node) {
	switch (node->kind) {
		case mc_node_function: return &node->as.function.attributes;
		case mc_node_struct:   return &node->as.struct_.attributes;
		case mc_node_constant: return &node->as.constant.attributes;
		default: mc_unreachable();
	}
}

bool mc_declaration_public(const mc_node_s* const node) {
	switch (node->kind) {
		case mc_node_function: return node->as.function.public_;
		case mc_node_struct:   return node->as.struct_.public_;
		case mc_node_constant: return node->as.constant.public_;
		default: mc_unreachable();
	}
}

const mc_node_s* mc_attribute_find(const mc_nodes_s* const attributes, const char* const name) {
	for (size_t i = 0; i < attributes->count; ++i) {
		if (mc_view_eq_cstr(attributes->data[i]->as.attribute.name, name)) return attributes->data[i];
	}
	return NULL;
}

const mc_nodes_s* mc_statement_attributes(const mc_node_s* const node) {
	static const mc_nodes_s none = {0};
	switch (node->kind) {
		case mc_node_let:                  return &node->as.let.attributes;
		case mc_node_expression_statement: return &node->as.expression_statement.attributes;
		case mc_node_assign:               return &node->as.assign.attributes;
		case mc_node_if:                   return &node->as.if_.attributes;
		case mc_node_while:                return &node->as.while_.attributes;
		case mc_node_for:                  return &node->as.for_.attributes;
		case mc_node_return:               return &node->as.return_.attributes;
		case mc_node_break:
		case mc_node_continue:             return &node->as.jump.attributes;
		case mc_node_block:                return &node->as.block.attributes;
		default:                           return &none;
	}
}
