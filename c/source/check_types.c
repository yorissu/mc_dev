
/// the checker: resolving written types, struct layouts (one per instance),
/// sizes and zero values.

#include "check.h"

#include <string.h>

const mc_type_s* mc_scalar_type(mc_checker_s* const c, const mc_scalar_e scalar) {
	return mc_type_of_scalar(c->types, scalar);
}

const mc_type_s* mc_special_type(mc_checker_s* const c, const mc_special_e special) {
	return mc_type_of_special(c->types, special);
}

static const char* _mc_path_text(mc_checker_s* const c, const mc_views_s* const path) {
	mc_string_s text = {0};
	for (size_t i = 0; i < path->count; ++i) {
		if (i > 0) mc_string_append_char(&text, '.');
		mc_string_append(&text, path->data[i]);
	}
	const char* const kept = mc_arena_strndup(c->arena, mc_string_cstr(&text), text.length);
	mc_string_free(&text);
	return kept;
}

static const char* _mc_generics_text(mc_checker_s* const c, const mc_nodes_s* const generics) {
	mc_string_s text = {0};
	for (size_t i = 0; i < generics->count; ++i) {
		if (i > 0) mc_string_append_cstr(&text, ", ");
		mc_string_append(&text, generics->data[i]->as.generic.name);
	}
	const char* const kept = mc_arena_strndup(c->arena, mc_string_cstr(&text), text.length);
	mc_string_free(&text);
	return kept;
}

/// an array length: a constant non-negative integer; -1 when it is not one.
static int64_t _mc_constant_length(mc_checker_s* const c, mc_node_s* const node, mc_context_s* const context) {
	const mc_type_s* const type = mc_expression(c, node, context, NULL);
	if (mc_type_is_special(type, mc_special_error)) return -1;
	if (!mc_type_is_integer(type)) {
		mc_label_main(mc_error(c->diag, mc_E0502, node->span, "an array length must be an integer"), "found `%s`", mc_name_of(c, type));
		return -1;
	}
	if (mc_type_is_special(type, mc_special_untyped_integer)) {
		mc_cvalue_s probe = {0};
		const bool known = mc_try_value(c, node, &probe);
		const bool negative = known && mc_cvalue_integer == probe.kind && probe.as.integer < 0;
		mc_finalize(c, node, mc_scalar_type(c, negative ? mc_scalar_int : mc_scalar_uint));
	}
	mc_cvalue_s value = {0};
	if (!mc_require_constant(c, node, context, "an array length", &value) || value.kind != mc_cvalue_integer) return -2;
	if (value.as.integer < 0) {
		mc_label_main(mc_error(c->diag, mc_E0502, node->span, "an array length cannot be negative"), "length is %lld", (long long)value.as.integer);
		return -1;
	}
	return (int64_t)value.as.integer;
}

