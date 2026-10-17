
/// the checker: statements, assignments and the places they write to,
/// control flow, and the tracking of variables that may not be assigned yet.

#include "check.h"

#include <stdio.h>
#include <string.h>

static void _mc_statement(mc_checker_s* const c, mc_node_s* const node, mc_context_s* const context);

void mc_block(mc_checker_s* const c, mc_node_s* const node, mc_context_s* const context, const bool new_scope) {
	if (new_scope) mc_context_push_scope(c, context);
	const mc_nodes_s* const statements = &node->as.block.statements;
	// local constants and functions can be used anywhere in their block.
	for (size_t i = 0; i < statements->count; ++i) {
		mc_node_s* const statement = statements->data[i];
		if (statement->kind != mc_node_declaration_statement) continue;
		mc_node_s* const declaration = statement->as.declaration_statement.declaration;
		mc_symbol_s* symbol = NULL;
		if (mc_node_function == declaration->kind) {
			mc_function_s* const info = mc_arena_new(c->arena, mc_function_s);
			info->base.kind = mc_symbol_function;
			info->node = declaration;
			info->module = context->module;
			info->parent = context;
			symbol = &info->base;
		}
		else if (mc_node_constant == declaration->kind) {
			mc_constant_s* const info = mc_arena_new(c->arena, mc_constant_s);
			info->base.kind = mc_symbol_constant;
			info->node = declaration;
			info->module = context->module;
			info->context = context;
			symbol = &info->base;
		}
		else {
			continue;
		}
		statement->note.symbol = symbol;
		mc_symbol_s* const previous = mc_context_declare(context, mc_declaration_name(declaration), symbol);
		if (previous != NULL) {
			mc_span_s first = mc_no_span;
			if (mc_symbol_function == previous->kind) first = ((mc_function_s*)previous)->node->as.function.name_span;
			else if (mc_symbol_constant == previous->kind) first = ((mc_constant_s*)previous)->node->as.constant.name_span;
			else if (mc_symbol_variable == previous->kind) first = ((mc_variable_s*)previous)->span;
			mc_label(mc_label_main(mc_error(c->diag, mc_E0401, mc_declaration_name_span(declaration), "`" mc_view_fmt "` is declared twice in this block",
				mc_view_arg(mc_declaration_name(declaration))), "declared again here"), first, "first declared here");
		}
	}
	for (size_t i = 0; i < statements->count; ++i) _mc_statement(c, statements->data[i], context);
	if (new_scope) mc_context_pop_scope(context);
}

static void _mc_declaration_statement(mc_checker_s* const c, mc_node_s* const node, mc_context_s* const context) {
	mc_symbol_s* const symbol = node->note.symbol;
	if (NULL == symbol) return;
	if (mc_symbol_constant == symbol->kind) {
		(void)mc_constant_type(c, (mc_constant_s*)symbol);
	}
	else if (mc_symbol_function == symbol->kind) {
		// checked where it is declared, so that earlier variables are in scope
		// (and reported as captures when the body uses them).
		mc_function_s* const info = (mc_function_s*)symbol;
		mc_function_declaration(c, info, context);
		if (!mc_function_is_generic(info)) {
			mc_instance_s* const instance = mc_instance(c, info, NULL, 0, context, info->node->as.function.name_span);
			if (instance != NULL) mc_check_instance(c, instance);
		}
	}
}

