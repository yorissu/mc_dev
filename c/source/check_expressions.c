
/// the checker: expressions. calls (with generic inference and reference
/// arguments), operators (built in, or operator functions), literals that
/// take their type from where they are used, casts, lambdas and intrinsics.

#include "check.h"
#include "mc/format.h"

#include <stdio.h>
#include <string.h>

#define _mc_error(_c) mc_special_type((_c), mc_special_error)
#define _mc_bool(_c) mc_scalar_type((_c), mc_scalar_bool)

static bool _mc_is_error(const mc_type_s* const type) {
	return mc_type_is_special(type, mc_special_error);
}

/// whether an untyped literal of type 'actual' can become a 'target'.
static bool _mc_accepts_literal(const mc_type_s* const target, const mc_type_s* const actual) {
	if (NULL == target || target->kind != mc_type_scalar) return false;
	if (mc_type_is_special(actual, mc_special_untyped_integer)) return mc_type_is_numeric(target);
	if (mc_type_is_special(actual, mc_special_untyped_float)) return mc_type_is_float(target);
	return false;
}

static const mc_type_s* _mc_literal_default(mc_checker_s* const c, const mc_type_s* const type) {
	if (mc_type_is_special(type, mc_special_untyped_integer)) return mc_scalar_type(c, mc_scalar_int);
	if (mc_type_is_special(type, mc_special_untyped_float)) return mc_scalar_type(c, mc_scalar_float);
	return type;
}

static bool _mc_is_arithmetic(const mc_operator_e operator_) {
	return operator_ >= mc_operator_add && operator_ <= mc_operator_remainder;
}

static bool _mc_is_bitwise(const mc_operator_e operator_) {
	return operator_ >= mc_operator_bit_and && operator_ <= mc_operator_bit_xor;
}

static bool _mc_is_shift(const mc_operator_e operator_) {
	return mc_operator_shift_left == operator_ || mc_operator_shift_right == operator_;
}

static bool _mc_is_equality(const mc_operator_e operator_) {
	return mc_operator_equal == operator_ || mc_operator_not_equal == operator_;
}

static bool _mc_is_ordering(const mc_operator_e operator_) {
	return operator_ >= mc_operator_less && operator_ <= mc_operator_greater_equal;
}

/// whether the type of 'node' comes from where it is used (literals, '{...}', null).
static bool _mc_context_typed(const mc_node_s* const node) {
	switch (node->kind) {
		case mc_node_integer: return mc_scalar_none_ == node->as.integer.suffix;
		case mc_node_float:   return mc_scalar_none_ == node->as.float_.suffix;
		case mc_node_null:    return true;
		case mc_node_composite: return NULL == node->as.composite.type;
		case mc_node_paren:   return _mc_context_typed(node->as.paren.inner);
		case mc_node_unary:
			return (mc_operator_negate == node->as.unary.operator_ || mc_operator_bit_not == node->as.unary.operator_)
				&& _mc_context_typed(node->as.unary.operand);
		case mc_node_binary: {
			const mc_operator_e operator_ = node->as.binary.operator_;
			return (_mc_is_arithmetic(operator_) || _mc_is_bitwise(operator_) || _mc_is_shift(operator_))
				&& _mc_context_typed(node->as.binary.left) && _mc_context_typed(node->as.binary.right);
		}
		default: return false;
	}
}

/// 'i' for integer literals, 'f' for float literals, 0 otherwise.
static char _mc_untyped_kind(const mc_node_s* const node) {
	switch (node->kind) {
		case mc_node_integer: return mc_scalar_none_ == node->as.integer.suffix ? 'i' : 0;
		case mc_node_float:   return mc_scalar_none_ == node->as.float_.suffix ? 'f' : 0;
		case mc_node_paren:   return _mc_untyped_kind(node->as.paren.inner);
		case mc_node_unary:   return _mc_untyped_kind(node->as.unary.operand);
		case mc_node_binary: {
			const char left = _mc_untyped_kind(node->as.binary.left);
			const char right = _mc_untyped_kind(node->as.binary.right);
			if (0 == left || 0 == right) return 0;
			return ('f' == left || 'f' == right) ? 'f' : 'i';
		}
		default: return 0;
	}
}

static const mc_type_s* _mc_checked(mc_checker_s* const c, mc_node_s* const node, mc_context_s* const context, const mc_type_s* const expected) {
	if (node->note.prechecked) return node->note.type;
	return mc_expression(c, node, context, expected);
}

/// checks the arguments of a call that failed, to find their own errors.
static void _mc_loose(mc_checker_s* const c, const mc_nodes_s* const nodes, const size_t from, mc_context_s* const context) {
	for (size_t i = from; i < nodes->count; ++i) {
		mc_node_s* const operand = (mc_node_reference == nodes->data[i]->kind) ? nodes->data[i]->as.reference.operand : nodes->data[i];
		if (operand->note.prechecked) continue;
		const mc_type_s* const type = mc_expression(c, operand, context, NULL);
		if (mc_type_is_untyped(type)) (void)mc_settle(c, operand, type);
	}
}

static void _mc_loose_one(mc_checker_s* const c, mc_node_s* const node, mc_context_s* const context) {
	mc_node_s* holder[1] = { node };
	const mc_nodes_s single = { .data = holder, .count = 1, .capacity = 1, };
	_mc_loose(c, &single, 0, context);
}

// --- literals and names --------------------------------------------------------------

static const mc_type_s* _mc_integer(mc_checker_s* const c, mc_node_s* const node) {
	if (mc_scalar_none_ == node->as.integer.suffix) return mc_special_type(c, mc_special_untyped_integer);
	const mc_type_s* const type = mc_scalar_type(c, node->as.integer.suffix);
	const __int128 value = node->note.negative ? -(__int128)node->as.integer.value : (__int128)node->as.integer.value;
	if (value < mc_type_minimum(type) || value > mc_type_maximum(type)) {
		mc_string_s text = {0}, low = {0}, high = {0};
		mc_format_int128(&text, value);
		mc_format_int128(&low, mc_type_minimum(type));
		mc_format_int128(&high, mc_type_maximum(type));
		mc_label_main(mc_error(c->diag, mc_E0511, node->span, "`%s` does not fit in `%s`", mc_string_cstr(&text), mc_name_of(c, type)),
			"`%s` holds %s to %s", mc_name_of(c, type), mc_string_cstr(&low), mc_string_cstr(&high));
		mc_string_free(&text);
		mc_string_free(&low);
		mc_string_free(&high);
	}
	return type;
}

const mc_type_s* mc_symbol_value(mc_checker_s* const c, mc_symbol_s* const symbol, mc_node_s* const node, mc_context_s* const context,
								 const mc_type_s* const expected) {
	if (NULL == symbol) return _mc_error(c);
	switch (symbol->kind) {
		case mc_symbol_variable: {
			mc_variable_s* const variable = (mc_variable_s*)symbol;
			node->note.variable = variable;
			if (mc_set_contains(&context->unassigned, variable)) {
				mc_set_discard(&context->unassigned, variable);
				mc_help(mc_label(mc_label_main(mc_warning(c->diag, mc_W0502, node->span, "`" mc_view_fmt "` may be read before it is assigned",
					mc_view_arg(variable->name)), "read here"), variable->span, "declared without a value"),
					"assign it first, or mark the declaration `[uninit]`");
			}
			return variable->type;
		}
		case mc_symbol_constant:
			node->note.constant = (mc_constant_s*)symbol;
			return mc_constant_type(c, (mc_constant_s*)symbol);
		case mc_symbol_function: {
			mc_function_s* const info = (mc_function_s*)symbol;
			const mc_nodes_s* const generics = &info->node->as.function.generics;
			if (generics->count > 0) {
				if (expected != NULL && mc_type_function == expected->kind) {
					mc_type_list_s parameters = {0};
					const mc_type_s* result = NULL;
					mc_signature(c, info, &parameters, &result);
					mc_bindings_s bindings = {0};
					const mc_type_s* const pattern = mc_type_function_of(c->types, parameters.data, parameters.count, result);
					bool complete = mc_type_unify(pattern, expected, &bindings);
					mc_vector_of(const mc_type_s*) arguments = {0};
					for (size_t i = 0; i < generics->count && complete; ++i) {
						const mc_type_s* const bound = mc_bindings_get(&bindings, generics->data[i]->as.generic.name);
						if (NULL == bound) complete = false;
						else mc_vector_push(&arguments, bound);
					}
					mc_instance_s* instance = NULL;
					if (complete) instance = mc_instance(c, info, arguments.data, arguments.count, context, node->span);
					mc_vector_free(&arguments);
					mc_vector_free(&bindings);
					mc_vector_free(&parameters);
					if (complete) {
						if (NULL == instance) return _mc_error(c);
						node->note.instance = instance;
						return mc_instance_type(c->program, instance);
					}
				}
				mc_string_s names = {0};
				for (size_t i = 0; i < generics->count; ++i) {
					if (i > 0) mc_string_append_cstr(&names, ", ");
					mc_string_append(&names, generics->data[i]->as.generic.name);
				}
				mc_help(mc_label_main(mc_error(c->diag, mc_E0506, node->span, "generic function `" mc_view_fmt "` needs type arguments here",
					mc_view_arg(info->node->as.function.name)), "cannot infer them"), "write `" mc_view_fmt "<%s>`",
					mc_view_arg(info->node->as.function.name), mc_string_cstr(&names));
				mc_string_free(&names);
				return _mc_error(c);
			}
			mc_instance_s* const instance = mc_instance(c, info, NULL, 0, context, node->span);
			if (NULL == instance) return _mc_error(c);
			for (size_t i = 0; i < instance->parameters.count; ++i) {
				if (mc_type_is_special(instance->parameters.data[i], mc_special_variadic)) {
					mc_label_main(mc_error(c->diag, mc_E0529, node->span, "variadic function `" mc_view_fmt "` can only be called",
						mc_view_arg(info->node->as.function.name)), "not a value");
					return _mc_error(c);
				}
			}
			node->note.instance = instance;
			return mc_instance_type(c->program, instance);
		}
		case mc_symbol_struct: {
			const mc_struct_s* const info = (const mc_struct_s*)symbol;
			mc_help(mc_label_main(mc_error(c->diag, mc_E0529, node->span, "`" mc_view_fmt "` is a type, not a value", mc_view_arg(info->node->as.struct_.name)),
				"expected a value"), "create a value with a literal: `(" mc_view_fmt "){ ... }`", mc_view_arg(info->node->as.struct_.name));
			return _mc_error(c);
		}
		default: {
			const mc_view_s text = mc_node_text(c, node);
			mc_label_main(mc_error(c->diag, mc_E0529, node->span, "`" mc_view_fmt "` is a module, not a value", mc_view_arg(text)), "expected a value");
			return _mc_error(c);
		}
	}
}

