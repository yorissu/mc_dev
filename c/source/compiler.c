
/// the compiler: a checked program -> bytecode.
///
/// every function gets a frame laid out here: its parameters first, then a
/// slot for every local and every temporary, each at an offset aligned for
/// its type. values are moved through the operand stack in words (see
/// bytecode.h); places (locals, fields, elements) are read and written
/// where they are, through constant offsets when the place is a local and
/// through locations otherwise.

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

// --- types ------------------------------------------------------------------------------

static uint64_t _mc_size(_mc_compiler_s* const c, const mc_type_s* const type, uint64_t* const alignment) {
	uint64_t ignored = 0;
	uint64_t* const align = (alignment != NULL) ? alignment : &ignored;
	if (mc_type_reference == type->kind) {
		*align = 8;
		return 16;
	}
	if (mc_type_is_special(type, mc_special_nothing)) {
		*align = 1;
		return 0;
	}
	if (mc_type_is_special(type, mc_special_null)) {
		*align = 8;
		return 16;
	}
	if (mc_type_is_untyped(type)) {
		*align = 8;
		return 8;
	}
	const uint64_t size = mc_size_of(c->checker, type, align);
	if (0 == *align) *align = 1;
	return size;
}

/// the words a value of 'type' takes on the operand stack.
static int64_t _mc_words(_mc_compiler_s* const c, const mc_type_s* const type) {
	if (NULL == type) return 0;
	return (int64_t)((_mc_size(c, type, NULL) + 7) / 8);
}

/// the type code of a scalar or function type (the 'T' of typed instructions).
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

/// how a value of 'type' is loaded and stored: a type code, or
/// mc_type_code_count_ for the bytes of a struct or an array.
static mc_type_code_e _mc_memory_code(_mc_compiler_s* const c, const mc_type_s* const type) {
	if (mc_type_slice == type->kind || mc_type_reference == type->kind || mc_type_is_special(type, mc_special_null)) return mc_code_slice;
	if (mc_type_array == type->kind || mc_type_struct == type->kind) return mc_type_code_count_;
	return _mc_type_code(c, type);
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
		function.parameters = (uint32_t)instance->parameters.count;
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
		function.parameters = (uint32_t)node->as.lambda.parameters.count;
	}
	const int64_t index = (int64_t)c->module->functions.count;
	mc_vector_push(&c->module->functions, function);
	const _mc_pending_s pending = { .target = target, .index = index, .lambda = lambda, };
	mc_vector_push(&c->functions, pending);
	return index;
}

static bool _mc_constant_equal(const mc_bc_constant_s* const a, const mc_bc_constant_s* const b) {
	if (a->kind != b->kind || a->type != b->type) return false;
	switch (a->kind) {
		case mc_constant_none:      return true;
		case mc_constant_integer:   return a->integer == b->integer && a->signed_ == b->signed_;
		case mc_constant_boolean:   return a->boolean == b->boolean;
		case mc_constant_text:      return 0 == strcmp(a->text, b->text);
		case mc_constant_real:      return 0 == memcmp(&a->real, &b->real, sizeof(a->real));
		case mc_constant_function:
		case mc_constant_intrinsic: return a->integer == b->integer;
		case mc_constant_slice:
			if (a->null != b->null || a->read_only != b->read_only || a->start != b->start || a->length != b->length) return false;
			break;
		case mc_constant_list:
			break;
	}
	if (a->items.count != b->items.count) return false;
	for (size_t i = 0; i < a->items.count; ++i) if (!_mc_constant_equal(&a->items.data[i], &b->items.data[i])) return false;
	return true;
}

/// adds a constant (taking ownership); equal constants are shared.
static int64_t _mc_constant(_mc_compiler_s* const c, mc_bc_constant_s constant) {
	for (size_t i = 0; i < c->module->constants.count; ++i) {
		if (_mc_constant_equal(&c->module->constants.data[i], &constant)) {
			mc_bc_constant_free(&constant);
			return (int64_t)i;
		}
	}
	mc_vector_push(&c->module->constants, constant);
	return (int64_t)c->module->constants.count - 1;
}

static mc_bc_constant_s _mc_integer_constant(const __int128 value, const bool signed_) {
	return (mc_bc_constant_s) { .kind = mc_constant_integer, .type = -1, .integer = (uint64_t)value, .signed_ = signed_, };
}

static mc_bc_constant_s _mc_real_constant(const double value) {
	return (mc_bc_constant_s) { .kind = mc_constant_real, .type = -1, .real = value, };
}

static mc_bc_constant_s _mc_boolean_constant(const bool value) {
	return (mc_bc_constant_s) { .kind = mc_constant_boolean, .type = -1, .boolean = value, };
}

static mc_bc_constant_s _mc_null_slice(void) {
	return (mc_bc_constant_s) { .kind = mc_constant_slice, .type = -1, .null = true, };
}

static int64_t _mc_describe(_mc_compiler_s* const c, const mc_type_s* const type);

static void _mc_describe_all(_mc_compiler_s* const c, const mc_type_s* const* const types, const size_t count, mc_bc_indexes_s* const out) {
	for (size_t i = 0; i < count; ++i) {
		const mc_type_s* type = types[i];
		if (mc_type_reference == type->kind) type = type->as.reference.element;
		if (mc_type_is_special(type, mc_special_variadic)) continue;
		mc_vector_push(out, _mc_describe(c, type));
	}
}