static void _mc_let(mc_checker_s* const c, mc_node_s* const node, mc_context_s* const context) {
	const mc_attribute_set_s found = mc_attributes(c, &node->as.let.attributes, "let");
	if (found.wrap != NULL) context->wrapping += 1;
	const mc_type_s* const error = mc_special_type(c, mc_special_error);
	const mc_type_s* const declared = (node->as.let.type != NULL) ? mc_resolve_type(c, node->as.let.type, context, false, true) : NULL;
	const mc_type_s* type = declared;
	mc_node_s* const value = node->as.let.value;
	if (value != NULL) {
		if (declared != NULL) {
			const mc_type_s* const actual = mc_expression(c, value, context, declared);
			if (mc_type_array == declared->kind && declared->as.array.length < 0) {
				if (mc_type_array == actual->kind && actual->as.array.element == declared->as.array.element) {
					type = actual;
				}
				else if (actual != error) {
					mc_label_main(mc_error(c->diag, mc_E0502, value->span, "mismatched types"), "expected `%s`, found `%s`",
						mc_name_of(c, declared), mc_name_of(c, actual));
					type = error;
				}
			}
			else {
				mc_coerce(c, value, actual, declared);
			}
		}
		else {
			type = mc_settle(c, value, mc_expression(c, value, context, NULL));
		}
	}
	else if (NULL == declared) {
		mc_help(mc_label_main(mc_error(c->diag, mc_E0506, node->as.let.name_span, "variable `" mc_view_fmt "` needs a type or a value", mc_view_arg(node->as.let.name)),
			"cannot infer its type"), "write `let " mc_view_fmt ": T;` or `let " mc_view_fmt " = value;`", mc_view_arg(node->as.let.name), mc_view_arg(node->as.let.name));
		type = error;
	}
	else if (mc_type_array == declared->kind && declared->as.array.length < 0) {
		mc_label_main(mc_error(c->diag, mc_E0506, node->as.let.type->span, "`[_]` needs an initializer to take its length from"), "no value given");
		type = error;
	}
	if (found.wrap != NULL) context->wrapping -= 1;
	mc_variable_s* const variable = mc_arena_new(c->arena, mc_variable_s);
	variable->base.kind = mc_symbol_variable;
	variable->name = node->as.let.name;
	variable->type = (type != NULL) ? type : error;
	variable->kind = mc_variable_let;
	variable->mutable_ = node->as.let.mutable_;
	variable->span = node->as.let.name_span;
	variable->level = context->level;
	variable->loops = context->loops;
	variable->initializer = value;
	node->note.variable = variable;
	if (NULL == value && declared != NULL && NULL == found.uninit) mc_set_add(&context->unassigned, variable);
	mc_symbol_s* const previous = mc_map_get(mc_vector_last(&context->scopes), node->as.let.name);
	if (previous != NULL && mc_symbol_variable == previous->kind) {
		mc_help(mc_label(mc_label_main(mc_error(c->diag, mc_E0401, node->as.let.name_span, "`" mc_view_fmt "` is already declared in this scope",
			mc_view_arg(node->as.let.name)), "declared again"), ((mc_variable_s*)previous)->span, "first declared here"),
			"use another name, or assign with `=`");
	}
	(void)mc_context_declare(context, node->as.let.name, &variable->base);
	mc_vector_push(&context->variables, variable);
}

// --- places ----------------------------------------------------------------------

void mc_place_free(mc_place_s* const place) {
	mc_vector_free(&place->path);
}

static mc_place_s _mc_place(const bool valid, mc_variable_s* const variable, const bool mutable_, const char* const reason,
							const bool needs_root, const char* const help) {
	return (mc_place_s) {
		.valid = valid, .variable = variable, .mutable_ = mutable_, .reason = reason, .needs_root = needs_root, .help = help,
	};
}

static void _mc_path_copy(mc_place_s* const place, const mc_place_s* const base) {
	for (size_t i = 0; i < base->path.count; ++i) mc_vector_push(&place->path, base->path.data[i]);
}