const mc_type_s* mc_instance_type(mc_program_s* const program, const mc_instance_s* const instance) {
	return mc_type_function_of(&program->types, instance->parameters.data, instance->parameters.count, instance->result);
}

static const mc_type_s* _mc_name(mc_checker_s* const c, mc_node_s* const node, mc_context_s* const context, const mc_type_s* const expected) {
	if (mc_view_eq_cstr(node->as.name.name, "_")) {
		mc_label_main(mc_error(c->diag, mc_E0501, node->span, "`_` is not a value"), "only `_ = value;` discards");
		return _mc_error(c);
	}
	return mc_symbol_value(c, mc_lookup(c, node->as.name.name, context, node->span, true), node, context, expected);
}

static bool _mc_is_module_like(const mc_symbol_s* const symbol) {
	return symbol != NULL && (mc_symbol_module == symbol->kind || mc_symbol_prefix == symbol->kind);
}

static const mc_type_s* _mc_member(mc_checker_s* const c, mc_node_s* const node, mc_context_s* const context, const mc_type_s* const expected) {
	mc_node_s* const target_node = node->as.member.target;
	if (mc_node_name == target_node->kind || mc_node_member == target_node->kind) {
		mc_symbol_s* const base = mc_qualified(c, target_node, context, false);
		if (_mc_is_module_like(base)) {
			mc_symbol_s* const symbol = mc_member_of(c, base, node->as.member.name, node->as.member.name_span, context);
			return mc_symbol_value(c, symbol, node, context, expected);
		}
	}
	const mc_type_s* const target = mc_expression(c, target_node, context, NULL);
	if (_mc_is_error(target)) return target;
	if (mc_type_struct == target->kind) {
		mc_layout_s* const layout = mc_layout(c, target);
		mc_field_s* const field = mc_layout_field(layout, node->as.member.name);
		if (NULL == field) {
			mc_string_s fields = {0};
			for (size_t i = 0; i < layout->fields.count; ++i) {
				if (i > 0) mc_string_append_cstr(&fields, ", ");
				mc_string_append(&fields, layout->fields.data[i]->name);
			}
			mc_note(mc_label_main(mc_error(c->diag, mc_E0512, node->as.member.name_span, "`%s` has no field `" mc_view_fmt "`",
				mc_name_of(c, target), mc_view_arg(node->as.member.name)), "unknown field"), "fields: %s", fields.length > 0 ? mc_string_cstr(&fields) : "none");
			mc_string_free(&fields);
			return _mc_error(c);
		}
		const mc_struct_s* const info = target->as.struct_.info;
		if (!info->node->as.struct_.public_ && info->module != context->module) {
			mc_note(mc_label_main(mc_error(c->diag, mc_E0522, node->as.member.name_span, "field `" mc_view_fmt "` of `%s` is private",
				mc_view_arg(node->as.member.name), mc_name_of(c, target)), "not accessible here"),
				"`" mc_view_fmt "` is not `pub` in `%s`, so its fields are private", mc_view_arg(info->node->as.struct_.name), info->module->name);
		}
		node->note.field = field;
		return field->type;
	}
	mc_label_main(mc_error(c->diag, mc_E0512, node->as.member.name_span, "`%s` has no fields", mc_name_of(c, target)),
		"`." mc_view_fmt "` on a `%s`", mc_view_arg(node->as.member.name), mc_name_of(c, target));
	return _mc_error(c);
}

static const char* _mc_generics_list(mc_checker_s* const c, const mc_function_s* const info) {
	mc_string_s names = {0};
	const mc_nodes_s* const generics = &info->node->as.function.generics;
	for (size_t i = 0; i < generics->count; ++i) {
		if (i > 0) mc_string_append_cstr(&names, ", ");
		mc_string_append(&names, generics->data[i]->as.generic.name);
	}
	const char* const kept = mc_arena_strndup(c->arena, mc_string_cstr(&names), names.length);
	mc_string_free(&names);
	return kept;
}

static const mc_type_s* _mc_specialize(mc_checker_s* const c, mc_node_s* const node, mc_context_s* const context) {
	mc_symbol_s* const symbol = mc_qualified(c, node->as.specialize.target, context, true);
	if (NULL == symbol) return _mc_error(c);
	mc_function_s* const info = (mc_symbol_function == symbol->kind) ? (mc_function_s*)symbol : NULL;
	if (NULL == info || !mc_function_is_generic(info)) {
		const mc_view_s text = mc_node_text(c, node->as.specialize.target);
		mc_label_main(mc_error(c->diag, mc_E0521, node->span, "`" mc_view_fmt "` is not a generic function", mc_view_arg(text)), "remove the type arguments");
		return _mc_error(c);
	}
	mc_vector_of(const mc_type_s*) arguments = {0};
	bool failed = false;
	for (size_t i = 0; i < node->as.specialize.type_arguments.count; ++i) {
		const mc_type_s* const argument = mc_resolve_type(c, node->as.specialize.type_arguments.data[i], context, false, false);
		failed = failed || _mc_is_error(argument);
		mc_vector_push(&arguments, argument);
	}
	const size_t expected_count = info->node->as.function.generics.count;
	const mc_type_s* result = _mc_error(c);
	if (arguments.count != expected_count) {
		mc_label_main(mc_error(c->diag, mc_E0520, node->span, "`" mc_view_fmt "` takes %zu type argument(s), found %zu",
			mc_view_arg(info->node->as.function.name), expected_count, arguments.count), "wrong count");
	}
	else if (!failed) {
		mc_instance_s* const instance = mc_instance(c, info, arguments.data, arguments.count, context, node->span);
		if (instance != NULL) {
			node->note.instance = instance;
			result = mc_instance_type(c->program, instance);
		}
	}
	mc_vector_free(&arguments);
	return result;
}

// --- calls ---------------------------------------------------------------------------

static mc_instance_s* _mc_call_instance(mc_checker_s* const c, mc_function_s* const info, mc_node_s* const node, mc_context_s* const context,
										const mc_type_s* const expected) {
	const mc_nodes_s* const generics = &info->node->as.function.generics;
	const mc_nodes_s* const arguments = &node->as.call.arguments;
	if (node->as.call.has_type_arguments) {
		if (0 == generics->count) {
			mc_label_main(mc_error(c->diag, mc_E0521, node->as.call.callee->span, "`" mc_view_fmt "` is not generic", mc_view_arg(info->node->as.function.name)),
				"remove the type arguments");
			return NULL;
		}
		mc_vector_of(const mc_type_s*) types = {0};
		bool failed = false;
		for (size_t i = 0; i < node->as.call.type_arguments.count; ++i) {
			const mc_type_s* const type = mc_resolve_type(c, node->as.call.type_arguments.data[i], context, false, false);
			failed = failed || _mc_is_error(type);
			mc_vector_push(&types, type);
		}
		mc_instance_s* instance = NULL;
		if (types.count != generics->count) {
			mc_label_main(mc_error(c->diag, mc_E0520, node->as.call.callee->span, "`" mc_view_fmt "` takes %zu type argument(s), found %zu",
				mc_view_arg(info->node->as.function.name), generics->count, types.count),
				"expected `" mc_view_fmt "<%s>`", mc_view_arg(info->node->as.function.name), _mc_generics_list(c, info));
		}
		else if (!failed) {
			instance = mc_instance(c, info, types.data, types.count, context, node->span);
		}
		mc_vector_free(&types);
		return instance;
	}
	if (0 == generics->count) return mc_instance(c, info, NULL, 0, context, node->span);
	mc_type_list_s parameters = {0};
	const mc_type_s* result = NULL;
	mc_signature(c, info, &parameters, &result);
	mc_bindings_s bindings = {0};
	mc_instance_s* instance = NULL;
	bool failed = false;
	for (size_t i = 0; i < arguments->count && i < parameters.count && !failed; ++i) {
		mc_node_s* const argument = arguments->data[i];
		mc_node_s* const operand = (mc_node_reference == argument->kind) ? argument->as.reference.operand : argument;
		const mc_type_s* const parameter = parameters.data[i];
		const mc_type_s* const pattern = (mc_type_reference == parameter->kind) ? parameter->as.reference.element : parameter;
		if (!mc_type_contains_parameter(pattern) || _mc_context_typed(operand)) continue;
		if (mc_node_reference == argument->kind && argument->as.reference.mutable_ && mc_node_name == operand->kind) {
			mc_symbol_s* const variable = mc_lookup(c, operand->as.name.name, context, operand->span, false);
			if (variable != NULL && mc_symbol_variable == variable->kind) mc_set_discard(&context->unassigned, (mc_variable_s*)variable);
		}
		const mc_type_s* const actual = mc_expression(c, operand, context, NULL);
		operand->note.prechecked = true;
		if (_mc_is_error(actual) || mc_type_is_untyped(actual) || mc_type_is_special(actual, mc_special_null)) continue;
		if (!mc_type_unify(pattern, actual, &bindings)) {
			mc_label_main(mc_error(c->diag, mc_E0502, operand->span, "mismatched types in the call of `" mc_view_fmt "`", mc_view_arg(info->node->as.function.name)),
				"expected `%s`, found `%s`", mc_name_of(c, mc_type_substitute(c->types, pattern, &bindings)), mc_name_of(c, actual));
			failed = true;
		}
	}
	if (!failed) {
		if (expected != NULL && !_mc_is_error(expected) && result != NULL && mc_type_contains_parameter(result)) {
			mc_bindings_s trial = {0};
			for (size_t i = 0; i < bindings.count; ++i) mc_vector_push(&trial, bindings.data[i]);
			if (mc_type_unify(result, expected, &trial)) {
				mc_vector_free(&bindings);
				bindings = trial;
			}
			else {
				mc_vector_free(&trial);
			}
		}
		for (size_t i = 0; i < arguments->count && i < parameters.count; ++i) {
			mc_node_s* const argument = arguments->data[i];
			mc_node_s* const operand = (mc_node_reference == argument->kind) ? argument->as.reference.operand : argument;
			const mc_type_s* const parameter = parameters.data[i];
			const mc_type_s* const pattern = (mc_type_reference == parameter->kind) ? parameter->as.reference.element : parameter;
			if (pattern->kind != mc_type_parameter || mc_bindings_get(&bindings, pattern->as.parameter.name) != NULL) continue;
			char kind = 0;
			if (_mc_context_typed(operand)) kind = _mc_untyped_kind(operand);
			else if (mc_type_is_special(operand->note.type, mc_special_untyped_integer)) kind = 'i';
			else if (mc_type_is_special(operand->note.type, mc_special_untyped_float)) kind = 'f';
			if ('i' == kind) mc_bindings_set(&bindings, pattern->as.parameter.name, mc_scalar_type(c, mc_scalar_int));
			else if ('f' == kind) mc_bindings_set(&bindings, pattern->as.parameter.name, mc_scalar_type(c, mc_scalar_float));
		}
		mc_vector_of(const mc_type_s*) types = {0};
		const mc_node_s* missing = NULL;
		for (size_t i = 0; i < generics->count; ++i) {
			const mc_type_s* const bound = mc_bindings_get(&bindings, generics->data[i]->as.generic.name);
			if (NULL == bound) {
				if (NULL == missing) missing = generics->data[i];
			}
			else {
				mc_vector_push(&types, bound);
			}
		}
		if (missing != NULL) {
			mc_help(mc_label_main(mc_error(c->diag, mc_E0506, node->span, "cannot infer the type argument `" mc_view_fmt "` of `" mc_view_fmt "`",
				mc_view_arg(missing->as.generic.name), mc_view_arg(info->node->as.function.name)), "not determined by the arguments"),
				"write it: `" mc_view_fmt "<%s>(...)`", mc_view_arg(info->node->as.function.name), _mc_generics_list(c, info));
		}
		else {
			instance = mc_instance(c, info, types.data, types.count, context, node->span);
		}
		mc_vector_free(&types);
	}
	mc_vector_free(&bindings);
	mc_vector_free(&parameters);
	return instance;
}