/// the intrinsic table entry of a use of an external function.
static int64_t _mc_intrinsic(_mc_compiler_s* const c, const mc_instance_s* const instance, const mc_type_list_s* const variadic,
							 const mc_node_s* const call) {
	mc_bc_intrinsic_s entry = { .name = mc_function_qualified_name(instance->function), .result = -1, };
	if (instance->result != NULL) entry.result = _mc_describe(c, instance->result);
	if (call != NULL) for (uint32_t i = 0; i < call->note.flag_count; ++i) mc_vector_push(&entry.flags, call->note.flags[i]);
	_mc_describe_all(c, instance->arguments, instance->argument_count, &entry.arguments);
	_mc_describe_all(c, instance->parameters.data, instance->parameters.count, &entry.parameters);
	if (variadic != NULL) _mc_describe_all(c, variadic->data, variadic->count, &entry.variadic);
	for (size_t i = 0; i < c->module->intrinsics.count; ++i) {
		const mc_bc_intrinsic_s* const other = &c->module->intrinsics.data[i];
		bool same = 0 == strcmp(other->name, entry.name) && other->result == entry.result && other->arguments.count == entry.arguments.count
			&& other->parameters.count == entry.parameters.count && other->variadic.count == entry.variadic.count;
		for (size_t t = 0; same && t < entry.arguments.count; ++t) same = other->arguments.data[t] == entry.arguments.data[t];
		for (size_t t = 0; same && t < entry.parameters.count; ++t) same = other->parameters.data[t] == entry.parameters.data[t];
		for (size_t t = 0; same && t < entry.variadic.count; ++t) same = other->variadic.data[t] == entry.variadic.data[t];
		same = same && other->flags.count == entry.flags.count;
		for (size_t t = 0; same && t < entry.flags.count; ++t) same = other->flags.data[t] == entry.flags.data[t];
		if (same) {
			mc_free(entry.name);
			mc_vector_free(&entry.arguments);
			mc_vector_free(&entry.parameters);
			mc_vector_free(&entry.variadic);
			mc_vector_free(&entry.flags);
			return (int64_t)i;
		}
	}
	mc_vector_push(&c->module->intrinsics, entry);
	return (int64_t)c->module->intrinsics.count - 1;
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

/// whether the zero value of 'type' has bytes that are not zero (field defaults).
static bool _mc_zero_has_defaults(_mc_compiler_s* const c, const mc_type_s* const type) {
	if (mc_type_array == type->kind) return type->as.array.length > 0 && _mc_zero_has_defaults(c, type->as.array.element);
	if (mc_type_struct != type->kind) return false;
	const mc_layout_s* const layout = mc_layout(c->checker, type);
	for (size_t i = 0; i < layout->fields.count; ++i) {
		if (layout->fields.data[i]->default_ != NULL || _mc_zero_has_defaults(c, layout->fields.data[i]->type)) return true;
	}
	return false;
}

static mc_bc_constant_s _mc_zero(_mc_compiler_s* const c, const mc_type_s* const type) {
	switch (type->kind) {
		case mc_type_scalar: {
			const char kind = mc_scalar_kind(type->as.scalar);
			if ('b' == kind) return _mc_boolean_constant(false);
			if ('f' == kind) return _mc_real_constant(0.0);
			return _mc_integer_constant(0, mc_scalar_signed(type->as.scalar));
		}
		case mc_type_slice:
			return _mc_null_slice();
		case mc_type_array: {
			mc_bc_constant_s list = { .kind = mc_constant_list, .type = _mc_describe(c, type), };
			for (int64_t i = 0; i < type->as.array.length; ++i) mc_vector_push(&list.items, _mc_zero(c, type->as.array.element));
			return list;
		}
		case mc_type_struct: {
			mc_bc_constant_s list = { .kind = mc_constant_list, .type = _mc_describe(c, type), };
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
			return (mc_bc_constant_s) { .kind = mc_constant_none, .type = -1, };
	}
}

static mc_bc_constant_s _mc_convert(_mc_compiler_s* const c, const mc_cvalue_s* const value, const mc_type_s* const type) {
	if (mc_type_is_untyped(type)) {
		if (mc_type_is_special(type, mc_special_untyped_float)) {
			return _mc_real_constant(mc_cvalue_real == value->kind ? value->as.real : (double)value->as.integer);
		}
		return (mc_cvalue_real == value->kind) ? _mc_real_constant(value->as.real) : _mc_integer_constant(value->as.integer, true);
	}
	switch (type->kind) {
		case mc_type_scalar: {
			const char kind = mc_scalar_kind(type->as.scalar);
			if ('b' == kind) return _mc_boolean_constant(mc_cvalue_boolean == value->kind ? value->as.boolean : value->as.integer != 0);
			if ('f' == kind) return _mc_real_constant(mc_float_wrap(mc_cvalue_real == value->kind ? value->as.real : (double)value->as.integer, type));
			return _mc_integer_constant(mc_integer_wrap(value->as.integer, type), mc_scalar_signed(type->as.scalar));
		}
		case mc_type_array: {
			mc_bc_constant_s list = { .kind = mc_constant_list, .type = _mc_describe(c, type), };
			for (size_t i = 0; mc_cvalue_list == value->kind && i < value->as.list->count; ++i) {
				mc_vector_push(&list.items, _mc_convert(c, &value->as.list->items[i], type->as.array.element));
			}
			return list;
		}
		case mc_type_slice: {
			if (value->kind != mc_cvalue_slice || NULL == value->as.slice.list || (0 == value->as.slice.list->count && 0 == value->as.slice.length)) {
				return _mc_null_slice();
			}
			mc_bc_constant_s slice = { .kind = mc_constant_slice, .type = _mc_describe(c, type), .start = value->as.slice.start,
				.length = value->as.slice.length, .read_only = true, };
			for (size_t i = 0; i < value->as.slice.list->count; ++i) mc_vector_push(&slice.items, _mc_convert(c, &value->as.slice.list->items[i], type->as.slice.element));
			return slice;
		}
		case mc_type_struct: {
			mc_bc_constant_s list = { .kind = mc_constant_list, .type = _mc_describe(c, type), };
			const mc_layout_s* const layout = mc_layout(c->checker, type);
			for (size_t i = 0; i < layout->fields.count && mc_cvalue_list == value->kind; ++i) {
				mc_vector_push(&list.items, _mc_convert(c, &value->as.list->items[i], layout->fields.data[i]->type));
			}
			return list;
		}
		case mc_type_function:
			if (mc_cvalue_function == value->kind) {
				if (value->as.function.lambda != NULL) {
					return (mc_bc_constant_s) { .kind = mc_constant_function, .type = -1, .integer = (uint64_t)_mc_function(c, value->as.function.lambda, true), };
				}
				const mc_instance_s* const instance = value->as.function.instance;
				if (mc_function_is_plugin(instance->function)) {
					return (mc_bc_constant_s) { .kind = mc_constant_intrinsic, .type = -1, .integer = (uint64_t)_mc_intrinsic(c, instance, NULL, NULL), };
				}
				return (mc_bc_constant_s) { .kind = mc_constant_function, .type = -1, .integer = (uint64_t)_mc_function(c, instance, false), };
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
	uint64_t alignment = 1;
	const uint64_t size = _mc_size(c, type, &alignment);
	mc_bc_type_s descriptor = { .zero = -1, .name = mc_type_name(type), .size = size, .alignment = alignment, .code = mc_type_code_count_, };
	mc_vector_push(&c->module->types, descriptor);
	mc_descriptor_kind_e kind = mc_descriptor_int;
	mc_type_code_e code = mc_type_code_count_;
	int64_t element = 0, length = 0;
	switch (type->kind) {
		case mc_type_scalar: {
			const char scalar = mc_scalar_kind(type->as.scalar);
			kind = ('b' == scalar) ? mc_descriptor_bool : ('f' == scalar) ? mc_descriptor_float : mc_descriptor_int;
			code = _mc_type_code(c, type);
			break;
		}
		case mc_type_array:
			kind = mc_descriptor_array;
			element = _mc_describe(c, type->as.array.element);
			length = type->as.array.length;
			break;
		case mc_type_slice:
			kind = mc_descriptor_slice;
			code = mc_code_slice;
			element = _mc_describe(c, type->as.slice.element);
			break;
		case mc_type_struct: {
			kind = mc_descriptor_struct;
			const mc_layout_s* const layout = mc_layout(c->checker, type);
			for (size_t i = 0; i < layout->fields.count; ++i) {
				const int64_t field_type = _mc_describe(c, layout->fields.data[i]->type);
				mc_bc_type_s* const self = &c->module->types.data[index];
				mc_vector_push(&self->field_names, mc_format(mc_view_fmt, mc_view_arg(layout->fields.data[i]->name)));
				mc_vector_push(&self->field_types, field_type);
				mc_vector_push(&self->field_offsets, layout->fields.data[i]->offset);
			}
			break;
		}
		case mc_type_function:
			kind = mc_descriptor_function;
			code = mc_code_fn;
			break;
		default:
			_mc_fail(c, "cannot describe `%s`", mc_name_of(c->checker, type));
	}
	mc_bc_type_s* self = &c->module->types.data[index];
	self->kind = kind;
	self->code = code;
	self->element = element;
	self->length = length;
	if (_mc_zero_has_defaults(c, type)) {
		const int64_t zero = _mc_constant(c, _mc_zero(c, type));
		self = &c->module->types.data[index];
		self->zero = zero;
	}
	return index;
}

// --- functions ---------------------------------------------------------------------------

typedef struct {
	const mc_variable_s* variable;
	int64_t              offset  ;
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
	uint64_t                 frame   ;   // bytes of the frame so far
	mc_span_s                span    ;
	bool                     has_span;
	mc_vector_of(_mc_loop_s) loops   ;
} _mc_function_s;

static mc_bc_function_s* _mc_target(_mc_function_s* const f) {
	return &f->c->module->functions.data[f->index];
}

/// room in the frame for 'size' bytes.
static int64_t _mc_reserve(_mc_function_s* const f, const uint64_t size, uint64_t alignment) {
	if (alignment < 1) alignment = 1;
	if (alignment > 8) alignment = 8;
	const uint64_t offset = (f->frame + alignment - 1) / alignment * alignment;
	f->frame = offset + size;
	return (int64_t)offset;
}

static int64_t _mc_reserve_type(_mc_function_s* const f, const mc_type_s* const type) {
	uint64_t alignment = 1;
	const uint64_t size = _mc_size(f->c, type, &alignment);
	return _mc_reserve(f, size, alignment);
}

static bool _mc_has_slot(_mc_function_s* const f, const mc_variable_s* const variable, int64_t* const offset) {
	for (size_t i = 0; i < f->slots.count; ++i) {
		if (f->slots.data[i].variable == variable) {
			*offset = f->slots.data[i].offset;
			return true;
		}
	}
	return false;
}

/// the offset of a variable's slot (reference parameters hold a location).
static int64_t _mc_slot(_mc_function_s* const f, const mc_variable_s* const variable) {
	int64_t offset = 0;
	if (_mc_has_slot(f, variable, &offset)) return offset;
	offset = variable->reference ? _mc_reserve(f, 16, 8) : _mc_reserve_type(f, variable->type);
	const _mc_slot_s entry = { .variable = variable, .offset = offset, };
	mc_vector_push(&f->slots, entry);
	return offset;
}

static int64_t _mc_emit_full(_mc_function_s* const f, const mc_instruction_s instruction) {
	const mc_position_s position = _mc_position(f->c, f->span, f->has_span);
	mc_bc_function_s* const function = _mc_target(f);
	mc_vector_push(&function->code, instruction);
	mc_vector_push(&function->positions, position);
	return (int64_t)function->code.count - 1;
}

static int64_t _mc_emit(_mc_function_s* const f, const mc_opcode_e opcode) {
	return _mc_emit_full(f, (mc_instruction_s) { .opcode = opcode, });
}

static int64_t _mc_emit_a(_mc_function_s* const f, const mc_opcode_e opcode, const int64_t a) {
	return _mc_emit_full(f, (mc_instruction_s) { .opcode = opcode, .a = a, });
}

static int64_t _mc_emit_ab(_mc_function_s* const f, const mc_opcode_e opcode, const int64_t a, const int64_t b) {
	return _mc_emit_full(f, (mc_instruction_s) { .opcode = opcode, .a = a, .b = b, });
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

static int64_t _mc_emit_typed(_mc_function_s* const f, const mc_opcode_e opcode, const mc_type_s* const type) {
	return _mc_emit_full(f, (mc_instruction_s) { .opcode = opcode, .type = _mc_type_code(f->c, type), });
}

/// a typed memory instruction: 'typed' for scalars, functions and slices,
/// 'bytes' (with the size as its last operand) for structs and arrays.
static void _mc_emit_memory(_mc_function_s* const f, const mc_opcode_e typed, const mc_opcode_e bytes, const mc_type_s* const type,
							const int64_t a, const int64_t b) {
	const mc_type_code_e code = _mc_memory_code(f->c, type);
	const int64_t size = (int64_t)_mc_size(f->c, type, NULL);
	if (code != mc_type_code_count_) {
		_mc_emit_full(f, (mc_instruction_s) { .opcode = typed, .type = code, .a = a, .b = b, });
		return;
	}
	mc_instruction_s instruction = { .opcode = bytes, .a = a, .b = b, .c = size, };
	const int operands = mc_opcode_operands(bytes);
	if (1 == operands) instruction.a = size;
	else if (2 == operands) instruction.b = size;
	_mc_emit_full(f, instruction);
}

static void _mc_load_local(_mc_function_s* const f, const int64_t offset, const mc_type_s* const type) {
	_mc_emit_memory(f, mc_op_LOAD, mc_op_LOAD_BYTES, type, offset, 0);
}

static void _mc_store_local(_mc_function_s* const f, const int64_t offset, const mc_type_s* const type) {
	_mc_emit_memory(f, mc_op_STORE, mc_op_STORE_BYTES, type, offset, 0);
}

static void _mc_pop(_mc_function_s* const f, const mc_type_s* const type) {
	const int64_t words = _mc_words(f->c, type);
	if (words > 0) _mc_emit_a(f, mc_op_POP, words);
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

/// an arithmetic or comparison instruction on 'left' (and the type of a shift count).
static void _mc_emit_operator(_mc_function_s* const f, const mc_operator_e operator_, const mc_type_s* const left, const mc_type_s* const right) {
	const mc_opcode_e opcode = _mc_operator_opcode(operator_);
	mc_instruction_s instruction = { .opcode = opcode, .type = _mc_type_code(f->c, left), };
	if (mc_op_SHL == opcode || mc_op_SHR == opcode) instruction.target = _mc_type_code(f->c, right);
	_mc_emit_full(f, instruction);
}

static void _mc_value(_mc_function_s* const f, const mc_node_s* const node);
static void _mc_statement(_mc_function_s* const f, const mc_node_s* const node);

static void _mc_block(_mc_function_s* const f, const mc_node_s* const node) {
	for (size_t i = 0; i < node->as.block.statements.count; ++i) _mc_statement(f, node->as.block.statements.data[i]);
}

/// the zero value of 'type' on the stack.
static void _mc_zero_value(_mc_function_s* const f, const mc_type_s* const type) {
	if (_mc_zero_has_defaults(f->c, type)) _mc_push_constant(f, _mc_zero(f->c, type));
	else _mc_emit_a(f, mc_op_ZERO, _mc_words(f->c, type));
}

// --- calls -------------------------------------------------------------------------------

static void _mc_emit_call(_mc_function_s* const f, const mc_instance_s* const instance, const int64_t words, const mc_node_s* const call) {
	if (mc_function_is_plugin(instance->function)) {
		_mc_emit_ab(f, mc_op_CALL_INTRINSIC, _mc_intrinsic(f->c, instance, (call != NULL) ? call->note.variadic : NULL, call), words);
	}
	else _mc_emit_a(f, mc_op_CALL, _mc_function(f->c, instance, false));
}

static void _mc_function_value(_mc_function_s* const f, const mc_instance_s* const instance) {
	if (mc_function_is_plugin(instance->function)) _mc_emit_a(f, mc_op_INTRINSIC, _mc_intrinsic(f->c, instance, NULL, NULL));
	else _mc_emit_a(f, mc_op_FUNCTION, _mc_function(f->c, instance, false));
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
	int64_t words = 0;
	for (size_t i = 0; i < call->parameters.count; ++i) words += _mc_words(f->c, call->parameters.data[i]);
	_mc_emit_call(f, call, words, NULL);
	if (node->note.negated) _mc_emit(f, mc_op_NOT);
	return true;
}

// --- places ------------------------------------------------------------------------------

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
		case mc_node_member: return NULL == node->note.field && node->note.constant != NULL;
		default:             return false;
	}
}

/// the constant of a static node (see _mc_static).
static int64_t _mc_static_constant(_mc_function_s* const f, const mc_node_s* const node) {
	if (mc_node_paren == node->kind) return _mc_static_constant(f, node->as.paren.inner);
	if (mc_node_string == node->kind) {
		mc_bc_constant_s list = { .kind = mc_constant_list, .type = _mc_describe(f->c, node->note.type), };
		const mc_view_s data = node->as.string.data;
		for (size_t i = 0; i < data.length; ++i) mc_vector_push(&list.items, _mc_integer_constant((uint8_t)data.data[i], false));
		return _mc_constant(f->c, list);
	}
	const mc_constant_s* const constant = node->note.constant;
	if (NULL == constant->value) _mc_fail(f->c, "the constant `%.*s` has no value", mc_view_arg(mc_declaration_name(constant->node)));
	const mc_type_s* const type = mc_type_is_untyped(node->note.type) ? constant->type : node->note.type;
	return _mc_constant(f->c, _mc_convert(f->c, constant->value, type));
}

/// a place at a fixed offset of the frame: a local that is not a
/// reference, its fields, and elements of its arrays at constant indexes.
static bool _mc_frame_place(_mc_function_s* const f, const mc_node_s* const node, int64_t* const offset) {
	switch (node->kind) {
		case mc_node_paren:
			return _mc_frame_place(f, node->as.paren.inner, offset);
		case mc_node_name: {
			const mc_variable_s* const variable = node->note.variable;
			if (NULL == variable || variable->reference || !_mc_has_slot(f, variable, offset)) return false;
			return true;
		}
		case mc_node_member:
			if (NULL == node->note.field || !_mc_frame_place(f, node->as.member.target, offset)) return false;
			*offset += (int64_t)node->note.field->offset;
			return true;
		case mc_node_index: {
			const mc_type_s* const target = node->as.index.target->note.type;
			if (node->note.operator_call != NULL || target->kind != mc_type_array) return false;
			mc_cvalue_s value = {0};
			if (!mc_try_value(f->c->checker, node->as.index.index, &value) || value.kind != mc_cvalue_integer) return false;
			if (value.as.integer < 0 || value.as.integer >= target->as.array.length) return false;
			if (!_mc_frame_place(f, node->as.index.target, offset)) return false;
			*offset += (int64_t)value.as.integer * (int64_t)_mc_size(f->c, target->as.array.element, NULL);
			return true;
		}
		default:
			return false;
	}
}

/// whether 'node' has an address: a variable, a field of one, an element of
/// an array that has one, an element of a slice.
static bool _mc_addressable(const mc_node_s* const node) {
	switch (node->kind) {
		case mc_node_paren:  return _mc_addressable(node->as.paren.inner);
		case mc_node_name:   return node->note.variable != NULL;
		case mc_node_member: return node->note.field != NULL && _mc_addressable(node->as.member.target);
		case mc_node_index:
			if (node->note.operator_call != NULL) return false;
			return mc_type_slice == node->as.index.target->note.type->kind || _mc_addressable(node->as.index.target);
		default: return false;
	}
}

static void _mc_location(_mc_function_s* const f, const mc_node_s* const node);

/// pushes an index for an element of 'length' elements of 'element'; the
/// location or slice is on the stack already.
static void _mc_index_location(_mc_function_s* const f, const mc_node_s* const node) {
	const mc_node_s* const index = node->as.index.index;
	const mc_type_s* const target = node->as.index.target->note.type;
	_mc_value(f, index);
	if (mc_type_array == target->kind) {
		const mc_instruction_s instruction = { .opcode = mc_op_LOC_INDEX, .type = _mc_type_code(f->c, index->note.type),
			.a = (int64_t)_mc_size(f->c, target->as.array.element, NULL), .b = target->as.array.length, };
		_mc_emit_full(f, instruction);
	}
	else {
		const mc_instruction_s instruction = { .opcode = mc_op_LOC_INDEX_SLICE, .type = _mc_type_code(f->c, index->note.type),
			.a = (int64_t)_mc_size(f->c, target->as.slice.element, NULL), };
		_mc_emit_full(f, instruction);
	}
}

/// pushes the location of a place (a temporary for other values).
static void _mc_location(_mc_function_s* const f, const mc_node_s* const node) {
	if (mc_node_paren == node->kind) {
		_mc_location(f, node->as.paren.inner);
		return;
	}
	const mc_variable_s* const variable = node->note.variable;
	if (mc_node_name == node->kind && variable != NULL) {
		if (variable->reference) _mc_emit_full(f, (mc_instruction_s) { .opcode = mc_op_LOAD, .type = mc_code_slice, .a = _mc_slot(f, variable), });
		else _mc_emit_a(f, mc_op_LOC_LOCAL, _mc_slot(f, variable));
		return;
	}
	if (_mc_static(node) && (mc_type_array == node->note.type->kind || mc_type_struct == node->note.type->kind)) {
		_mc_emit_a(f, mc_op_LOC_CONST, _mc_static_constant(f, node));
		return;
	}
	if (mc_node_member == node->kind && node->note.field != NULL) {
		_mc_location(f, node->as.member.target);
		if (node->note.field->offset > 0) _mc_emit_a(f, mc_op_LOC_FIELD, (int64_t)node->note.field->offset);
		return;
	}
	if (mc_node_index == node->kind && NULL == node->note.operator_call) {
		if (mc_type_array == node->as.index.target->note.type->kind) _mc_location(f, node->as.index.target);
		else _mc_value(f, node->as.index.target);
		_mc_index_location(f, node);
		return;
	}
	_mc_value(f, node);
	const int64_t offset = _mc_reserve_type(f, node->note.type);
	_mc_emit_memory(f, mc_op_TEMP_LOC, mc_op_TEMP_LOC_BYTES, node->note.type, offset, 0);
}

/// reads a place: from the frame, through a location, or from a value.
static void _mc_read_place(_mc_function_s* const f, const mc_node_s* const node) {
	int64_t offset = 0;
	if (_mc_frame_place(f, node, &offset)) {
		_mc_load_local(f, offset, node->note.type);
		return;
	}
	_mc_location(f, node);
	_mc_emit_memory(f, mc_op_LOAD_LOC, mc_op_LOAD_LOC_BYTES, node->note.type, 0, 0);
}

// --- expressions -------------------------------------------------------------------------

/// a literal number as a constant of 'type'.
static void _mc_scalar(_mc_function_s* const f, const bool real, const __int128 integer, const double number, const mc_type_s* const type) {
	if (mc_type_scalar == type->kind) {
		const char kind = mc_scalar_kind(type->as.scalar);
		if ('b' == kind) _mc_push_constant(f, _mc_boolean_constant(real ? number != 0.0 : integer != 0));
		else if ('f' == kind) _mc_push_constant(f, _mc_real_constant(mc_float_wrap(real ? number : (double)integer, type)));
		else _mc_push_constant(f, _mc_integer_constant(mc_integer_wrap(integer, type), mc_scalar_signed(type->as.scalar)));
	}
	else if (real || mc_type_is_special(type, mc_special_untyped_float)) {
		_mc_push_constant(f, _mc_real_constant(real ? number : (double)integer));
	}
	else {
		_mc_push_constant(f, _mc_integer_constant(integer, true));
	}
}

static void _mc_symbol(_mc_function_s* const f, const mc_node_s* const node) {
	const mc_variable_s* const variable = node->note.variable;
	if (variable != NULL) {
		int64_t offset = 0;
		if (!_mc_has_slot(f, variable, &offset)) _mc_fail(f->c, "`%.*s` is not a local of this function", mc_view_arg(variable->name));
		if (variable->reference) {
			_mc_emit_full(f, (mc_instruction_s) { .opcode = mc_op_LOAD, .type = mc_code_slice, .a = offset, });
			_mc_emit_memory(f, mc_op_LOAD_LOC, mc_op_LOAD_LOC_BYTES, variable->type, 0, 0);
		}
		else {
			_mc_load_local(f, offset, variable->type);
		}
		return;
	}
	const mc_constant_s* const constant = node->note.constant;
	if (constant != NULL) {
		if (NULL == constant->value) _mc_fail(f->c, "the constant `%.*s` has no value", mc_view_arg(mc_declaration_name(constant->node)));
		const mc_type_s* const type = mc_type_is_untyped(node->note.type) ? constant->type : node->note.type;
		const mc_bc_constant_s converted = _mc_convert(f->c, constant->value, type);
		if (mc_constant_slice == converted.kind && converted.null) _mc_emit_a(f, mc_op_ZERO, 2);
		else _mc_push_constant(f, converted);
		return;
	}
	if (node->note.instance != NULL) {
		_mc_function_value(f, node->note.instance);
		return;
	}
	const mc_view_s text = mc_span_text(f->c->program->sources, node->span);
	_mc_fail(f->c, "cannot compile `%.*s`", mc_view_arg(text));
}

static int64_t _mc_argument(_mc_function_s* const f, const mc_node_s* const argument, const mc_type_s* const parameter) {
	if (mc_type_reference == parameter->kind) {
		_mc_location(f, mc_node_reference == argument->kind ? argument->as.reference.operand : argument);
		return 2;
	}
	_mc_value(f, argument);
	return _mc_words(f->c, parameter);
}

/// a slice of all of the array 'node' (a format string given as a literal).
static void _mc_whole_slice(_mc_function_s* const f, const mc_node_s* const node) {
	const mc_type_s* const type = node->note.type;
	_mc_location(f, node);
	const mc_instruction_s instruction = { .opcode = mc_op_SLICE_LOC, .type = mc_code_int, .target = mc_code_int,
		.a = _mc_read_only(node) ? mc_slice_read_only : 0, .b = type->as.array.length, .c = (int64_t)_mc_size(f->c, type->as.array.element, NULL), };
	_mc_emit_full(f, instruction);
}

static void _mc_call(_mc_function_s* const f, const mc_node_s* const node) {
	const mc_nodes_s* const arguments = &node->as.call.arguments;
	const mc_instance_s* const instance = node->note.instance;
	if (instance != NULL && instance->parameters.count > 0 && mc_type_is_special(mc_vector_last(&instance->parameters), mc_special_variadic)) {
		const size_t fixed = instance->parameters.count - 1;
		int64_t words = 0;
		for (size_t i = 0; i < arguments->count && i < fixed; ++i) {
			const mc_node_s* const argument = arguments->data[i];
			const mc_type_s* const parameter = instance->parameters.data[i];
			if (mc_type_slice == parameter->kind && mc_type_array == argument->note.type->kind) {
				_mc_whole_slice(f, argument);
				words += 2;
			}
			else {
				words += _mc_argument(f, argument, parameter);
			}
		}
		for (size_t i = fixed; i < arguments->count; ++i) {
			_mc_value(f, arguments->data[i]);
			words += _mc_words(f->c, arguments->data[i]->note.type);
		}
		_mc_emit_call(f, instance, words, node);
		return;
	}
	if (instance != NULL) {
		const mc_node_s* const declaration = (instance->node != NULL) ? instance->node : instance->function->node;
		int64_t words = 0;
		for (size_t i = 0; i < instance->parameters.count; ++i) {
			if (i < arguments->count) {
				words += _mc_argument(f, arguments->data[i], instance->parameters.data[i]);
			}
			else {
				_mc_value(f, declaration->as.function.parameters.data[i]->as.param.default_);
				words += _mc_words(f->c, instance->parameters.data[i]);
			}
		}
		_mc_emit_call(f, instance, words, node);
		return;
	}
	const mc_node_s* const callee = node->as.call.callee;
	_mc_value(f, callee);
	const mc_type_s* const type = callee->note.type;
	int64_t words = 0;
	for (size_t i = 0; i < arguments->count && i < type->as.function.count; ++i) words += _mc_argument(f, arguments->data[i], type->as.function.parameters[i]);
	_mc_emit_ab(f, mc_op_CALL_VALUE, words, _mc_words(f->c, type->as.function.result));
}

static mc_type_code_e _mc_index_code(_mc_function_s* const f, const mc_node_s* const node) {
	return (node != NULL) ? _mc_type_code(f->c, node->note.type) : mc_code_int;
}

static void _mc_slice_of(_mc_function_s* const f, const mc_node_s* const node) {
	const mc_node_s* const target = node->as.slice_of.target;
	const mc_type_s* const type = target->note.type;
	int64_t flags = (node->as.slice_of.low != NULL ? mc_slice_low : 0) | (node->as.slice_of.high != NULL ? mc_slice_high : 0)
		| (node->as.slice_of.inclusive ? mc_slice_inclusive : 0);
	mc_instruction_s instruction = { .type = _mc_index_code(f, node->as.slice_of.low), .target = _mc_index_code(f, node->as.slice_of.high), };
	if (mc_type_slice == type->kind) {
		_mc_value(f, target);
		instruction.opcode = mc_op_SLICE_SLICE;
		instruction.b = (int64_t)_mc_size(f->c, type->as.slice.element, NULL);
	}
	else {
		_mc_location(f, target);
		if (_mc_read_only(target)) flags |= mc_slice_read_only;
		instruction.opcode = mc_op_SLICE_LOC;
		instruction.b = type->as.array.length;
		instruction.c = (int64_t)_mc_size(f->c, type->as.array.element, NULL);
	}
	instruction.a = flags;
	if (node->as.slice_of.low != NULL) _mc_value(f, node->as.slice_of.low);
	if (node->as.slice_of.high != NULL) _mc_value(f, node->as.slice_of.high);
	_mc_emit_full(f, instruction);
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
		_mc_emit_a(f, mc_op_DUP, 1);
		const int64_t jump = _mc_emit_a(f, mc_operator_and == operator_ ? mc_op_JUMP_IF_FALSE : mc_op_JUMP_IF_TRUE, 0);
		_mc_emit_a(f, mc_op_POP, 1);
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
	_mc_emit_operator(f, operator_, left->note.type, right->note.type);
}

static void _mc_composite(_mc_function_s* const f, const mc_node_s* const node) {
	const mc_type_s* const type = node->note.type;
	const mc_nodes_s* const entries = &node->as.composite.entries;
	if (0 == entries->count) {
		_mc_zero_value(f, type);
		return;
	}
	if (mc_type_struct == type->kind) {
		const mc_layout_s* const layout = mc_layout(f->c->checker, type);
		_mc_zero_value(f, type);
		for (size_t i = 0; i < entries->count; ++i) {
			const mc_node_s* const entry = entries->data[i];
			const mc_field_s* const field = mc_layout_field(layout, entry->as.entry.name);
			if (NULL == field) _mc_fail(f->c, "no field `%.*s`", mc_view_arg(entry->as.entry.name));
			_mc_value(f, entry->as.entry.value);
			_mc_emit_memory(f, mc_op_SET_FIELD, mc_op_SET_FIELD_BYTES, field->type, (int64_t)field->offset, _mc_words(f->c, type));
		}
		return;
	}
	const mc_type_s* const element = (mc_type_array == type->kind) ? type->as.array.element : type->as.slice.element;
	for (size_t i = 0; i < entries->count; ++i) _mc_value(f, entries->data[i]->as.entry.value);
	_mc_emit_memory(f, mc_op_MAKE_ARRAY, mc_op_MAKE_ARRAY_BYTES, element, (int64_t)entries->count, 0);
	if (mc_type_slice == type->kind) {
		const uint64_t size = _mc_size(f->c, element, NULL);
		uint64_t alignment = 1;
		(void)_mc_size(f->c, element, &alignment);
		const int64_t offset = _mc_reserve(f, size * entries->count, alignment);
		_mc_emit_full(f, (mc_instruction_s) { .opcode = mc_op_TEMP_LOC_BYTES, .a = offset, .b = (int64_t)(size * entries->count), });
		_mc_emit_full(f, (mc_instruction_s) { .opcode = mc_op_SLICE_LOC, .type = mc_code_int, .target = mc_code_int,
			.b = (int64_t)entries->count, .c = (int64_t)size, });
	}
}

static void _mc_assert(_mc_function_s* const f, const mc_node_s* const node) {
	_mc_value(f, node->as.assert_.condition);
	const int64_t skip = _mc_emit_a(f, mc_op_JUMP_IF_TRUE, 0);
	const mc_node_s* const message = node->as.assert_.message;
	mc_bc_constant_s text = { .kind = mc_constant_text, .type = -1, };
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
}

static void _mc_lenof(_mc_function_s* const f, const mc_node_s* const node) {
	const mc_node_s* const operand = node->as.lenof.operand;
	const mc_type_s* const type = operand->note.type;
	if (mc_type_array == type->kind) {
		const bool plain = mc_node_name == operand->kind || mc_node_string == operand->kind || mc_node_member == operand->kind || mc_node_paren == operand->kind;
		if (!plain) {
			_mc_value(f, operand);
			_mc_pop(f, type);
		}
		_mc_push_constant(f, _mc_integer_constant(type->as.array.length, false));
		return;
	}
	_mc_value(f, operand);
	_mc_emit(f, mc_op_LEN_SLICE);
}

static void _mc_member(_mc_function_s* const f, const mc_node_s* const node) {
	if (NULL == node->note.field) {
		_mc_symbol(f, node);
		return;
	}
	if (_mc_addressable(node)) {
		_mc_read_place(f, node);
		return;
	}
	const mc_node_s* const target = node->as.member.target;
	_mc_value(f, target);
	const mc_field_s* const field = node->note.field;
	_mc_emit_memory(f, mc_op_FIELD, mc_op_FIELD_BYTES, field->type, (int64_t)field->offset, _mc_words(f->c, target->note.type));
}

static void _mc_index(_mc_function_s* const f, const mc_node_s* const node) {
	if (_mc_operator(f, node, node->as.index.target, node->as.index.index)) return;
	const mc_node_s* const target = node->as.index.target;
	const mc_node_s* const index = node->as.index.index;
	const mc_type_s* const type = target->note.type;
	const mc_type_s* const element = node->note.type;
	if (mc_type_array == type->kind && _mc_addressable(target)) {
		_mc_read_place(f, node);
		return;
	}
	_mc_value(f, target);
	_mc_value(f, index);
	const mc_type_code_e code = _mc_memory_code(f->c, element);
	mc_instruction_s instruction = { .type = code, .target = _mc_type_code(f->c, index->note.type), };
	if (mc_type_array == type->kind) {
		instruction.opcode = (code != mc_type_code_count_) ? mc_op_INDEX : mc_op_INDEX_BYTES;
		instruction.a = type->as.array.length;
		instruction.b = _mc_words(f->c, type);
		instruction.c = (int64_t)_mc_size(f->c, element, NULL);
	}
	else {
		instruction.opcode = (code != mc_type_code_count_) ? mc_op_INDEX_SLICE : mc_op_INDEX_SLICE_BYTES;
		instruction.a = (int64_t)_mc_size(f->c, element, NULL);
	}
	if (code == mc_type_code_count_) {
		// the bytes variants carry one type code: the index's
		instruction.type = instruction.target;
		instruction.target = mc_code_s8;
	}
	_mc_emit_full(f, instruction);
}

static void _mc_expression(_mc_function_s* const f, const mc_node_s* const node) {
	switch (node->kind) {
		case mc_node_integer: _mc_scalar(f, false, (__int128)node->as.integer.value, 0.0, node->note.type); break;
		case mc_node_float:   _mc_scalar(f, true, 0, node->as.float_.value, node->note.type); break;
		case mc_node_char:    _mc_push_constant(f, _mc_integer_constant(node->as.char_.value, false)); break;
		case mc_node_bool:    _mc_push_constant(f, _mc_boolean_constant(node->as.bool_.value)); break;
		case mc_node_null:    _mc_emit_a(f, mc_op_ZERO, 2); break;
		case mc_node_string:  _mc_emit_a(f, mc_op_CONST, _mc_static_constant(f, node)); break;
		case mc_node_paren:   _mc_value(f, node->as.paren.inner); break;
		case mc_node_name:    _mc_symbol(f, node); break;
		case mc_node_member:  _mc_member(f, node); break;
		case mc_node_specialize: _mc_function_value(f, node->note.instance); break;
		case mc_node_lambda:  _mc_emit_a(f, mc_op_FUNCTION, _mc_function(f->c, node, true)); break;
		case mc_node_call:    _mc_call(f, node); break;
		case mc_node_index:   _mc_index(f, node); break;
		case mc_node_slice_of: _mc_slice_of(f, node); break;
		case mc_node_unary:   _mc_unary(f, node); break;
		case mc_node_binary:  _mc_binary(f, node); break;
		case mc_node_cast: {
			_mc_value(f, node->as.cast.operand);
			const mc_type_code_e source = _mc_type_code(f->c, node->as.cast.operand->note.type);
			const mc_type_code_e target = _mc_type_code(f->c, node->note.type);
			if (source != target) _mc_emit_full(f, (mc_instruction_s) { .opcode = mc_op_CAST, .type = source, .target = target, });
			break;
		}
		case mc_node_reference: _mc_fail(f->c, "a reference outside of a call argument");
		case mc_node_composite: _mc_composite(f, node); break;
		case mc_node_sizeof:  _mc_push_constant(f, _mc_integer_constant((__int128)node->note.size, false)); break;
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

/// the operator of a compound assignment, on the loaded value and the new one.
static void _mc_compound_operator(_mc_function_s* const f, const mc_node_s* const node) {
	const mc_node_s* const target = node->as.assign.target;
	_mc_value(f, node->as.assign.value);
	if (node->note.operator_call != NULL) {
		const mc_instance_s* const call = node->note.operator_call;
		int64_t words = 0;
		for (size_t i = 0; i < call->parameters.count; ++i) words += _mc_words(f->c, call->parameters.data[i]);
		_mc_emit_call(f, call, words, NULL);
	}
	else {
		_mc_emit_operator(f, node->as.assign.operator_, target->note.type, node->as.assign.value->note.type);
	}
}

static void _mc_assign(_mc_function_s* const f, const mc_node_s* const node) {
	const mc_node_s* const target = node->as.assign.target;
	const mc_node_s* const value = node->as.assign.value;
	if (mc_node_name == target->kind && mc_view_eq_cstr(target->as.name.name, "_")) {
		_mc_value(f, value);
		_mc_pop(f, value->note.type);
		return;
	}
	const mc_type_s* const type = target->note.type;
	int64_t offset = 0;
	if (_mc_frame_place(f, target, &offset)) {
		if (mc_operator_assign == node->as.assign.operator_) {
			_mc_value(f, value);
		}
		else {
			_mc_load_local(f, offset, type);
			_mc_compound_operator(f, node);
		}
		_mc_store_local(f, offset, type);
		return;
	}
	_mc_location(f, target);
	if (mc_operator_assign == node->as.assign.operator_) {
		_mc_value(f, value);
	}
	else {
		_mc_emit_a(f, mc_op_DUP, 2);
		_mc_emit_memory(f, mc_op_LOAD_LOC, mc_op_LOAD_LOC_BYTES, type, 0, 0);
		_mc_compound_operator(f, node);
	}
	_mc_emit_memory(f, mc_op_STORE_LOC, mc_op_STORE_LOC_BYTES, type, 0, 0);
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
	const int64_t base = _mc_reserve(f, 24, 8);
	const mc_type_s* const type = node->note.variable->type;
	_mc_value(f, node->as.for_.start);
	_mc_value(f, node->as.for_.end);
	_mc_emit_full(f, (mc_instruction_s) { .opcode = mc_op_RANGE_INIT, .type = _mc_type_code(f->c, type), .a = base, .b = node->as.for_.inclusive, });
	const int64_t test = _mc_here(f);
	const int64_t leave = _mc_emit_ab(f, mc_op_RANGE_TEST, base, 0);
	_mc_emit_full(f, (mc_instruction_s) { .opcode = mc_op_LOAD, .type = mc_code_int, .a = base, });
	_mc_store_local(f, _mc_slot(f, node->note.variable), type);
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
			const mc_type_s* const type = node->note.variable->type;
			if (node->as.let.value != NULL) _mc_value(f, node->as.let.value);
			else _mc_zero_value(f, type);
			_mc_store_local(f, _mc_slot(f, node->note.variable), type);
			break;
		}
		case mc_node_expression_statement: {
			const mc_node_s* const expression = node->as.expression_statement.expression;
			_mc_value(f, expression);
			_mc_pop(f, expression->note.type);
			break;
		}
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
		case mc_node_return: {
			const mc_node_s* const value = node->as.return_.value;
			if (value != NULL) _mc_value(f, value);
			_mc_emit_a(f, mc_op_RETURN, (int64_t)_mc_target(f)->result);
			break;
		}
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
	const mc_type_s* result = NULL;
	if (pending.lambda) {
		const mc_node_s* const node = pending.target;
		parameters = &node->as.lambda.parameters;
		body = node->as.lambda.body;
		const mc_type_s* const type = node->note.type;
		result = (type != NULL && mc_type_function == type->kind) ? type->as.function.result : NULL;
	}
	else {
		const mc_instance_s* const instance = pending.target;
		const mc_node_s* const node = (instance->node != NULL) ? instance->node : instance->function->node;
		parameters = &node->as.function.parameters;
		body = node->as.function.body;
		result = instance->result;
	}
	*f = (_mc_function_s) { .c = c, .index = pending.index, };
	_mc_target(f)->returns = result != NULL;
	_mc_target(f)->result = (uint32_t)_mc_words(c, result);
	for (size_t i = 0; i < parameters->count; ++i) {
		const mc_variable_s* const variable = parameters->data[i]->note.variable;
		const int64_t offset = _mc_slot(f, variable);
		const mc_type_s* const type = variable->reference ? NULL : variable->type;
		const mc_bc_slot_s slot = {
			.code = (NULL == type) ? mc_code_slice : _mc_memory_code(c, type),
			.offset = (uint64_t)offset,
			.size = (NULL == type) ? 16 : _mc_size(c, type, NULL),
		};
		mc_vector_push(&_mc_target(f)->slots, slot);
	}
	if (NULL == body) _mc_fail(c, "`%s` has no body", _mc_target(f)->name);
	f->span = body->span;
	f->has_span = true;
	_mc_block(f, body);
	f->span = (mc_span_s) { .file = body->span.file, .begin = body->span.end - 1, .end = body->span.end, };
	_mc_emit_a(f, mc_op_RETURN, 0);
	_mc_target(f)->frame = (f->frame + 7) / 8 * 8;
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
		return false;
	}
	return true;
}