mc_place_s mc_place_of(mc_checker_s* const c, const mc_node_s* const node, mc_context_s* const context) {
	if (mc_node_paren == node->kind) return mc_place_of(c, node->as.paren.inner, context);
	if (mc_node_name == node->kind) {
		mc_variable_s* const variable = node->note.variable;
		const char* const name = mc_view_cstring(c, node->as.name.name);
		if (NULL == variable) return _mc_place(true, NULL, false, mc_arena_strndup(c->arena, "", 0), false, "");
		mc_place_s place;
		if (variable->reference) {
			char* const reason = mc_format("`%s` is a read-only reference", name);
			char* const help = mc_format("declare the parameter as `mut %s: &...`", name);
			place = _mc_place(true, variable, variable->mutable_, mc_arena_strndup(c->arena, reason, strlen(reason)), true,
				mc_arena_strndup(c->arena, help, strlen(help)));
			mc_free(reason);
			mc_free(help);
		}
		else if (mc_variable_for == variable->kind) {
			place = _mc_place(true, variable, false, "a loop variable cannot be changed", true, "copy it into a `let mut` variable");
		}
		else {
			char* const reason = mc_format("`%s` is not mutable", name);
			char* const help = (mc_variable_let == variable->kind) ? mc_strdup("declare it with `let mut`") : mc_format("declare it as `mut %s`", name);
			place = _mc_place(true, variable, variable->mutable_, mc_arena_strndup(c->arena, reason, strlen(reason)), true,
				mc_arena_strndup(c->arena, help, strlen(help)));
			mc_free(reason);
			mc_free(help);
		}
		mc_vector_push(&place.path, (const char*)(const void*)variable);
		if (NULL == variable) place.path.count = 0;
		return place;
	}
	if (mc_node_member == node->kind) {
		const mc_field_s* const field = node->note.field;
		if (NULL == field) return _mc_place(true, NULL, false, "not a field of a variable", false, "");
		mc_place_s base = mc_place_of(c, node->as.member.target, context);
		if (!base.valid || (NULL == base.variable && !base.mutable_)) {
			mc_place_free(&base);
			return _mc_place(true, NULL, false, "part of a temporary value", false, "");
		}
		mc_place_s place;
		if (!field->mutable_) {
			char* const reason = mc_format("field `" mc_view_fmt "` is not `mut`", mc_view_arg(node->as.member.name));
			char* const help = mc_format("declare the field with `let mut " mc_view_fmt "`", mc_view_arg(node->as.member.name));
			place = _mc_place(true, base.variable, false, mc_arena_strndup(c->arena, reason, strlen(reason)), base.needs_root,
				mc_arena_strndup(c->arena, help, strlen(help)));
			mc_free(reason);
			mc_free(help);
		}
		else {
			place = _mc_place(true, base.variable, base.mutable_, base.reason, base.needs_root, base.help);
		}
		_mc_path_copy(&place, &base);
		mc_vector_push(&place.path, mc_view_cstring(c, node->as.member.name));
		mc_place_free(&base);
		return place;
	}
	if (mc_node_index == node->kind) {
		if (node->note.operator_call != NULL) return _mc_place(true, NULL, false, "an indexing operator function returns a copy", false, "");
		mc_cvalue_s value = {0};
		const char* component = "*";
		if (mc_try_value(c, node->as.index.index, &value) && mc_cvalue_integer == value.kind) {
			char text[64];
			(void)snprintf(text, sizeof(text), "%lld", (long long)mc_i128_to_int(value.as.integer));
			component = mc_arena_strndup(c->arena, text, strlen(text));
		}
		mc_place_s base = mc_place_of(c, node->as.index.target, context);
		const mc_type_s* const target_type = node->as.index.target->note.type;
		if (target_type != NULL && mc_type_slice == target_type->kind) {
			// the elements of a slice can always be written.
			mc_place_s place = _mc_place(true, base.valid ? base.variable : NULL, true, "", false, "");
			if (base.valid) _mc_path_copy(&place, &base);
			mc_vector_push(&place.path, component);
			mc_place_free(&base);
			return place;
		}
		if (!base.valid || (NULL == base.variable && !base.mutable_)) {
			mc_place_free(&base);
			return _mc_place(true, NULL, false, "part of a temporary value", false, "");
		}
		mc_place_s place = _mc_place(true, base.variable, base.mutable_, base.reason, base.needs_root, base.help);
		_mc_path_copy(&place, &base);
		mc_vector_push(&place.path, component);
		mc_place_free(&base);
		return place;
	}
	return _mc_place(false, NULL, false, "", false, "");
}

void mc_report_immutable(mc_checker_s* const c, const mc_place_s* const place, const mc_node_s* const node, const char* const action) {
	const mc_view_s text = mc_node_text(c, node);
	mc_diagnostic_s* const diagnostic = mc_label_main(mc_error(c->diag, mc_E0508, node->span, "cannot %s `" mc_view_fmt "`", action, mc_view_arg(text)),
		"%s", place->reason);
	if (place->help != NULL && place->help[0] != '\0') mc_help(diagnostic, "%s", place->help);
	if (place->variable != NULL) mc_label(diagnostic, place->variable->span, "declared here");
}

