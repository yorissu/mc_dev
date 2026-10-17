
/// the compiler: a checked program -> bytecode. it follows the python
/// implementation instruction for instruction, so both produce the same
/// module (which the differential tests compare).

#include "check.h"
#include "mc/compiler.h"
#include "mc/format.h"

#include <setjmp.h>
#include <string.h>

typedef struct {
	const void* target;   // an instance or a lambda node
	int64_t     index ;
	bool        lambda;
} _mc_pending_s;

typedef struct {
	mc_program_s*                 program ;
	mc_checker_s*                 checker ;
	mc_module_bc_s*               module  ;
	mc_vector_of(_mc_pending_s)   functions;   // every function: target -> index
	size_t                        next    ;    // the next function to compile
	mc_vector_of(const mc_type_s*) type_keys;
	jmp_buf*                      stop    ;
	char*                         error   ;
} _mc_compiler_s;

mc_noreturn static void _mc_fail(_mc_compiler_s* const c, const char* const format, ...) mc_printf_like(2, 3);

mc_noreturn static void _mc_fail(_mc_compiler_s* const c, const char* const format, ...) {
	va_list args; va_start(args, format);
	c->error = mc_vformat(format, args);
	va_end(args);
	longjmp(*c->stop, 1);
}

// --- tables ----------------------------------------------------------------------------

static int64_t _mc_function(_mc_compiler_s* const c, const void* const target, const bool lambda) {
	for (size_t i = 0; i < c->functions.count; ++i) if (c->functions.data[i].target == target) return c->functions.data[i].index;
	mc_bc_function_s function = {0};
	if (!lambda) {
		const mc_instance_s* const instance = target;
		mc_string_s name = {0};
		mc_instance_text(&name, instance);
		function.name = mc_string_take(&name);
		function.parameters = (int64_t)instance->parameters.count;
	}
	else {
		const mc_node_s* const node = target;
		const mc_source_s* const source = mc_sources_get(c->program->sources, node->span.file);
		const mc_location_s location = mc_source_location(source, node->span.begin);
		const char* const slash = strrchr(source->path, '/');
		const char* const backslash = strrchr(source->path, '\\');
		const char* base = source->path;
		if (slash != NULL && slash + 1 > base) base = slash + 1;
		if (backslash != NULL && backslash + 1 > base) base = backslash + 1;
		function.name = mc_format("lambda at %s:%u", base, location.line);
		function.parameters = (int64_t)node->as.lambda.parameters.count;
	}
	const int64_t index = (int64_t)c->module->functions.count;
	mc_vector_push(&c->module->functions, function);
	const _mc_pending_s pending = { .target = target, .index = index, .lambda = lambda, };
	mc_vector_push(&c->functions, pending);
	return index;
}

static bool _mc_constant_equal(const mc_bc_constant_s* const a, const mc_bc_constant_s* const b) {
	if (a->kind != b->kind) return false;
	switch (a->kind) {
		case mc_constant_none:    return true;
		case mc_constant_integer: return a->integer == b->integer;
		case mc_constant_boolean: return a->boolean == b->boolean;
		case mc_constant_text:    return 0 == strcmp(a->text, b->text);
		case mc_constant_real: {
			mc_string_s left = {0}, right = {0};
			mc_format_float_repr(&left, a->real);
			mc_format_float_repr(&right, b->real);
			const bool equal = 0 == strcmp(mc_string_cstr(&left), mc_string_cstr(&right));
			mc_string_free(&left);
			mc_string_free(&right);
			return equal;
		}
		default: return false;
	}
}

/// adds a constant (taking ownership); scalars are shared.
static int64_t _mc_constant(_mc_compiler_s* const c, mc_bc_constant_s constant) {
	const bool scalar = mc_constant_none == constant.kind || mc_constant_integer == constant.kind || mc_constant_real == constant.kind
		|| mc_constant_boolean == constant.kind || mc_constant_text == constant.kind;
	if (scalar) {
		for (size_t i = 0; i < c->module->constants.count; ++i) {
			if (_mc_constant_equal(&c->module->constants.data[i], &constant)) {
				mc_bc_constant_free(&constant);
				return (int64_t)i;
			}
		}
	}
	mc_vector_push(&c->module->constants, constant);
	return (int64_t)c->module->constants.count - 1;
}

static mc_bc_constant_s _mc_integer_constant(const __int128 value) {
	return (mc_bc_constant_s) { .kind = mc_constant_integer, .integer = value, };
}

static mc_bc_constant_s _mc_real_constant(const double value) {
	return (mc_bc_constant_s) { .kind = mc_constant_real, .real = value, };
}

static mc_bc_constant_s _mc_boolean_constant(const bool value) {
	return (mc_bc_constant_s) { .kind = mc_constant_boolean, .boolean = value, };
}

static mc_bc_constant_s _mc_null_slice(void) {
	return (mc_bc_constant_s) { .kind = mc_constant_slice, .null = true, };
}

static int64_t _mc_describe(_mc_compiler_s* const c, const mc_type_s* const type);

static int64_t _mc_builtin(_mc_compiler_s* const c, const mc_instance_s* const instance, const mc_type_list_s* const extra) {
	mc_vector_of(int64_t) types = {0};
	for (size_t i = 0; i < instance->argument_count; ++i) mc_vector_push(&types, _mc_describe(c, instance->arguments[i]));
	if (extra != NULL) for (size_t i = 0; i < extra->count; ++i) mc_vector_push(&types, _mc_describe(c, extra->data[i]));
	const mc_view_s name = instance->function->node->as.function.name;
	for (size_t i = 0; i < c->module->builtins.count; ++i) {
		const mc_bc_builtin_s* const builtin = &c->module->builtins.data[i];
		if (!mc_view_eq_cstr(name, builtin->name) || builtin->types.count != types.count) continue;
		bool same = true;
		for (size_t t = 0; t < types.count; ++t) same = same && builtin->types.data[t] == types.data[t];
		if (same) {
			mc_vector_free(&types);
			return (int64_t)i;
		}
	}
	mc_bc_builtin_s builtin = { .name = mc_format(mc_view_fmt, mc_view_arg(name)), };
	for (size_t t = 0; t < types.count; ++t) mc_vector_push(&builtin.types, types.data[t]);
	mc_vector_free(&types);
	mc_vector_push(&c->module->builtins, builtin);
	return (int64_t)c->module->builtins.count - 1;
}

