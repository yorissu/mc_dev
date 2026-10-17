
/// the program with every generic instantiated and every expression typed
/// (the 'mono' command): struct layouts, constant values and every function
/// instance with its typed body.

#include "check.h"
#include "mc/format.h"
#include "mc/printer.h"

#include <string.h>

static void _mc_value(mc_string_s* const out, mc_program_s* const program, const mc_cvalue_s* const value, const mc_type_s* const type);

static const mc_type_s* _mc_element_type(const mc_type_s* const type) {
	if (NULL == type) return NULL;
	if (mc_type_array == type->kind) return type->as.array.element;
	if (mc_type_slice == type->kind) return type->as.slice.element;
	return NULL;
}

static void _mc_list(mc_string_s* const out, mc_program_s* const program, const mc_cvalue_s* const items, const size_t count,
					 const mc_type_s* const type) {
	if (type != NULL && mc_type_struct == type->kind) {
		const mc_layout_s* const layout = mc_program_layout(program, type);
		mc_string_append_char(out, '{');
		for (size_t i = 0; i < count && i < layout->fields.count; ++i) {
			if (i > 0) mc_string_append_cstr(out, ", ");
			mc_string_appendf(out, mc_view_fmt ": ", mc_view_arg(layout->fields.data[i]->name));
			_mc_value(out, program, &items[i], layout->fields.data[i]->type);
		}
		mc_string_append_char(out, '}');
		return;
	}
	mc_string_append_char(out, '{');
	for (size_t i = 0; i < count; ++i) {
		if (i > 0) mc_string_append_cstr(out, ", ");
		_mc_value(out, program, &items[i], _mc_element_type(type));
	}
	mc_string_append_char(out, '}');
}

static void _mc_value(mc_string_s* const out, mc_program_s* const program, const mc_cvalue_s* const value, const mc_type_s* const type) {
	switch (value->kind) {
		case mc_cvalue_integer:  mc_format_int128(out, value->as.integer); return;
		case mc_cvalue_real:     mc_format_float_repr(out, value->as.real); return;
		case mc_cvalue_boolean:  mc_string_append_cstr(out, value->as.boolean ? "true" : "false"); return;
		case mc_cvalue_list:     _mc_list(out, program, value->as.list->items, value->as.list->count, type); return;
		case mc_cvalue_slice:
			mc_string_append_cstr(out, "slice ");
			if (NULL == value->as.slice.list) mc_string_append_cstr(out, "{}");
			else _mc_list(out, program, value->as.slice.list->items + value->as.slice.start, (size_t)value->as.slice.length, type);
			return;
		case mc_cvalue_function:
			if (value->as.function.lambda != NULL) {
				mc_string_append_cstr(out, "lambda");
			}
			else {
				mc_string_append_cstr(out, "function ");
				mc_instance_text(out, value->as.function.instance);
			}
			return;
		default:
			mc_string_append_cstr(out, "None");
			return;
	}
}

static void _mc_hook_type(mc_string_s* const out, const struct mc_type_s* const type) {
	mc_type_text(out, type);
}

static void _mc_hook_instance(mc_string_s* const out, const struct mc_instance_s* const instance) {
	mc_instance_text(out, instance);
}

