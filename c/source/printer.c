
#include "mc/printer.h"
#include "mc/format.h"

typedef struct {
	mc_string_s             header;
	const mc_print_hooks_s* hooks ;
	mc_string_s*            out   ;
	int                     depth ;
} _mc_printer_s;

typedef struct {
	const char*       name ;
	const mc_node_s*  node ;
	const mc_nodes_s* nodes;
} _mc_child_s;

typedef mc_vector_of(_mc_child_s) _mc_children_s;

static void _mc_write(_mc_printer_s* const printer, const mc_node_s* const node, const int depth, const char* const label);

/// a text field as python shows it: plain when it is a non-empty name, quoted otherwise.
static void _mc_text(mc_string_s* const header, const char* const name, const mc_view_s value) {
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
	mc_string_appendf(header, " %s=", name);
	if (plain) mc_string_append(header, value);
	else mc_format_repr_text(header, value);
}

static void _mc_flag(mc_string_s* const header, const char* const name, const bool value) {
	if (value) mc_string_appendf(header, " %s", name);
}

static void _mc_suffix(mc_string_s* const header, const mc_scalar_e suffix) {
	if (suffix != mc_scalar_none_) _mc_text(header, "suffix", mc_view_cstr(mc_scalar_name(suffix)));
}

static void _mc_path(mc_string_s* const header, const char* const name, const mc_views_s* const path) {
	if (0 == path->count) return;
	mc_string_appendf(header, " %s=[", name);
	for (size_t i = 0; i < path->count; ++i) {
		if (i > 0) mc_string_append_cstr(header, ", ");
		mc_string_s item = {0};
		_mc_text(&item, "x", path->data[i]);
		mc_string_append(header, mc_view_slice(mc_string_view(&item), 3, item.length));
		mc_string_free(&item);
	}
	mc_string_append_char(header, ']');
}

static void _mc_node(_mc_children_s* const children, const char* const name, const mc_node_s* const node) {
	if (NULL == node) return;
	const _mc_child_s child = { .name = name, .node = node, };
	mc_vector_push(children, child);
}

static void _mc_list(_mc_children_s* const children, const char* const name, const mc_nodes_s* const nodes) {
	if (0 == nodes->count) return;
	const _mc_child_s child = { .name = name, .nodes = nodes, };
	mc_vector_push(children, child);
}