static int64_t _mc_native(_mc_compiler_s* const c, const mc_view_s name) {
	for (size_t i = 0; i < c->module->natives.count; ++i) if (mc_view_eq_cstr(name, c->module->natives.data[i])) return (int64_t)i;
	mc_vector_push(&c->module->natives, mc_format(mc_view_fmt, mc_view_arg(name)));
	return (int64_t)c->module->natives.count - 1;
}

static mc_position_s _mc_position(_mc_compiler_s* const c, const mc_span_s span, const bool has_span) {
	if (!has_span) return (mc_position_s) { .file = -1, };
	const mc_source_s* const source = mc_sources_get(c->program->sources, span.file);
	int32_t file = -1;
	for (size_t i = 0; i < c->module->files.count; ++i) if (0 == strcmp(c->module->files.data[i], source->path)) file = (int32_t)i;
	if (file < 0) {
		mc_vector_push(&c->module->files, mc_strdup(source->path));
		file = (int32_t)c->module->files.count - 1;
	}
	const mc_location_s first = mc_source_location(source, span.begin);
	const mc_location_s last = mc_source_location(source, span.end > span.begin ? span.end - 1 : span.begin);
	int64_t length = 0;
	if (last.line == first.line) {
		length = (int64_t)last.column - first.column + 1;
	}
	else {
		const mc_view_s line = mc_source_line(source, first.line);
		size_t points = 0;
		for (size_t i = 0; i < line.length; ++i) if (0x80 != ((uint8_t)line.data[i] & 0xC0)) ++points;
		length = (int64_t)points - first.column + 1;
	}
	return (mc_position_s) { .file = file, .line = first.line, .column = first.column, .length = (uint32_t)(length > 1 ? length : 1), };
}

// --- values ------------------------------------------------------------------------------

static mc_bc_constant_s _mc_convert(_mc_compiler_s* const c, const mc_cvalue_s* const value, const mc_type_s* const type);

static mc_bc_constant_s _mc_zero(_mc_compiler_s* const c, const mc_type_s* const type) {
	switch (type->kind) {
		case mc_type_scalar: {
			const char kind = mc_scalar_kind(type->as.scalar);
			if ('b' == kind) return _mc_boolean_constant(false);
			if ('f' == kind) return _mc_real_constant(0.0);
			return _mc_integer_constant(0);
		}
		case mc_type_slice:
			return _mc_null_slice();
		case mc_type_array: {
			mc_bc_constant_s list = { .kind = mc_constant_list, };
			for (int64_t i = 0; i < type->as.array.length; ++i) mc_vector_push(&list.items, _mc_zero(c, type->as.array.element));
			return list;
		}
		case mc_type_struct: {
			mc_bc_constant_s list = { .kind = mc_constant_list, };
			const mc_layout_s* const layout = mc_layout(c->checker, type);
			for (size_t i = 0; i < layout->fields.count; ++i) {
				const mc_field_s* const field = layout->fields.data[i];
				if (field->default_ != NULL) {
					const mc_evaluation_s result = mc_evaluate(&c->checker->evaluator, field->default_, false);
					if (result.status != mc_evaluation_ok) _mc_fail(c, "a field default cannot be evaluated");
					mc_vector_push(&list.items, _mc_convert(c, &result.value, field->type));
				}
				else {
					mc_vector_push(&list.items, _mc_zero(c, field->type));
				}
			}
			return list;
		}
		default:
			return (mc_bc_constant_s) { .kind = mc_constant_none, };
	}
}

static mc_bc_constant_s _mc_convert(_mc_compiler_s* const c, const mc_cvalue_s* const value, const mc_type_s* const type) {
	if (mc_type_is_untyped(type)) {
		if (mc_type_is_special(type, mc_special_untyped_float)) {
			return _mc_real_constant(mc_cvalue_real == value->kind ? value->as.real : (double)value->as.integer);
		}
		return (mc_cvalue_real == value->kind) ? _mc_real_constant(value->as.real) : _mc_integer_constant(value->as.integer);
	}
	switch (type->kind) {
		case mc_type_scalar: {
			const char kind = mc_scalar_kind(type->as.scalar);
			if ('b' == kind) return _mc_boolean_constant(mc_cvalue_boolean == value->kind ? value->as.boolean : value->as.integer != 0);
			if ('f' == kind) return _mc_real_constant(mc_float_wrap(mc_cvalue_real == value->kind ? value->as.real : (double)value->as.integer, type));
			return _mc_integer_constant(mc_integer_wrap(value->as.integer, type));
		}
		case mc_type_array: {
			mc_bc_constant_s list = { .kind = mc_constant_list, };
			for (size_t i = 0; mc_cvalue_list == value->kind && i < value->as.list->count; ++i) {
				mc_vector_push(&list.items, _mc_convert(c, &value->as.list->items[i], type->as.array.element));
			}
			return list;
		}
		case mc_type_slice: {
			if (value->kind != mc_cvalue_slice || NULL == value->as.slice.list || (0 == value->as.slice.list->count && 0 == value->as.slice.length)) {
				return _mc_null_slice();
			}
			mc_bc_constant_s slice = { .kind = mc_constant_slice, .start = value->as.slice.start, .length = value->as.slice.length, .read_only = true, };
			for (size_t i = 0; i < value->as.slice.list->count; ++i) mc_vector_push(&slice.items, _mc_convert(c, &value->as.slice.list->items[i], type->as.slice.element));
			return slice;
		}
		case mc_type_struct: {
			mc_bc_constant_s list = { .kind = mc_constant_list, };
			const mc_layout_s* const layout = mc_layout(c->checker, type);
			for (size_t i = 0; i < layout->fields.count && mc_cvalue_list == value->kind; ++i) {
				mc_vector_push(&list.items, _mc_convert(c, &value->as.list->items[i], layout->fields.data[i]->type));
			}
			return list;
		}
		case mc_type_function:
			if (mc_cvalue_function == value->kind) {
				if (value->as.function.lambda != NULL) {
					return (mc_bc_constant_s) { .kind = mc_constant_function, .integer = _mc_function(c, value->as.function.lambda, true), };
				}
				const mc_instance_s* const instance = value->as.function.instance;
				if (mc_function_is_external(instance->function)) {
					return (mc_bc_constant_s) { .kind = mc_constant_builtin, .integer = _mc_builtin(c, instance, NULL), };
				}
				return (mc_bc_constant_s) { .kind = mc_constant_function, .integer = _mc_function(c, instance, false), };
			}
			break;
		default:
			break;
	}
	_mc_fail(c, "cannot convert a constant of type `%s`", mc_name_of(c->checker, type));
}