typedef struct {
	mc_node_s* argument;
	mc_place_s place   ;
} _mc_reference_s;

static bool _mc_overlaps(const mc_place_s* const first, const mc_place_s* const second) {
	if (0 == first->path.count || 0 == second->path.count || first->path.data[0] != second->path.data[0]) return false;
	for (size_t i = 1; i < first->path.count && i < second->path.count; ++i) {
		const char* const left = first->path.data[i];
		const char* const right = second->path.data[i];
		if (0 == strcmp(left, "*") || 0 == strcmp(right, "*")) continue;
		if (0 != strcmp(left, right)) return false;
	}
	return true;
}

/// the number of '{}' in a format string; false (with 'problem') when it is invalid.
static bool _mc_format_placeholders(const mc_view_s data, size_t* const count, const char** const problem) {
	*count = 0;
	for (size_t index = 0; index < data.length;) {
		const char character = data.data[index];
		const char following = (index + 1 < data.length) ? data.data[index + 1] : '\0';
		const bool has_following = index + 1 < data.length;
		if ('{' == character) {
			if (has_following && '{' == following) { index += 2; continue; }
			if (has_following && '}' == following) { *count += 1; index += 2; continue; }
			*problem = "`{` must be followed by `}` (format options are not supported yet)";
			return false;
		}
		if ('}' == character) {
			if (has_following && '}' == following) { index += 2; continue; }
			*problem = "a single `}` must be written `}}`";
			return false;
		}
		index += 1;
	}
	return true;
}

static void _mc_cvalue_bytes(const mc_cvalue_s* const value, mc_string_s* const out) {
	if (mc_cvalue_list == value->kind) {
		for (size_t i = 0; i < value->as.list->count; ++i) mc_string_append_char(out, (char)value->as.list->items[i].as.integer);
	}
	else if (mc_cvalue_slice == value->kind && value->as.slice.list != NULL) {
		for (int64_t i = 0; i < value->as.slice.length; ++i) mc_string_append_char(out, (char)value->as.slice.list->items[value->as.slice.start + i].as.integer);
	}
}

/// checks the values given for '...'; when the parameter before it is a
/// '[]u8', it is a format string whose '{}' must match the values.
static void _mc_variadic_arguments(mc_checker_s* const c, mc_node_s* const node, const mc_type_s* const* const parameters, const size_t count,
								   mc_context_s* const context) {
	const mc_nodes_s* const arguments = &node->as.call.arguments;
	const mc_type_s* const u8_slice = mc_type_slice_of(c->types, mc_scalar_type(c, mc_scalar_u8));
	mc_node_s* format = NULL;
	if (count > 0 && parameters[count - 1] == u8_slice && arguments->count >= count) format = arguments->data[count - 1];
	mc_type_list_s* const types = mc_arena_new(c->arena, mc_type_list_s);
	for (size_t i = count; i < arguments->count; ++i) {
		mc_node_s* const argument = arguments->data[i];
		if (mc_node_reference == argument->kind) {
			mc_label_main(mc_error(c->diag, mc_E0519, argument->span, "values for `...` are passed by value"), "remove the `&`");
			_mc_loose_one(c, argument, context);
			mc_arena_push(c->arena, types, _mc_error(c));
			continue;
		}
		mc_arena_push(c->arena, types, mc_settle(c, argument, mc_expression(c, argument, context, NULL)));
	}
	node->note.variadic = types;
	if (NULL == format || mc_node_reference == format->kind) return;
	const mc_type_s* const actual = mc_expression(c, format, context, u8_slice);
	if (mc_type_array == actual->kind && actual->as.array.element == mc_scalar_type(c, mc_scalar_u8)) {
		format->note.format = true;
	}
	else if (!mc_coerce(c, format, actual, u8_slice)) {
		format->note.format = true;
		return;
	}
	format->note.format = true;
	mc_cvalue_s value = {0};
	if (!mc_require_constant(c, format, context, "the format string", &value)) return;
	mc_string_s data = {0};
	_mc_cvalue_bytes(&value, &data);
	size_t placeholders = 0;
	const char* problem = NULL;
	if (!_mc_format_placeholders(mc_string_view(&data), &placeholders, &problem)) {
		mc_help(mc_label_main(mc_error(c->diag, mc_E0530, format->span, "invalid format string"), "%s", problem),
			"`{}` is replaced by the next value; write `{{` and `}}` for braces");
	}
	else if (placeholders != types->count) {
		mc_label(mc_label_main(mc_error(c->diag, mc_E0530, node->span, "the format string has %zu placeholder(s), but %zu value(s) are given",
			placeholders, types->count), "they must match"), format->span, "%zu `{}`", placeholders);
	}
	mc_string_free(&data);
}

