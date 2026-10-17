
/// plugin functions in the checker: whether a loaded plugin provides each
/// '[plugin]' function that is used, and the compile-time half of the
/// plugin api (see mc/plugin.h): the check of a plugin function runs on
/// every call of it, after the arguments are checked.

#include "check.h"
#include "plugin_host.h"

#include <string.h>

typedef struct {
	mc_plugin_check_s     public_  ;   // first: the handle the plugin gets
	mc_checker_s*         checker  ;
	mc_node_s*            node     ;   // the call
	const char*           name     ;
	size_t                count    ;
	size_t                fixed    ;
	mc_node_s**           arguments;
	const mc_type_s**     types    ;
	const mc_instance_s*  instance ;
} _mc_check_s;

/// a diagnostic being reported, with what its labels need.
struct mc_plugin_diagnostic_s {
	mc_diagnostic_s* diagnostic;
	_mc_check_s*     check     ;
	size_t           index     ;   // where it was reported
	bool             main      ;   // the main label is given
};

static _mc_check_s* _mc_check_of(mc_plugin_check_s* const check) {
	return (_mc_check_s*)check;
}

static const mc_plugins_s* _mc_plugins(const mc_checker_s* const c) {
	return (c->options != NULL && c->options->plugins != NULL) ? c->options->plugins : mc_plugins_default();
}

// --- whether a plugin provides a function -----------------------------------------------------

/// the plugin a '[plugin = "name"]' declaration names, or null.
static const char* _mc_wanted_plugin(mc_checker_s* const c, const mc_function_s* const function) {
	const mc_node_s* const attribute = mc_attribute_find(&function->node->as.function.attributes, "plugin");
	if (NULL == attribute || mc_attribute_value_none == attribute->as.attribute.value_kind) return NULL;
	return mc_view_cstring(c, attribute->as.attribute.text);
}

void mc_plugin_use(mc_checker_s* const c, mc_function_s* const function, const mc_span_s span) {
	if (!mc_function_is_plugin(function) || function->plugin_checked) return;
	function->plugin_checked = true;
	const mc_plugins_s* const plugins = _mc_plugins(c);
	char* const name = mc_function_qualified_name(function);
	const mc_plugin_function_s* const provided = mc_plugins_find(plugins, name);
	const char* const wanted = _mc_wanted_plugin(c, function);
	const mc_span_s declared = function->node->as.function.name_span;
	if (NULL == provided) {
		mc_diagnostic_s* const diagnostic = mc_label(mc_label_main(mc_error(c->diag, mc_E0416, span, "no plugin provides `%s`", name),
			"used here"), declared, "declared `[plugin]` here");
		if (wanted != NULL && !mc_plugins_loaded(plugins, wanted)) {
			mc_help(mc_note(diagnostic, "it comes from the plugin `%s`, which is not loaded", wanted), "load it with `--plugin <file>`");
		}
		else if (wanted != NULL) {
			mc_note(diagnostic, "the plugin `%s` is loaded, but it does not provide `%s`", wanted, name);
		}
		else {
			mc_help(diagnostic, "load a plugin that provides it with `--plugin <file>`");
		}
	}
	else if (wanted != NULL && strcmp(wanted, provided->plugin) != 0) {
		mc_note(mc_label(mc_label_main(mc_error(c->diag, mc_E0416, span, "the plugin `%s` does not provide `%s`", wanted, name), "used here"),
			declared, "declared `[plugin = \"%s\"]` here", wanted), "`%s` comes from the plugin `%s`", name, provided->plugin);
	}
	mc_free(name);
}

// --- types -------------------------------------------------------------------------------

static mc_type_code_e _mc_scalar_code(const mc_scalar_e scalar) {
	switch (scalar) {
#define mc_keyword(_name)
#define mc_punctuator(_name, _text)
#define mc_scalar(_name, _kind, _bits, _signed) case mc_scalar_##_name: return mc_code_##_name;
#include "mc/tokens.def"
#undef mc_scalar
#undef mc_punctuator
#undef mc_keyword
		default: return mc_code_int;
	}
}