static int64_t _mc_describe(_mc_compiler_s* const c, const mc_type_s* const type) {
	for (size_t i = 0; i < c->type_keys.count; ++i) if (c->type_keys.data[i] == type) return (int64_t)i;
	const int64_t index = (int64_t)c->module->types.count;
	mc_vector_push(&c->type_keys, type);
	mc_bc_type_s descriptor = { .zero = -1, .name = mc_type_name(type), };
	mc_vector_push(&c->module->types, descriptor);
	const char* kind = NULL;
	int64_t element = 0, length = 0;
	switch (type->kind) {
		case mc_type_scalar: {
			const char scalar = mc_scalar_kind(type->as.scalar);
			kind = ('b' == scalar) ? "bool" : ('f' == scalar) ? "float" : "int";
			break;
		}
		case mc_type_array:
			kind = "array";
			element = _mc_describe(c, type->as.array.element);
			length = type->as.array.length;
			break;
		case mc_type_slice:
			kind = "slice";
			element = _mc_describe(c, type->as.slice.element);
			break;
		case mc_type_struct: {
			kind = "struct";
			const mc_layout_s* const layout = mc_layout(c->checker, type);
			for (size_t i = 0; i < layout->fields.count; ++i) {
				const int64_t field_type = _mc_describe(c, layout->fields.data[i]->type);
				mc_bc_type_s* const self = &c->module->types.data[index];
				mc_vector_push(&self->field_names, mc_format(mc_view_fmt, mc_view_arg(layout->fields.data[i]->name)));
				mc_vector_push(&self->field_types, field_type);
			}
			break;
		}
		case mc_type_function:
			kind = "function";
			break;
		default:
			_mc_fail(c, "cannot describe `%s`", mc_name_of(c->checker, type));
	}
	const int64_t zero = (mc_type_function == type->kind) ? -1 : _mc_constant(c, _mc_zero(c, type));
	mc_bc_type_s* const self = &c->module->types.data[index];
	self->kind = mc_strdup(kind);
	self->element = element;
	self->length = length;
	self->zero = zero;
	return index;
}

// --- functions ---------------------------------------------------------------------------

typedef struct {
	const mc_variable_s* variable;
	int64_t              slot    ;
} _mc_slot_s;

typedef struct {
	mc_vector_of(int64_t) breaks   ;
	mc_vector_of(int64_t) continues;
	int64_t               target   ;   // where 'continue' jumps; -1: patched later
} _mc_loop_s;

typedef struct {
	_mc_compiler_s*          c       ;
	int64_t                  index   ;   // of the function being compiled
	mc_vector_of(_mc_slot_s) slots   ;
	int64_t                  count   ;
	mc_span_s                span    ;
	bool                     has_span;
	mc_vector_of(_mc_loop_s) loops   ;
} _mc_function_s;

static mc_bc_function_s* _mc_target(_mc_function_s* const f) {
	return &f->c->module->functions.data[f->index];
}

static int64_t _mc_new_slot(_mc_function_s* const f, const int64_t size) {
	const int64_t slot = f->count;
	f->count += size;
	return slot;
}

static bool _mc_has_slot(_mc_function_s* const f, const mc_variable_s* const variable, int64_t* const slot) {
	for (size_t i = 0; i < f->slots.count; ++i) {
		if (f->slots.data[i].variable == variable) {
			*slot = f->slots.data[i].slot;
			return true;
		}
	}
	return false;
}

static int64_t _mc_slot(_mc_function_s* const f, const mc_variable_s* const variable) {
	int64_t slot = 0;
	if (_mc_has_slot(f, variable, &slot)) return slot;
	const _mc_slot_s entry = { .variable = variable, .slot = _mc_new_slot(f, 1), };
	mc_vector_push(&f->slots, entry);
	return entry.slot;
}

static int64_t _mc_emit_full(_mc_function_s* const f, const mc_instruction_s instruction) {
	const mc_position_s position = _mc_position(f->c, f->span, f->has_span);
	mc_bc_function_s* const function = _mc_target(f);
	mc_vector_push(&function->code, instruction);
	mc_vector_push(&function->positions, position);
	return (int64_t)function->code.count - 1;
}

static int64_t _mc_emit(_mc_function_s* const f, const mc_opcode_e opcode) {
	const mc_instruction_s instruction = { .opcode = opcode, };
	return _mc_emit_full(f, instruction);
}

static int64_t _mc_emit_a(_mc_function_s* const f, const mc_opcode_e opcode, const int64_t a) {
	const mc_instruction_s instruction = { .opcode = opcode, .has_a = true, .a = a, };
	return _mc_emit_full(f, instruction);
}

static int64_t _mc_emit_ab(_mc_function_s* const f, const mc_opcode_e opcode, const int64_t a, const int64_t b) {
	const mc_instruction_s instruction = { .opcode = opcode, .has_a = true, .a = a, .has_b = true, .b = b, };
	return _mc_emit_full(f, instruction);
}

static int64_t _mc_here(_mc_function_s* const f) {
	return (int64_t)_mc_target(f)->code.count;
}

static void _mc_patch(_mc_function_s* const f, const int64_t index, const int64_t target) {
	mc_instruction_s* const instruction = &_mc_target(f)->code.data[index];
	if (mc_op_RANGE_TEST == instruction->opcode) instruction->b = target;
	else instruction->a = target;
}

static void _mc_push_constant(_mc_function_s* const f, const mc_bc_constant_s constant) {
	_mc_emit_a(f, mc_op_CONST, _mc_constant(f->c, constant));
}

static mc_type_code_e _mc_type_code(_mc_compiler_s* const c, const mc_type_s* const type) {
	if (mc_type_scalar == type->kind) {
		switch (type->as.scalar) {
#define mc_keyword(_name)
#define mc_punctuator(_name, _text)
#define mc_scalar(_name, _kind, _bits, _signed) case mc_scalar_##_name: return mc_code_##_name;
#include "mc/tokens.def"
#undef mc_scalar
#undef mc_punctuator
#undef mc_keyword
			default: break;
		}
	}
	if (mc_type_is_special(type, mc_special_untyped_integer)) return mc_code_int;
	if (mc_type_is_special(type, mc_special_untyped_float)) return mc_code_float;
	if (mc_type_function == type->kind) return mc_code_fn;
	_mc_fail(c, "no opcode type for `%s`", mc_name_of(c->checker, type));
}

