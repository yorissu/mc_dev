
#include "mia/printer.h"
#include "mia/format.h"

typedef struct {
	mia_string_s               header;
	const mia_printer_hooks_s* hooks ;
	mia_string_s*              out   ;
	int                        depth ;
} _mia_printer_s;

typedef struct {
	const char*            name ;
	const mia_ast_node_s*  node ;
	const mia_ast_nodes_s* nodes;
} _mia_printer_child_s;

typedef mia_vector_of(_mia_printer_child_s) _mia_printer_children_s;

static void _mia_printer_write(_mia_printer_s* const printer, const mia_ast_node_s* const node, const int depth, const char* const label);

/// a text field as python shows it: plain when it is a non-empty name, quoted otherwise.
static void _mia_printer_text(mia_string_s* const header, const char* const name, const mia_view_s value) {
	bool plain = false;
	for (size_t i = 0; i < value.length; ++i) {
		const uint8_t c = (uint8_t)value.data[i];
		const bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c >= 0x80;
		if (alnum) plain = true;
		else if (c != '_') {
			plain = false;
			break;
		}
	}
	mia_string_appendf(header, " %s=", name);
	if (plain) mia_string_append(header, value);
	else mia_format_repr_text(header, value);
}

static void _mia_printer_flag(mia_string_s* const header, const char* const name, const bool value) {
	if (value) mia_string_appendf(header, " %s", name);
}

static void _mia_printer_suffix(mia_string_s* const header, const mia_scalar_e suffix) {
	if (suffix != mia_scalar_none) _mia_printer_text(header, "suffix", mia_view_cstr(mia_scalar_name(suffix)));
}

static void _mia_printer_path(mia_string_s* const header, const char* const name, const mia_view_list_s* const path) {
	if (0 == path->count) return;
	mia_string_appendf(header, " %s=[", name);
	for (size_t i = 0; i < path->count; ++i) {
		if (i > 0) mia_string_append_cstr(header, ", ");
		mia_string_s item = {0};
		_mia_printer_text(&item, "x", path->data[i]);
		mia_string_append(header, mia_view_slice(mia_string_view(&item), 3, item.length));
		mia_string_free(&item);
	}
	mia_string_append_char(header, ']');
}

static void _mia_printer_node(_mia_printer_children_s* const children, const char* const name, const mia_ast_node_s* const node) {
	if (NULL == node) return;
	const _mia_printer_child_s child = { .name = name, .node = node, };
	mia_vector_push(children, child);
}

static void _mia_printer_list(_mia_printer_children_s* const children, const char* const name, const mia_ast_nodes_s* const nodes) {
	if (0 == nodes->count) return;
	const _mia_printer_child_s child = { .name = name, .nodes = nodes, };
	mia_vector_push(children, child);
}