static void _mc_arguments(mc_checker_s* const c, mc_node_s* const node, const mc_type_s* const* parameters, size_t count,
						  const mc_nodes_s* parameter_nodes, mc_context_s* const context, const char* const name) {
	const mc_nodes_s* const all = &node->as.call.arguments;
	size_t argument_count = all->count;
	const bool variadic = count > 0 && mc_type_is_special(parameters[count - 1], mc_special_variadic);
	if (variadic) {
		count -= 1;
		if (all->count < count) {
			mc_label_main(mc_error(c->diag, mc_E0505, node->span, "`%s` takes at least %zu argument(s), found %zu", name, count, all->count),
				"too few arguments");
			_mc_loose(c, all, 0, context);
			return;
		}
		_mc_variadic_arguments(c, node, parameters, count, context);
		argument_count = count;
		parameter_nodes = NULL;
	}
	size_t required = count;
	if (parameter_nodes != NULL) {
		required = 0;
		for (size_t i = 0; i < parameter_nodes->count; ++i) if (NULL == parameter_nodes->data[i]->as.param.default_) required += 1;
	}
	if (argument_count > count || argument_count < required) {
		if (required == count) {
			mc_label_main(mc_error(c->diag, mc_E0505, node->span, "`%s` takes %zu argument(s), found %zu", name, count, argument_count),
				"wrong number of arguments");
		}
		else {
			mc_label_main(mc_error(c->diag, mc_E0505, node->span, "`%s` takes %zu to %zu argument(s), found %zu", name, required, count, argument_count),
				"wrong number of arguments");
		}
		_mc_loose(c, all, 0, context);
		return;
	}
	mc_vector_of(_mc_reference_s) references = {0};
	for (size_t i = 0; i < argument_count; ++i) {
		mc_node_s* const argument = all->data[i];
		const mc_type_s* const parameter = parameters[i];
		if (mc_type_reference == parameter->kind) {
			const char* const prefix = parameter->as.reference.mutable_ ? "mut &" : "&";
			const mc_type_s* const element = parameter->as.reference.element;
			if (argument->kind != mc_node_reference) {
				const mc_type_s* const actual = _mc_checked(c, argument, context, element);
				mc_coerce(c, argument, actual, element);
				const mc_view_s text = mc_node_text(c, argument);
				mc_help(mc_label_main(mc_error(c->diag, mc_E0517, argument->span, "this parameter is a reference"), "expected `%s...`", prefix),
					"write `%s" mc_view_fmt "`", prefix, mc_view_arg(text));
				continue;
			}
			mc_node_s* const operand = argument->as.reference.operand;
			if (argument->as.reference.mutable_ != parameter->as.reference.mutable_) {
				const mc_view_s text = mc_node_text(c, operand);
				if (parameter->as.reference.mutable_) {
					mc_help(mc_label_main(mc_error(c->diag, mc_E0518, argument->span, "this parameter is a writable reference"), "passed read-only"),
						"write `mut &" mc_view_fmt "`", mc_view_arg(text));
				}
				else {
					mc_help(mc_label_main(mc_error(c->diag, mc_E0518, argument->span, "this parameter is a read-only reference"), "passed as writable"),
						"write `&" mc_view_fmt "`", mc_view_arg(text));
				}
			}
			if (argument->as.reference.mutable_ && mc_node_name == operand->kind) {
				mc_symbol_s* const variable = mc_lookup(c, operand->as.name.name, context, operand->span, false);
				if (variable != NULL && mc_symbol_variable == variable->kind) mc_set_discard(&context->unassigned, (mc_variable_s*)variable);
			}
			const mc_type_s* const actual = _mc_checked(c, operand, context, element);
			argument->note.type = parameter;
			mc_coerce(c, operand, actual, element);
			mc_place_s place = mc_place_of(c, operand, context);
			if (argument->as.reference.mutable_ && !_mc_is_error(actual)) {
				if (!place.valid) {
					mc_label_main(mc_error(c->diag, mc_E0508, operand->span, "cannot pass a temporary value as `mut &`"), "not a variable");
				}
				else if (!place.mutable_) {
					mc_report_immutable(c, &place, operand, "pass as `mut &`");
				}
				else if (place.needs_root && place.variable != NULL) {
					place.variable->mutated = true;
				}
			}
			if (place.valid && place.variable != NULL) {
				const _mc_reference_s reference = { .argument = argument, .place = place, };
				mc_vector_push(&references, reference);
			}
			else {
				mc_place_free(&place);
			}
			continue;
		}
		if (mc_node_reference == argument->kind) {
			mc_label_main(mc_error(c->diag, mc_E0519, argument->span, "this parameter is not a reference"), "remove the `&`");
			const mc_type_s* const actual = _mc_checked(c, argument->as.reference.operand, context, parameter);
			mc_coerce(c, argument->as.reference.operand, actual, parameter);
			continue;
		}
		if (argument->note.format) continue;
		mc_coerce(c, argument, _mc_checked(c, argument, context, parameter), parameter);
	}
	for (size_t i = 0; i < references.count; ++i) {
		for (size_t j = i + 1; j < references.count; ++j) {
			const _mc_reference_s* const first = &references.data[i];
			const _mc_reference_s* const second = &references.data[j];
			if ((first->argument->as.reference.mutable_ || second->argument->as.reference.mutable_) && _mc_overlaps(&first->place, &second->place)) {
				mc_help(mc_label(mc_label_main(mc_error(c->diag, mc_E0528, second->argument->span, "two references of this call overlap"),
					"overlaps the other reference"), first->argument->span, "this one"),
					"a `mut &` argument must not share memory with another reference argument");
			}
		}
	}
	for (size_t i = 0; i < references.count; ++i) mc_place_free(&references.data[i].place);
	mc_vector_free(&references);
}

static const mc_type_s* _mc_call(mc_checker_s* const c, mc_node_s* const node, mc_context_s* const context, const mc_type_s* const expected) {
	mc_node_s* const callee = node->as.call.callee;
	const mc_nodes_s* const arguments = &node->as.call.arguments;
	mc_symbol_s* symbol = NULL;
	if (mc_node_name == callee->kind) {
		symbol = mc_lookup(c, callee->as.name.name, context, callee->span, true);
		if (NULL == symbol) {
			_mc_loose(c, arguments, 0, context);
			return _mc_error(c);
		}
	}
	else if (mc_node_member == callee->kind && (mc_node_name == callee->as.member.target->kind || mc_node_member == callee->as.member.target->kind)) {
		mc_symbol_s* const base = mc_qualified(c, callee->as.member.target, context, false);
		if (_mc_is_module_like(base)) {
			symbol = mc_member_of(c, base, callee->as.member.name, callee->as.member.name_span, context);
			if (NULL == symbol) {
				_mc_loose(c, arguments, 0, context);
				return _mc_error(c);
			}
		}
	}
	if (symbol != NULL && mc_symbol_function == symbol->kind) {
		mc_instance_s* const instance = _mc_call_instance(c, (mc_function_s*)symbol, node, context, expected);
		if (NULL == instance) {
			_mc_loose(c, arguments, 0, context);
			return _mc_error(c);
		}
		callee->note.type = mc_instance_type(c->program, instance);
		callee->note.instance = instance;
		node->note.instance = instance;
		_mc_arguments(c, node, instance->parameters.data, instance->parameters.count, &instance->node->as.function.parameters, context,
			mc_instance_name(c, instance));
		return (instance->result != NULL) ? instance->result : mc_special_type(c, mc_special_nothing);
	}
	if (symbol != NULL && mc_symbol_struct == symbol->kind) {
		const mc_struct_s* const info = (const mc_struct_s*)symbol;
		mc_help(mc_label_main(mc_error(c->diag, mc_E0504, callee->span, "`" mc_view_fmt "` is a struct, not a function", mc_view_arg(info->node->as.struct_.name)),
			"cannot be called"), "create a value with a literal: `(" mc_view_fmt "){ ... }`", mc_view_arg(info->node->as.struct_.name));
		_mc_loose(c, arguments, 0, context);
		return _mc_error(c);
	}
	if (node->as.call.has_type_arguments) {
		const mc_view_s text = mc_node_text(c, callee);
		mc_label_main(mc_error(c->diag, mc_E0521, callee->span, "`" mc_view_fmt "` is not a generic function", mc_view_arg(text)),
			"only generic functions take type arguments");
	}
	const mc_type_s* const callee_type = (symbol != NULL) ? mc_symbol_value(c, symbol, callee, context, NULL) : mc_expression(c, callee, context, NULL);
	callee->note.type = callee_type;
	if (_mc_is_error(callee_type)) {
		_mc_loose(c, arguments, 0, context);
		return _mc_error(c);
	}
	if (callee_type->kind != mc_type_function) {
		const mc_view_s text = mc_node_text(c, callee);
		mc_label_main(mc_error(c->diag, mc_E0504, callee->span, "`" mc_view_fmt "` is not a function", mc_view_arg(text)), "it is a `%s`", mc_name_of(c, callee_type));
		_mc_loose(c, arguments, 0, context);
		return _mc_error(c);
	}
	const mc_view_s text = mc_node_text(c, callee);
	_mc_arguments(c, node, callee_type->as.function.parameters, callee_type->as.function.count, NULL, context, mc_view_cstring(c, text));
	return (callee_type->as.function.result != NULL) ? callee_type->as.function.result : mc_special_type(c, mc_special_nothing);
}

// --- operators -------------------------------------------------------------------------

static void _mc_visible_operators(mc_checker_s* const c, const mc_pattern_e pattern, const mc_type_s* const* const types, const size_t count,
								  mc_context_s* const context, mc_functions_s* const result) {
	mc_unused(c);
	mc_vector_of(mc_module_s*) modules = {0};
	mc_vector_push(&modules, context->module);
	for (size_t i = 0; i < context->module->imports.count; ++i) mc_vector_push(&modules, context->module->imports.data[i].target);
	for (size_t i = 0; i < count; ++i) {
		if (types[i] != NULL && mc_type_struct == types[i]->kind) mc_vector_push(&modules, ((mc_struct_s*)types[i]->as.struct_.info)->module);
	}
	for (size_t i = 0; i < modules.count; ++i) {
		mc_module_s* const module = modules.data[i];
		bool seen = false;
		for (size_t j = 0; j < i; ++j) seen = seen || modules.data[j] == module;
		if (seen) continue;
		const mc_functions_s* const registered = &module->operators[pattern];
		for (size_t f = 0; f < registered->count; ++f) {
			if (module == context->module || registered->data[f]->node->as.function.public_) mc_vector_push(result, registered->data[f]);
		}
	}
	mc_vector_free(&modules);
}

static const char* _mc_pattern_text(const mc_pattern_e pattern) {
	static const char* const texts[] = {
#define mc_operator_pattern(_name, _text, _arity) _text,
#include "mc/operators.def"
#undef mc_operator_pattern
	};
	return texts[pattern];
}

static void _mc_candidate_note(mc_checker_s* const c, mc_diagnostic_s* const diagnostic, const mc_instance_s* const match) {
	mc_string_s types = {0};
	for (size_t p = 0; p < match->parameters.count; ++p) {
		if (p > 0) mc_string_append_cstr(&types, ", ");
		mc_type_text(&types, match->parameters.data[p]);
	}
	mc_note(diagnostic, "candidate: `%s` (%s)", mc_instance_name(c, match), mc_string_cstr(&types));
	mc_string_free(&types);
}