static int64_t _mc_emit_typed(_mc_function_s* const f, const mc_opcode_e opcode, const mc_type_s* const type) {
	const mc_instruction_s instruction = { .opcode = opcode, .type = _mc_type_code(f->c, type), };
	return _mc_emit_full(f, instruction);
}

static mc_opcode_e _mc_operator_opcode(const mc_operator_e operator_) {
	switch (operator_) {
		case mc_operator_add:           return mc_op_ADD;
		case mc_operator_subtract:      return mc_op_SUB;
		case mc_operator_multiply:      return mc_op_MUL;
		case mc_operator_divide:        return mc_op_DIV;
		case mc_operator_remainder:     return mc_op_MOD;
		case mc_operator_bit_and:       return mc_op_AND;
		case mc_operator_bit_or:        return mc_op_OR;
		case mc_operator_bit_xor:       return mc_op_XOR;
		case mc_operator_shift_left:    return mc_op_SHL;
		case mc_operator_shift_right:   return mc_op_SHR;
		case mc_operator_equal:         return mc_op_EQ;
		case mc_operator_not_equal:     return mc_op_NE;
		case mc_operator_less:          return mc_op_LT;
		case mc_operator_greater:       return mc_op_GT;
		case mc_operator_less_equal:    return mc_op_LE;
		case mc_operator_greater_equal: return mc_op_GE;
		default:                        mc_panic("not an arithmetic operator");
	}
}

static void _mc_value(_mc_function_s* const f, const mc_node_s* const node);
static void _mc_statement(_mc_function_s* const f, const mc_node_s* const node);

static void _mc_block(_mc_function_s* const f, const mc_node_s* const node) {
	for (size_t i = 0; i < node->as.block.statements.count; ++i) _mc_statement(f, node->as.block.statements.data[i]);
}

// --- calls -------------------------------------------------------------------------------

static bool _mc_is_builtin(const mc_instance_s* const instance) {
	return NULL != mc_attribute_find(&instance->function->node->as.function.attributes, "builtin");
}

static void _mc_emit_call(_mc_function_s* const f, const mc_instance_s* const instance, const int64_t count, const mc_type_list_s* const extra) {
	const mc_node_s* const extern_ = mc_attribute_find(&instance->function->node->as.function.attributes, "extern");
	if (_mc_is_builtin(instance)) {
		_mc_emit_ab(f, mc_op_CALL_BUILTIN, _mc_builtin(f->c, instance, extra), count);
	}
	else if (extern_ != NULL) {
		_mc_emit_ab(f, mc_op_CALL_NATIVE, _mc_native(f->c, extern_->as.attribute.text), count);
	}
	else {
		_mc_emit_ab(f, mc_op_CALL, _mc_function(f->c, instance, false), count);
	}
}

static void _mc_function_value(_mc_function_s* const f, const mc_instance_s* const instance) {
	if (_mc_is_builtin(instance)) {
		_mc_emit_a(f, mc_op_FUNCTION_BUILTIN, _mc_builtin(f->c, instance, NULL));
	}
	else if (mc_attribute_find(&instance->function->node->as.function.attributes, "extern") != NULL) {
		_mc_fail(f->c, "extern functions cannot be used as values yet; wrap them in a function");
	}
	else {
		_mc_emit_a(f, mc_op_FUNCTION, _mc_function(f->c, instance, false));
	}
}

/// an operator implemented by a function: pushes the operands and calls it.
static bool _mc_operator(_mc_function_s* const f, const mc_node_s* const node, const mc_node_s* const first, const mc_node_s* const second) {
	const mc_instance_s* const call = node->note.operator_call;
	if (NULL == call) return false;
	if (NULL == second) {
		_mc_value(f, first);
	}
	else if (node->note.swapped) {
		_mc_value(f, second);
		_mc_value(f, first);
	}
	else {
		_mc_value(f, first);
		_mc_value(f, second);
	}
	_mc_emit_call(f, call, (NULL == second) ? 1 : 2, NULL);
	if (node->note.negated) _mc_emit(f, mc_op_NOT);
	return true;
}

// --- places ------------------------------------------------------------------------------

static int64_t _mc_field_index(_mc_function_s* const f, const mc_node_s* const node) {
	const mc_layout_s* const layout = mc_layout(f->c->checker, node->as.member.target->note.type);
	for (size_t i = 0; i < layout->fields.count; ++i) if (mc_view_eq(layout->fields.data[i]->name, node->as.member.name)) return (int64_t)i;
	_mc_fail(f->c, "no field `%.*s`", mc_view_arg(node->as.member.name));
}

/// pushes the location of a place (a temporary for other values).
static void _mc_location(_mc_function_s* const f, const mc_node_s* const node) {
	if (mc_node_paren == node->kind) {
		_mc_location(f, node->as.paren.inner);
		return;
	}
	const mc_variable_s* const variable = node->note.variable;
	if (mc_node_name == node->kind && variable != NULL) {
		_mc_emit_a(f, variable->reference ? mc_op_LOAD : mc_op_LOC_LOCAL, _mc_slot(f, variable));
		return;
	}
	if (mc_node_member == node->kind && node->note.field != NULL) {
		_mc_location(f, node->as.member.target);
		_mc_emit_a(f, mc_op_LOC_FIELD, _mc_field_index(f, node));
		return;
	}
	if (mc_node_index == node->kind && NULL == node->note.operator_call) {
		if (mc_type_array == node->as.index.target->note.type->kind) {
			_mc_location(f, node->as.index.target);
			_mc_value(f, node->as.index.index);
			_mc_emit(f, mc_op_LOC_INDEX_ARRAY);
		}
		else {
			_mc_value(f, node->as.index.target);
			_mc_value(f, node->as.index.index);
			_mc_emit(f, mc_op_LOC_INDEX_SLICE);
		}
		return;
	}
	_mc_value(f, node);
	_mc_emit(f, mc_op_TEMP_LOC);
}