static void _mc_assign(mc_checker_s* const c, mc_node_s* const node, mc_context_s* const context) {
	mc_node_s* const target = node->as.assign.target;
	const mc_type_s* const error = mc_special_type(c, mc_special_error);
	if (mc_node_name == target->kind && mc_view_eq_cstr(target->as.name.name, "_")) {
		if (node->as.assign.operator_ != mc_operator_assign) {
			mc_label_main(mc_error(c->diag, mc_E0508, target->span, "`_` can only be assigned with `=`"), "discards a value");
		}
		(void)mc_settle(c, node->as.assign.value, mc_expression(c, node->as.assign.value, context, NULL));
		return;
	}
	const mc_type_s* target_type = NULL;
	if (mc_operator_assign == node->as.assign.operator_ && mc_node_name == target->kind) {
		mc_symbol_s* const symbol = mc_lookup(c, target->as.name.name, context, target->span, true);
		if (symbol != NULL && mc_symbol_variable == symbol->kind) {
			mc_variable_s* const variable = (mc_variable_s*)symbol;
			target->note.variable = variable;
			target->note.type = target_type = variable->type;
		}
		else {
			target_type = (symbol != NULL) ? mc_symbol_value(c, symbol, target, context, NULL) : error;
			target->note.type = target_type;
		}
	}
	else {
		target_type = mc_expression(c, target, context, NULL);
	}
	mc_place_s place = mc_place_of(c, target, context);
	// the first assignment of 'let a: T;' initializes it, even when it is not 'mut'.
	const bool initializing = mc_operator_assign == node->as.assign.operator_ && mc_node_name == target->kind && place.valid
		&& place.variable != NULL && mc_set_contains(&context->unassigned, place.variable) && mc_variable_let == place.variable->kind
		&& place.variable->loops == context->loops;
	if (target_type != error && !initializing) {
		if (!place.valid) {
			const mc_view_s text = mc_node_text(c, target);
			mc_label_main(mc_error(c->diag, mc_E0508, target->span, "cannot assign to `" mc_view_fmt "`", mc_view_arg(text)),
				"not a variable, field or element");
		}
		else if (!place.mutable_) {
			mc_report_immutable(c, &place, target, "assign to");
		}
		else if (place.needs_root && place.variable != NULL) {
			place.variable->mutated = true;
		}
	}
	if (mc_operator_assign == node->as.assign.operator_) {
		const mc_type_s* const actual = mc_expression(c, node->as.assign.value, context, target_type);
		mc_coerce(c, node->as.assign.value, actual, target_type);
		if (mc_node_name == target->kind && place.valid && place.variable != NULL) mc_set_discard(&context->unassigned, place.variable);
		mc_place_free(&place);
		return;
	}
	mc_place_free(&place);
	const mc_operator_e operator_ = node->as.assign.operator_;
	const mc_type_s* const right = mc_expression(c, node->as.assign.value, context, NULL);
	const mc_type_s* const result = mc_binary_operation(c, node, operator_, target, target_type, node->as.assign.value, right, context, node->span);
	if (result != error && target_type != error && result != target_type) {
		mc_label_main(mc_error(c->diag, mc_E0502, node->span, "`%s` gives `%s`, which cannot be stored in `%s`", mc_operator_text(operator_),
			mc_name_of(c, result), mc_name_of(c, target_type)), "mismatched types");
	}
}

static void _mc_condition(mc_checker_s* const c, mc_node_s* const node, mc_context_s* const context) {
	const mc_type_s* const boolean = mc_scalar_type(c, mc_scalar_bool);
	mc_coerce(c, node, mc_expression(c, node, context, boolean), boolean);
}

static void _mc_if(mc_checker_s* const c, mc_node_s* const node, mc_context_s* const context) {
	_mc_condition(c, node->as.if_.condition, context);
	mc_variable_set_s before = {0}, after_then = {0}, after_else = {0};
	mc_set_assign(&before, &context->unassigned);
	mc_block(c, node->as.if_.then, context, true);
	mc_set_assign(&after_then, &context->unassigned);
	mc_set_assign(&context->unassigned, &before);
	if (node->as.if_.otherwise != NULL) {
		if (mc_node_if == node->as.if_.otherwise->kind) _mc_if(c, node->as.if_.otherwise, context);
		else mc_block(c, node->as.if_.otherwise, context, true);
		mc_set_assign(&after_else, &context->unassigned);
	}
	else {
		mc_set_assign(&after_else, &before);
	}
	const bool then_ends = mc_terminates(node->as.if_.then);
	const bool else_ends = node->as.if_.otherwise != NULL && mc_terminates(node->as.if_.otherwise);
	if (then_ends && !else_ends) {
		mc_set_assign(&context->unassigned, &after_else);
	}
	else if (else_ends && !then_ends) {
		mc_set_assign(&context->unassigned, &after_then);
	}
	else {
		mc_set_assign(&context->unassigned, &after_then);
		mc_set_union(&context->unassigned, &after_else);
	}
	mc_vector_free(&before);
	mc_vector_free(&after_then);
	mc_vector_free(&after_else);
}