/// the handle of a checker type (made once per type, in the program's arena).
static const mc_plugin_type_s* _mc_type_handle(mc_checker_s* const c, const mc_type_s* type) {
	if (mc_type_reference == type->kind) type = type->as.reference.element;
	if (mc_type_is_special(type, mc_special_untyped_integer)) type = mc_scalar_type(c, mc_scalar_int);
	if (mc_type_is_special(type, mc_special_untyped_float)) type = mc_scalar_type(c, mc_scalar_float);
	for (size_t i = 0; i < c->plugin_types.count; ++i) if (c->plugin_types.data[i].type == type) return c->plugin_types.data[i].handle;
	mc_plugin_type_s* const handle = mc_arena_new(c->arena, mc_plugin_type_s);
	const mc_checker_plugin_type_s entry = { .type = type, .handle = handle, };
	mc_vector_push(&c->plugin_types, entry);
	handle->name = mc_name_of(c, type);
	handle->index = -1;
	handle->code = mc_type_code_count_;
	uint64_t alignment = 1;
	const bool sized = !mc_type_is_special(type, mc_special_error) && type->kind != mc_type_special && type->kind != mc_type_parameter;
	handle->size = sized ? mc_size_of(c, type, &alignment) : 0;
	handle->alignment = (alignment > 0) ? alignment : 1;
	switch (type->kind) {
		case mc_type_scalar: {
			const char kind = mc_scalar_kind(type->as.scalar);
			handle->kind = ('b' == kind) ? mc_plugin_kind_bool : ('f' == kind) ? mc_plugin_kind_float : mc_plugin_kind_int;
			handle->code = _mc_scalar_code(type->as.scalar);
			break;
		}
		case mc_type_array:
			handle->kind = mc_plugin_kind_array;
			handle->element = _mc_type_handle(c, type->as.array.element);
			handle->length = (type->as.array.length > 0) ? (uint64_t)type->as.array.length : 0;
			break;
		case mc_type_slice:
			handle->kind = mc_plugin_kind_slice;
			handle->code = mc_code_slice;
			handle->element = _mc_type_handle(c, type->as.slice.element);
			break;
		case mc_type_function:
			handle->kind = mc_plugin_kind_function;
			handle->code = mc_code_fn;
			break;
		case mc_type_struct: {
			handle->kind = mc_plugin_kind_struct;
			const mc_layout_s* const layout = mc_layout(c, type);
			const size_t count = layout->fields.count;
			const char** const names = mc_arena_alloc(c->arena, (count + 1) * sizeof(*names));
			const mc_plugin_type_s** const types = mc_arena_alloc(c->arena, (count + 1) * sizeof(*types));
			uint64_t* const offsets = mc_arena_alloc(c->arena, (count + 1) * sizeof(*offsets));
			handle->field_count = count;
			handle->field_names = names;
			handle->field_types = types;
			handle->field_offsets = offsets;
			for (size_t i = 0; i < count; ++i) {
				names[i] = mc_view_cstring(c, layout->fields.data[i]->name);
				offsets[i] = layout->fields.data[i]->offset;
				types[i] = _mc_type_handle(c, layout->fields.data[i]->type);
			}
			break;
		}
		default:
			// a type without a value (an error, '...'): a placeholder
			handle->kind = mc_plugin_kind_int;
			handle->code = mc_code_int;
			break;
	}
	return handle;
}

// --- the check of a call -------------------------------------------------------------------

static size_t _mc_index(_mc_check_s* const check, const size_t index) {
	if (index >= check->count) mc_panic("the check of `%s` asks for argument %zu, but the call has %zu", check->name, index, check->count);
	return index;
}

static const char* _mc_check_name(mc_plugin_check_s* const check) {
	return _mc_check_of(check)->name;
}

static size_t _mc_check_count(mc_plugin_check_s* const check) {
	return _mc_check_of(check)->count;
}

static size_t _mc_check_fixed_count(mc_plugin_check_s* const check) {
	return _mc_check_of(check)->fixed;
}

static const mc_plugin_type_s* _mc_check_type(mc_plugin_check_s* const check, const size_t index) {
	_mc_check_s* const self = _mc_check_of(check);
	return _mc_type_handle(self->checker, self->types[_mc_index(self, index)]);
}

static size_t _mc_check_type_argument_count(mc_plugin_check_s* const check) {
	return _mc_check_of(check)->instance->argument_count;
}

static const mc_plugin_type_s* _mc_check_type_argument(mc_plugin_check_s* const check, const size_t index) {
	_mc_check_s* const self = _mc_check_of(check);
	if (index >= self->instance->argument_count) mc_panic("the check of `%s` asks for type argument %zu", self->name, index);
	return _mc_type_handle(self->checker, self->instance->arguments[index]);
}