/// whether a slice of the array 'node' must not be written through.
static bool _mc_read_only(const mc_node_s* const node) {
	switch (node->kind) {
		case mc_node_paren:  return _mc_read_only(node->as.paren.inner);
		case mc_node_name:   return NULL == node->note.variable || !node->note.variable->mutable_;
		case mc_node_member: return NULL == node->note.field || !node->note.field->mutable_ || _mc_read_only(node->as.member.target);
		case mc_node_index:  return mc_type_array == node->as.index.target->note.type->kind && _mc_read_only(node->as.index.target);
		case mc_node_string: return true;
		default:             return false;
	}
}

/// whether 'node' is constant data (a string literal or a constant).
static bool _mc_static(const mc_node_s* const node) {
	switch (node->kind) {
		case mc_node_paren:  return _mc_static(node->as.paren.inner);
		case mc_node_string: return true;
		case mc_node_name:
		case mc_node_member: return node->note.constant != NULL;
		default:             return false;
	}
}

// --- expressions -------------------------------------------------------------------------

/// a literal number as a constant of 'type'.
static void _mc_scalar(_mc_function_s* const f, const bool real, const __int128 integer, const double number, const mc_type_s* const type) {
	if (mc_type_scalar == type->kind) {
		const char kind = mc_scalar_kind(type->as.scalar);
		if ('b' == kind) _mc_push_constant(f, _mc_boolean_constant(real ? number != 0.0 : integer != 0));
		else if ('f' == kind) _mc_push_constant(f, _mc_real_constant(mc_float_wrap(real ? number : (double)integer, type)));
		else _mc_push_constant(f, _mc_integer_constant(mc_integer_wrap(integer, type)));
	}
	else if (real || mc_type_is_special(type, mc_special_untyped_float)) {
		_mc_push_constant(f, _mc_real_constant(real ? number : (double)integer));
	}
	else {
		_mc_push_constant(f, _mc_integer_constant(integer));
	}
}

static void _mc_symbol(_mc_function_s* const f, const mc_node_s* const node) {
	const mc_variable_s* const variable = node->note.variable;
	if (variable != NULL) {
		int64_t slot = 0;
		if (!_mc_has_slot(f, variable, &slot)) _mc_fail(f->c, "`%.*s` is not a local of this function", mc_view_arg(variable->name));
		_mc_emit_a(f, variable->reference ? mc_op_LOAD_DEREF : mc_op_LOAD, slot);
		return;
	}
	const mc_constant_s* const constant = node->note.constant;
	if (constant != NULL) {
		if (NULL == constant->value) _mc_fail(f->c, "the constant `%.*s` has no value", mc_view_arg(mc_declaration_name(constant->node)));
		const mc_type_s* const type = mc_type_is_untyped(node->note.type) ? constant->type : node->note.type;
		_mc_push_constant(f, _mc_convert(f->c, constant->value, type));
		return;
	}
	if (node->note.instance != NULL) {
		_mc_function_value(f, node->note.instance);
		return;
	}
	const mc_view_s text = mc_span_text(f->c->program->sources, node->span);
	_mc_fail(f->c, "cannot compile `%.*s`", mc_view_arg(text));
}

static void _mc_argument(_mc_function_s* const f, const mc_node_s* const argument, const mc_type_s* const parameter) {
	if (mc_type_reference == parameter->kind) _mc_location(f, mc_node_reference == argument->kind ? argument->as.reference.operand : argument);
	else _mc_value(f, argument);
}

static void _mc_call(_mc_function_s* const f, const mc_node_s* const node) {
	const mc_nodes_s* const arguments = &node->as.call.arguments;
	const mc_instance_s* const instance = node->note.instance;
	if (instance != NULL && instance->parameters.count > 0
		&& mc_type_is_special(mc_vector_last(&instance->parameters), mc_special_variadic)) {
		const size_t fixed = instance->parameters.count - 1;
		for (size_t i = 0; i < arguments->count && i < fixed; ++i) {
			const mc_node_s* const argument = arguments->data[i];
			const mc_type_s* const parameter = instance->parameters.data[i];
			if (mc_type_slice == parameter->kind && mc_type_array == argument->note.type->kind) {
				// a string literal given as the format: a slice of all of it
				_mc_location(f, argument);
				_mc_emit_a(f, mc_op_SLICE_LOC, (_mc_read_only(argument) ? mc_slice_read_only : 0) | (_mc_static(argument) ? mc_slice_static : 0));
			}
			else {
				_mc_argument(f, argument, parameter);
			}
		}
		for (size_t i = fixed; i < arguments->count; ++i) _mc_value(f, arguments->data[i]);
		_mc_emit_call(f, instance, (int64_t)arguments->count, node->note.variadic);
		return;
	}
	if (instance != NULL) {
		const mc_node_s* const declaration = (instance->node != NULL) ? instance->node : instance->function->node;
		for (size_t i = 0; i < instance->parameters.count; ++i) {
			if (i < arguments->count) _mc_argument(f, arguments->data[i], instance->parameters.data[i]);
			else _mc_value(f, declaration->as.function.parameters.data[i]->as.param.default_);
		}
		_mc_emit_call(f, instance, (int64_t)instance->parameters.count, NULL);
		return;
	}
	const mc_node_s* const callee = node->as.call.callee;
	_mc_value(f, callee);
	const mc_type_s* const type = callee->note.type;
	for (size_t i = 0; i < arguments->count && i < type->as.function.count; ++i) _mc_argument(f, arguments->data[i], type->as.function.parameters[i]);
	_mc_emit_a(f, mc_op_CALL_VALUE, (int64_t)arguments->count);
}

static void _mc_slice_of(_mc_function_s* const f, const mc_node_s* const node) {
	const mc_node_s* const target = node->as.slice_of.target;
	int64_t flags = (node->as.slice_of.low != NULL ? mc_slice_low : 0) | (node->as.slice_of.high != NULL ? mc_slice_high : 0)
		| (node->as.slice_of.inclusive ? mc_slice_inclusive : 0);
	mc_opcode_e opcode = mc_op_SLICE_SLICE;
	if (mc_type_slice == target->note.type->kind) {
		_mc_value(f, target);
	}
	else {
		_mc_location(f, target);
		if (_mc_read_only(target)) flags |= mc_slice_read_only;
		if (_mc_static(target)) flags |= mc_slice_static;
		opcode = mc_op_SLICE_LOC;
	}
	if (node->as.slice_of.low != NULL) _mc_value(f, node->as.slice_of.low);
	if (node->as.slice_of.high != NULL) _mc_value(f, node->as.slice_of.high);
	_mc_emit_a(f, opcode, flags);
}