static const mc_type_s* _mc_common_integer(mc_checker_s* const c, mc_node_s* const left_node, const mc_type_s* left,
										   mc_node_s* const right_node, const mc_type_s* right) {
	const mc_type_s* const error = mc_special_type(c, mc_special_error);
	if (left == error || right == error) return error;
	if (mc_type_is_untyped(left) && mc_type_is_untyped(right)) {
		left = mc_settle(c, left_node, left);
		right = mc_settle(c, right_node, right);
	}
	else if (mc_type_is_untyped(left)) {
		mc_coerce(c, left_node, left, right);
		left = right;
	}
	else if (mc_type_is_untyped(right)) {
		mc_coerce(c, right_node, right, left);
		right = left;
	}
	if (!(mc_type_scalar == left->kind && 'i' == mc_scalar_kind(left->as.scalar))) {
		mc_label_main(mc_error(c->diag, mc_E0502, left_node->span, "a range needs integers"), "found `%s`", mc_name_of(c, left));
		return error;
	}
	if (left != right) {
		mc_label(mc_label_main(mc_error(c->diag, mc_E0502, right_node->span, "both ends of a range need the same type"),
			"expected `%s`, found `%s`", mc_name_of(c, left), mc_name_of(c, right)), left_node->span, "this is `%s`", mc_name_of(c, left));
		return error;
	}
	return left;
}

static void _mc_loop_body(mc_checker_s* const c, mc_node_s* const body, mc_context_s* const context) {
	mc_variable_set_s before = {0};
	mc_set_assign(&before, &context->unassigned);
	context->loops += 1;
	mc_block(c, body, context, true);
	context->loops -= 1;
	mc_set_union(&context->unassigned, &before);
	mc_vector_free(&before);
}

static void _mc_for(mc_checker_s* const c, mc_node_s* const node, mc_context_s* const context) {
	const mc_type_s* const start = mc_expression(c, node->as.for_.start, context, NULL);
	const mc_type_s* const end = mc_expression(c, node->as.for_.end, context, NULL);
	const mc_type_s* const type = _mc_common_integer(c, node->as.for_.start, start, node->as.for_.end, end);
	mc_context_push_scope(c, context);
	mc_variable_s* const variable = mc_arena_new(c->arena, mc_variable_s);
	variable->base.kind = mc_symbol_variable;
	variable->name = node->as.for_.name;
	variable->type = type;
	variable->kind = mc_variable_for;
	variable->span = node->as.for_.name_span;
	variable->level = context->level;
	node->note.variable = variable;
	(void)mc_context_declare(context, node->as.for_.name, &variable->base);
	_mc_loop_body(c, node->as.for_.body, context);
	mc_context_pop_scope(context);
}

static void _mc_return(mc_checker_s* const c, mc_node_s* const node, mc_context_s* const context) {
	mc_node_s* const value = node->as.return_.value;
	if (NULL == context->result) {
		if (value != NULL) {
			const mc_type_s* const actual = mc_expression(c, value, context, NULL);
			if (!mc_type_is_special(actual, mc_special_nothing) && !mc_type_is_special(actual, mc_special_error)) {
				mc_help(mc_label_main(mc_error(c->diag, mc_E0524, value->span, "this function returns nothing"), "remove the value"),
					"declare a result type with `-> type`");
			}
		}
		return;
	}
	if (NULL == value) {
		mc_label_main(mc_error(c->diag, mc_E0524, node->span, "this function must return `%s`", mc_name_of(c, context->result)), "missing value");
		return;
	}
	mc_coerce(c, value, mc_expression(c, value, context, context->result), context->result);
}