void mc_program_text(mc_program_s* const program, mc_string_s* const out) {
	const mc_print_hooks_s hooks = { .type = _mc_hook_type, .instance = _mc_hook_instance, };
	for (size_t m = 0; m < program->modules.count; ++m) {
		const mc_module_s* const module = program->modules.data[m];
		mc_string_appendf(out, "module %s (%s)\n", module->name, module->path);
		for (size_t s = 0; s < module->ordered.count; ++s) {
			const mc_symbol_s* const symbol = module->ordered.data[s];
			if (mc_symbol_struct == symbol->kind) {
				const mc_struct_s* const info = (const mc_struct_s*)symbol;
				for (size_t i = 0; i < info->instances.count; ++i) {
					const mc_layout_s* const layout = info->instances.data[i];
					mc_string_append_cstr(out, "  struct ");
					mc_type_text(out, layout->type);
					mc_string_append_cstr(out, " { ");
					for (size_t f = 0; f < layout->fields.count; ++f) {
						const mc_field_s* const field = layout->fields.data[f];
						if (f > 0) mc_string_append_cstr(out, ", ");
						mc_string_appendf(out, "%s" mc_view_fmt ": ", field->mutable_ ? "mut " : "", mc_view_arg(field->name));
						mc_type_text(out, field->type);
					}
					mc_string_append_cstr(out, " }\n");
				}
			}
			else if (mc_symbol_constant == symbol->kind) {
				const mc_constant_s* const info = (const mc_constant_s*)symbol;
				mc_string_appendf(out, "  const " mc_view_fmt ": ", mc_view_arg(info->node->as.constant.name));
				mc_type_text(out, info->type);
				mc_string_append_cstr(out, " = ");
				if (info->value != NULL) _mc_value(out, program, info->value, info->type);
				else mc_string_append_cstr(out, "None");
				mc_string_append_char(out, '\n');
			}
		}
		for (size_t s = 0; s < module->ordered.count; ++s) {
			const mc_symbol_s* const symbol = module->ordered.data[s];
			if (symbol->kind != mc_symbol_function) continue;
			const mc_function_s* const info = (const mc_function_s*)symbol;
			const mc_nodes_s* const generics = &info->node->as.function.generics;
			if (generics->count > 0 && 0 == info->instances.count) {
				mc_string_appendf(out, "  func " mc_view_fmt "<", mc_view_arg(info->node->as.function.name));
				for (size_t g = 0; g < generics->count; ++g) {
					if (g > 0) mc_string_append_cstr(out, ", ");
					mc_string_append(out, generics->data[g]->as.generic.name);
				}
				mc_string_append_cstr(out, "> (never instantiated)\n");
			}
			for (size_t i = 0; i < info->instances.count; ++i) {
				const mc_instance_s* const instance = info->instances.data[i];
				mc_string_append_cstr(out, "  func ");
				mc_instance_text(out, instance);
				mc_string_append_char(out, '(');
				const mc_nodes_s* const parameters = &instance->node->as.function.parameters;
				for (size_t p = 0; p < parameters->count && p < instance->parameters.count; ++p) {
					const mc_node_s* const parameter = parameters->data[p];
					const mc_type_s* const type = instance->parameters.data[p];
					if (p > 0) mc_string_append_cstr(out, ", ");
					mc_string_appendf(out, "%s" mc_view_fmt ": ", parameter->as.param.mutable_ ? "mut " : "", mc_view_arg(parameter->as.param.name));
					if (mc_type_reference == type->kind) {
						mc_string_append_char(out, '&');
						mc_type_text(out, type->as.reference.element);
					}
					else {
						mc_type_text(out, type);
					}
				}
				mc_string_append_char(out, ')');
				if (instance->result != NULL) {
					mc_string_append_cstr(out, " -> ");
					mc_type_text(out, instance->result);
				}
				if (mc_function_is_external(info)) mc_string_append_cstr(out, " [builtin]");
				mc_string_append_char(out, '\n');
				if (instance->node->as.function.body != NULL) {
					mc_string_s tree = {0};
					mc_print_tree(instance->node->as.function.body, &hooks, &tree);
					const char* cursor = mc_string_cstr(&tree);
					while (*cursor != '\0') {
						const char* const end = strchr(cursor, '\n');
						const size_t length = (end != NULL) ? (size_t)(end - cursor) : strlen(cursor);
						mc_string_append_cstr(out, "    ");
						mc_string_append(out, mc_view_new(cursor, length));
						mc_string_append_char(out, '\n');
						cursor += length + (end != NULL ? 1 : 0);
					}
					mc_string_free(&tree);
				}
			}
		}
		mc_string_append_char(out, '\n');
	}
}