static void _mc_unary(_mc_function_s* const f, const mc_node_s* const node) {
	const mc_node_s* const operand = node->as.unary.operand;
	if (_mc_operator(f, node, operand, NULL)) return;
	const mc_operator_e operator_ = node->as.unary.operator_;
	if (mc_operator_negate == operator_ && mc_node_integer == operand->kind) {
		_mc_scalar(f, false, -(__int128)operand->as.integer.value, 0.0, node->note.type);
		return;
	}
	if (mc_operator_negate == operator_ && mc_node_float == operand->kind) {
		_mc_scalar(f, true, 0, -operand->as.float_.value, node->note.type);
		return;
	}
	_mc_value(f, operand);
	if (mc_operator_not == operator_) _mc_emit(f, mc_op_NOT);
	else _mc_emit_typed(f, mc_operator_negate == operator_ ? mc_op_NEG : mc_op_BITNOT, node->note.type);
}

static void _mc_binary(_mc_function_s* const f, const mc_node_s* const node) {
	const mc_operator_e operator_ = node->as.binary.operator_;
	const mc_node_s* const left = node->as.binary.left;
	const mc_node_s* const right = node->as.binary.right;
	if (mc_operator_and == operator_ || mc_operator_or == operator_) {
		_mc_value(f, left);
		_mc_emit(f, mc_op_DUP);
		const int64_t jump = _mc_emit_a(f, mc_operator_and == operator_ ? mc_op_JUMP_IF_FALSE : mc_op_JUMP_IF_TRUE, 0);
		_mc_emit(f, mc_op_POP);
		_mc_value(f, right);
		_mc_patch(f, jump, _mc_here(f));
		return;
	}
	if (_mc_operator(f, node, left, right)) return;
	const bool left_slice = mc_type_slice == left->note.type->kind;
	const bool right_slice = mc_type_slice == right->note.type->kind;
	if ((mc_operator_equal == operator_ || mc_operator_not_equal == operator_) && (left_slice || right_slice)) {
		_mc_value(f, (right_slice && !left_slice) ? right : left);
		_mc_emit(f, mc_op_SLICE_IS_NULL);
		if (mc_operator_not_equal == operator_) _mc_emit(f, mc_op_NOT);
		return;
	}
	_mc_value(f, left);
	_mc_value(f, right);
	_mc_emit_typed(f, _mc_operator_opcode(operator_), left->note.type);
}

static void _mc_composite(_mc_function_s* const f, const mc_node_s* const node) {
	const mc_type_s* const type = node->note.type;
	const mc_nodes_s* const entries = &node->as.composite.entries;
	if (0 == entries->count) {
		_mc_push_constant(f, _mc_zero(f->c, type));
		return;
	}
	if (mc_type_struct == type->kind) {
		const mc_layout_s* const layout = mc_layout(f->c->checker, type);
		_mc_push_constant(f, _mc_zero(f->c, type));
		for (size_t i = 0; i < entries->count; ++i) {
			const mc_node_s* const entry = entries->data[i];
			int64_t index = -1;
			for (size_t k = 0; k < layout->fields.count; ++k) if (mc_view_eq(layout->fields.data[k]->name, entry->as.entry.name)) index = (int64_t)k;
			if (index < 0) _mc_fail(f->c, "no field `%.*s`", mc_view_arg(entry->as.entry.name));
			_mc_value(f, entry->as.entry.value);
			_mc_emit_a(f, mc_op_SET_FIELD, index);
		}
		return;
	}
	for (size_t i = 0; i < entries->count; ++i) _mc_value(f, entries->data[i]->as.entry.value);
	_mc_emit_a(f, mc_op_MAKE_ARRAY, (int64_t)entries->count);
	if (mc_type_slice == type->kind) {
		_mc_emit(f, mc_op_TEMP_LOC);
		_mc_emit_a(f, mc_op_SLICE_LOC, 0);
	}
}

static void _mc_assert(_mc_function_s* const f, const mc_node_s* const node) {
	_mc_value(f, node->as.assert_.condition);
	const int64_t skip = _mc_emit_a(f, mc_op_JUMP_IF_TRUE, 0);
	const mc_node_s* const message = node->as.assert_.message;
	mc_bc_constant_s text = { .kind = mc_constant_text, };
	if (message != NULL && mc_node_string == message->kind) {
		mc_string_s decoded = {0};
		mc_format_utf8(&decoded, message->as.string.data);
		text.text = mc_string_take(&decoded);
	}
	else {
		const mc_view_s condition = mc_span_text(f->c->program->sources, node->as.assert_.condition->span);
		text.text = mc_format("assertion failed: " mc_view_fmt, mc_view_arg(condition));
	}
	_mc_emit_a(f, mc_op_ASSERT_FAIL, _mc_constant(f->c, text));
	_mc_patch(f, skip, _mc_here(f));
	_mc_emit(f, mc_op_NONE);
}

static void _mc_lenof(_mc_function_s* const f, const mc_node_s* const node) {
	const mc_node_s* const operand = node->as.lenof.operand;
	const mc_type_s* const type = operand->note.type;
	if (mc_type_array == type->kind) {
		const bool plain = mc_node_name == operand->kind || mc_node_string == operand->kind || mc_node_member == operand->kind || mc_node_paren == operand->kind;
		if (!plain) {
			_mc_value(f, operand);
			_mc_emit(f, mc_op_POP);
		}
		_mc_push_constant(f, _mc_integer_constant(type->as.array.length));
		return;
	}
	_mc_value(f, operand);
	_mc_emit(f, mc_op_LEN_SLICE);
}