/// the result of an operator function for 'types' (null when there is none,
/// the error type when it is ambiguous).
static const mc_type_s* _mc_operator_call(mc_checker_s* const c, mc_node_s* const node, const mc_pattern_e pattern, mc_node_s* const* const operands,
										  const mc_type_s* const* const types, const size_t count, mc_context_s* const context) {
	mc_functions_s candidates = {0};
	_mc_visible_operators(c, pattern, types, count, context, &candidates);
	mc_instances_s matches = {0};
	for (size_t i = 0; i < candidates.count; ++i) {
		mc_function_s* const info = candidates.data[i];
		mc_instance_s* const instance = info->instances.count > 0 ? info->instances.data[0] : NULL;
		if (NULL == instance || instance->parameters.count != count) continue;
		bool all = true;
		for (size_t p = 0; p < count; ++p) {
			const mc_type_s* const parameter = instance->parameters.data[p];
			const mc_type_s* const actual = types[p];
			if (!(parameter == actual || _mc_accepts_literal(parameter, actual) || (mc_type_is_special(actual, mc_special_null) && mc_type_slice == parameter->kind))) all = false;
		}
		if (all) mc_vector_push(&matches, instance);
	}
	mc_vector_free(&candidates);
	if (0 == matches.count) {
		mc_vector_free(&matches);
		return NULL;
	}
	mc_instance_s* chosen = matches.data[0];
	if (matches.count > 1) {
		mc_instances_s defaults = {0};
		for (size_t m = 0; m < matches.count; ++m) {
			bool fits = true;
			for (size_t p = 0; p < count; ++p) {
				if (mc_type_is_untyped(types[p]) && matches.data[m]->parameters.data[p] != _mc_literal_default(c, types[p])) fits = false;
			}
			if (fits) mc_vector_push(&defaults, matches.data[m]);
		}
		if (defaults.count != 1) {
			mc_diagnostic_s* const diagnostic = mc_label_main(mc_error(c->diag, mc_E0507, node->span, "several operator functions `%s` match",
				_mc_pattern_text(pattern)), "ambiguous");
			for (size_t m = 0; m < matches.count; ++m) _mc_candidate_note(c, diagnostic, matches.data[m]);
			mc_vector_free(&defaults);
			mc_vector_free(&matches);
			return _mc_error(c);
		}
		chosen = defaults.data[0];
		mc_vector_free(&defaults);
		mc_diagnostic_s* const diagnostic = mc_label_main(mc_warning(c->diag, mc_W0501, node->span, "ambiguous literal: several operator functions match"),
			"the literal fits more than one");
		for (size_t m = 0; m < matches.count; ++m) _mc_candidate_note(c, diagnostic, matches.data[m]);
		mc_help(mc_note(diagnostic, "using `%s`, the default type of the literal", mc_instance_name(c, chosen)),
			"cast the literal to choose, for example `(f32)2`");
	}
	mc_vector_free(&matches);
	for (size_t p = 0; p < count; ++p) {
		if (mc_type_is_untyped(types[p]) || mc_type_is_special(types[p], mc_special_null)) mc_coerce(c, operands[p], types[p], chosen->parameters.data[p]);
	}
	node->note.operator_call = chosen;
	return chosen->result;
}

static void _mc_check_overflow(mc_checker_s* const c, mc_node_s* const node, const mc_operator_e operator_, mc_node_s* const left_node,
							   mc_node_s* const right_node, const mc_type_s* const type, mc_context_s* const context) {
	if (!mc_type_is_integer(type) || mc_type_scalar != type->kind) return;
	mc_cvalue_s left = {0}, right = {0};
	// both are probed, like the python implementation (probes count as evaluation steps)
	const bool left_known = mc_try_value(c, left_node, &left);
	const bool right_known = mc_try_value(c, right_node, &right);
	if (!left_known || !right_known) return;
	if (left.kind != mc_cvalue_integer || right.kind != mc_cvalue_integer) return;
	if ((mc_operator_divide == operator_ || mc_operator_remainder == operator_) && 0 == right.as.integer) {
		mc_label_main(mc_error(c->diag, mc_E0704, node->span, "division by zero"), "the divisor is zero at compile time");
		return;
	}
	if (context->wrap || (_mc_is_shift(operator_) && right.as.integer < 0)) return;
	__int128 exact = 0;
	if (!mc_integer_arithmetic(operator_, left.as.integer, right.as.integer, &exact)) return;
	if (exact < mc_type_minimum(type) || exact > mc_type_maximum(type)) {
		const mc_view_s text = mc_node_text(c, node);
		mc_string_s wrapped = {0};
		mc_format_int128(&wrapped, mc_integer_wrap(exact, type));
		mc_help(mc_label_main(mc_warning(c->diag, mc_W0701, node->span, "`" mc_view_fmt "` overflows `%s`", mc_view_arg(text), mc_name_of(c, type)),
			"wraps around to %s", mc_string_cstr(&wrapped)), "mark the statement or declaration `[wrap]` if the wrap-around is intended");
		mc_string_free(&wrapped);
	}
}

typedef struct {
	mc_pattern_e pattern;
	bool         swapped;
	bool         negated;
} _mc_operator_use_s;

static _mc_operator_use_s _mc_operator_use(const mc_operator_e operator_) {
	switch (operator_) {
		case mc_operator_add:           return (_mc_operator_use_s) { mc_pattern_add, false, false, };
		case mc_operator_subtract:      return (_mc_operator_use_s) { mc_pattern_subtract, false, false, };
		case mc_operator_multiply:      return (_mc_operator_use_s) { mc_pattern_multiply, false, false, };
		case mc_operator_divide:        return (_mc_operator_use_s) { mc_pattern_divide, false, false, };
		case mc_operator_remainder:     return (_mc_operator_use_s) { mc_pattern_remainder, false, false, };
		case mc_operator_bit_and:       return (_mc_operator_use_s) { mc_pattern_bit_and, false, false, };
		case mc_operator_bit_or:        return (_mc_operator_use_s) { mc_pattern_bit_or, false, false, };
		case mc_operator_bit_xor:       return (_mc_operator_use_s) { mc_pattern_bit_xor, false, false, };
		case mc_operator_shift_left:    return (_mc_operator_use_s) { mc_pattern_shift_left, false, false, };
		case mc_operator_shift_right:   return (_mc_operator_use_s) { mc_pattern_shift_right, false, false, };
		case mc_operator_equal:         return (_mc_operator_use_s) { mc_pattern_equal, false, false, };
		case mc_operator_not_equal:     return (_mc_operator_use_s) { mc_pattern_equal, false, true, };
		case mc_operator_less:          return (_mc_operator_use_s) { mc_pattern_less, false, false, };
		case mc_operator_greater:       return (_mc_operator_use_s) { mc_pattern_less, true, false, };
		case mc_operator_less_equal:    return (_mc_operator_use_s) { mc_pattern_less, true, true, };
		case mc_operator_greater_equal: return (_mc_operator_use_s) { mc_pattern_less, false, true, };
		default: mc_unreachable();
	}
}

const mc_type_s* mc_binary_operation(mc_checker_s* const c, mc_node_s* const node, const mc_operator_e operator_, mc_node_s* const left_node,
									 const mc_type_s* left, mc_node_s* const right_node, const mc_type_s* right, mc_context_s* const context,
									 const mc_span_s span) {
	if (_mc_is_error(left) || _mc_is_error(right)) return _mc_error(c);
	const mc_type_s* const untyped_float = mc_special_type(c, mc_special_untyped_float);
	if (mc_type_is_untyped(left) && mc_type_is_untyped(right)) {
		const bool any_float = left == untyped_float || right == untyped_float;
		if (_mc_is_equality(operator_) || _mc_is_ordering(operator_)) {
			mc_finalize(c, left_node, mc_scalar_type(c, any_float ? mc_scalar_float : mc_scalar_int));
			mc_finalize(c, right_node, mc_scalar_type(c, any_float ? mc_scalar_float : mc_scalar_int));
			return _mc_bool(c);
		}
		if ((_mc_is_bitwise(operator_) || _mc_is_shift(operator_) || mc_operator_remainder == operator_) && any_float) {
			mc_label_main(mc_error(c->diag, mc_E0507, span, "`%s` needs integers", mc_operator_text(operator_)), "a float operand");
			return _mc_error(c);
		}
		if ((mc_operator_divide == operator_ || mc_operator_remainder == operator_) && !any_float) {
			mc_cvalue_s divisor = {0};
			if (mc_untyped_value(c, right_node, &divisor) && mc_cvalue_integer == divisor.kind && 0 == divisor.as.integer) {
				mc_label_main(mc_error(c->diag, mc_E0704, node->span, "division by zero"), "the divisor is zero at compile time");
				return _mc_error(c);
			}
		}
		return any_float ? untyped_float : mc_special_type(c, mc_special_untyped_integer);
	}
	const mc_type_s* const untyped_integer = mc_special_type(c, mc_special_untyped_integer);
	if (mc_type_is_untyped(left) && _mc_accepts_literal(right, left)) {
		mc_coerce(c, left_node, left, right);
		left = right;
	}
	else if (mc_type_is_untyped(right) && _mc_is_shift(operator_) && right == untyped_integer && mc_type_scalar == left->kind
			&& 'i' == mc_scalar_kind(left->as.scalar)) {
		mc_coerce(c, right_node, right, mc_scalar_type(c, mc_scalar_uint));
		right = mc_scalar_type(c, mc_scalar_uint);
	}
	else if (mc_type_is_untyped(right) && _mc_accepts_literal(left, right)) {
		mc_coerce(c, right_node, right, left);
		right = left;
	}
	const bool left_null = mc_type_is_special(left, mc_special_null), right_null = mc_type_is_special(right, mc_special_null);
	if (_mc_is_equality(operator_) && ((left_null && mc_type_slice == right->kind) || (right_null && mc_type_slice == left->kind))) {
		mc_coerce(c, left_null ? left_node : right_node, mc_special_type(c, mc_special_null), left_null ? right : left);
		return _mc_bool(c);
	}
	if (mc_type_scalar == left->kind && mc_type_scalar == right->kind) {
		if (_mc_is_arithmetic(operator_) && left == right && mc_type_is_numeric(left) && (operator_ != mc_operator_remainder || mc_type_is_integer(left))) {
			_mc_check_overflow(c, node, operator_, left_node, right_node, left, context);
			return left;
		}
		if (_mc_is_bitwise(operator_) && left == right && mc_type_is_integer(left)) return left;
		if (_mc_is_shift(operator_) && mc_type_is_integer(left) && mc_type_is_integer(right)) {
			_mc_check_overflow(c, node, operator_, left_node, right_node, left, context);
			return left;
		}
		if (_mc_is_equality(operator_) && left == right) return _mc_bool(c);
		if (_mc_is_ordering(operator_) && left == right && mc_type_is_numeric(left)) return _mc_bool(c);
	}
	if (mc_type_function == left->kind && left == right && _mc_is_equality(operator_)) return _mc_bool(c);
	const _mc_operator_use_s use = _mc_operator_use(operator_);
	mc_node_s* operands[2] = { left_node, right_node };
	const mc_type_s* types[2] = { left, right };
	if (use.swapped) {
		operands[0] = right_node;
		operands[1] = left_node;
		types[0] = right;
		types[1] = left;
	}
	const mc_type_s* const result = _mc_operator_call(c, node, use.pattern, operands, types, 2, context);
	if (result != NULL && _mc_is_error(result)) return result;
	if (result != NULL) {
		node->note.swapped = use.swapped;
		node->note.negated = use.negated;
		return (_mc_is_equality(operator_) || _mc_is_ordering(operator_)) ? _mc_bool(c) : result;
	}
	mc_diagnostic_s* const diagnostic = mc_label_main(mc_error(c->diag, mc_E0507, span, "cannot apply `%s` to `%s` and `%s`", mc_operator_text(operator_),
		mc_name_of(c, left), mc_name_of(c, right)), "no such operation");
	if (mc_type_is_numeric(left) && mc_type_is_numeric(right)) {
		const mc_view_s text = mc_node_text(c, right_node);
		mc_help(diagnostic, "mixed types need an explicit cast, for example `(%s)" mc_view_fmt "`", mc_name_of(c, left), mc_view_arg(text));
	}
	else if (mc_type_struct == left->kind || mc_type_struct == right->kind) {
		mc_help(diagnostic, "declare a function marked `[op.%s]` for (%s, %s)", _mc_pattern_text(use.pattern), mc_name_of(c, types[0]), mc_name_of(c, types[1]));
	}
	else if (mc_type_is_untyped(left) || mc_type_is_untyped(right)) {
		mc_help(diagnostic, "a float literal cannot become an integer");
	}
	return _mc_error(c);
}