const mc_type_s* mc_resolve_type(mc_checker_s* const c, const mc_node_s* const node, mc_context_s* const context,
								 const bool reference, const bool infer) {
	const mc_type_s* const error = mc_special_type(c, mc_special_error);
	switch (node->kind) {
		case mc_node_type_name: {
			const mc_views_s* const path = &node->as.type_name.path;
			if (1 == path->count) {
				const mc_scalar_e scalar = mc_scalar_from_name(path->data[0]);
				if (scalar != mc_scalar_none_) return mc_scalar_type(c, scalar);
				const mc_type_s* const bound = mc_bindings_get(&context->bindings, path->data[0]);
				if (bound != NULL) {
					if (node->as.type_name.arguments.count > 0) {
						mc_error(c->diag, mc_E0521, node->span, "type parameter `" mc_view_fmt "` takes no type arguments", mc_view_arg(path->data[0]));
					}
					return bound;
				}
			}
			mc_symbol_s* symbol = NULL;
			if (1 == path->count) {
				symbol = mc_lookup(c, path->data[0], context, node->span, true);
			}
			else {
				mc_symbol_s* base = mc_lookup(c, path->data[0], context, node->span, true);
				for (size_t i = 1; i < path->count; ++i) {
					if (NULL == base || (base->kind != mc_symbol_module && base->kind != mc_symbol_prefix)) break;
					base = mc_member_of(c, base, path->data[i], node->span, context);
				}
				symbol = base;
			}
			if (NULL == symbol) return error;
			if (symbol->kind != mc_symbol_struct) {
				mc_label_main(mc_error(c->diag, mc_E0503, node->span, "`%s` is not a type", _mc_path_text(c, path)), "expected a type");
				return error;
			}
			mc_struct_s* const info = (mc_struct_s*)symbol;
			const mc_nodes_s* const generics = &info->node->as.struct_.generics;
			mc_vector_of(const mc_type_s*) arguments = {0};
			bool failed = false;
			for (size_t i = 0; i < node->as.type_name.arguments.count; ++i) {
				const mc_type_s* const argument = mc_resolve_type(c, node->as.type_name.arguments.data[i], context, false, false);
				failed = failed || argument == error;
				mc_vector_push(&arguments, argument);
			}
			const mc_type_s* result = error;
			if (arguments.count != generics->count) {
				if (generics->count > 0) {
					mc_label_main(mc_error(c->diag, mc_E0520, node->span, "`" mc_view_fmt "` takes %zu type argument(s), found %zu",
						mc_view_arg(info->node->as.struct_.name), generics->count, arguments.count),
						"expected `" mc_view_fmt "<%s>`", mc_view_arg(info->node->as.struct_.name), _mc_generics_text(c, generics));
				}
				else {
					mc_label_main(mc_error(c->diag, mc_E0521, node->span, "`" mc_view_fmt "` is not generic", mc_view_arg(info->node->as.struct_.name)),
						"remove the type arguments");
				}
			}
			else if (!failed) {
				result = mc_struct_type(c, info, arguments.data, arguments.count, node->span);
			}
			mc_vector_free(&arguments);
			return result;
		}
		case mc_node_type_slice:
			return mc_type_slice_of(c->types, mc_resolve_type(c, node->as.type_slice.element, context, false, false));
		case mc_node_type_array: {
			const mc_type_s* const element = mc_resolve_type(c, node->as.type_array.element, context, false, false);
			if (NULL == node->as.type_array.length) {
				if (!infer) {
					mc_label_main(mc_error(c->diag, mc_E0506, node->span, "`[_]` needs an initializer to take its length from"),
						"only allowed in `let` with a value");
					return error;
				}
				return mc_type_array_of(c->types, element, -1);
			}
			const int64_t length = _mc_constant_length(c, node->as.type_array.length, context);
			if (length < 0) return error;
			return mc_type_array_of(c->types, element, length);
		}
		case mc_node_type_reference: {
			const mc_type_s* const element = mc_resolve_type(c, node->as.type_reference.element, context, false, false);
			if (!reference) {
				mc_help(mc_label_main(mc_error(c->diag, mc_E0413, node->span, "references are only allowed as parameter types"),
					"a reference cannot be stored"), "use a slice to keep a view of an array");
				return element;
			}
			return mc_type_reference_to(c->types, element, node->as.type_reference.mutable_);
		}
		case mc_node_type_function: {
			mc_vector_of(const mc_type_s*) parameters = {0};
			for (size_t i = 0; i < node->as.type_function.parameters.count; ++i) {
				mc_vector_push(&parameters, mc_resolve_type(c, node->as.type_function.parameters.data[i], context, true, false));
			}
			const mc_type_s* const result = (node->as.type_function.result != NULL)
				? mc_resolve_type(c, node->as.type_function.result, context, false, false) : NULL;
			const mc_type_s* const type = mc_type_function_of(c->types, parameters.data, parameters.count, result);
			mc_vector_free(&parameters);
			return type;
		}
		default:
			mc_unreachable();
	}
}

mc_field_s* mc_layout_field(const mc_layout_s* const layout, const mc_view_s name) {
	for (size_t i = 0; i < layout->fields.count; ++i) if (mc_view_eq(layout->fields.data[i]->name, name)) return layout->fields.data[i];
	return NULL;
}

static mc_layout_s* _mc_find_layout(const mc_struct_s* const info, const mc_type_s* const type) {
	for (size_t i = 0; i < info->instances.count; ++i) if (info->instances.data[i]->type == type) return info->instances.data[i];
	return NULL;
}

const mc_type_s* mc_struct_type(mc_checker_s* const c, mc_struct_s* const info, const mc_type_s* const* const arguments,
								const size_t count, const mc_span_s span) {
	mc_unused(span);
	const mc_type_s* const type = mc_type_struct_of(c->types, info, info->node->as.struct_.name, arguments, count);
	bool parameter = false;
	for (size_t i = 0; i < count; ++i) parameter = parameter || mc_type_contains_parameter(arguments[i]);
	if (parameter || _mc_find_layout(info, type) != NULL) return type;
	mc_layout_s* const layout = mc_arena_new(c->arena, mc_layout_s);
	layout->type = type;
	mc_arena_push(c->arena, &info->instances, layout);
	mc_vector_push(&c->program->layouts, layout);
	mc_bindings_s bindings = {0};
	const mc_nodes_s* const generics = &info->node->as.struct_.generics;
	for (size_t i = 0; i < generics->count && i < count; ++i) mc_bindings_set(&bindings, generics->data[i]->as.generic.name, arguments[i]);
	mc_context_s* const context = mc_context_new(c, info->module, &bindings, NULL, NULL, NULL, false);
	mc_vector_free(&bindings);
	const mc_nodes_s* const fields = &info->node->as.struct_.fields;
	for (size_t i = 0; i < fields->count; ++i) {
		mc_node_s* const node = fields->data[i];
		(void)mc_attributes(c, &node->as.field.attributes, "field");
		const mc_field_s* const previous = mc_layout_field(layout, node->as.field.name);
		if (previous != NULL) {
			mc_label(mc_label_main(mc_error(c->diag, mc_E0401, node->span, "field `" mc_view_fmt "` is declared twice", mc_view_arg(node->as.field.name)),
				"declared again here"), previous->node->span, "first declared here");
			continue;
		}
		mc_field_s* const field = mc_arena_new(c->arena, mc_field_s);
		field->name = node->as.field.name;
		field->type = mc_resolve_type(c, node->as.field.type, context, false, false);
		field->mutable_ = node->as.field.mutable_;
		field->default_ = (node->as.field.default_ != NULL) ? mc_node_clone(c->arena, node->as.field.default_) : NULL;
		field->node = node;
		field->index = layout->fields.count;
		mc_vector_push(&layout->fields, field);
	}
	for (size_t i = 0; i < layout->fields.count; ++i) {
		mc_field_s* const field = layout->fields.data[i];
		if (NULL == field->default_) continue;
		const mc_type_s* const actual = mc_expression(c, field->default_, context, field->type);
		mc_coerce(c, field->default_, actual, field->type);
		mc_cvalue_s ignored = {0};
		(void)mc_require_constant(c, field->default_, context, "a field default", &ignored);
	}
	(void)mc_size_of(c, type, NULL);
	return type;
}