static void _mc_expression(_mc_function_s* const f, const mc_node_s* const node) {
	switch (node->kind) {
		case mc_node_integer: _mc_scalar(f, false, (__int128)node->as.integer.value, 0.0, node->note.type); break;
		case mc_node_float:   _mc_scalar(f, true, 0, node->as.float_.value, node->note.type); break;
		case mc_node_char:    _mc_push_constant(f, _mc_integer_constant(node->as.char_.value)); break;
		case mc_node_bool:    _mc_push_constant(f, _mc_boolean_constant(node->as.bool_.value)); break;
		case mc_node_null:    _mc_push_constant(f, _mc_null_slice()); break;
		case mc_node_string: {
			mc_bc_constant_s list = { .kind = mc_constant_list, };
			const mc_view_s data = node->as.string.data;
			for (size_t i = 0; i < data.length; ++i) mc_vector_push(&list.items, _mc_integer_constant((uint8_t)data.data[i]));
			_mc_push_constant(f, list);
			break;
		}
		case mc_node_paren:   _mc_value(f, node->as.paren.inner); break;
		case mc_node_name:    _mc_symbol(f, node); break;
		case mc_node_member:
			if (NULL == node->note.field) {
				_mc_symbol(f, node);
				break;
			}
			_mc_value(f, node->as.member.target);
			_mc_emit_a(f, mc_op_FIELD, _mc_field_index(f, node));
			break;
		case mc_node_specialize: _mc_function_value(f, node->note.instance); break;
		case mc_node_lambda:  _mc_emit_a(f, mc_op_FUNCTION, _mc_function(f->c, node, true)); break;
		case mc_node_call:    _mc_call(f, node); break;
		case mc_node_index:
			if (_mc_operator(f, node, node->as.index.target, node->as.index.index)) break;
			_mc_value(f, node->as.index.target);
			_mc_value(f, node->as.index.index);
			_mc_emit(f, mc_type_array == node->as.index.target->note.type->kind ? mc_op_INDEX_ARRAY : mc_op_INDEX_SLICE);
			break;
		case mc_node_slice_of: _mc_slice_of(f, node); break;
		case mc_node_unary:   _mc_unary(f, node); break;
		case mc_node_binary:  _mc_binary(f, node); break;
		case mc_node_cast: {
			_mc_value(f, node->as.cast.operand);
			const mc_type_code_e source = _mc_type_code(f->c, node->as.cast.operand->note.type);
			const mc_type_code_e target = _mc_type_code(f->c, node->note.type);
			if (source != target) {
				const mc_instruction_s instruction = { .opcode = mc_op_CAST, .type = source, .target = target, };
				_mc_emit_full(f, instruction);
			}
			break;
		}
		case mc_node_reference: _mc_fail(f->c, "a reference outside of a call argument");
		case mc_node_composite: _mc_composite(f, node); break;
		case mc_node_sizeof:  _mc_push_constant(f, _mc_integer_constant((__int128)node->note.size)); break;
		case mc_node_lenof:   _mc_lenof(f, node); break;
		case mc_node_assert:  _mc_assert(f, node); break;
		default: _mc_fail(f->c, "cannot compile a `%s` node", mc_node_kind_name(node->kind));
	}
}

static void _mc_value(_mc_function_s* const f, const mc_node_s* const node) {
	const mc_span_s saved = f->span;
	const bool had = f->has_span;
	f->span = node->span;
	f->has_span = true;
	_mc_expression(f, node);
	f->span = saved;
	f->has_span = had;
}

// --- statements --------------------------------------------------------------------------

static void _mc_assign(_mc_function_s* const f, const mc_node_s* const node) {
	const mc_node_s* const target = node->as.assign.target;
	const mc_node_s* const value = node->as.assign.value;
	if (mc_node_name == target->kind && mc_view_eq_cstr(target->as.name.name, "_")) {
		_mc_value(f, value);
		_mc_emit(f, mc_op_POP);
		return;
	}
	if (mc_operator_assign == node->as.assign.operator_) {
		const mc_variable_s* const variable = target->note.variable;
		if (mc_node_name == target->kind && variable != NULL && !variable->reference) {
			_mc_value(f, value);
			_mc_emit_a(f, mc_op_STORE, _mc_slot(f, variable));
			return;
		}
		_mc_location(f, target);
		_mc_value(f, value);
		_mc_emit(f, mc_op_STORE_LOC);
		return;
	}
	_mc_location(f, target);
	_mc_emit(f, mc_op_DUP);
	_mc_emit(f, mc_op_LOAD_LOC);
	_mc_value(f, value);
	if (node->note.operator_call != NULL) _mc_emit_call(f, node->note.operator_call, 2, NULL);
	else _mc_emit_typed(f, _mc_operator_opcode(node->as.assign.operator_), target->note.type);
	_mc_emit(f, mc_op_STORE_LOC);
}

static void _mc_finish_loop(_mc_function_s* const f, const int64_t end) {
	_mc_loop_s loop = mc_vector_pop(&f->loops);
	for (size_t i = 0; i < loop.breaks.count; ++i) _mc_patch(f, loop.breaks.data[i], end);
	mc_vector_free(&loop.breaks);
	mc_vector_free(&loop.continues);
}

static void _mc_open_loop(_mc_function_s* const f, const int64_t leave, const int64_t target) {
	_mc_loop_s loop = { .target = target, };
	mc_vector_push(&loop.breaks, leave);
	mc_vector_push(&f->loops, loop);
}

static void _mc_if(_mc_function_s* const f, const mc_node_s* const node) {
	_mc_value(f, node->as.if_.condition);
	const int64_t skip = _mc_emit_a(f, mc_op_JUMP_IF_FALSE, 0);
	_mc_block(f, node->as.if_.then);
	const mc_node_s* const otherwise = node->as.if_.otherwise;
	if (NULL == otherwise) {
		_mc_patch(f, skip, _mc_here(f));
		return;
	}
	const int64_t end = _mc_emit_a(f, mc_op_JUMP, 0);
	_mc_patch(f, skip, _mc_here(f));
	if (mc_node_if == otherwise->kind) _mc_statement(f, otherwise);
	else _mc_block(f, otherwise);
	_mc_patch(f, end, _mc_here(f));
}

static void _mc_for(_mc_function_s* const f, const mc_node_s* const node) {
	const int64_t base = _mc_new_slot(f, 3);
	_mc_value(f, node->as.for_.start);
	_mc_value(f, node->as.for_.end);
	const mc_instruction_s init = { .opcode = mc_op_RANGE_INIT, .has_a = true, .a = base, .has_b = true, .b = node->as.for_.inclusive, .b_bool = true, };
	_mc_emit_full(f, init);
	const int64_t test = _mc_here(f);
	const int64_t leave = _mc_emit_ab(f, mc_op_RANGE_TEST, base, 0);
	_mc_emit_a(f, mc_op_LOAD, base);
	_mc_emit_a(f, mc_op_STORE_NEW, _mc_slot(f, node->note.variable));
	_mc_open_loop(f, leave, -1);
	_mc_block(f, node->as.for_.body);
	const int64_t step = _mc_here(f);
	const _mc_loop_s* const loop = &mc_vector_last(&f->loops);
	for (size_t i = 0; i < loop->continues.count; ++i) _mc_patch(f, loop->continues.data[i], step);
	_mc_emit_a(f, mc_op_RANGE_STEP, base);
	_mc_emit_a(f, mc_op_JUMP, test);
	_mc_finish_loop(f, _mc_here(f));
}