static void _mc_expression_statement(mc_checker_s* const c, mc_node_s* const node, mc_context_s* const context) {
	mc_node_s* const expression = node->as.expression_statement.expression;
	const mc_type_s* type = mc_expression(c, expression, context, NULL);
	if (mc_type_is_untyped(type)) type = mc_settle(c, expression, type);
	if (!mc_type_is_special(type, mc_special_nothing) && !mc_type_is_special(type, mc_special_error)) {
		const mc_view_s text = mc_node_text(c, expression);
		mc_help(mc_label_main(mc_error(c->diag, mc_E0509, expression->span, "the result of this expression is not used"),
			"a `%s` is discarded", mc_name_of(c, type)), "write `_ = " mc_view_fmt ";` to discard it on purpose", mc_view_arg(text));
	}
}

/// what the checker calls the statement in messages about its attributes.
static const char* _mc_statement_kind(const mc_node_s* const node) {
	switch (node->kind) {
		case mc_node_block:    return "block";
		case mc_node_if:       return "`if`";
		case mc_node_while:    return "`while` loop";
		case mc_node_for:      return "`for` loop";
		case mc_node_return:   return "`return`";
		case mc_node_break:    return "`break`";
		case mc_node_continue: return "`continue`";
		default:               return "statement";
	}
}

static void _mc_statement_kinds(mc_checker_s* const c, mc_node_s* const node, mc_context_s* const context);

/// a statement: its attributes (but a let's, which it checks itself), then
/// the statement; '[wrap]' covers everything in it but nested functions.
static void _mc_statement(mc_checker_s* const c, mc_node_s* const node, mc_context_s* const context) {
	if (mc_node_let == node->kind || mc_node_declaration_statement == node->kind) {
		_mc_statement_kinds(c, node, context);
		return;
	}
	const mc_attribute_set_s found = mc_attributes(c, mc_statement_attributes(node), _mc_statement_kind(node));
	if (found.wrap != NULL) context->wrapping += 1;
	_mc_statement_kinds(c, node, context);
	if (found.wrap != NULL) context->wrapping -= 1;
}

static void _mc_statement_kinds(mc_checker_s* const c, mc_node_s* const node, mc_context_s* const context) {
	switch (node->kind) {
		case mc_node_declaration_statement: _mc_declaration_statement(c, node, context); return;
		case mc_node_let:                   _mc_let(c, node, context); return;
		case mc_node_assign:                _mc_assign(c, node, context); return;
		case mc_node_if: _mc_if(c, node, context); return;
		case mc_node_while:
			_mc_condition(c, node->as.while_.condition, context);
			_mc_loop_body(c, node->as.while_.body, context);
			return;
		case mc_node_for:    _mc_for(c, node, context); return;
		case mc_node_return: _mc_return(c, node, context); return;
		case mc_node_break:
			if (0 == context->loops) mc_label_main(mc_error(c->diag, mc_E0515, node->span, "`break` outside of a loop"), "not inside `while` or `for`");
			return;
		case mc_node_continue:
			if (0 == context->loops) mc_label_main(mc_error(c->diag, mc_E0515, node->span, "`continue` outside of a loop"), "not inside `while` or `for`");
			return;
		case mc_node_block: mc_block(c, node, context, true); return;
		case mc_node_expression_statement: _mc_expression_statement(c, node, context); return;
		default: mc_unreachable();
	}
}

static bool _mc_breaks(const mc_node_s* const node) {
	switch (node->kind) {
		case mc_node_break: return true;
		case mc_node_while: case mc_node_for: case mc_node_lambda: return false;
		case mc_node_block:
			for (size_t i = 0; i < node->as.block.statements.count; ++i) if (_mc_breaks(node->as.block.statements.data[i])) return true;
			return false;
		case mc_node_if: return _mc_breaks(node->as.if_.then) || (node->as.if_.otherwise != NULL && _mc_breaks(node->as.if_.otherwise));
		default: return false;
	}
}

bool mc_terminates(const mc_node_s* const node) {
	if (NULL == node) return false;
	switch (node->kind) {
		case mc_node_return: return true;
		case mc_node_block:
			for (size_t i = 0; i < node->as.block.statements.count; ++i) if (mc_terminates(node->as.block.statements.data[i])) return true;
			return false;
		case mc_node_if: return node->as.if_.otherwise != NULL && mc_terminates(node->as.if_.then) && mc_terminates(node->as.if_.otherwise);
		case mc_node_while:
			return mc_node_bool == node->as.while_.condition->kind && node->as.while_.condition->as.bool_.value && !_mc_breaks(node->as.while_.body);
		default: return false;
	}
}