mc_layout_s* mc_layout(mc_checker_s* const c, const mc_type_s* const type) {
	mc_struct_s* const info = type->as.struct_.info;
	mc_layout_s* layout = _mc_find_layout(info, type);
	if (NULL == layout) {
		(void)mc_struct_type(c, info, type->as.struct_.arguments, type->as.struct_.count, info->node->as.struct_.name_span);
		layout = _mc_find_layout(info, type);
	}
	return layout;
}

uint64_t mc_size_of(mc_checker_s* const c, const mc_type_s* const type, uint64_t* const alignment) {
	uint64_t ignored = 0;
	uint64_t* const align = (alignment != NULL) ? alignment : &ignored;
	switch (type->kind) {
		case mc_type_scalar: {
			const uint64_t size = (mc_scalar_bool == type->as.scalar) ? 1 : mc_scalar_bits(type->as.scalar) / 8;
			*align = size;
			return size;
		}
		case mc_type_slice:
			*align = mc_word_size;
			return 2 * mc_word_size;
		case mc_type_function:
			*align = mc_word_size;
			return mc_word_size;
		case mc_type_array: {
			const uint64_t size = mc_size_of(c, type->as.array.element, align);
			return size * (uint64_t)(type->as.array.length > 0 ? type->as.array.length : 0);
		}
		case mc_type_struct: {
			mc_struct_s* const info = type->as.struct_.info;
			for (size_t i = 0; i < c->sizing.count; ++i) {
				if (c->sizing.data[i] != type) continue;
				bool reported = false;
				for (size_t j = 0; j < c->recursive_reported.count; ++j) reported = reported || c->recursive_reported.data[j] == info;
				if (!reported) {
					mc_vector_push(&c->recursive_reported, info);
					mc_help(mc_label_main(mc_error(c->diag, mc_E0414, info->node->as.struct_.name_span, "struct `" mc_view_fmt "` contains itself",
						mc_view_arg(info->node->as.struct_.name)), "it would have infinite size"), "store it through a slice, for example `next: []Node`");
				}
				*align = 1;
				return 0;
			}
			mc_vector_push(&c->sizing, type);
			uint64_t offset = 0, struct_alignment = 1;
			const mc_layout_s* const layout = mc_layout(c, type);
			for (size_t i = 0; i < layout->fields.count; ++i) {
				uint64_t field_alignment = 1;
				const uint64_t size = mc_size_of(c, layout->fields.data[i]->type, &field_alignment);
				if (0 == field_alignment) field_alignment = 1;
				offset = (offset + field_alignment - 1) / field_alignment * field_alignment + size;
				if (field_alignment > struct_alignment) struct_alignment = field_alignment;
			}
			for (size_t i = 0; i < c->sizing.count; ++i) {
				if (c->sizing.data[i] == type) {
					c->sizing.data[i] = c->sizing.data[c->sizing.count - 1];
					c->sizing.count -= 1;
					break;
				}
			}
			*align = struct_alignment;
			return (offset + struct_alignment - 1) / struct_alignment * struct_alignment;
		}
		default:
			*align = 1;
			return 0;
	}
}

bool mc_zeroable(mc_checker_s* const c, const mc_type_s* const type, mc_type_list_s* const seen) {
	if (mc_type_function == type->kind) return false;
	if (mc_type_array == type->kind) return mc_zeroable(c, type->as.array.element, seen);
	if (mc_type_struct == type->kind) {
		for (size_t i = 0; i < seen->count; ++i) if (seen->data[i] == type) return true;
		mc_vector_push(seen, type);
		const mc_layout_s* const layout = mc_layout(c, type);
		for (size_t i = 0; i < layout->fields.count; ++i) {
			const mc_field_s* const field = layout->fields.data[i];
			if (NULL == field->default_ && !mc_zeroable(c, field->type, seen)) return false;
		}
		return true;
	}
	return true;
}

mc_layout_s* mc_program_layout(mc_program_s* const program, const mc_type_s* const type) {
	return mc_layout(program->checker, type);
}

uint64_t mc_program_size_of(mc_program_s* const program, const mc_type_s* const type) {
	return mc_size_of(program->checker, type, NULL);
}