static void _mc_fields(const mc_node_s* const node, mc_string_s* const header, _mc_children_s* const children) {
	switch (node->kind) {
		case mc_node_type_name:
			_mc_path(header, "path", &node->as.type_name.path);
			_mc_list(children, "arguments", &node->as.type_name.arguments);
			break;
		case mc_node_type_slice:     _mc_node(children, "element", node->as.type_slice.element); break;
		case mc_node_type_array:     _mc_node(children, "length", node->as.type_array.length); _mc_node(children, "element", node->as.type_array.element); break;
		case mc_node_type_reference: _mc_node(children, "element", node->as.type_reference.element); _mc_flag(header, "mutable", node->as.type_reference.mutable_); break;
		case mc_node_type_function:  _mc_list(children, "parameters", &node->as.type_function.parameters); _mc_node(children, "result", node->as.type_function.result); break;
		case mc_node_integer:
			mc_string_append_cstr(header, " value=");
			mc_format_uint128(header, node->as.integer.value);
			_mc_suffix(header, node->as.integer.suffix);
			break;
		case mc_node_float:
			mc_string_append_cstr(header, " value=");
			mc_format_float_repr(header, node->as.float_.value);
			_mc_suffix(header, node->as.float_.suffix);
			break;
		case mc_node_char:   mc_string_appendf(header, " value=%u", node->as.char_.value); break;
		case mc_node_string:
			mc_string_append_cstr(header, " data=");
			mc_format_repr_bytes(header, node->as.string.data);
			_mc_flag(header, "c", node->as.string.c);
			break;
		case mc_node_bool:   _mc_flag(header, "value", node->as.bool_.value); break;
		case mc_node_null:   break;
		case mc_node_name:   _mc_text(header, "name", node->as.name.name); break;
		case mc_node_member: _mc_node(children, "target", node->as.member.target); _mc_text(header, "name", node->as.member.name); break;
		case mc_node_call:
			_mc_node(children, "callee", node->as.call.callee);
			_mc_list(children, "type_arguments", &node->as.call.type_arguments);
			_mc_list(children, "arguments", &node->as.call.arguments);
			break;
		case mc_node_specialize: _mc_node(children, "target", node->as.specialize.target); _mc_list(children, "type_arguments", &node->as.specialize.type_arguments); break;
		case mc_node_index:      _mc_node(children, "target", node->as.index.target); _mc_node(children, "index", node->as.index.index); break;
		case mc_node_slice_of:
			_mc_node(children, "target", node->as.slice_of.target);
			_mc_node(children, "low", node->as.slice_of.low);
			_mc_node(children, "high", node->as.slice_of.high);
			_mc_flag(header, "inclusive", node->as.slice_of.inclusive);
			break;
		case mc_node_unary:
			_mc_text(header, "operator", mc_view_cstr(mc_operator_text(node->as.unary.operator_)));
			_mc_node(children, "operand", node->as.unary.operand);
			break;
		case mc_node_binary:
			_mc_text(header, "operator", mc_view_cstr(mc_operator_text(node->as.binary.operator_)));
			_mc_node(children, "left", node->as.binary.left);
			_mc_node(children, "right", node->as.binary.right);
			break;
		case mc_node_paren:     _mc_node(children, "inner", node->as.paren.inner); break;
		case mc_node_cast:      _mc_node(children, "type", node->as.cast.type); _mc_node(children, "operand", node->as.cast.operand); break;
		case mc_node_reference: _mc_node(children, "operand", node->as.reference.operand); _mc_flag(header, "mutable", node->as.reference.mutable_); break;
		case mc_node_entry:
			if (node->as.entry.named) _mc_text(header, "name", node->as.entry.name);
			_mc_node(children, "value", node->as.entry.value);
			break;
		case mc_node_composite: _mc_node(children, "type", node->as.composite.type); _mc_list(children, "entries", &node->as.composite.entries); break;
		case mc_node_lambda:
			_mc_list(children, "parameters", &node->as.lambda.parameters);
			_mc_node(children, "result", node->as.lambda.result);
			_mc_node(children, "body", node->as.lambda.body);
			break;
		case mc_node_sizeof: _mc_node(children, "type", node->as.sizeof_.type); break;
		case mc_node_lenof:  _mc_node(children, "operand", node->as.lenof.operand); break;
		case mc_node_assert: _mc_node(children, "condition", node->as.assert_.condition); _mc_node(children, "message", node->as.assert_.message); break;
		case mc_node_attribute:
			_mc_text(header, "name", node->as.attribute.name);
			if (mc_attribute_value_integer == node->as.attribute.value_kind) mc_string_appendf(header, " value=%lld", (long long)node->as.attribute.integer);
			else if (node->as.attribute.value_kind != mc_attribute_value_none) _mc_text(header, "value", node->as.attribute.text);
			break;
		case mc_node_let:
			_mc_text(header, "name", node->as.let.name);
			_mc_flag(header, "mutable", node->as.let.mutable_);
			_mc_node(children, "type", node->as.let.type);
			_mc_node(children, "value", node->as.let.value);
			_mc_list(children, "attributes", &node->as.let.attributes);
			break;
		case mc_node_declaration_statement: _mc_node(children, "declaration", node->as.declaration_statement.declaration); break;
		case mc_node_if:
			_mc_node(children, "condition", node->as.if_.condition);
			_mc_node(children, "then", node->as.if_.then);
			_mc_node(children, "otherwise", node->as.if_.otherwise);
			_mc_list(children, "attributes", &node->as.if_.attributes);
			break;
		case mc_node_while:
			_mc_node(children, "condition", node->as.while_.condition);
			_mc_node(children, "body", node->as.while_.body);
			_mc_list(children, "attributes", &node->as.while_.attributes);
			break;
		case mc_node_for:
			_mc_text(header, "name", node->as.for_.name);
			_mc_node(children, "start", node->as.for_.start);
			_mc_node(children, "end", node->as.for_.end);
			_mc_flag(header, "inclusive", node->as.for_.inclusive);
			_mc_node(children, "body", node->as.for_.body);
			_mc_list(children, "attributes", &node->as.for_.attributes);
			break;
		case mc_node_return:   _mc_node(children, "value", node->as.return_.value); _mc_list(children, "attributes", &node->as.return_.attributes); break;
		case mc_node_break:
		case mc_node_continue: _mc_list(children, "attributes", &node->as.jump.attributes); break;
		case mc_node_block:
			_mc_list(children, "statements", &node->as.block.statements);
			_mc_flag(header, "braceless", node->as.block.braceless);
			_mc_list(children, "attributes", &node->as.block.attributes);
			break;
		case mc_node_expression_statement:
			_mc_node(children, "expression", node->as.expression_statement.expression);
			_mc_list(children, "attributes", &node->as.expression_statement.attributes);
			break;
		case mc_node_assign: {
			char text[8];
			const mc_operator_e operator_ = node->as.assign.operator_;
			(void)snprintf(text, sizeof(text), "%s%s", mc_operator_text(operator_), mc_operator_assign == operator_ ? "" : "=");
			_mc_text(header, "operator", mc_view_cstr(text));
			_mc_node(children, "target", node->as.assign.target);
			_mc_node(children, "value", node->as.assign.value);
			_mc_list(children, "attributes", &node->as.assign.attributes);
			break;
		}
		case mc_node_generic: _mc_text(header, "name", node->as.generic.name); break;
		case mc_node_param:
			_mc_text(header, "name", node->as.param.name);
			_mc_flag(header, "mutable", node->as.param.mutable_);
			_mc_node(children, "type", node->as.param.type);
			_mc_node(children, "default", node->as.param.default_);
			_mc_flag(header, "variadic", node->as.param.variadic);
			break;
		case mc_node_field:
			_mc_text(header, "name", node->as.field.name);
			_mc_flag(header, "mutable", node->as.field.mutable_);
			_mc_node(children, "type", node->as.field.type);
			_mc_node(children, "default", node->as.field.default_);
			_mc_list(children, "attributes", &node->as.field.attributes);
			break;
		case mc_node_function:
			_mc_text(header, "name", node->as.function.name);
			_mc_list(children, "generics", &node->as.function.generics);
			_mc_list(children, "parameters", &node->as.function.parameters);
			_mc_node(children, "result", node->as.function.result);
			_mc_node(children, "body", node->as.function.body);
			_mc_list(children, "attributes", &node->as.function.attributes);
			_mc_flag(header, "public", node->as.function.public_);
			break;
		case mc_node_struct:
			_mc_text(header, "name", node->as.struct_.name);
			_mc_list(children, "generics", &node->as.struct_.generics);
			_mc_list(children, "fields", &node->as.struct_.fields);
			_mc_list(children, "attributes", &node->as.struct_.attributes);
			_mc_flag(header, "public", node->as.struct_.public_);
			break;
		case mc_node_constant:
			_mc_text(header, "name", node->as.constant.name);
			_mc_node(children, "value", node->as.constant.value);
			_mc_list(children, "attributes", &node->as.constant.attributes);
			_mc_flag(header, "public", node->as.constant.public_);
			break;
		case mc_node_import_name:
			_mc_text(header, "name", node->as.import_name.name);
			if (node->as.import_name.has_alias) _mc_text(header, "alias", node->as.import_name.alias);
			break;
		case mc_node_import:
			_mc_path(header, "path", &node->as.import.path);
			if (node->as.import.has_alias) _mc_text(header, "alias", node->as.import.alias);
			break;
		case mc_node_from_import:
			_mc_path(header, "path", &node->as.from_import.path);
			_mc_list(children, "names", &node->as.from_import.names);
			_mc_flag(header, "star", node->as.from_import.star);
			break;
		case mc_node_module: _mc_list(children, "imports", &node->as.module.imports); _mc_list(children, "declarations", &node->as.module.declarations); break;
		default: mc_unreachable();
	}
}