// --- indexing --------------------------------------------------------------------------

static void _mc_index_value(mc_checker_s* const c, mc_node_s* const node, mc_context_s* const context, const mc_type_s* const target, const bool end) {
	const mc_type_s* const type = mc_expression(c, node, context, NULL);
	if (_mc_is_error(type)) return;
	if (mc_type_is_special(type, mc_special_untyped_integer)) {
		(void)mc_settle(c, node, type);
	}
	else if (!(mc_type_scalar == type->kind && 'i' == mc_scalar_kind(type->as.scalar))) {
		mc_label_main(mc_error(c->diag, mc_E0502, node->span, "an index must be an integer"), "found `%s`", mc_name_of(c, type));
		return;
	}
	mc_cvalue_s value = {0};
	if (mc_try_value(c, node, &value) && mc_cvalue_integer == value.kind) {
		const __int128 index = value.as.integer;
		const bool array = mc_type_array == target->kind;
		const __int128 limit = array ? target->as.array.length : 0;
		if (index < 0 || (array && (end ? index > limit : index >= limit))) {
			if (array) {
				mc_label_main(mc_error(c->diag, mc_E0706, node->span, "index %lld is out of bounds", (long long)index), "the array has %lld elements", (long long)limit);
			}
			else {
				mc_label_main(mc_error(c->diag, mc_E0706, node->span, "index %lld is out of bounds", (long long)index), "indexes cannot be negative");
			}
		}
	}
}

static const mc_type_s* _mc_index(mc_checker_s* const c, mc_node_s* const node, mc_context_s* const context) {
	const mc_type_s* const target = mc_expression(c, node->as.index.target, context, NULL);
	if (_mc_is_error(target)) {
		(void)mc_expression(c, node->as.index.index, context, NULL);
		return target;
	}
	if (mc_type_struct == target->kind) {
		const mc_type_s* const index = mc_expression(c, node->as.index.index, context, NULL);
		mc_node_s* const operands[2] = { node->as.index.target, node->as.index.index };
		const mc_type_s* const types[2] = { target, index };
		const mc_type_s* const result = _mc_operator_call(c, node, mc_pattern_index, operands, types, 2, context);
		if (result != NULL) return result;
		mc_label_main(mc_error(c->diag, mc_E0513, node->span, "`%s` cannot be indexed", mc_name_of(c, target)), "no `[op._[_]]` function for it");
		return _mc_error(c);
	}
	if (target->kind != mc_type_array && target->kind != mc_type_slice) {
		mc_label_main(mc_error(c->diag, mc_E0513, node->as.index.target->span, "`%s` cannot be indexed", mc_name_of(c, target)), "not an array or a slice");
		(void)mc_expression(c, node->as.index.index, context, NULL);
		return _mc_error(c);
	}
	_mc_index_value(c, node->as.index.index, context, target, false);
	return (mc_type_array == target->kind) ? target->as.array.element : target->as.slice.element;
}

static const mc_type_s* _mc_slice_of(mc_checker_s* const c, mc_node_s* const node, mc_context_s* const context) {
	const mc_type_s* const target = mc_expression(c, node->as.slice_of.target, context, NULL);
	if (_mc_is_error(target)) return target;
	if (target->kind != mc_type_array && target->kind != mc_type_slice) {
		mc_label_main(mc_error(c->diag, mc_E0513, node->as.slice_of.target->span, "`%s` cannot be sliced", mc_name_of(c, target)), "not an array or a slice");
		return _mc_error(c);
	}
	if (node->as.slice_of.low != NULL) _mc_index_value(c, node->as.slice_of.low, context, target, true);
	if (node->as.slice_of.high != NULL) _mc_index_value(c, node->as.slice_of.high, context, target, !node->as.slice_of.inclusive);
	const mc_type_s* const element = (mc_type_array == target->kind) ? target->as.array.element : target->as.slice.element;
	return mc_type_slice_of(c->types, element);
}

// --- operators on nodes ----------------------------------------------------------------

static const mc_type_s* _mc_unary(mc_checker_s* const c, mc_node_s* const node, mc_context_s* const context, const mc_type_s* const expected) {
	const mc_operator_e operator_ = node->as.unary.operator_;
	mc_node_s* const operand_node = node->as.unary.operand;
	if (mc_operator_negate == operator_ && mc_node_integer == operand_node->kind) operand_node->note.negative = true;
	const mc_type_s* const operand = mc_expression(c, operand_node, context, mc_operator_not == operator_ ? _mc_bool(c) : expected);
	if (_mc_is_error(operand)) return operand;
	if (mc_operator_not == operator_ && operand == _mc_bool(c)) return operand;
	if (mc_operator_negate == operator_ && (mc_type_is_untyped(operand)
			|| (mc_type_scalar == operand->kind && (mc_type_is_float(operand) || (mc_type_is_integer(operand) && mc_type_is_signed(operand)))))) {
		return operand;
	}
	if (mc_operator_bit_not == operator_ && (mc_type_is_special(operand, mc_special_untyped_integer)
			|| (mc_type_scalar == operand->kind && 'i' == mc_scalar_kind(operand->as.scalar)))) {
		return operand;
	}
	const mc_pattern_e pattern = (mc_operator_negate == operator_) ? mc_pattern_negate : (mc_operator_not == operator_) ? mc_pattern_not : mc_pattern_bit_not;
	mc_node_s* const operands[1] = { operand_node };
	const mc_type_s* const types[1] = { operand };
	const mc_type_s* const result = _mc_operator_call(c, node, pattern, operands, types, 1, context);
	if (result != NULL) return result;
	mc_diagnostic_s* const diagnostic = mc_label_main(mc_error(c->diag, mc_E0507, node->span, "cannot apply unary `%s` to `%s`", mc_operator_text(operator_),
		mc_name_of(c, operand)), "no such operation");
	if (mc_operator_negate == operator_ && mc_type_scalar == operand->kind && 'i' == mc_scalar_kind(operand->as.scalar)) {
		mc_help(diagnostic, "unsigned values cannot be negated; cast to a signed type first");
	}
	return _mc_error(c);
}

static const mc_type_s* _mc_binary(mc_checker_s* const c, mc_node_s* const node, mc_context_s* const context) {
	const mc_operator_e operator_ = node->as.binary.operator_;
	if (mc_operator_and == operator_ || mc_operator_or == operator_) {
		mc_node_s* const sides[2] = { node->as.binary.left, node->as.binary.right };
		for (size_t i = 0; i < 2; ++i) mc_coerce(c, sides[i], mc_expression(c, sides[i], context, _mc_bool(c)), _mc_bool(c));
		return _mc_bool(c);
	}
	const mc_type_s* const left = mc_expression(c, node->as.binary.left, context, NULL);
	const mc_type_s* const right = mc_expression(c, node->as.binary.right, context, NULL);
	return mc_binary_operation(c, node, operator_, node->as.binary.left, left, node->as.binary.right, right, context, node->as.binary.operator_span);
}

static const mc_type_s* _mc_cast(mc_checker_s* const c, mc_node_s* const node, mc_context_s* const context) {
	const mc_type_s* const target = mc_resolve_type(c, node->as.cast.type, context, false, false);
	mc_node_s* const operand_node = node->as.cast.operand;
	const mc_type_s* operand = mc_expression(c, operand_node, context, NULL);
	if (_mc_is_error(operand) || _mc_is_error(target)) return target;
	if (mc_type_is_untyped(operand)) {
		mc_cvalue_s value = {0};
		const bool known = mc_untyped_value(c, operand_node, &value);
		const bool integer = known && mc_cvalue_integer == value.kind;
		if (mc_type_scalar == target->kind && _mc_accepts_literal(target, operand)
				&& (!integer || mc_type_is_float(target) || (value.as.integer >= mc_type_minimum(target) && value.as.integer <= mc_type_maximum(target)))) {
			mc_finalize(c, operand_node, target);
			operand = target;
		}
		else {
			operand = mc_settle(c, operand_node, operand);
		}
	}
	if (mc_type_scalar == target->kind && mc_type_scalar == operand->kind && (target == operand || (mc_type_is_numeric(target) && mc_type_is_numeric(operand)))) {
		return target;
	}
	mc_help(mc_label_main(mc_error(c->diag, mc_E0510, node->span, "cannot cast `%s` to `%s`", mc_name_of(c, operand), mc_name_of(c, target)), "invalid cast"),
		"casts convert between number types");
	return target;
}