static void _mc_statement_kind(_mc_function_s* const f, const mc_node_s* const node) {
	switch (node->kind) {
		case mc_node_declaration_statement: break;
		case mc_node_block: _mc_block(f, node); break;
		case mc_node_let: {
			if (node->as.let.value != NULL) _mc_value(f, node->as.let.value);
			else _mc_push_constant(f, _mc_zero(f->c, node->note.variable->type));
			_mc_emit_a(f, mc_op_STORE_NEW, _mc_slot(f, node->note.variable));
			break;
		}
		case mc_node_expression_statement:
			_mc_value(f, node->as.expression_statement.expression);
			_mc_emit(f, mc_op_POP);
			break;
		case mc_node_assign: _mc_assign(f, node); break;
		case mc_node_if: _mc_if(f, node); break;
		case mc_node_while: {
			const int64_t start = _mc_here(f);
			_mc_value(f, node->as.while_.condition);
			const int64_t leave = _mc_emit_a(f, mc_op_JUMP_IF_FALSE, 0);
			_mc_open_loop(f, leave, start);
			_mc_block(f, node->as.while_.body);
			_mc_emit_a(f, mc_op_JUMP, start);
			_mc_finish_loop(f, _mc_here(f));
			break;
		}
		case mc_node_for: _mc_for(f, node); break;
		case mc_node_break: {
			const int64_t jump = _mc_emit_a(f, mc_op_JUMP, 0);
			mc_vector_push(&mc_vector_last(&f->loops).breaks, jump);
			break;
		}
		case mc_node_continue: {
			_mc_loop_s* const loop = &mc_vector_last(&f->loops);
			if (loop->target >= 0) {
				_mc_emit_a(f, mc_op_JUMP, loop->target);
			}
			else {
				const int64_t jump = _mc_emit_a(f, mc_op_JUMP, 0);
				mc_vector_push(&mc_vector_last(&f->loops).continues, jump);
			}
			break;
		}
		case mc_node_return:
			if (node->as.return_.value != NULL) _mc_value(f, node->as.return_.value);
			else _mc_emit(f, mc_op_NONE);
			_mc_emit(f, mc_op_RETURN);
			break;
		default: _mc_fail(f->c, "cannot compile a `%s` statement", mc_node_kind_name(node->kind));
	}
}

static void _mc_statement(_mc_function_s* const f, const mc_node_s* const node) {
	const mc_span_s saved = f->span;
	const bool had = f->has_span;
	f->span = node->span;
	f->has_span = true;
	_mc_statement_kind(f, node);
	f->span = saved;
	f->has_span = had;
}

static void _mc_function_free(_mc_function_s* const f) {
	for (size_t i = 0; i < f->loops.count; ++i) {
		mc_vector_free(&f->loops.data[i].breaks);
		mc_vector_free(&f->loops.data[i].continues);
	}
	mc_vector_free(&f->loops);
	mc_vector_free(&f->slots);
}

static void _mc_compile_function(_mc_compiler_s* const c, _mc_function_s* const f, const _mc_pending_s pending) {
	const mc_node_s* body = NULL;
	const mc_nodes_s* parameters = NULL;
	bool returns = false;
	if (pending.lambda) {
		const mc_node_s* const node = pending.target;
		parameters = &node->as.lambda.parameters;
		body = node->as.lambda.body;
		returns = node->as.lambda.result != NULL;
	}
	else {
		const mc_instance_s* const instance = pending.target;
		const mc_node_s* const node = (instance->node != NULL) ? instance->node : instance->function->node;
		parameters = &node->as.function.parameters;
		body = node->as.function.body;
		returns = instance->result != NULL;
	}
	*f = (_mc_function_s) { .c = c, .index = pending.index, };
	for (size_t i = 0; i < parameters->count; ++i) {
		const _mc_slot_s slot = { .variable = parameters->data[i]->note.variable, .slot = _mc_new_slot(f, 1), };
		mc_vector_push(&f->slots, slot);
	}
	_mc_target(f)->returns = returns;
	if (NULL == body) _mc_fail(c, "`%s` has no body", _mc_target(f)->name);
	f->span = body->span;
	f->has_span = true;
	_mc_block(f, body);
	f->span = (mc_span_s) { .file = body->span.file, .begin = body->span.end - 1, .end = body->span.end, };
	_mc_emit(f, mc_op_NONE);
	_mc_emit(f, mc_op_RETURN);
	_mc_target(f)->locals = f->count;
	_mc_function_free(f);
}

/// compiles 'main' and every function it reaches.
static void _mc_compile_all(_mc_compiler_s* const c, _mc_function_s* const f) {
	const mc_symbol_s* const symbol = mc_map_get_cstr(&c->program->entry->symbols, "main");
	const mc_function_s* const main = (symbol != NULL && mc_symbol_function == symbol->kind) ? (const mc_function_s*)symbol : NULL;
	const mc_instance_s* entry = NULL;
	for (size_t i = 0; main != NULL && i < main->instances.count; ++i) if (0 == main->instances.data[i]->argument_count) entry = main->instances.data[i];
	if (NULL == entry) _mc_fail(c, "the program has no `main` function");
	c->module->entry = _mc_function(c, entry, false);
	c->module->entry_returns = entry->result != NULL;
	while (c->next < c->functions.count) {
		const _mc_pending_s pending = c->functions.data[c->next++];
		_mc_compile_function(c, f, pending);
	}
}

bool mc_compile(mc_program_s* const program, mc_module_bc_s* const module, char** const error) {
	*module = (mc_module_bc_s) {0};
	jmp_buf stop;
	_mc_compiler_s c = { .program = program, .checker = program->checker, .module = module, .stop = &stop, };
	_mc_function_s f = {0};
	if (0 == setjmp(stop)) _mc_compile_all(&c, &f);
	_mc_function_free(&f);
	mc_vector_free(&c.functions);
	mc_vector_free(&c.type_keys);
	if (c.error != NULL) {
		*error = c.error;
		mc_module_bc_free(module);
		*module = (mc_module_bc_s) {0};
		return false;
	}
	return true;
}