static void _mc_write(_mc_printer_s* const printer, const mc_node_s* const node, const int depth, const char* const label) {
	mc_string_s header = {0};
	_mc_children_s children = {0};
	mc_string_append_cstr(&header, mc_node_kind_name(node->kind));
	_mc_fields(node, &header, &children);
	if (printer->hooks != NULL && node->note.type != NULL) {
		mc_string_append_cstr(&header, " : ");
		printer->hooks->type(&header, node->note.type);
	}
	if (printer->hooks != NULL && node->note.instance != NULL) {
		mc_string_append_cstr(&header, " -> ");
		printer->hooks->instance(&header, node->note.instance);
	}
	mc_string_append_repeat(printer->out, ' ', (size_t)depth * 2);
	if (label != NULL) mc_string_appendf(printer->out, "%s: ", label);
	mc_string_append(printer->out, mc_string_view(&header));
	mc_string_append_char(printer->out, '\n');
	for (size_t i = 0; i < children.count; ++i) {
		const _mc_child_s* const child = &children.data[i];
		if (child->node != NULL) {
			_mc_write(printer, child->node, depth + 1, child->name);
			continue;
		}
		mc_string_append_repeat(printer->out, ' ', (size_t)depth * 2 + 2);
		mc_string_appendf(printer->out, "%s:\n", child->name);
		for (size_t j = 0; j < child->nodes->count; ++j) _mc_write(printer, child->nodes->data[j], depth + 2, NULL);
	}
	mc_vector_free(&children);
	mc_string_free(&header);
}

void mc_print_tree(const mc_node_s* const node, const mc_print_hooks_s* const hooks, mc_string_s* const out) {
	_mc_printer_s printer = { .hooks = hooks, .out = out, };
	_mc_write(&printer, node, 0, NULL);
}