// --- literals of structs and arrays ------------------------------------------------------

static const mc_type_s* _mc_composite_of(mc_checker_s* const c, mc_node_s* const node, const mc_type_s* const target, mc_context_s* const context) {
	const mc_nodes_s* const entries = &node->as.composite.entries;
	const bool named = entries->count > 0 && entries->data[0]->as.entry.named;
	if (0 == entries->count) {
		mc_type_list_s seen = {0};
		const bool zeroable = mc_zeroable(c, target, &seen);
		mc_vector_free(&seen);
		if (!zeroable) {
			mc_label_main(mc_error(c->diag, mc_E0526, node->span, "`%s` has no zero value", mc_name_of(c, target)), "a function value cannot be empty");
			return _mc_error(c);
		}
		if (mc_type_array == target->kind && target->as.array.length < 0) {
			mc_label_main(mc_error(c->diag, mc_E0506, node->span, "`[_]` cannot take its length from `{}`"), "no elements");
			return _mc_error(c);
		}
		return target;
	}
	if (mc_type_struct == target->kind) {
		if (!named) {
			mc_help(mc_label_main(mc_error(c->diag, mc_E0525, node->span, "struct literals need field names"), "positional values"),
				"write `{ field: value, ... }`");
			for (size_t i = 0; i < entries->count; ++i) _mc_loose_one(c, entries->data[i]->as.entry.value, context);
			return target;
		}
		mc_layout_s* const layout = mc_layout(c, target);
		const mc_struct_s* const info = target->as.struct_.info;
		if (!info->node->as.struct_.public_ && info->module != context->module) {
			mc_note(mc_label_main(mc_error(c->diag, mc_E0522, node->span, "the fields of `%s` are private", mc_name_of(c, target)), "cannot be initialized here"),
				"`" mc_view_fmt "` is not `pub` in `%s`", mc_view_arg(info->node->as.struct_.name), info->module->name);
		}
		mc_vector_of(mc_view_s) given = {0};
		for (size_t i = 0; i < entries->count; ++i) {
			mc_node_s* const entry = entries->data[i];
			if (!entry->as.entry.named) {
				// a positional entry among named ones (reported by the parser).
				_mc_loose_one(c, entry->as.entry.value, context);
				continue;
			}
			mc_field_s* const field = mc_layout_field(layout, entry->as.entry.name);
			if (NULL == field) {
				mc_string_s fields = {0};
				for (size_t f = 0; f < layout->fields.count; ++f) {
					if (f > 0) mc_string_append_cstr(&fields, ", ");
					mc_string_append(&fields, layout->fields.data[f]->name);
				}
				mc_note(mc_label_main(mc_error(c->diag, mc_E0512, entry->span, "`%s` has no field `" mc_view_fmt "`", mc_name_of(c, target), mc_view_arg(entry->as.entry.name)),
					"unknown field"), "fields: %s", mc_string_cstr(&fields));
				mc_string_free(&fields);
				_mc_loose_one(c, entry->as.entry.value, context);
				continue;
			}
			bool repeated = false;
			for (size_t g = 0; g < given.count; ++g) repeated = repeated || mc_view_eq(given.data[g], entry->as.entry.name);
			if (repeated) {
				mc_label_main(mc_error(c->diag, mc_E0525, entry->span, "field `" mc_view_fmt "` is given twice", mc_view_arg(entry->as.entry.name)), "repeated");
				continue;
			}
			mc_vector_push(&given, entry->as.entry.name);
			mc_coerce(c, entry->as.entry.value, mc_expression(c, entry->as.entry.value, context, field->type), field->type);
		}
		for (size_t f = 0; f < layout->fields.count; ++f) {
			const mc_field_s* const field = layout->fields.data[f];
			bool present = field->default_ != NULL;
			for (size_t g = 0; g < given.count; ++g) present = present || mc_view_eq(given.data[g], field->name);
			if (present) continue;
			mc_type_list_s seen = {0};
			const bool zeroable = mc_zeroable(c, field->type, &seen);
			mc_vector_free(&seen);
			if (!zeroable) {
				mc_label_main(mc_error(c->diag, mc_E0526, node->span, "field `" mc_view_fmt "` must be given", mc_view_arg(field->name)),
					"a function value has no zero value");
			}
			else {
				mc_help(mc_label_main(mc_warning(c->diag, mc_W0504, node->span, "field `" mc_view_fmt "` is not initialized", mc_view_arg(field->name)),
					"it will be zero"), "give it a value, or a default in `" mc_view_fmt "`", mc_view_arg(info->node->as.struct_.name));
			}
		}
		mc_vector_free(&given);
		return target;
	}
	if (mc_type_array == target->kind || mc_type_slice == target->kind) {
		if (named) {
			mc_label_main(mc_error(c->diag, mc_E0525, node->span, "array literals take values without names"), "named entries");
			for (size_t i = 0; i < entries->count; ++i) _mc_loose_one(c, entries->data[i]->as.entry.value, context);
			return target;
		}
		const mc_type_s* const element = (mc_type_array == target->kind) ? target->as.array.element : target->as.slice.element;
		for (size_t i = 0; i < entries->count; ++i) {
			mc_node_s* const value = entries->data[i]->as.entry.value;
			mc_coerce(c, value, mc_expression(c, value, context, element), element);
		}
		if (mc_type_array == target->kind) {
			if (target->as.array.length < 0) return mc_type_array_of(c->types, element, (int64_t)entries->count);
			if ((int64_t)entries->count != target->as.array.length) {
				mc_help(mc_label_main(mc_error(c->diag, mc_E0525, node->span, "expected %lld elements, found %zu", (long long)target->as.array.length, entries->count),
					"for `%s`", mc_name_of(c, target)), "write `[_]` in the type to take the length from the literal");
			}
		}
		return target;
	}
	mc_help(mc_label_main(mc_error(c->diag, mc_E0525, node->span, "a `%s` cannot be written as `{ ... }`", mc_name_of(c, target)), "only `{}` (zero) is allowed"),
		"`{ ... }` with values is for structs, arrays and slices");
	return _mc_error(c);
}

static const mc_type_s* _mc_composite(mc_checker_s* const c, mc_node_s* const node, mc_context_s* const context, const mc_type_s* const expected) {
	const mc_type_s* target = expected;
	if (node->as.composite.type != NULL) target = mc_resolve_type(c, node->as.composite.type, context, false, true);
	if (NULL == target || _mc_is_error(target)) {
		if (NULL == target) {
			mc_help(mc_label_main(mc_error(c->diag, mc_E0506, node->span, "cannot infer the type of this literal"), "no type known here"),
				"write the type, `(T){ ... }`, or give the variable a type");
		}
		for (size_t i = 0; i < node->as.composite.entries.count; ++i) _mc_loose_one(c, node->as.composite.entries.data[i]->as.entry.value, context);
		return _mc_error(c);
	}
	return _mc_composite_of(c, node, target, context);
}

// --- lambdas and intrinsics ---------------------------------------------------------------

static const mc_type_s* _mc_lambda(mc_checker_s* const c, mc_node_s* const node, mc_context_s* const context) {
	mc_context_s* const inner = mc_context_new(c, context->module, &context->bindings, NULL, context, NULL, true);
	mc_type_list_s parameters = {0};
	const mc_nodes_s* const nodes = &node->as.lambda.parameters;
	for (size_t i = 0; i < nodes->count; ++i) {
		const mc_node_s* const parameter = nodes->data[i];
		if (parameter->as.param.variadic) {
			mc_label_main(mc_error(c->diag, mc_E0415, parameter->span, "lambdas cannot take `...`"), "variadic parameter");
			mc_vector_push(&parameters, _mc_error(c));
			continue;
		}
		const mc_type_s* type = mc_resolve_type(c, parameter->as.param.type, inner, true, false);
		if (mc_type_reference == type->kind) type = mc_type_reference_to(c->types, type->as.reference.element, parameter->as.param.mutable_);
		if (parameter->as.param.default_ != NULL) {
			mc_label_main(mc_error(c->diag, mc_E0412, parameter->as.param.default_->span, "lambda parameters cannot have defaults"), "remove the default");
		}
		mc_vector_push(&parameters, type);
	}
	const mc_type_s* const result = (node->as.lambda.result != NULL) ? mc_resolve_type(c, node->as.lambda.result, inner, false, false) : NULL;
	inner->result = result;
	for (size_t i = 0; i < nodes->count; ++i) {
		mc_node_s* const parameter = nodes->data[i];
		const mc_type_s* const type = parameters.data[i];
		const bool reference = mc_type_reference == type->kind;
		mc_variable_s* const variable = mc_arena_new(c->arena, mc_variable_s);
		variable->base.kind = mc_symbol_variable;
		variable->name = parameter->as.param.name;
		variable->type = reference ? type->as.reference.element : type;
		variable->kind = mc_variable_param;
		variable->mutable_ = parameter->as.param.mutable_;
		variable->span = parameter->span;
		variable->reference = reference;
		variable->level = inner->level;
		parameter->note.variable = variable;
		(void)mc_context_declare(inner, parameter->as.param.name, &variable->base);
		mc_vector_push(&inner->variables, variable);
	}
	mc_block(c, node->as.lambda.body, inner, false);
	if (result != NULL && !mc_terminates(node->as.lambda.body)) {
		mc_label_main(mc_error(c->diag, mc_E0514, node->span, "this lambda does not return a value on every path"), "must return `%s`", mc_name_of(c, result));
	}
	mc_unused_mut(c, inner);
	const mc_type_s* const type = mc_type_function_of(c->types, parameters.data, parameters.count, result);
	mc_vector_free(&parameters);
	return type;
}