/// whether 'node' is a "..."c literal, a constant or a variable that is not
/// 'mut' made from one, or a slice of any of them.
static bool _mc_c_string(const mc_node_s* const node, const int depth) {
	if (NULL == node || depth > 32) return false;
	switch (node->kind) {
		case mc_node_paren:    return _mc_c_string(node->as.paren.inner, depth + 1);
		case mc_node_string:   return node->as.string.c;
		case mc_node_slice_of: return _mc_c_string(node->as.slice_of.target, depth + 1);
		case mc_node_name:
		case mc_node_member: {
			if (node->note.field != NULL) return false;
			const mc_variable_s* const variable = node->note.variable;
			if (variable != NULL) return mc_variable_let == variable->kind && !variable->mutable_ && _mc_c_string(variable->initializer, depth + 1);
			if (NULL == node->note.constant) return false;
			return _mc_c_string(node->note.constant->node->as.constant.value, depth + 1);
		}
		default:
			return false;
	}
}

static mc_plugin_constant_s _mc_check_constant(mc_plugin_check_s* const check, const size_t index) {
	_mc_check_s* const self = _mc_check_of(check);
	mc_checker_s* const c = self->checker;
	mc_node_s* const node = self->arguments[_mc_index(self, index)];
	mc_plugin_constant_s constant = { .kind = mc_plugin_constant_none, .c_string = _mc_c_string(node, 0), };
	mc_cvalue_s value = {0};
	if (mc_node_reference == node->kind || !mc_try_value(c, node, &value)) return constant;
	switch (value.kind) {
		case mc_cvalue_integer:
			constant.kind = mc_plugin_constant_integer;
			constant.integer = mc_i128_to_int(value.as.integer);
			return constant;
		case mc_cvalue_real:
			constant.kind = mc_plugin_constant_float;
			constant.real = value.as.real;
			return constant;
		case mc_cvalue_boolean:
			constant.kind = mc_plugin_constant_bool;
			constant.boolean = value.as.boolean;
			return constant;
		case mc_cvalue_list:
		case mc_cvalue_slice: {
			const mc_type_s* const type = self->types[index];
			const mc_type_s* const element = (mc_type_array == type->kind) ? type->as.array.element
				: (mc_type_slice == type->kind) ? type->as.slice.element : NULL;
			if (element != mc_scalar_type(c, mc_scalar_u8)) break;
			const mc_clist_s* const list = (mc_cvalue_list == value.kind) ? value.as.list : value.as.slice.list;
			const int64_t start = (mc_cvalue_list == value.kind) ? 0 : value.as.slice.start;
			const int64_t length = (mc_cvalue_list == value.kind) ? (int64_t)list->count : value.as.slice.length;
			uint8_t* const bytes = mc_arena_alloc(c->arena, (size_t)length + 1);
			for (int64_t i = 0; i < length && list != NULL; ++i) bytes[i] = (uint8_t)mc_i128_low(list->items[start + i].as.integer);
			constant.kind = mc_plugin_constant_bytes;
			constant.bytes = bytes;
			constant.length = (size_t)length;
			return constant;
		}
		default:
			break;
	}
	constant.kind = mc_plugin_constant_other;
	return constant;
}

static void _mc_check_mark(mc_plugin_check_s* const check, const size_t index, const uint32_t flags) {
	_mc_check_s* const self = _mc_check_of(check);
	mc_node_s* const node = self->node;
	if (NULL == node->note.flags) {
		node->note.flags = mc_arena_alloc(self->checker->arena, (self->count + 1) * sizeof(uint32_t));
		node->note.flag_count = (uint32_t)self->count;
	}
	node->note.flags[_mc_index(self, index)] = flags;
}

static mc_span_s _mc_span(_mc_check_s* const check, const size_t index) {
	if (MC_PLUGIN_CALL == index) return check->node->span;
	return check->arguments[_mc_index(check, index)]->span;
}