static void _mia_printer_fields(const mia_ast_node_s* const node, mia_string_s* const header, _mia_printer_children_s* const children) {
	switch (node->kind) {
		case mia_ast_node_type_name:
			_mia_printer_path(header, "path", &node->as.type_name.path);
			_mia_printer_list(children, "arguments", &node->as.type_name.arguments);
			break;
		case mia_ast_node_type_slice:     _mia_printer_node(children, "element", node->as.type_slice.element); break;
		case mia_ast_node_type_array:     _mia_printer_node(children, "length", node->as.type_array.length); _mia_printer_node(children, "element", node->as.type_array.element); break;
		case mia_ast_node_type_reference: _mia_printer_node(children, "element", node->as.type_reference.element); break;
		case mia_ast_node_type_function:  _mia_printer_list(children, "parameters", &node->as.type_function.parameters); _mia_printer_node(children, "result", node->as.type_function.result); break;
		case mia_ast_node_integer:
			mia_string_append_cstr(header, " value=");
			mia_format_uint128(header, node->as.integer.value);
			_mia_printer_suffix(header, node->as.integer.suffix);
			break;
		case mia_ast_node_float:
			mia_string_append_cstr(header, " value=");
			mia_format_float_repr(header, node->as.floating.value);
			_mia_printer_suffix(header, node->as.floating.suffix);
			break;
		case mia_ast_node_char:   mia_string_appendf(header, " value=%u", node->as.character.value); break;
		case mia_ast_node_string:
			mia_string_append_cstr(header, " data=");
			mia_format_repr_bytes(header, node->as.string.data);
			_mia_printer_flag(header, "c", node->as.string.c);
			break;
		case mia_ast_node_bool:   _mia_printer_flag(header, "value", node->as.boolean.value); break;
		case mia_ast_node_name:   _mia_printer_text(header, "name", node->as.name.name); break;
		case mia_ast_node_member: _mia_printer_node(children, "target", node->as.member.target); _mia_printer_text(header, "name", node->as.member.name); break;
		case mia_ast_node_call:
			_mia_printer_flag(header, "piped", node->as.call.piped);
			_mia_printer_node(children, "callee", node->as.call.callee);
			_mia_printer_list(children, "type_arguments", &node->as.call.type_arguments);
			_mia_printer_list(children, "arguments", &node->as.call.arguments);
			break;
		case mia_ast_node_specialize: _mia_printer_node(children, "target", node->as.specialize.target); _mia_printer_list(children, "type_arguments", &node->as.specialize.type_arguments); break;
		case mia_ast_node_index:      _mia_printer_node(children, "target", node->as.index.target); _mia_printer_node(children, "index", node->as.index.index); break;
		case mia_ast_node_slice_of:
			_mia_printer_node(children, "target", node->as.slice_of.target);
			_mia_printer_node(children, "low", node->as.slice_of.low);
			_mia_printer_node(children, "high", node->as.slice_of.high);
			_mia_printer_flag(header, "inclusive", node->as.slice_of.inclusive);
			break;
		case mia_ast_node_unary:
			_mia_printer_text(header, "operator", mia_view_cstr(mia_ast_operator_text(node->as.unary.operation)));
			_mia_printer_node(children, "operand", node->as.unary.operand);
			break;
		case mia_ast_node_binary:
			_mia_printer_text(header, "operator", mia_view_cstr(mia_ast_operator_text(node->as.binary.operation)));
			_mia_printer_node(children, "left", node->as.binary.left);
			_mia_printer_node(children, "right", node->as.binary.right);
			break;
		case mia_ast_node_paren:     _mia_printer_node(children, "inner", node->as.paren.inner); break;
		case mia_ast_node_cast:      _mia_printer_node(children, "type", node->as.cast.type); _mia_printer_node(children, "operand", node->as.cast.operand); break;
		case mia_ast_node_reference: _mia_printer_node(children, "operand", node->as.reference.operand); break;
		case mia_ast_node_entry:
			if (node->as.entry.named) _mia_printer_text(header, "name", node->as.entry.name);
			_mia_printer_node(children, "value", node->as.entry.value);
			break;
		case mia_ast_node_composite: _mia_printer_node(children, "type", node->as.composite.type); _mia_printer_list(children, "entries", &node->as.composite.entries); break;
		case mia_ast_node_lambda:
			_mia_printer_list(children, "parameters", &node->as.lambda.parameters);
			_mia_printer_node(children, "result", node->as.lambda.result);
			_mia_printer_node(children, "body", node->as.lambda.body);
			break;
		case mia_ast_node_sizeof: _mia_printer_node(children, "type", node->as.size_of.type); break;
		case mia_ast_node_lenof:  _mia_printer_node(children, "operand", node->as.lenof.operand); break;
		case mia_ast_node_assert: _mia_printer_node(children, "condition", node->as.assert_statement.condition); _mia_printer_node(children, "message", node->as.assert_statement.message); break;
		case mia_ast_node_attribute:
			_mia_printer_text(header, "name", node->as.attribute.name);
			if (mia_ast_attribute_value_integer == node->as.attribute.value_kind) mia_string_appendf(header, " value=%lld", (long long)node->as.attribute.integer);
			else if (node->as.attribute.value_kind != mia_ast_attribute_value_none) _mia_printer_text(header, "value", node->as.attribute.text);
			break;
		case mia_ast_node_let:
			_mia_printer_text(header, "name", node->as.let.name);
			_mia_printer_node(children, "type", node->as.let.type);
			_mia_printer_node(children, "value", node->as.let.value);
			_mia_printer_list(children, "attributes", &node->as.let.attributes);
			break;
		case mia_ast_node_declaration_statement: _mia_printer_node(children, "declaration", node->as.declaration_statement.declaration); break;
		case mia_ast_node_if:
			_mia_printer_node(children, "condition", node->as.if_statement.condition);
			_mia_printer_node(children, "then", node->as.if_statement.then);
			_mia_printer_node(children, "otherwise", node->as.if_statement.otherwise);
			_mia_printer_list(children, "attributes", &node->as.if_statement.attributes);
			break;
		case mia_ast_node_while:
			_mia_printer_node(children, "condition", node->as.while_statement.condition);
			_mia_printer_node(children, "body", node->as.while_statement.body);
			_mia_printer_list(children, "attributes", &node->as.while_statement.attributes);
			break;
		case mia_ast_node_for:
			_mia_printer_text(header, "name", node->as.for_statement.name);
			_mia_printer_node(children, "start", node->as.for_statement.start);
			_mia_printer_node(children, "end", node->as.for_statement.end);
			_mia_printer_flag(header, "inclusive", node->as.for_statement.inclusive);
			_mia_printer_node(children, "body", node->as.for_statement.body);
			_mia_printer_list(children, "attributes", &node->as.for_statement.attributes);
			break;
		case mia_ast_node_return:   _mia_printer_node(children, "value", node->as.return_statement.value); _mia_printer_list(children, "attributes", &node->as.return_statement.attributes); break;
		case mia_ast_node_break:
		case mia_ast_node_continue: _mia_printer_list(children, "attributes", &node->as.jump.attributes); break;
		case mia_ast_node_block:
			_mia_printer_list(children, "statements", &node->as.block.statements);
			_mia_printer_flag(header, "braceless", node->as.block.braceless);
			_mia_printer_list(children, "attributes", &node->as.block.attributes);
			break;
		case mia_ast_node_expression_statement:
			_mia_printer_node(children, "expression", node->as.expression_statement.expression);
			_mia_printer_list(children, "attributes", &node->as.expression_statement.attributes);
			break;
		case mia_ast_node_assign: {
			char text[8];
			const mia_ast_operator_e operation = node->as.assign.operation;
			(void)snprintf(text, sizeof(text), "%s%s", mia_ast_operator_text(operation), mia_ast_operator_assign == operation ? "" : "=");
			_mia_printer_text(header, "operator", mia_view_cstr(text));
			_mia_printer_node(children, "target", node->as.assign.target);
			_mia_printer_node(children, "value", node->as.assign.value);
			_mia_printer_list(children, "attributes", &node->as.assign.attributes);
			break;
		}
		case mia_ast_node_generic: _mia_printer_text(header, "name", node->as.generic.name); break;
		case mia_ast_node_param:
			_mia_printer_text(header, "name", node->as.param.name);
			_mia_printer_node(children, "type", node->as.param.type);
			_mia_printer_node(children, "default", node->as.param.default_value);
			_mia_printer_flag(header, "variadic", node->as.param.variadic);
			break;
		case mia_ast_node_field:
			_mia_printer_text(header, "name", node->as.field.name);
			_mia_printer_node(children, "type", node->as.field.type);
			_mia_printer_node(children, "default", node->as.field.default_value);
			_mia_printer_list(children, "attributes", &node->as.field.attributes);
			break;
		case mia_ast_node_function:
			_mia_printer_text(header, "name", node->as.function.name);
			_mia_printer_list(children, "generics", &node->as.function.generics);
			_mia_printer_list(children, "parameters", &node->as.function.parameters);
			_mia_printer_node(children, "result", node->as.function.result);
			_mia_printer_node(children, "body", node->as.function.body);
			_mia_printer_list(children, "attributes", &node->as.function.attributes);
			_mia_printer_flag(header, "public", node->as.function.is_public);
			break;
		case mia_ast_node_struct:
			_mia_printer_text(header, "name", node->as.structure.name);
			_mia_printer_list(children, "generics", &node->as.structure.generics);
			_mia_printer_list(children, "fields", &node->as.structure.fields);
			_mia_printer_list(children, "attributes", &node->as.structure.attributes);
			_mia_printer_flag(header, "public", node->as.structure.is_public);
			break;
		case mia_ast_node_constant:
			_mia_printer_text(header, "name", node->as.constant.name);
			_mia_printer_node(children, "value", node->as.constant.value);
			_mia_printer_list(children, "attributes", &node->as.constant.attributes);
			_mia_printer_flag(header, "public", node->as.constant.is_public);
			break;
		case mia_ast_node_import_name:
			_mia_printer_text(header, "name", node->as.import_name.name);
			if (node->as.import_name.has_alias) _mia_printer_text(header, "alias", node->as.import_name.alias);
			break;
		case mia_ast_node_import:
			_mia_printer_path(header, "path", &node->as.import.path);
			if (node->as.import.has_alias) _mia_printer_text(header, "alias", node->as.import.alias);
			break;
		case mia_ast_node_from_import:
			_mia_printer_path(header, "path", &node->as.from_import.path);
			_mia_printer_list(children, "names", &node->as.from_import.names);
			_mia_printer_flag(header, "star", node->as.from_import.star);
			break;
		case mia_ast_node_module: _mia_printer_list(children, "imports", &node->as.module.imports); _mia_printer_list(children, "declarations", &node->as.module.declarations); break;
		default: mia_unreachable();
	}
}