static const mc_type_s* _mc_assert(mc_checker_s* const c, mc_node_s* const node, mc_context_s* const context) {
	mc_node_s* const condition = node->as.assert_.condition;
	const mc_type_s* const actual = mc_expression(c, condition, context, _mc_bool(c));
	mc_coerce(c, condition, actual, _mc_bool(c));
	mc_node_s* const message = node->as.assert_.message;
	if (message != NULL) {
		if (message->kind != mc_node_string) {
			mc_label_main(mc_error(c->diag, mc_E0502, message->span, "the message of `assert` must be a string literal"), "not a string literal");
		}
		else {
			message->note.type = mc_type_array_of(c->types, mc_scalar_type(c, mc_scalar_u8), (int64_t)message->as.string.data.length);
		}
	}
	mc_cvalue_s value = {0};
	if (actual == _mc_bool(c) && mc_try_value(c, condition, &value) && mc_cvalue_boolean == value.kind && !value.as.boolean) {
		mc_diagnostic_s* const diagnostic = mc_label_main(mc_error(c->diag, mc_E0703, condition->span, "assertion is always false"), "checked at compile time");
		if (message != NULL && mc_node_string == message->kind) mc_note(diagnostic, mc_view_fmt, mc_view_arg(message->as.string.data));
	}
	return mc_special_type(c, mc_special_nothing);
}

// --- the dispatcher ------------------------------------------------------------------------

const mc_type_s* mc_expression(mc_checker_s* const c, mc_node_s* const node, mc_context_s* const context, const mc_type_s* const expected) {
	const mc_type_s* type = NULL;
	switch (node->kind) {
		case mc_node_integer: type = _mc_integer(c, node); break;
		case mc_node_float:
			type = (node->as.float_.suffix != mc_scalar_none_) ? mc_scalar_type(c, node->as.float_.suffix) : mc_special_type(c, mc_special_untyped_float);
			break;
		case mc_node_char:   type = mc_scalar_type(c, mc_scalar_u8); break;
		case mc_node_string: type = mc_type_array_of(c->types, mc_scalar_type(c, mc_scalar_u8), (int64_t)node->as.string.data.length); break;
		case mc_node_bool:   type = _mc_bool(c); break;
		case mc_node_null:   type = mc_special_type(c, mc_special_null); break;
		case mc_node_paren:  type = mc_expression(c, node->as.paren.inner, context, expected); break;
		case mc_node_name:   type = _mc_name(c, node, context, expected); break;
		case mc_node_member: type = _mc_member(c, node, context, expected); break;
		case mc_node_specialize: type = _mc_specialize(c, node, context); break;
		case mc_node_call:   type = _mc_call(c, node, context, expected); break;
		case mc_node_index:  type = _mc_index(c, node, context); break;
		case mc_node_slice_of: type = _mc_slice_of(c, node, context); break;
		case mc_node_unary:  type = _mc_unary(c, node, context, expected); break;
		case mc_node_binary: type = _mc_binary(c, node, context); break;
		case mc_node_cast:   type = _mc_cast(c, node, context); break;
		case mc_node_reference:
			mc_help(mc_label_main(mc_error(c->diag, mc_E0519, node->span, "`&` is only allowed on a call argument"), "not an argument of a reference parameter"),
				"references are passed to functions; use a slice to keep a view of an array");
			_mc_loose_one(c, node->as.reference.operand, context);
			type = _mc_error(c);
			break;
		case mc_node_composite: type = _mc_composite(c, node, context, expected); break;
		case mc_node_lambda: type = _mc_lambda(c, node, context); break;
		case mc_node_sizeof: {
			const mc_type_s* const resolved = mc_resolve_type(c, node->as.sizeof_.type, context, false, false);
			node->note.size = _mc_is_error(resolved) ? 0 : mc_size_of(c, resolved, NULL);
			type = mc_scalar_type(c, mc_scalar_uint);
			break;
		}
		case mc_node_lenof: {
			const mc_type_s* const operand = mc_expression(c, node->as.lenof.operand, context, NULL);
			if (!_mc_is_error(operand) && operand->kind != mc_type_array && operand->kind != mc_type_slice) {
				mc_label_main(mc_error(c->diag, mc_E0507, node->as.lenof.operand->span, "`lenof` needs an array or a slice, found `%s`", mc_name_of(c, operand)),
					"no length");
			}
			type = mc_scalar_type(c, mc_scalar_uint);
			break;
		}
		case mc_node_assert: type = _mc_assert(c, node, context); break;
		default: mc_unreachable();
	}
	node->note.type = type;
	return type;
}

// --- conversions ------------------------------------------------------------------------------

bool mc_untyped_value(mc_checker_s* const c, const mc_node_s* const node, mc_cvalue_s* const value) {
	return mc_try_value(c, node, value);
}

void mc_finalize(mc_checker_s* const c, mc_node_s* const node, const mc_type_s* const target) {
	const mc_type_s* const current = node->note.type;
	if (current != NULL && !mc_type_is_untyped(current)) return;
	node->note.type = target;
	if (mc_node_paren == node->kind) {
		mc_finalize(c, node->as.paren.inner, target);
	}
	else if (mc_node_unary == node->kind) {
		mc_finalize(c, node->as.unary.operand, target);
	}
	else if (mc_node_binary == node->kind) {
		const mc_operator_e operator_ = node->as.binary.operator_;
		if (!_mc_is_equality(operator_) && !_mc_is_ordering(operator_) && operator_ != mc_operator_and && operator_ != mc_operator_or) {
			mc_finalize(c, node->as.binary.left, target);
			mc_finalize(c, node->as.binary.right, target);
		}
	}
}

const mc_type_s* mc_settle(mc_checker_s* const c, mc_node_s* const node, const mc_type_s* const actual) {
	if (mc_type_is_special(actual, mc_special_untyped_integer)) {
		const mc_type_s* const integer = mc_scalar_type(c, mc_scalar_int);
		mc_cvalue_s value = {0};
		if (mc_untyped_value(c, node, &value) && mc_cvalue_integer == value.kind
				&& (value.as.integer < mc_type_minimum(integer) || value.as.integer > mc_type_maximum(integer))) {
			mc_string_s text = {0};
			mc_format_int128(&text, value.as.integer);
			mc_help(mc_label_main(mc_error(c->diag, mc_E0511, node->span, "`%s` does not fit in `int`", mc_string_cstr(&text)), "too large for the default type"),
				"give it a type, for example with the suffix `u64`");
			mc_string_free(&text);
		}
		mc_finalize(c, node, integer);
		return integer;
	}
	if (mc_type_is_special(actual, mc_special_untyped_float)) {
		mc_finalize(c, node, mc_scalar_type(c, mc_scalar_float));
		return mc_scalar_type(c, mc_scalar_float);
	}
	if (mc_type_is_special(actual, mc_special_null)) {
		mc_label_main(mc_error(c->diag, mc_E0506, node->span, "cannot infer the type of `null`"), "no slice type known here");
		return _mc_error(c);
	}
	if (mc_type_is_special(actual, mc_special_nothing)) {
		mc_label_main(mc_error(c->diag, mc_E0523, node->span, "this has no value"), "the function returns nothing");
		return _mc_error(c);
	}
	return actual;
}

bool mc_coerce(mc_checker_s* const c, mc_node_s* const node, const mc_type_s* const actual, const mc_type_s* const target) {
	if (NULL == target || _mc_is_error(actual) || _mc_is_error(target) || actual == target) return true;
	if (_mc_accepts_literal(target, actual)) {
		mc_cvalue_s value = {0};
		if (mc_untyped_value(c, node, &value) && mc_cvalue_integer == value.kind && mc_type_is_integer(target)
				&& (value.as.integer < mc_type_minimum(target) || value.as.integer > mc_type_maximum(target))) {
			mc_string_s text = {0}, low = {0}, high = {0};
			mc_format_int128(&text, value.as.integer);
			mc_format_int128(&low, mc_type_minimum(target));
			mc_format_int128(&high, mc_type_maximum(target));
			mc_label_main(mc_error(c->diag, mc_E0511, node->span, "`%s` does not fit in `%s`", mc_string_cstr(&text), mc_name_of(c, target)),
				"`%s` holds %s to %s", mc_name_of(c, target), mc_string_cstr(&low), mc_string_cstr(&high));
			mc_string_free(&text);
			mc_string_free(&low);
			mc_string_free(&high);
			mc_finalize(c, node, target);
			return false;
		}
		mc_finalize(c, node, target);
		return true;
	}
	if (mc_type_is_special(actual, mc_special_null) && mc_type_slice == target->kind) {
		node->note.type = target;
		return true;
	}
	if (mc_type_is_special(actual, mc_special_nothing)) {
		mc_label_main(mc_error(c->diag, mc_E0523, node->span, "this has no value"), "the function returns nothing");
		return false;
	}
	mc_diagnostic_s* const diagnostic = mc_label_main(mc_error(c->diag, mc_E0502, node->span, "mismatched types"), "expected `%s`, found `%s`",
		mc_name_of(c, target), mc_name_of(c, actual));
	const mc_view_s text = mc_node_text(c, node);
	if (mc_type_array == actual->kind && mc_type_slice == target->kind && actual->as.array.element == target->as.slice.element) {
		mc_help(diagnostic, "take a slice of the array: `" mc_view_fmt "[..]`", mc_view_arg(text));
	}
	else if (mc_type_is_numeric(actual) && mc_type_is_numeric(target)) {
		mc_help(diagnostic, "convert explicitly: `(%s)" mc_view_fmt "`", mc_name_of(c, target), mc_view_arg(text));
	}
	else if (mc_type_is_special(actual, mc_special_untyped_float) && mc_type_scalar == target->kind && mc_type_is_integer(target)) {
		mc_help(diagnostic, "a float literal cannot become an integer");
	}
	return false;
}