static mc_plugin_diagnostic_s* _mc_report(mc_plugin_check_s* const check, const bool warning, const char* const code, const size_t index,
										  const char* const message) {
	_mc_check_s* const self = _mc_check_of(check);
	mc_code_e parsed = (code != NULL) ? mc_code_parse(code) : mc_code_count_;
	if (mc_code_count_ == parsed || warning != ('W' == mc_code_text(parsed)[0])) parsed = warning ? mc_W0531 : mc_E0531;
	const mc_span_s span = _mc_span(self, index);
	mc_diagnostic_s* const diagnostic = warning ? mc_warning(self->checker->diag, parsed, span, "%s", message)
		: mc_error(self->checker->diag, parsed, span, "%s", message);
	mc_plugin_diagnostic_s* const handle = mc_arena_new(self->checker->arena, mc_plugin_diagnostic_s);
	handle->diagnostic = diagnostic;
	handle->check = self;
	handle->index = index;
	return handle;
}

static void _mc_label(mc_plugin_diagnostic_s* const diagnostic, const size_t index, const char* const text) {
	if (index == diagnostic->index && !diagnostic->main) {
		diagnostic->main = true;
		mc_label_main(diagnostic->diagnostic, "%s", text);
		return;
	}
	mc_label(diagnostic->diagnostic, _mc_span(diagnostic->check, index), "%s", text);
}

static void _mc_note(mc_plugin_diagnostic_s* const diagnostic, const char* const text) {
	mc_note(diagnostic->diagnostic, "%s", text);
}

static void _mc_help(mc_plugin_diagnostic_s* const diagnostic, const char* const text) {
	mc_help(diagnostic->diagnostic, "%s", text);
}

void mc_plugin_check_call(mc_checker_s* const c, mc_node_s* const node, mc_instance_s* const instance) {
	mc_function_s* const function = instance->function;
	mc_plugin_use(c, function, node->as.call.callee->span);
	char* const name = mc_function_qualified_name(function);
	const mc_plugin_function_s* const provided = mc_plugins_find(_mc_plugins(c), name);
	const mc_type_list_s* const parameters = &instance->parameters;
	const bool variadic = parameters->count > 0 && mc_type_is_special(mc_vector_last(parameters), mc_special_variadic);
	const size_t fixed = variadic ? parameters->count - 1 : parameters->count;
	const mc_nodes_s* const arguments = &node->as.call.arguments;
	const size_t extra = (variadic && node->note.variadic != NULL) ? node->note.variadic->count : 0;
	// a call with errors in its arguments is not checked further
	bool clean = NULL != provided && NULL != provided->check && arguments->count == fixed + extra;
	for (size_t i = 0; clean && i < arguments->count; ++i) {
		const mc_type_s* const type = arguments->data[i]->note.type;
		clean = type != NULL && !mc_type_is_special(type, mc_special_error);
	}
	if (!clean) {
		mc_free(name);
		return;
	}
	_mc_check_s check = {
		.public_ = { .interface = mc_plugin_table(), }, .checker = c, .node = node, .name = mc_arena_strndup(c->arena, name, strlen(name)),
		.count = arguments->count, .fixed = fixed, .instance = instance,
	};
	mc_free(name);
	check.arguments = mc_arena_alloc(c->arena, (check.count + 1) * sizeof(*check.arguments));
	check.types = mc_arena_alloc(c->arena, (check.count + 1) * sizeof(*check.types));
	for (size_t i = 0; i < check.count; ++i) {
		mc_node_s* const argument = arguments->data[i];
		check.arguments[i] = (mc_node_reference == argument->kind) ? argument->as.reference.operand : argument;
		check.types[i] = (i < fixed) ? parameters->data[i] : node->note.variadic->data[i - fixed];
		if (i < fixed && mc_type_reference == check.types[i]->kind) check.types[i] = check.types[i]->as.reference.element;
		// a string literal given for a []u8 parameter is seen as its array
		if (i < fixed && argument->note.format) check.types[i] = argument->note.type;
	}
	provided->check(&check.public_);
}

void mc_check_plugin_table(mc_plugin_interface_s* const table) {
	table->check_name = _mc_check_name;
	table->check_count = _mc_check_count;
	table->check_fixed_count = _mc_check_fixed_count;
	table->check_type = _mc_check_type;
	table->check_type_argument_count = _mc_check_type_argument_count;
	table->check_type_argument = _mc_check_type_argument;
	table->check_constant = _mc_check_constant;
	table->check_mark = _mc_check_mark;
	table->report = _mc_report;
	table->label = _mc_label;
	table->note = _mc_note;
	table->help = _mc_help;
}