static void _mia_printer_write(_mia_printer_s* const printer, const mia_ast_node_s* const node, const int depth, const char* const label) {
	mia_string_s header = {0};
	_mia_printer_children_s children = {0};
	mia_string_append_cstr(&header, mia_ast_node_kind_name(node->kind));
	_mia_printer_fields(node, &header, &children);
	if (printer->hooks != NULL && node->note.type != NULL) {
		mia_string_append_cstr(&header, " : ");
		printer->hooks->type(&header, node->note.type);
	}
	if (printer->hooks != NULL && node->note.instance != NULL) {
		mia_string_append_cstr(&header, " -> ");
		printer->hooks->instance(&header, node->note.instance);
	}
	mia_string_append_repeat(printer->out, ' ', (size_t)depth * 2);
	if (label != NULL) mia_string_appendf(printer->out, "%s: ", label);
	mia_string_append(printer->out, mia_string_view(&header));
	mia_string_append_char(printer->out, '\n');
	for (size_t i = 0; i < children.count; ++i) {
		const _mia_printer_child_s* const child = &children.data[i];
		if (child->node != NULL) {
			_mia_printer_write(printer, child->node, depth + 1, child->name);
			continue;
		}
		mia_string_append_repeat(printer->out, ' ', (size_t)depth * 2 + 2);
		mia_string_appendf(printer->out, "%s:\n", child->name);
		for (size_t j = 0; j < child->nodes->count; ++j) _mia_printer_write(printer, child->nodes->data[j], depth + 2, NULL);
	}
	mia_vector_free(&children);
	mia_string_free(&header);
}

void mia_printer_print(const mia_ast_node_s* const node, const mia_printer_hooks_s* const hooks, mia_string_s* const out) {
	_mia_printer_s printer = { .hooks = hooks, .out = out, };
	_mia_printer_write(&printer, node, 0, NULL);
}
