
/// the compiler: a checked program -> bytecode.
///
/// every function gets a frame laid out here: its parameters first, then a
/// slot for every local and every temporary, each at an offset aligned for
/// its type. values are moved through the operand stack in words (see
/// bytecode.h); places (locals, fields, elements) are read and written
/// where they are, through constant offsets when the place is a local and
/// through locations otherwise.

#include "mia/checker.h"
#include "mia/compiler.h"
#include "mia/format.h"
#include "mia/map.h"

#include <string.h>
#include <setjmp.h>

typedef struct {
	const void* target;   // an instance or a lambda node
	int64_t     index ;
	bool        lambda;
} _mia_compiler_pending_s;

typedef struct {
	mia_program_s*                         program    ;
	mia_checker_s*                         checker    ;
	mia_bytecode_module_s*                 module     ;
	mia_vector_of(_mia_compiler_pending_s) functions  ;   // every function, in the order they are compiled
	size_t                                 next       ;   // the next function to compile
	mia_map_index_s                        function_of;   // instance or lambda node -> function index
	mia_map_index_s                        type_of    ;   // type -> type descriptor
	mia_map_index_s                        file_of    ;   // source id -> file index
	mia_map_index_s                        constant_of;   // hash -> the first constant with it
	mia_vector_of(int64_t)                 same_hash  ;   // per constant: the next one with its hash (-1: none)
	jmp_buf*                               stop       ;
	char*                                  error      ;
} _mia_compiler_s;

mia_noreturn static void _mia_compiler_fail(_mia_compiler_s* const c, const char* const format, ...) mia_printf_like(2, 3);

mia_noreturn static void _mia_compiler_fail(_mia_compiler_s* const c, const char* const format, ...) {
	va_list args; va_start(args, format);
	c->error = mia_string_vformat(format, args);
	va_end(args);
	longjmp(*c->stop, 1);
}

// --- types ------------------------------------------------------------------------------

static uint64_t _mia_compiler_size(_mia_compiler_s* const c, const mia_type_s* const type, uint64_t* const alignment) {
	uint64_t ignored = 0;
	uint64_t* const align = (alignment != NULL) ? alignment : &ignored;
	if (mia_type_reference == type->kind) {
		*align = 8;
		return 16;
	}
	if (mia_type_is_special(type, mia_type_special_nothing)) {
		*align = 1;
		return 0;
	}
	if (mia_type_is_untyped(type)) {
		*align = 8;
		return 8;
	}
	const uint64_t size = mia_checker_size_of(c->checker, type, align);
	if (0 == *align) *align = 1;
	return size;
}

/// the words a value of 'type' takes on the operand stack.
static int64_t _mia_compiler_words(_mia_compiler_s* const c, const mia_type_s* const type) {
	if (NULL == type) return 0;
	return (int64_t)((_mia_compiler_size(c, type, NULL) + 7) / 8);
}

/// the type code of a scalar or function type (the 'T' of typed instructions).
static mia_code_e _mia_compiler_type_code(_mia_compiler_s* const c, const mia_type_s* const type) {
	if (mia_type_scalar == type->kind) {
		switch (type->as.scalar) {
#define mia_token_keyword(_name)
#define mia_token_punctuator(_name, _text)
#define mia_token_scalar(_name, _kind, _bits, _signed) case mia_scalar_##_name: return mia_code_##_name;
#include "mia/tokens.def"
#undef mia_token_scalar
#undef mia_token_punctuator
#undef mia_token_keyword
			default: break;
		}
	}
	if (mia_type_is_special(type, mia_type_special_untyped_integer)) return mia_code_int;
	if (mia_type_is_special(type, mia_type_special_untyped_float)) return mia_code_float;
	if (mia_type_function == type->kind) return mia_code_fn;
	_mia_compiler_fail(c, "no opcode type for `%s`", mia_checker_name_of(c->checker, type));
}

/// how a value of 'type' is loaded and stored: its type code (BYTES for a struct or an array).
static mia_code_e _mia_compiler_memory_code(_mia_compiler_s* const c, const mia_type_s* const type) {
	if (mia_type_slice == type->kind || mia_type_reference == type->kind) return mia_code_slice;
	if (mia_type_array == type->kind || mia_type_struct == type->kind) return mia_code_bytes;
	return _mia_compiler_type_code(c, type);
}

// --- tables ----------------------------------------------------------------------------

static int64_t _mia_compiler_function(_mia_compiler_s* const c, const void* const target, const bool lambda) {
	int64_t known = 0;
	if (mia_map_index_find(&c->function_of, mia_map_key_of(target), &known)) return known;
	mia_bytecode_function_s function = {0};
	if (!lambda) {
		const mia_program_instance_s* const instance = target;
		mia_string_s name = {0};
		mia_program_instance_text(&name, instance);
		function.name = mia_string_take(&name);
		function.parameters = (uint32_t)instance->parameters.count;
	}
	else {
		const mia_ast_node_s* const node = target;
		const mia_source_s* const source = mia_source_get(c->program->sources, node->span.file);
		const mia_source_location_s location = mia_source_location(source, node->span.begin);
		const char* const slash = strrchr(source->path, '/');
		const char* const backslash = strrchr(source->path, '\\');
		const char* base = source->path;
		if (slash != NULL && slash + 1 > base) base = slash + 1;
		if (backslash != NULL && backslash + 1 > base) base = backslash + 1;
		function.name = mia_string_format("lambda at %s:%u", base, location.line);
		function.parameters = (uint32_t)node->as.lambda.parameters.count;
	}
	const int64_t index = (int64_t)c->module->functions.count;
	mia_vector_push(&c->module->functions, function);
	const _mia_compiler_pending_s pending = { .target = target, .index = index, .lambda = lambda, };
	mia_vector_push(&c->functions, pending);
	mia_map_index_set(&c->function_of, mia_map_key_of(target), index);
	return index;
}

static bool _mia_compiler_constant_equal(const mia_bytecode_constant_s* const a, const mia_bytecode_constant_s* const b) {
	if (a->kind != b->kind || a->type != b->type) return false;
	switch (a->kind) {
		case mia_bytecode_constant_none:      return true;
		case mia_bytecode_constant_integer:   return a->integer == b->integer && a->is_signed == b->is_signed;
		case mia_bytecode_constant_boolean:   return a->boolean == b->boolean;
		case mia_bytecode_constant_text:      return 0 == strcmp(a->text, b->text);
		case mia_bytecode_constant_real:      return 0 == memcmp(&a->real, &b->real, sizeof(a->real));
		case mia_bytecode_constant_function:
		case mia_bytecode_constant_intrinsic: return a->integer == b->integer;
		case mia_bytecode_constant_slice:
			if (a->null != b->null || a->read_only != b->read_only || a->start != b->start || a->length != b->length) return false;
			break;
		case mia_bytecode_constant_list:
			break;
	}
	if (a->items.count != b->items.count) return false;
	for (size_t i = 0; i < a->items.count; ++i) if (!_mia_compiler_constant_equal(&a->items.data[i], &b->items.data[i])) return false;
	return true;
}

/// a hash of what _mia_compiler_constant_equal compares.
static uint64_t _mia_compiler_constant_hash(const mia_bytecode_constant_s* const constant) {
	uint64_t hash = 0xCBF29CE484222325ull;
#define _mia_compiler_mix(_value) (hash = (hash ^ (uint64_t)(_value)) * 0x100000001B3ull)
	_mia_compiler_mix(constant->kind);
	_mia_compiler_mix(constant->type);
	switch (constant->kind) {
		case mia_bytecode_constant_integer:
		case mia_bytecode_constant_function:
		case mia_bytecode_constant_intrinsic: _mia_compiler_mix(constant->integer); break;
		case mia_bytecode_constant_real: {
			uint64_t bits = 0;
			memcpy(&bits, &constant->real, sizeof(bits));
			_mia_compiler_mix(bits);
			break;
		}
		case mia_bytecode_constant_boolean: _mia_compiler_mix(constant->boolean); break;
		case mia_bytecode_constant_text: for (const char* at = constant->text; *at != '\0'; ++at) _mia_compiler_mix((uint8_t)*at); break;
		case mia_bytecode_constant_slice: _mia_compiler_mix(constant->start); _mia_compiler_mix(constant->length); _mia_compiler_mix(constant->null); break;
		default: break;
	}
	for (size_t i = 0; i < constant->items.count; ++i) _mia_compiler_mix(_mia_compiler_constant_hash(&constant->items.data[i]));
#undef _mia_compiler_mix
	return hash;
}

/// adds a constant (taking ownership); equal constants are shared.
static int64_t _mia_compiler_constant(_mia_compiler_s* const c, const mia_bytecode_constant_s given) {
	mia_bytecode_constant_s constant = given;
	const uint64_t hash = _mia_compiler_constant_hash(&constant);
	int64_t first = -1;
	(void)mia_map_index_find(&c->constant_of, hash, &first);
	for (int64_t i = first; i >= 0; i = c->same_hash.data[i]) {
		if (_mia_compiler_constant_equal(&c->module->constants.data[i], &constant)) {
			mia_bytecode_constant_free(&constant);
			return i;
		}
	}
	const int64_t index = (int64_t)c->module->constants.count;
	mia_vector_push(&c->module->constants, constant);
	mia_vector_push(&c->same_hash, first);
	mia_map_index_set(&c->constant_of, hash, index);
	return index;
}

static mia_bytecode_constant_s _mia_compiler_integer_constant(const mia_int128_s value, const bool is_signed) {
	return (mia_bytecode_constant_s) { .kind = mia_bytecode_constant_integer, .type = -1, .integer = mia_int128_low(value), .is_signed = is_signed, };
}

static mia_bytecode_constant_s _mia_compiler_real_constant(const double value) {
	return (mia_bytecode_constant_s) { .kind = mia_bytecode_constant_real, .type = -1, .real = value, };
}

static mia_bytecode_constant_s _mia_compiler_boolean_constant(const bool value) {
	return (mia_bytecode_constant_s) { .kind = mia_bytecode_constant_boolean, .type = -1, .boolean = value, };
}

static mia_bytecode_constant_s _mia_compiler_null_slice(void) {
	return (mia_bytecode_constant_s) { .kind = mia_bytecode_constant_slice, .type = -1, .null = true, };
}

static int64_t _mia_compiler_describe(_mia_compiler_s* const c, const mia_type_s* const type);

static void _mia_compiler_describe_all(_mia_compiler_s* const c, const mia_type_s* const* const types, const size_t count, mia_bytecode_indexes_s* const out) {
	for (size_t i = 0; i < count; ++i) {
		const mia_type_s* type = types[i];
		if (mia_type_reference == type->kind) type = type->as.reference.element;
		if (mia_type_is_special(type, mia_type_special_variadic)) continue;
		mia_vector_push(out, _mia_compiler_describe(c, type));
	}
}

/// the intrinsic table entry of a use of an external function.
static int64_t _mia_compiler_intrinsic(_mia_compiler_s* const c, const mia_program_instance_s* const instance, const mia_type_list_s* const variadic,
									   const mia_ast_node_s* const call) {
	mia_bytecode_intrinsic_s entry = { .name = mia_program_function_qualified_name(instance->function), .result = -1, };
	entry.plugin = mia_program_function_plugin(instance->function);
	if (NULL == entry.plugin) entry.plugin = mia_strdup("");   // reported by the checker
	if (instance->result != NULL) entry.result = _mia_compiler_describe(c, instance->result);
	if (call != NULL) for (uint32_t i = 0; i < call->note.flag_count; ++i) mia_vector_push(&entry.flags, call->note.flags[i]);
	_mia_compiler_describe_all(c, instance->arguments, instance->argument_count, &entry.arguments);
	_mia_compiler_describe_all(c, instance->parameters.data, instance->parameters.count, &entry.parameters);
	if (variadic != NULL) _mia_compiler_describe_all(c, variadic->data, variadic->count, &entry.variadic);
	for (size_t i = 0; i < c->module->intrinsics.count; ++i) {
		const mia_bytecode_intrinsic_s* const other = &c->module->intrinsics.data[i];
		bool same = 0 == strcmp(other->name, entry.name) && other->result == entry.result && other->arguments.count == entry.arguments.count
			&& other->parameters.count == entry.parameters.count && other->variadic.count == entry.variadic.count;
		for (size_t t = 0; same && t < entry.arguments.count; ++t) same = other->arguments.data[t] == entry.arguments.data[t];
		for (size_t t = 0; same && t < entry.parameters.count; ++t) same = other->parameters.data[t] == entry.parameters.data[t];
		for (size_t t = 0; same && t < entry.variadic.count; ++t) same = other->variadic.data[t] == entry.variadic.data[t];
		same = same && other->flags.count == entry.flags.count;
		for (size_t t = 0; same && t < entry.flags.count; ++t) same = other->flags.data[t] == entry.flags.data[t];
		if (same) {
			mia_free(entry.name);
			mia_free(entry.plugin);
			mia_vector_free(&entry.arguments);
			mia_vector_free(&entry.parameters);
			mia_vector_free(&entry.variadic);
			mia_vector_free(&entry.flags);
			return (int64_t)i;
		}
	}
	mia_vector_push(&c->module->intrinsics, entry);
	return (int64_t)c->module->intrinsics.count - 1;
}

static mia_bytecode_position_s _mia_compiler_position(_mia_compiler_s* const c, const mia_span_s span, const bool has_span) {
	if (!has_span) return (mia_bytecode_position_s) { .file = -1, };
	const mia_source_s* const source = mia_source_get(c->program->sources, span.file);
	int64_t file = -1;
	if (!mia_map_index_find(&c->file_of, span.file, &file)) {
		file = (int64_t)c->module->files.count;
		mia_vector_push(&c->module->files, mia_strdup(source->path));
		mia_map_index_set(&c->file_of, span.file, file);
	}
	const mia_source_location_s first = mia_source_location(source, span.begin);
	const mia_source_location_s last = mia_source_location(source, span.end > span.begin ? span.end - 1 : span.begin);
	int64_t length = 0;
	if (last.line == first.line) {
		length = (int64_t)last.column - first.column + 1;
	}
	else {
		const mia_view_s line = mia_source_line(source, first.line);
		size_t points = 0;
		for (size_t i = 0; i < line.length; ++i) if (0x80 != ((uint8_t)line.data[i] & 0xC0)) ++points;
		length = (int64_t)points - first.column + 1;
	}
	return (mia_bytecode_position_s) { .file = (int32_t)file, .line = first.line, .column = first.column, .length = (uint32_t)(length > 1 ? length : 1), };
}

// --- values ------------------------------------------------------------------------------

static mia_bytecode_constant_s _mia_compiler_convert(_mia_compiler_s* const c, const mia_evaluator_value_s* const value, const mia_type_s* const type);

/// whether the zero value of 'type' has bytes that are not zero (field defaults).
static bool _mia_compiler_zero_has_defaults(_mia_compiler_s* const c, const mia_type_s* const type) {
	if (mia_type_array == type->kind) return type->as.array.length > 0 && _mia_compiler_zero_has_defaults(c, type->as.array.element);
	if (mia_type_struct != type->kind) return false;
	const mia_program_layout_s* const layout = mia_checker_layout(c->checker, type);
	for (size_t i = 0; i < layout->fields.count; ++i) {
		if (layout->fields.data[i]->default_value != NULL || _mia_compiler_zero_has_defaults(c, layout->fields.data[i]->type)) return true;
	}
	return false;
}

static mia_bytecode_constant_s _mia_compiler_zero(_mia_compiler_s* const c, const mia_type_s* const type) {
	switch (type->kind) {
		case mia_type_scalar: {
			const char kind = mia_scalar_kind(type->as.scalar);
			if ('b' == kind) return _mia_compiler_boolean_constant(false);
			if ('f' == kind) return _mia_compiler_real_constant(0.0);
			return _mia_compiler_integer_constant(mia_int128_zero(), mia_scalar_signed(type->as.scalar));
		}
		case mia_type_slice:
			return _mia_compiler_null_slice();
		case mia_type_array: {
			mia_bytecode_constant_s list = { .kind = mia_bytecode_constant_list, .type = _mia_compiler_describe(c, type), };
			for (int64_t i = 0; i < type->as.array.length; ++i) mia_vector_push(&list.items, _mia_compiler_zero(c, type->as.array.element));
			return list;
		}
		case mia_type_struct: {
			mia_bytecode_constant_s list = { .kind = mia_bytecode_constant_list, .type = _mia_compiler_describe(c, type), };
			const mia_program_layout_s* const layout = mia_checker_layout(c->checker, type);
			for (size_t i = 0; i < layout->fields.count; ++i) {
				const mia_program_field_s* const field = layout->fields.data[i];
				if (field->default_value != NULL) {
					const mia_evaluator_result_s result = mia_evaluator_evaluate(&c->checker->evaluator, field->default_value, false);
					if (result.status != mia_evaluator_ok) _mia_compiler_fail(c, "a field default cannot be evaluated");
					mia_vector_push(&list.items, _mia_compiler_convert(c, &result.value, field->type));
				}
				else {
					mia_vector_push(&list.items, _mia_compiler_zero(c, field->type));
				}
			}
			return list;
		}
		default:
			return (mia_bytecode_constant_s) { .kind = mia_bytecode_constant_none, .type = -1, };
	}
}

static mia_bytecode_constant_s _mia_compiler_convert(_mia_compiler_s* const c, const mia_evaluator_value_s* const value, const mia_type_s* const type) {
	if (mia_type_is_untyped(type)) {
		if (mia_type_is_special(type, mia_type_special_untyped_float)) {
			return _mia_compiler_real_constant(mia_evaluator_value_real == value->kind ? value->as.real : mia_int128_to_double(value->as.integer));
		}
		return (mia_evaluator_value_real == value->kind) ? _mia_compiler_real_constant(value->as.real) : _mia_compiler_integer_constant(value->as.integer, true);
	}
	switch (type->kind) {
		case mia_type_scalar: {
			const char kind = mia_scalar_kind(type->as.scalar);
			if ('b' == kind) return _mia_compiler_boolean_constant(mia_evaluator_value_boolean == value->kind ? value->as.boolean : !mia_int128_is_zero(value->as.integer));
			if ('f' == kind) return _mia_compiler_real_constant(mia_evaluator_float_wrap(mia_evaluator_value_real == value->kind ? value->as.real : mia_int128_to_double(value->as.integer), type));
			return _mia_compiler_integer_constant(mia_evaluator_integer_wrap(value->as.integer, type), mia_scalar_signed(type->as.scalar));
		}
		case mia_type_array: {
			mia_bytecode_constant_s list = { .kind = mia_bytecode_constant_list, .type = _mia_compiler_describe(c, type), };
			for (size_t i = 0; mia_evaluator_value_list == value->kind && i < value->as.list->count; ++i) {
				mia_vector_push(&list.items, _mia_compiler_convert(c, &value->as.list->items[i], type->as.array.element));
			}
			return list;
		}
		case mia_type_slice: {
			if (value->kind != mia_evaluator_value_slice || NULL == value->as.slice.list || (0 == value->as.slice.list->count && 0 == value->as.slice.length)) {
				return _mia_compiler_null_slice();
			}
			mia_bytecode_constant_s slice = { .kind = mia_bytecode_constant_slice, .type = _mia_compiler_describe(c, type), .start = value->as.slice.start,
				.length = value->as.slice.length, .read_only = true, };
			for (size_t i = 0; i < value->as.slice.list->count; ++i) mia_vector_push(&slice.items, _mia_compiler_convert(c, &value->as.slice.list->items[i], type->as.slice.element));
			return slice;
		}
		case mia_type_struct: {
			mia_bytecode_constant_s list = { .kind = mia_bytecode_constant_list, .type = _mia_compiler_describe(c, type), };
			const mia_program_layout_s* const layout = mia_checker_layout(c->checker, type);
			for (size_t i = 0; i < layout->fields.count && mia_evaluator_value_list == value->kind; ++i) {
				mia_vector_push(&list.items, _mia_compiler_convert(c, &value->as.list->items[i], layout->fields.data[i]->type));
			}
			return list;
		}
		case mia_type_function:
			if (mia_evaluator_value_function == value->kind) {
				if (value->as.function.lambda != NULL) {
					return (mia_bytecode_constant_s) { .kind = mia_bytecode_constant_function, .type = -1, .integer = (uint64_t)_mia_compiler_function(c, value->as.function.lambda, true), };
				}
				const mia_program_instance_s* const instance = value->as.function.instance;
				if (mia_program_function_is_plugin(instance->function)) {
					return (mia_bytecode_constant_s) { .kind = mia_bytecode_constant_intrinsic, .type = -1, .integer = (uint64_t)_mia_compiler_intrinsic(c, instance, NULL, NULL), };
				}
				return (mia_bytecode_constant_s) { .kind = mia_bytecode_constant_function, .type = -1, .integer = (uint64_t)_mia_compiler_function(c, instance, false), };
			}
			break;
		default:
			break;
	}
	_mia_compiler_fail(c, "cannot convert a constant of type `%s`", mia_checker_name_of(c->checker, type));
}

static int64_t _mia_compiler_describe(_mia_compiler_s* const c, const mia_type_s* const type) {
	int64_t known = 0;
	if (mia_map_index_find(&c->type_of, mia_map_key_of(type), &known)) return known;
	const int64_t index = (int64_t)c->module->types.count;
	mia_map_index_set(&c->type_of, mia_map_key_of(type), index);
	uint64_t alignment = 1;
	const uint64_t size = _mia_compiler_size(c, type, &alignment);
	mia_bytecode_type_s descriptor = { .zero = -1, .name = mia_type_name(type), .size = size, .alignment = alignment, .code = mia_code_bytes, };
	mia_vector_push(&c->module->types, descriptor);
	mia_bytecode_descriptor_kind_e kind = mia_bytecode_descriptor_int;
	mia_code_e code = mia_code_bytes;
	int64_t element = 0, length = 0;
	switch (type->kind) {
		case mia_type_scalar: {
			const char scalar = mia_scalar_kind(type->as.scalar);
			kind = ('b' == scalar) ? mia_bytecode_descriptor_bool : ('f' == scalar) ? mia_bytecode_descriptor_float : mia_bytecode_descriptor_int;
			code = _mia_compiler_type_code(c, type);
			break;
		}
		case mia_type_array:
			kind = mia_bytecode_descriptor_array;
			element = _mia_compiler_describe(c, type->as.array.element);
			length = type->as.array.length;
			break;
		case mia_type_slice:
			kind = mia_bytecode_descriptor_slice;
			code = mia_code_slice;
			element = _mia_compiler_describe(c, type->as.slice.element);
			break;
		case mia_type_struct: {
			kind = mia_bytecode_descriptor_struct;
			const mia_program_layout_s* const layout = mia_checker_layout(c->checker, type);
			for (size_t i = 0; i < layout->fields.count; ++i) {
				const int64_t field_type = _mia_compiler_describe(c, layout->fields.data[i]->type);
				mia_bytecode_type_s* const self = &c->module->types.data[index];
				mia_vector_push(&self->field_names, mia_string_format(mia_view_fmt, mia_view_arg(layout->fields.data[i]->name)));
				mia_vector_push(&self->field_types, field_type);
				mia_vector_push(&self->field_offsets, layout->fields.data[i]->offset);
			}
			break;
		}
		case mia_type_function:
			kind = mia_bytecode_descriptor_function;
			code = mia_code_fn;
			break;
		default:
			_mia_compiler_fail(c, "cannot describe `%s`", mia_checker_name_of(c->checker, type));
	}
	mia_bytecode_type_s* self = &c->module->types.data[index];
	self->kind = kind;
	self->code = code;
	self->element = element;
	self->length = length;
	if (_mia_compiler_zero_has_defaults(c, type)) {
		const int64_t zero = _mia_compiler_constant(c, _mia_compiler_zero(c, type));
		self = &c->module->types.data[index];
		self->zero = zero;
	}
	return index;
}

// --- functions ---------------------------------------------------------------------------

typedef struct {
	mia_vector_of(int64_t) breaks   ;
	mia_vector_of(int64_t) continues;
	int64_t                target   ;   // where 'continue' jumps; -1: patched later
} _mia_compiler_loop_s;

typedef struct {
	_mia_compiler_s*                    c       ;
	int64_t                             index   ;   // of the function being compiled
	mia_map_index_s                     slots   ;   // variable -> its offset in the frame
	uint64_t                            frame   ;   // bytes of the frame so far
	mia_span_s                          span    ;
	bool                                has_span;
	mia_vector_of(_mia_compiler_loop_s) loops   ;
} _mia_compiler_function_s;

static mia_bytecode_function_s* _mia_compiler_target(_mia_compiler_function_s* const f) {
	return &f->c->module->functions.data[f->index];
}

/// room in the frame for 'size' bytes.
static int64_t _mia_compiler_reserve(_mia_compiler_function_s* const f, const uint64_t size, const uint64_t wanted) {
	const uint64_t alignment = (wanted < 1) ? 1 : (wanted > 8) ? 8 : wanted;
	const uint64_t offset = (f->frame + alignment - 1) / alignment * alignment;
	f->frame = offset + size;
	return (int64_t)offset;
}

static int64_t _mia_compiler_reserve_type(_mia_compiler_function_s* const f, const mia_type_s* const type) {
	uint64_t alignment = 1;
	const uint64_t size = _mia_compiler_size(f->c, type, &alignment);
	return _mia_compiler_reserve(f, size, alignment);
}

static bool _mia_compiler_has_slot(_mia_compiler_function_s* const f, const mia_program_variable_s* const variable, int64_t* const offset) {
	return mia_map_index_find(&f->slots, mia_map_key_of(variable), offset);
}

/// the offset of a variable's slot (reference parameters hold a location).
static int64_t _mia_compiler_slot(_mia_compiler_function_s* const f, const mia_program_variable_s* const variable) {
	int64_t offset = 0;
	if (_mia_compiler_has_slot(f, variable, &offset)) return offset;
	offset = variable->reference ? _mia_compiler_reserve(f, 16, 8) : _mia_compiler_reserve_type(f, variable->type);
	mia_map_index_set(&f->slots, mia_map_key_of(variable), offset);
	return offset;
}

static int64_t _mia_compiler_emit_full(_mia_compiler_function_s* const f, const mia_bytecode_instruction_s instruction) {
	const mia_bytecode_position_s position = _mia_compiler_position(f->c, f->span, f->has_span);
	mia_bytecode_function_s* const function = _mia_compiler_target(f);
	mia_vector_push(&function->code, instruction);
	mia_vector_push(&function->positions, position);
	return (int64_t)function->code.count - 1;
}

static int64_t _mia_compiler_emit(_mia_compiler_function_s* const f, const mia_opcode_e opcode) {
	return _mia_compiler_emit_full(f, (mia_bytecode_instruction_s) { .opcode = opcode, });
}

static int64_t _mia_compiler_emit_a(_mia_compiler_function_s* const f, const mia_opcode_e opcode, const int64_t a) {
	return _mia_compiler_emit_full(f, (mia_bytecode_instruction_s) { .opcode = opcode, .a = a, });
}

static int64_t _mia_compiler_emit_ab(_mia_compiler_function_s* const f, const mia_opcode_e opcode, const int64_t a, const int64_t b) {
	return _mia_compiler_emit_full(f, (mia_bytecode_instruction_s) { .opcode = opcode, .a = a, .b = b, });
}

static int64_t _mia_compiler_here(_mia_compiler_function_s* const f) {
	return (int64_t)_mia_compiler_target(f)->code.count;
}

/// where an instruction jumps to; -1 when it does not jump.
static int64_t _mia_compiler_jump_target(const mia_bytecode_instruction_s* const instruction) {
	switch (instruction->opcode) {
		case mia_opcode_JUMP: case mia_opcode_JUMP_IF_FALSE: case mia_opcode_JUMP_IF_TRUE: return instruction->a;
		case mia_opcode_RANGE_TEST: case mia_opcode_RANGE_NEXT: return instruction->b;
		default: return -1;
	}
}

static void _mia_compiler_set_jump_target(mia_bytecode_instruction_s* const instruction, const int64_t target) {
	if (mia_opcode_RANGE_TEST == instruction->opcode || mia_opcode_RANGE_NEXT == instruction->opcode) instruction->b = target;
	else instruction->a = target;
}

static void _mia_compiler_patch(_mia_compiler_function_s* const f, const int64_t index, const int64_t target) {
	_mia_compiler_set_jump_target(&_mia_compiler_target(f)->code.data[index], target);
}

static void _mia_compiler_push_constant(_mia_compiler_function_s* const f, const mia_bytecode_constant_s constant) {
	_mia_compiler_emit_a(f, mia_opcode_CONST, _mia_compiler_constant(f->c, constant));
}

static int64_t _mia_compiler_emit_typed(_mia_compiler_function_s* const f, const mia_opcode_e opcode, const mia_type_s* const type) {
	return _mia_compiler_emit_full(f, (mia_bytecode_instruction_s) { .opcode = opcode, .type = _mia_compiler_type_code(f->c, type), });
}

/// a memory instruction for a value of 'type': its type code, the operands
/// 'a' and 'b' (as far as it has them), and the value's size as its last operand.
static void _mia_compiler_emit_memory(_mia_compiler_function_s* const f, const mia_opcode_e opcode, const mia_type_s* const type, const int64_t a, const int64_t b) {
	mia_bytecode_instruction_s instruction = { .opcode = opcode, .type = _mia_compiler_memory_code(f->c, type), .a = a, .b = b, };
	const int64_t size = (int64_t)_mia_compiler_size(f->c, type, NULL);
	switch (mia_opcode_operands(opcode)) {
		case 1:  instruction.a = size; break;
		case 2:  instruction.b = size; break;
		default: instruction.c = size; break;
	}
	_mia_compiler_emit_full(f, instruction);
}

static void _mia_compiler_load_local(_mia_compiler_function_s* const f, const int64_t offset, const mia_type_s* const type) {
	_mia_compiler_emit_memory(f, mia_opcode_LOAD, type, offset, 0);
}

static void _mia_compiler_store_local(_mia_compiler_function_s* const f, const int64_t offset, const mia_type_s* const type) {
	_mia_compiler_emit_memory(f, mia_opcode_STORE, type, offset, 0);
}

/// the location a reference parameter holds.
static void _mia_compiler_load_reference(_mia_compiler_function_s* const f, const int64_t offset) {
	_mia_compiler_emit_full(f, (mia_bytecode_instruction_s) { .opcode = mia_opcode_LOAD, .type = mia_code_slice, .a = offset, .b = 16, });
}

static void _mia_compiler_pop(_mia_compiler_function_s* const f, const mia_type_s* const type) {
	const int64_t words = _mia_compiler_words(f->c, type);
	if (words > 0) _mia_compiler_emit_a(f, mia_opcode_POP, words);
}

static mia_opcode_e _mia_compiler_operator_opcode(const mia_ast_operator_e operation) {
	switch (operation) {
		case mia_ast_operator_add:           return mia_opcode_ADD;
		case mia_ast_operator_subtract:      return mia_opcode_SUB;
		case mia_ast_operator_multiply:      return mia_opcode_MUL;
		case mia_ast_operator_divide:        return mia_opcode_DIV;
		case mia_ast_operator_remainder:     return mia_opcode_MOD;
		case mia_ast_operator_bit_and:       return mia_opcode_AND;
		case mia_ast_operator_bit_or:        return mia_opcode_OR;
		case mia_ast_operator_bit_xor:       return mia_opcode_XOR;
		case mia_ast_operator_shift_left:    return mia_opcode_SHL;
		case mia_ast_operator_shift_right:   return mia_opcode_SHR;
		case mia_ast_operator_equal:         return mia_opcode_EQ;
		case mia_ast_operator_not_equal:     return mia_opcode_NE;
		case mia_ast_operator_less:          return mia_opcode_LT;
		case mia_ast_operator_greater:       return mia_opcode_GT;
		case mia_ast_operator_less_equal:    return mia_opcode_LE;
		case mia_ast_operator_greater_equal: return mia_opcode_GE;
		default:                        mia_panic("not an arithmetic operator");
	}
}

/// an arithmetic or comparison instruction on 'left' (and the type of a shift count).
static void _mia_compiler_emit_operator(_mia_compiler_function_s* const f, const mia_ast_operator_e operation, const mia_type_s* const left, const mia_type_s* const right) {
	const mia_opcode_e opcode = _mia_compiler_operator_opcode(operation);
	mia_bytecode_instruction_s instruction = { .opcode = opcode, .type = _mia_compiler_type_code(f->c, left), };
	if (mia_opcode_SHL == opcode || mia_opcode_SHR == opcode) instruction.target = _mia_compiler_type_code(f->c, right);
	_mia_compiler_emit_full(f, instruction);
}

static void _mia_compiler_value(_mia_compiler_function_s* const f, const mia_ast_node_s* const node);
static void _mia_compiler_statement(_mia_compiler_function_s* const f, const mia_ast_node_s* const node);

static void _mia_compiler_block(_mia_compiler_function_s* const f, const mia_ast_node_s* const node) {
	for (size_t i = 0; i < node->as.block.statements.count; ++i) _mia_compiler_statement(f, node->as.block.statements.data[i]);
}

/// the zero value of 'type' on the stack.
static void _mia_compiler_zero_value(_mia_compiler_function_s* const f, const mia_type_s* const type) {
	if (_mia_compiler_zero_has_defaults(f->c, type)) _mia_compiler_push_constant(f, _mia_compiler_zero(f->c, type));
	else _mia_compiler_emit_a(f, mia_opcode_ZERO, _mia_compiler_words(f->c, type));
}

// --- calls -------------------------------------------------------------------------------

static void _mia_compiler_emit_call(_mia_compiler_function_s* const f, const mia_program_instance_s* const instance, const int64_t words, const mia_ast_node_s* const call) {
	if (mia_program_function_is_plugin(instance->function)) {
		_mia_compiler_emit_ab(f, mia_opcode_CALL_INTRINSIC, _mia_compiler_intrinsic(f->c, instance, (call != NULL) ? call->note.variadic : NULL, call), words);
	}
	else _mia_compiler_emit_a(f, mia_opcode_CALL, _mia_compiler_function(f->c, instance, false));
}

static void _mia_compiler_function_value(_mia_compiler_function_s* const f, const mia_program_instance_s* const instance) {
	if (mia_program_function_is_plugin(instance->function)) _mia_compiler_emit_a(f, mia_opcode_INTRINSIC, _mia_compiler_intrinsic(f->c, instance, NULL, NULL));
	else _mia_compiler_emit_a(f, mia_opcode_FUNCTION, _mia_compiler_function(f->c, instance, false));
}

// --- places ------------------------------------------------------------------------------

/// whether a slice of the array 'node' must not be written through.
static bool _mia_compiler_read_only(const mia_ast_node_s* const node) {
	switch (node->kind) {
		case mia_ast_node_paren:  return _mia_compiler_read_only(node->as.paren.inner);
		case mia_ast_node_name:   return NULL == node->note.variable;
		case mia_ast_node_member: return NULL == node->note.field || _mia_compiler_read_only(node->as.member.target);
		case mia_ast_node_index:  return mia_type_array == node->as.index.target->note.type->kind && _mia_compiler_read_only(node->as.index.target);
		case mia_ast_node_string: return true;
		default:             return false;
	}
}

/// whether 'node' is constant data (a string literal or a constant).
static bool _mia_compiler_static(const mia_ast_node_s* const node) {
	switch (node->kind) {
		case mia_ast_node_paren:  return _mia_compiler_static(node->as.paren.inner);
		case mia_ast_node_string: return true;
		case mia_ast_node_name:
		case mia_ast_node_member: return NULL == node->note.field && node->note.constant != NULL;
		default:             return false;
	}
}

/// the constant of a static node (see _mia_compiler_static).
static int64_t _mia_compiler_static_constant(_mia_compiler_function_s* const f, const mia_ast_node_s* const node) {
	if (mia_ast_node_paren == node->kind) return _mia_compiler_static_constant(f, node->as.paren.inner);
	if (mia_ast_node_string == node->kind) {
		mia_bytecode_constant_s list = { .kind = mia_bytecode_constant_list, .type = _mia_compiler_describe(f->c, node->note.type), };
		const mia_view_s data = node->as.string.data;
		for (size_t i = 0; i < data.length; ++i) mia_vector_push(&list.items, _mia_compiler_integer_constant(mia_int128_from_uint((uint8_t)data.data[i]), false));
		return _mia_compiler_constant(f->c, list);
	}
	const mia_program_constant_s* const constant = node->note.constant;
	if (NULL == constant->value) _mia_compiler_fail(f->c, "the constant `%.*s` has no value", mia_view_arg(mia_ast_declaration_name(constant->node)));
	const mia_type_s* const type = mia_type_is_untyped(node->note.type) ? constant->type : node->note.type;
	return _mia_compiler_constant(f->c, _mia_compiler_convert(f->c, constant->value, type));
}

/// a place at a fixed offset of the frame: a local that is not a
/// reference, its fields, and elements of its arrays at constant indexes.
static bool _mia_compiler_frame_place(_mia_compiler_function_s* const f, const mia_ast_node_s* const node, int64_t* const offset) {
	switch (node->kind) {
		case mia_ast_node_paren:
			return _mia_compiler_frame_place(f, node->as.paren.inner, offset);
		case mia_ast_node_name: {
			const mia_program_variable_s* const variable = node->note.variable;
			if (NULL == variable || variable->reference || !_mia_compiler_has_slot(f, variable, offset)) return false;
			return true;
		}
		case mia_ast_node_member:
			if (NULL == node->note.field || !_mia_compiler_frame_place(f, node->as.member.target, offset)) return false;
			*offset += (int64_t)node->note.field->offset;
			return true;
		case mia_ast_node_index: {
			const mia_type_s* const target = node->as.index.target->note.type;
			if (target->kind != mia_type_array) return false;
			mia_evaluator_value_s value = {0};
			if (!mia_checker_try_value(f->c->checker, node->as.index.index, &value) || value.kind != mia_evaluator_value_integer) return false;
			if (mia_int128_negative(value.as.integer) || !mia_int128_less(value.as.integer, mia_int128_from_int(target->as.array.length))) return false;
			if (!_mia_compiler_frame_place(f, node->as.index.target, offset)) return false;
			*offset += mia_int128_to_int(value.as.integer) * (int64_t)_mia_compiler_size(f->c, target->as.array.element, NULL);
			return true;
		}
		default:
			return false;
	}
}

/// whether 'node' has an address: a variable, a field of one, an element of
/// an array that has one, an element of a slice.
static bool _mia_compiler_addressable(const mia_ast_node_s* const node) {
	switch (node->kind) {
		case mia_ast_node_paren:  return _mia_compiler_addressable(node->as.paren.inner);
		case mia_ast_node_name:   return node->note.variable != NULL;
		case mia_ast_node_member: return node->note.field != NULL && _mia_compiler_addressable(node->as.member.target);
		case mia_ast_node_index:
			return mia_type_slice == node->as.index.target->note.type->kind || _mia_compiler_addressable(node->as.index.target);
		default: return false;
	}
}

static void _mia_compiler_location(_mia_compiler_function_s* const f, const mia_ast_node_s* const node);

/// pushes an index for an element of 'length' elements of 'element'; the
/// location or slice is on the stack already.
static void _mia_compiler_index_location(_mia_compiler_function_s* const f, const mia_ast_node_s* const node) {
	const mia_ast_node_s* const index = node->as.index.index;
	const mia_type_s* const target = node->as.index.target->note.type;
	_mia_compiler_value(f, index);
	if (mia_type_array == target->kind) {
		const mia_bytecode_instruction_s instruction = { .opcode = mia_opcode_LOC_INDEX, .type = _mia_compiler_type_code(f->c, index->note.type),
			.a = (int64_t)_mia_compiler_size(f->c, target->as.array.element, NULL), .b = target->as.array.length, };
		_mia_compiler_emit_full(f, instruction);
	}
	else {
		const mia_bytecode_instruction_s instruction = { .opcode = mia_opcode_LOC_INDEX_SLICE, .type = _mia_compiler_type_code(f->c, index->note.type),
			.a = (int64_t)_mia_compiler_size(f->c, target->as.slice.element, NULL), };
		_mia_compiler_emit_full(f, instruction);
	}
}

/// pushes the location of a place (a temporary for other values).
static void _mia_compiler_location(_mia_compiler_function_s* const f, const mia_ast_node_s* const node) {
	if (mia_ast_node_paren == node->kind) {
		_mia_compiler_location(f, node->as.paren.inner);
		return;
	}
	const mia_program_variable_s* const variable = node->note.variable;
	if (mia_ast_node_name == node->kind && variable != NULL) {
		if (variable->reference) _mia_compiler_load_reference(f, _mia_compiler_slot(f, variable));
		else _mia_compiler_emit_a(f, mia_opcode_LOC_LOCAL, _mia_compiler_slot(f, variable));
		return;
	}
	if (_mia_compiler_static(node) && (mia_type_array == node->note.type->kind || mia_type_struct == node->note.type->kind)) {
		_mia_compiler_emit_a(f, mia_opcode_LOC_CONST, _mia_compiler_static_constant(f, node));
		return;
	}
	if (mia_ast_node_member == node->kind && node->note.field != NULL) {
		_mia_compiler_location(f, node->as.member.target);
		if (node->note.field->offset > 0) _mia_compiler_emit_a(f, mia_opcode_LOC_FIELD, (int64_t)node->note.field->offset);
		return;
	}
	if (mia_ast_node_index == node->kind) {
		if (mia_type_array == node->as.index.target->note.type->kind) _mia_compiler_location(f, node->as.index.target);
		else _mia_compiler_value(f, node->as.index.target);
		_mia_compiler_index_location(f, node);
		return;
	}
	_mia_compiler_value(f, node);
	const int64_t offset = _mia_compiler_reserve_type(f, node->note.type);
	_mia_compiler_emit_memory(f, mia_opcode_TEMP_LOC, node->note.type, offset, 0);
}

/// reads a place: from the frame, through a location, or from a value.
static void _mia_compiler_read_place(_mia_compiler_function_s* const f, const mia_ast_node_s* const node) {
	int64_t offset = 0;
	if (_mia_compiler_frame_place(f, node, &offset)) {
		_mia_compiler_load_local(f, offset, node->note.type);
		return;
	}
	_mia_compiler_location(f, node);
	_mia_compiler_emit_memory(f, mia_opcode_LOAD_LOC, node->note.type, 0, 0);
}

// --- expressions -------------------------------------------------------------------------

/// a literal number as a constant of 'type'.
static void _mia_compiler_scalar(_mia_compiler_function_s* const f, const bool real, const mia_int128_s integer, const double number, const mia_type_s* const type) {
	if (mia_type_scalar == type->kind) {
		const char kind = mia_scalar_kind(type->as.scalar);
		if ('b' == kind) _mia_compiler_push_constant(f, _mia_compiler_boolean_constant(real ? number != 0.0 : !mia_int128_is_zero(integer)));
		else if ('f' == kind) _mia_compiler_push_constant(f, _mia_compiler_real_constant(mia_evaluator_float_wrap(real ? number : mia_int128_to_double(integer), type)));
		else _mia_compiler_push_constant(f, _mia_compiler_integer_constant(mia_evaluator_integer_wrap(integer, type), mia_scalar_signed(type->as.scalar)));
	}
	else if (real || mia_type_is_special(type, mia_type_special_untyped_float)) {
		_mia_compiler_push_constant(f, _mia_compiler_real_constant(real ? number : mia_int128_to_double(integer)));
	}
	else {
		_mia_compiler_push_constant(f, _mia_compiler_integer_constant(integer, true));
	}
}

static void _mia_compiler_symbol(_mia_compiler_function_s* const f, const mia_ast_node_s* const node) {
	const mia_program_variable_s* const variable = node->note.variable;
	if (variable != NULL) {
		int64_t offset = 0;
		if (!_mia_compiler_has_slot(f, variable, &offset)) _mia_compiler_fail(f->c, "`%.*s` is not a local of this function", mia_view_arg(variable->name));
		if (variable->reference) {
			_mia_compiler_load_reference(f, offset);
			_mia_compiler_emit_memory(f, mia_opcode_LOAD_LOC, variable->type, 0, 0);
		}
		else {
			_mia_compiler_load_local(f, offset, variable->type);
		}
		return;
	}
	const mia_program_constant_s* const constant = node->note.constant;
	if (constant != NULL) {
		if (NULL == constant->value) _mia_compiler_fail(f->c, "the constant `%.*s` has no value", mia_view_arg(mia_ast_declaration_name(constant->node)));
		const mia_type_s* const type = mia_type_is_untyped(node->note.type) ? constant->type : node->note.type;
		const mia_bytecode_constant_s converted = _mia_compiler_convert(f->c, constant->value, type);
		if (mia_bytecode_constant_slice == converted.kind && converted.null) _mia_compiler_emit_a(f, mia_opcode_ZERO, 2);
		else _mia_compiler_push_constant(f, converted);
		return;
	}
	if (node->note.instance != NULL) {
		_mia_compiler_function_value(f, node->note.instance);
		return;
	}
	const mia_view_s text = mia_span_text(f->c->program->sources, node->span);
	_mia_compiler_fail(f->c, "cannot compile `%.*s`", mia_view_arg(text));
}

static int64_t _mia_compiler_argument(_mia_compiler_function_s* const f, const mia_ast_node_s* const argument, const mia_type_s* const parameter) {
	if (mia_type_reference == parameter->kind) {
		_mia_compiler_location(f, mia_ast_node_reference == argument->kind ? argument->as.reference.operand : argument);
		return 2;
	}
	_mia_compiler_value(f, argument);
	return _mia_compiler_words(f->c, parameter);
}

/// a slice of all of the array 'node' (a format string given as a literal).
static void _mia_compiler_whole_slice(_mia_compiler_function_s* const f, const mia_ast_node_s* const node) {
	const mia_type_s* const type = node->note.type;
	_mia_compiler_location(f, node);
	const mia_bytecode_instruction_s instruction = { .opcode = mia_opcode_SLICE_LOC, .type = mia_code_int, .target = mia_code_int,
		.a = _mia_compiler_read_only(node) ? mia_bytecode_slice_read_only : 0, .b = type->as.array.length, .c = (int64_t)_mia_compiler_size(f->c, type->as.array.element, NULL), };
	_mia_compiler_emit_full(f, instruction);
}

static void _mia_compiler_call(_mia_compiler_function_s* const f, const mia_ast_node_s* const node) {
	const mia_ast_nodes_s* const arguments = &node->as.call.arguments;
	const mia_program_instance_s* const instance = node->note.instance;
	if (instance != NULL && instance->parameters.count > 0 && mia_type_is_special(mia_vector_last(&instance->parameters), mia_type_special_variadic)) {
		const size_t fixed = instance->parameters.count - 1;
		int64_t words = 0;
		for (size_t i = 0; i < arguments->count && i < fixed; ++i) {
			const mia_ast_node_s* const argument = arguments->data[i];
			const mia_type_s* const parameter = instance->parameters.data[i];
			if (mia_type_slice == parameter->kind && mia_type_array == argument->note.type->kind) {
				_mia_compiler_whole_slice(f, argument);
				words += 2;
			}
			else {
				words += _mia_compiler_argument(f, argument, parameter);
			}
		}
		for (size_t i = fixed; i < arguments->count; ++i) {
			_mia_compiler_value(f, arguments->data[i]);
			words += _mia_compiler_words(f->c, arguments->data[i]->note.type);
		}
		_mia_compiler_emit_call(f, instance, words, node);
		return;
	}
	if (instance != NULL) {
		const mia_ast_node_s* const declaration = (instance->node != NULL) ? instance->node : instance->function->node;
		int64_t words = 0;
		for (size_t i = 0; i < instance->parameters.count; ++i) {
			if (i < arguments->count) {
				words += _mia_compiler_argument(f, arguments->data[i], instance->parameters.data[i]);
			}
			else {
				_mia_compiler_value(f, declaration->as.function.parameters.data[i]->as.param.default_value);
				words += _mia_compiler_words(f->c, instance->parameters.data[i]);
			}
		}
		_mia_compiler_emit_call(f, instance, words, node);
		return;
	}
	const mia_ast_node_s* const callee = node->as.call.callee;
	_mia_compiler_value(f, callee);
	const mia_type_s* const type = callee->note.type;
	int64_t words = 0;
	for (size_t i = 0; i < arguments->count && i < type->as.function.count; ++i) words += _mia_compiler_argument(f, arguments->data[i], type->as.function.parameters[i]);
	_mia_compiler_emit_ab(f, mia_opcode_CALL_VALUE, words, _mia_compiler_words(f->c, type->as.function.result));
}

static mia_code_e _mia_compiler_index_code(_mia_compiler_function_s* const f, const mia_ast_node_s* const node) {
	return (node != NULL) ? _mia_compiler_type_code(f->c, node->note.type) : mia_code_int;
}

static void _mia_compiler_slice_of(_mia_compiler_function_s* const f, const mia_ast_node_s* const node) {
	const mia_ast_node_s* const target = node->as.slice_of.target;
	const mia_type_s* const type = target->note.type;
	int64_t flags = (node->as.slice_of.low != NULL ? mia_bytecode_slice_low : 0) | (node->as.slice_of.high != NULL ? mia_bytecode_slice_high : 0)
		| (node->as.slice_of.inclusive ? mia_bytecode_slice_inclusive : 0);
	mia_bytecode_instruction_s instruction = { .type = _mia_compiler_index_code(f, node->as.slice_of.low), .target = _mia_compiler_index_code(f, node->as.slice_of.high), };
	if (mia_type_slice == type->kind) {
		_mia_compiler_value(f, target);
		instruction.opcode = mia_opcode_SLICE_SLICE;
		instruction.b = (int64_t)_mia_compiler_size(f->c, type->as.slice.element, NULL);
	}
	else {
		_mia_compiler_location(f, target);
		if (_mia_compiler_read_only(target)) flags |= mia_bytecode_slice_read_only;
		instruction.opcode = mia_opcode_SLICE_LOC;
		instruction.b = type->as.array.length;
		instruction.c = (int64_t)_mia_compiler_size(f->c, type->as.array.element, NULL);
	}
	instruction.a = flags;
	if (node->as.slice_of.low != NULL) _mia_compiler_value(f, node->as.slice_of.low);
	if (node->as.slice_of.high != NULL) _mia_compiler_value(f, node->as.slice_of.high);
	_mia_compiler_emit_full(f, instruction);
}

static void _mia_compiler_unary(_mia_compiler_function_s* const f, const mia_ast_node_s* const node) {
	const mia_ast_node_s* const operand = node->as.unary.operand;
	const mia_ast_operator_e operation = node->as.unary.operation;
	if (mia_ast_operator_negate == operation && mia_ast_node_integer == operand->kind) {
		_mia_compiler_scalar(f, false, mia_int128_negate(operand->as.integer.value), 0.0, node->note.type);
		return;
	}
	if (mia_ast_operator_negate == operation && mia_ast_node_float == operand->kind) {
		_mia_compiler_scalar(f, true, mia_int128_zero(), -operand->as.floating.value, node->note.type);
		return;
	}
	_mia_compiler_value(f, operand);
	if (mia_ast_operator_not == operation) _mia_compiler_emit(f, mia_opcode_NOT);
	else _mia_compiler_emit_typed(f, mia_ast_operator_negate == operation ? mia_opcode_NEG : mia_opcode_BITNOT, node->note.type);
}

static void _mia_compiler_binary(_mia_compiler_function_s* const f, const mia_ast_node_s* const node) {
	const mia_ast_operator_e operation = node->as.binary.operation;
	const mia_ast_node_s* const left = node->as.binary.left;
	const mia_ast_node_s* const right = node->as.binary.right;
	if (mia_ast_operator_and == operation || mia_ast_operator_or == operation) {
		_mia_compiler_value(f, left);
		_mia_compiler_emit_a(f, mia_opcode_DUP, 1);
		const int64_t jump = _mia_compiler_emit_a(f, mia_ast_operator_and == operation ? mia_opcode_JUMP_IF_FALSE : mia_opcode_JUMP_IF_TRUE, 0);
		_mia_compiler_emit_a(f, mia_opcode_POP, 1);
		_mia_compiler_value(f, right);
		_mia_compiler_patch(f, jump, _mia_compiler_here(f));
		return;
	}
	_mia_compiler_value(f, left);
	_mia_compiler_value(f, right);
	_mia_compiler_emit_operator(f, operation, left->note.type, right->note.type);
}

static void _mia_compiler_composite(_mia_compiler_function_s* const f, const mia_ast_node_s* const node) {
	const mia_type_s* const type = node->note.type;
	const mia_ast_nodes_s* const entries = &node->as.composite.entries;
	if (0 == entries->count) {
		_mia_compiler_zero_value(f, type);
		return;
	}
	if (mia_type_struct == type->kind) {
		const mia_program_layout_s* const layout = mia_checker_layout(f->c->checker, type);
		_mia_compiler_zero_value(f, type);
		for (size_t i = 0; i < entries->count; ++i) {
			const mia_ast_node_s* const entry = entries->data[i];
			const mia_program_field_s* const field = mia_checker_layout_field(layout, entry->as.entry.name);
			if (NULL == field) _mia_compiler_fail(f->c, "no field `%.*s`", mia_view_arg(entry->as.entry.name));
			_mia_compiler_value(f, entry->as.entry.value);
			_mia_compiler_emit_memory(f, mia_opcode_SET_FIELD, field->type, (int64_t)field->offset, _mia_compiler_words(f->c, type));
		}
		return;
	}
	const mia_type_s* const element = (mia_type_array == type->kind) ? type->as.array.element : type->as.slice.element;
	for (size_t i = 0; i < entries->count; ++i) _mia_compiler_value(f, entries->data[i]->as.entry.value);
	_mia_compiler_emit_memory(f, mia_opcode_MAKE_ARRAY, element, (int64_t)entries->count, 0);
	if (mia_type_slice == type->kind) {
		uint64_t alignment = 1;
		const uint64_t size = _mia_compiler_size(f->c, element, &alignment);
		const int64_t offset = _mia_compiler_reserve(f, size * entries->count, alignment);
		_mia_compiler_emit_full(f, (mia_bytecode_instruction_s) { .opcode = mia_opcode_TEMP_LOC, .type = mia_code_bytes, .a = offset, .b = (int64_t)(size * entries->count), });
		_mia_compiler_emit_full(f, (mia_bytecode_instruction_s) { .opcode = mia_opcode_SLICE_LOC, .type = mia_code_int, .target = mia_code_int,
			.b = (int64_t)entries->count, .c = (int64_t)size, });
	}
}

typedef mia_vector_of(int64_t) _mia_compiler_jumps_s;

static void _mia_compiler_jump_if(_mia_compiler_function_s* const f, const mia_ast_node_s* const node, const bool when, _mia_compiler_jumps_s* const jumps);

static void _mia_compiler_patch_all(_mia_compiler_function_s* const f, _mia_compiler_jumps_s* const jumps, const int64_t target) {
	for (size_t i = 0; i < jumps->count; ++i) _mia_compiler_patch(f, jumps->data[i], target);
	mia_vector_free(jumps);
}

static void _mia_compiler_assert(_mia_compiler_function_s* const f, const mia_ast_node_s* const node) {
	// (a condition known to hold leaves only a jump over the failure, which is tidied away)
	_mia_compiler_jumps_s skip = {0};
	_mia_compiler_jump_if(f, node->as.assert_statement.condition, true, &skip);
	const mia_ast_node_s* const message = node->as.assert_statement.message;
	mia_bytecode_constant_s text = { .kind = mia_bytecode_constant_text, .type = -1, };
	if (message != NULL && mia_ast_node_string == message->kind) {
		mia_string_s decoded = {0};
		mia_format_utf8(&decoded, message->as.string.data);
		text.text = mia_string_take(&decoded);
	}
	else {
		const mia_view_s condition = mia_span_text(f->c->program->sources, node->as.assert_statement.condition->span);
		text.text = mia_string_format("assertion failed: " mia_view_fmt, mia_view_arg(condition));
	}
	_mia_compiler_emit_a(f, mia_opcode_ASSERT_FAIL, _mia_compiler_constant(f->c, text));
	_mia_compiler_patch_all(f, &skip, _mia_compiler_here(f));
}

static void _mia_compiler_lenof(_mia_compiler_function_s* const f, const mia_ast_node_s* const node) {
	const mia_ast_node_s* const operand = node->as.lenof.operand;
	const mia_type_s* const type = operand->note.type;
	if (mia_type_array == type->kind) {
		const bool plain = mia_ast_node_name == operand->kind || mia_ast_node_string == operand->kind || mia_ast_node_member == operand->kind || mia_ast_node_paren == operand->kind;
		if (!plain) {
			_mia_compiler_value(f, operand);
			_mia_compiler_pop(f, type);
		}
		_mia_compiler_push_constant(f, _mia_compiler_integer_constant(mia_int128_from_int(type->as.array.length), false));
		return;
	}
	_mia_compiler_value(f, operand);
	_mia_compiler_emit(f, mia_opcode_LEN_SLICE);
}

static void _mia_compiler_member(_mia_compiler_function_s* const f, const mia_ast_node_s* const node) {
	if (NULL == node->note.field) {
		_mia_compiler_symbol(f, node);
		return;
	}
	if (_mia_compiler_addressable(node)) {
		_mia_compiler_read_place(f, node);
		return;
	}
	const mia_ast_node_s* const target = node->as.member.target;
	_mia_compiler_value(f, target);
	const mia_program_field_s* const field = node->note.field;
	_mia_compiler_emit_memory(f, mia_opcode_FIELD, field->type, (int64_t)field->offset, _mia_compiler_words(f->c, target->note.type));
}

static void _mia_compiler_index(_mia_compiler_function_s* const f, const mia_ast_node_s* const node) {
	const mia_ast_node_s* const target = node->as.index.target;
	const mia_ast_node_s* const index = node->as.index.index;
	const mia_type_s* const type = target->note.type;
	const mia_type_s* const element = node->note.type;
	if (mia_type_array == type->kind && _mia_compiler_addressable(target)) {
		_mia_compiler_read_place(f, node);
		return;
	}
	_mia_compiler_value(f, target);
	_mia_compiler_value(f, index);
	const mia_code_e code = _mia_compiler_memory_code(f->c, element);
	const int64_t size = (int64_t)_mia_compiler_size(f->c, element, NULL);
	mia_bytecode_instruction_s instruction = { .type = code, .target = _mia_compiler_type_code(f->c, index->note.type), };
	if (mia_type_array == type->kind) {
		instruction.opcode = mia_opcode_INDEX;
		instruction.a = type->as.array.length;
		instruction.b = _mia_compiler_words(f->c, type);
		instruction.c = size;
	}
	else {
		instruction.opcode = mia_opcode_INDEX_SLICE;
		instruction.a = size;
	}
	_mia_compiler_emit_full(f, instruction);
}

/// whether 'node' is made of literals and constants only (no variable, no
/// call): what may have a value at compile time, checked before asking the
/// evaluator (which costs more).
static bool _mia_compiler_constant_shape(const mia_ast_node_s* const node) {
	switch (node->kind) {
		case mia_ast_node_integer: case mia_ast_node_float: case mia_ast_node_char: case mia_ast_node_bool: case mia_ast_node_string:
		case mia_ast_node_sizeof:
			return true;
		case mia_ast_node_name:   return node->note.constant != NULL;
		case mia_ast_node_paren:  return _mia_compiler_constant_shape(node->as.paren.inner);
		case mia_ast_node_cast:   return _mia_compiler_constant_shape(node->as.cast.operand);
		case mia_ast_node_unary:  return _mia_compiler_constant_shape(node->as.unary.operand);
		case mia_ast_node_binary: return _mia_compiler_constant_shape(node->as.binary.left) && _mia_compiler_constant_shape(node->as.binary.right);
		case mia_ast_node_member: return (NULL == node->note.field) ? node->note.constant != NULL : _mia_compiler_constant_shape(node->as.member.target);
		case mia_ast_node_index:  return _mia_compiler_constant_shape(node->as.index.target) && _mia_compiler_constant_shape(node->as.index.index);
		default:                  return false;
	}
}

/// an operation whose value is known at compile time (on literals and
/// constants: '2 * SIZE', 'ORIGIN.x', 'TABLE[3]') as that value: one
/// constant. the evaluator computes as the machine does (integers wrap to
/// their type, f32 rounds), and an operation the machine would stop on
/// (division by zero) is not known, so it stays for the machine.
static bool _mia_compiler_fold(_mia_compiler_function_s* const f, const mia_ast_node_s* const node) {
	const mia_type_s* const type = node->note.type;
	if (NULL == type) return false;
	switch (node->kind) {
		case mia_ast_node_binary: case mia_ast_node_unary: case mia_ast_node_index: break;
		case mia_ast_node_member: if (NULL == node->note.field) return false; break;
		case mia_ast_node_cast: {
			// a float made an integer is left to the machine (the evaluator converts through 128 bits, the machine through 64)
			const mia_type_s* const source = node->as.cast.operand->note.type;
			const bool from_float = mia_type_is_special(source, mia_type_special_untyped_float) || (mia_type_scalar == source->kind && 'f' == mia_scalar_kind(source->as.scalar));
			if (from_float && mia_type_scalar == type->kind && 'i' == mia_scalar_kind(type->as.scalar)) return false;
			break;
		}
		default: return false;
	}
	mia_evaluator_value_s value = {0};
	if (!_mia_compiler_constant_shape(node) || !mia_checker_try_value(f->c->checker, node, &value)) return false;
	const bool scalar = mia_evaluator_value_integer == value.kind || mia_evaluator_value_real == value.kind || mia_evaluator_value_boolean == value.kind;
	if (scalar && !(mia_type_scalar == type->kind || mia_type_is_untyped(type))) return false;
	if (!scalar && !(mia_evaluator_value_list == value.kind && (mia_type_array == type->kind || mia_type_struct == type->kind))) return false;
	_mia_compiler_push_constant(f, _mia_compiler_convert(f->c, &value, type));
	return true;
}

static void _mia_compiler_expression(_mia_compiler_function_s* const f, const mia_ast_node_s* const node) {
	if (_mia_compiler_fold(f, node)) return;
	switch (node->kind) {
		case mia_ast_node_integer: _mia_compiler_scalar(f, false, node->as.integer.value, 0.0, node->note.type); break;
		case mia_ast_node_float:   _mia_compiler_scalar(f, true, mia_int128_zero(), node->as.floating.value, node->note.type); break;
		case mia_ast_node_char:    _mia_compiler_push_constant(f, _mia_compiler_integer_constant(mia_int128_from_uint(node->as.character.value), false)); break;
		case mia_ast_node_bool:    _mia_compiler_push_constant(f, _mia_compiler_boolean_constant(node->as.boolean.value)); break;
		case mia_ast_node_string:  _mia_compiler_emit_a(f, mia_opcode_CONST, _mia_compiler_static_constant(f, node)); break;
		case mia_ast_node_paren:   _mia_compiler_value(f, node->as.paren.inner); break;
		case mia_ast_node_name:    _mia_compiler_symbol(f, node); break;
		case mia_ast_node_member:  _mia_compiler_member(f, node); break;
		case mia_ast_node_specialize: _mia_compiler_function_value(f, node->note.instance); break;
		case mia_ast_node_lambda:  _mia_compiler_emit_a(f, mia_opcode_FUNCTION, _mia_compiler_function(f->c, node, true)); break;
		case mia_ast_node_call:    _mia_compiler_call(f, node); break;
		case mia_ast_node_index:   _mia_compiler_index(f, node); break;
		case mia_ast_node_slice_of: _mia_compiler_slice_of(f, node); break;
		case mia_ast_node_unary:   _mia_compiler_unary(f, node); break;
		case mia_ast_node_binary:  _mia_compiler_binary(f, node); break;
		case mia_ast_node_cast: {
			_mia_compiler_value(f, node->as.cast.operand);
			const mia_code_e source = _mia_compiler_type_code(f->c, node->as.cast.operand->note.type);
			const mia_code_e target = _mia_compiler_type_code(f->c, node->note.type);
			if (source != target) _mia_compiler_emit_full(f, (mia_bytecode_instruction_s) { .opcode = mia_opcode_CAST, .type = source, .target = target, });
			break;
		}
		case mia_ast_node_reference: _mia_compiler_fail(f->c, "a reference outside of a call argument");
		case mia_ast_node_composite: _mia_compiler_composite(f, node); break;
		case mia_ast_node_sizeof:  _mia_compiler_push_constant(f, _mia_compiler_integer_constant(mia_int128_from_uint(node->note.size), false)); break;
		case mia_ast_node_lenof:   _mia_compiler_lenof(f, node); break;
		case mia_ast_node_assert:  _mia_compiler_assert(f, node); break;
		default: _mia_compiler_fail(f->c, "cannot compile a `%s` node", mia_ast_node_kind_name(node->kind));
	}
}

/// compiles 'node' with 'compile', its instructions at the node's position.
static void _mia_compiler_at_node(_mia_compiler_function_s* const f, const mia_ast_node_s* const node, void (*compile)(_mia_compiler_function_s* const, const mia_ast_node_s* const)) {
	const mia_span_s saved = f->span;
	const bool had = f->has_span;
	f->span = node->span;
	f->has_span = true;
	compile(f, node);
	f->span = saved;
	f->has_span = had;
}

static void _mia_compiler_value(_mia_compiler_function_s* const f, const mia_ast_node_s* const node) {
	_mia_compiler_at_node(f, node, _mia_compiler_expression);
}

// --- statements --------------------------------------------------------------------------

/// the operator of a compound assignment, on the loaded value and the new one.
static void _mia_compiler_compound_operator(_mia_compiler_function_s* const f, const mia_ast_node_s* const node) {
	const mia_ast_node_s* const target = node->as.assign.target;
	_mia_compiler_value(f, node->as.assign.value);
	_mia_compiler_emit_operator(f, node->as.assign.operation, target->note.type, node->as.assign.value->note.type);
}

static void _mia_compiler_assign(_mia_compiler_function_s* const f, const mia_ast_node_s* const node) {
	const mia_ast_node_s* const target = node->as.assign.target;
	const mia_ast_node_s* const value = node->as.assign.value;
	if (mia_ast_node_name == target->kind && mia_view_eq_cstr(target->as.name.name, "_")) {
		_mia_compiler_value(f, value);
		_mia_compiler_pop(f, value->note.type);
		return;
	}
	const mia_type_s* const type = target->note.type;
	int64_t offset = 0;
	if (_mia_compiler_frame_place(f, target, &offset)) {
		if (mia_ast_operator_assign == node->as.assign.operation) {
			_mia_compiler_value(f, value);
		}
		else {
			_mia_compiler_load_local(f, offset, type);
			_mia_compiler_compound_operator(f, node);
		}
		_mia_compiler_store_local(f, offset, type);
		return;
	}
	_mia_compiler_location(f, target);
	if (mia_ast_operator_assign == node->as.assign.operation) {
		_mia_compiler_value(f, value);
	}
	else {
		_mia_compiler_emit_a(f, mia_opcode_DUP, 2);
		_mia_compiler_emit_memory(f, mia_opcode_LOAD_LOC, type, 0, 0);
		_mia_compiler_compound_operator(f, node);
	}
	_mia_compiler_emit_memory(f, mia_opcode_STORE_LOC, type, 0, 0);
}

static void _mia_compiler_finish_loop(_mia_compiler_function_s* const f, const int64_t end) {
	_mia_compiler_loop_s loop = mia_vector_pop(&f->loops);
	for (size_t i = 0; i < loop.breaks.count; ++i) _mia_compiler_patch(f, loop.breaks.data[i], end);
	mia_vector_free(&loop.breaks);
	mia_vector_free(&loop.continues);
}

/// a loop begins: 'leave' is its exit test's jump (-1: none), 'target' where 'continue' goes (-1: patched later).
static void _mia_compiler_open_loop(_mia_compiler_function_s* const f, const int64_t leave, const int64_t target) {
	_mia_compiler_loop_s loop = { .target = target, };
	if (leave >= 0) mia_vector_push(&loop.breaks, leave);
	mia_vector_push(&f->loops, loop);
}

/// whether a condition is known at compile time ('true', 'DEBUG', 'SIZE > 4'), and its value.
static bool _mia_compiler_known(_mia_compiler_function_s* const f, const mia_ast_node_s* const node, bool* const holds) {
	if (mia_ast_node_bool == node->kind) {
		*holds = node->as.boolean.value;
		return true;
	}
	mia_evaluator_value_s value = {0};
	if (!_mia_compiler_constant_shape(node) || !mia_checker_try_value(f->c->checker, node, &value) || value.kind != mia_evaluator_value_boolean) return false;
	*holds = value.as.boolean;
	return true;
}

/// a condition as jumps: adds to 'jumps' the jumps taken when 'node' is
/// 'when' (to be patched), and falls through otherwise. '&&', '||' and '!'
/// become jumps too, so no bool is made, and a comparison stays right before
/// its jump (which the machine runs as one operation). a condition known at
/// compile time is a plain jump, or nothing.
static void _mia_compiler_jump_if(_mia_compiler_function_s* const f, const mia_ast_node_s* const node, const bool when, _mia_compiler_jumps_s* const jumps) {
	if (mia_ast_node_paren == node->kind) {
		_mia_compiler_jump_if(f, node->as.paren.inner, when, jumps);
		return;
	}
	if (mia_ast_node_unary == node->kind && mia_ast_operator_not == node->as.unary.operation) {
		_mia_compiler_jump_if(f, node->as.unary.operand, !when, jumps);
		return;
	}
	bool holds = false;
	if (_mia_compiler_known(f, node, &holds)) {
		if (holds == when) mia_vector_push(jumps, _mia_compiler_emit_a(f, mia_opcode_JUMP, 0));
		return;
	}
	if (mia_ast_node_binary == node->kind && (mia_ast_operator_and == node->as.binary.operation || mia_ast_operator_or == node->as.binary.operation)) {
		const bool is_and = mia_ast_operator_and == node->as.binary.operation;
		if (is_and != when) {
			// 'a && b' is false as soon as one is; 'a || b' is true as soon as one is
			_mia_compiler_jump_if(f, node->as.binary.left, when, jumps);
			_mia_compiler_jump_if(f, node->as.binary.right, when, jumps);
			return;
		}
		// 'a && b' is true only when both are; 'a || b' is false only when both are
		_mia_compiler_jumps_s past = {0};
		_mia_compiler_jump_if(f, node->as.binary.left, !when, &past);
		_mia_compiler_jump_if(f, node->as.binary.right, when, jumps);
		_mia_compiler_patch_all(f, &past, _mia_compiler_here(f));
		return;
	}
	_mia_compiler_value(f, node);
	mia_vector_push(jumps, _mia_compiler_emit_a(f, when ? mia_opcode_JUMP_IF_TRUE : mia_opcode_JUMP_IF_FALSE, 0));
}

static void _mia_compiler_if(_mia_compiler_function_s* const f, const mia_ast_node_s* const node) {
	_mia_compiler_jumps_s skip = {0};
	_mia_compiler_jump_if(f, node->as.if_statement.condition, false, &skip);
	_mia_compiler_block(f, node->as.if_statement.then);
	const mia_ast_node_s* const otherwise = node->as.if_statement.otherwise;
	if (NULL == otherwise) {
		_mia_compiler_patch_all(f, &skip, _mia_compiler_here(f));
		return;
	}
	const int64_t end = _mia_compiler_emit_a(f, mia_opcode_JUMP, 0);
	_mia_compiler_patch_all(f, &skip, _mia_compiler_here(f));
	if (mia_ast_node_if == otherwise->kind) _mia_compiler_statement(f, otherwise);
	else _mia_compiler_block(f, otherwise);
	_mia_compiler_patch(f, end, _mia_compiler_here(f));
}

/// 'for': the range is tested once before the first round, and RANGE_NEXT
/// steps it and jumps back after each, so a round costs one instruction.
/// the variable is the range's counter itself when it is a word the body
/// never changes (no copy per round); otherwise a copy of it.
static void _mia_compiler_for(_mia_compiler_function_s* const f, const mia_ast_node_s* const node) {
	const int64_t base = _mia_compiler_reserve(f, 24, 8);
	const mia_program_variable_s* const variable = node->note.variable;
	const mia_type_s* const type = variable->type;
	const mia_code_e code = _mia_compiler_type_code(f->c, type);
	const bool counter = !variable->changed && 8 == mia_code_size(code) && 'i' == mia_code_kind(code);
	if (counter) mia_map_index_set(&f->slots, mia_map_key_of(variable), base);
	_mia_compiler_value(f, node->as.for_statement.start);
	_mia_compiler_value(f, node->as.for_statement.end);
	_mia_compiler_emit_full(f, (mia_bytecode_instruction_s) { .opcode = mia_opcode_RANGE_INIT, .type = code, .a = base, .b = node->as.for_statement.inclusive, });
	const int64_t leave = _mia_compiler_emit_ab(f, mia_opcode_RANGE_TEST, base, 0);
	const int64_t first = _mia_compiler_here(f);
	if (!counter) {
		_mia_compiler_emit_full(f, (mia_bytecode_instruction_s) { .opcode = mia_opcode_LOAD, .type = mia_code_int, .a = base, .b = 8, });
		_mia_compiler_store_local(f, _mia_compiler_slot(f, variable), type);
	}
	_mia_compiler_open_loop(f, leave, -1);
	_mia_compiler_block(f, node->as.for_statement.body);
	const int64_t step = _mia_compiler_here(f);
	const _mia_compiler_loop_s* const loop = &mia_vector_last(&f->loops);
	for (size_t i = 0; i < loop->continues.count; ++i) _mia_compiler_patch(f, loop->continues.data[i], step);
	_mia_compiler_emit_ab(f, mia_opcode_RANGE_NEXT, base, first);
	_mia_compiler_finish_loop(f, _mia_compiler_here(f));
}

/// 'while': the test is at the bottom (a first jump goes to it), so a round
/// ends in the test's jump back, not in a jump to a test at the top. a
/// condition known to hold has no test (only its 'break's leave the loop;
/// as for the checker, code after it is reached through a 'break' only).
static void _mia_compiler_while(_mia_compiler_function_s* const f, const mia_ast_node_s* const node) {
	const mia_ast_node_s* const condition = node->as.while_statement.condition;
	bool holds = false;
	const bool forever = _mia_compiler_known(f, condition, &holds) && holds;
	const int64_t entry = forever ? -1 : _mia_compiler_emit_a(f, mia_opcode_JUMP, 0);
	const int64_t first = _mia_compiler_here(f);
	_mia_compiler_open_loop(f, -1, forever ? first : -1);
	_mia_compiler_block(f, node->as.while_statement.body);
	if (forever) {
		_mia_compiler_emit_a(f, mia_opcode_JUMP, first);
	}
	else {
		const int64_t test = _mia_compiler_here(f);
		_mia_compiler_patch(f, entry, test);
		const _mia_compiler_loop_s* const loop = &mia_vector_last(&f->loops);
		for (size_t i = 0; i < loop->continues.count; ++i) _mia_compiler_patch(f, loop->continues.data[i], test);
		_mia_compiler_jumps_s again = {0};
		_mia_compiler_jump_if(f, condition, true, &again);
		_mia_compiler_patch_all(f, &again, first);
	}
	_mia_compiler_finish_loop(f, _mia_compiler_here(f));
}

static void _mia_compiler_statement_kind(_mia_compiler_function_s* const f, const mia_ast_node_s* const node) {
	switch (node->kind) {
		case mia_ast_node_declaration_statement: break;
		case mia_ast_node_block: _mia_compiler_block(f, node); break;
		case mia_ast_node_let: {
			const mia_type_s* const type = node->note.variable->type;
			if (node->as.let.value != NULL) _mia_compiler_value(f, node->as.let.value);
			else _mia_compiler_zero_value(f, type);
			_mia_compiler_store_local(f, _mia_compiler_slot(f, node->note.variable), type);
			break;
		}
		case mia_ast_node_expression_statement: {
			const mia_ast_node_s* const expression = node->as.expression_statement.expression;
			_mia_compiler_value(f, expression);
			_mia_compiler_pop(f, expression->note.type);
			break;
		}
		case mia_ast_node_assign: _mia_compiler_assign(f, node); break;
		case mia_ast_node_if: _mia_compiler_if(f, node); break;
		case mia_ast_node_while: _mia_compiler_while(f, node); break;
		case mia_ast_node_for: _mia_compiler_for(f, node); break;
		case mia_ast_node_break: {
			const int64_t jump = _mia_compiler_emit_a(f, mia_opcode_JUMP, 0);
			mia_vector_push(&mia_vector_last(&f->loops).breaks, jump);
			break;
		}
		case mia_ast_node_continue: {
			_mia_compiler_loop_s* const loop = &mia_vector_last(&f->loops);
			if (loop->target >= 0) {
				_mia_compiler_emit_a(f, mia_opcode_JUMP, loop->target);
			}
			else {
				const int64_t jump = _mia_compiler_emit_a(f, mia_opcode_JUMP, 0);
				mia_vector_push(&mia_vector_last(&f->loops).continues, jump);
			}
			break;
		}
		case mia_ast_node_return: {
			const mia_ast_node_s* const value = node->as.return_statement.value;
			if (value != NULL) _mia_compiler_value(f, value);
			_mia_compiler_emit_a(f, mia_opcode_RETURN, (int64_t)_mia_compiler_target(f)->result);
			break;
		}
		default: _mia_compiler_fail(f->c, "cannot compile a `%s` statement", mia_ast_node_kind_name(node->kind));
	}
}

static void _mia_compiler_statement(_mia_compiler_function_s* const f, const mia_ast_node_s* const node) {
	_mia_compiler_at_node(f, node, _mia_compiler_statement_kind);
}

// --- tidying ------------------------------------------------------------------------------

/// one round of tidying a finished function's jumps; false when nothing changed.
/// a jump to a jump goes straight to where that one goes (and a jump to a
/// return returns there); a conditional jump over a jump becomes the opposite
/// jump; then what cannot run (code no path reaches, a jump to the next
/// instruction) is removed and the jumps are renumbered.
static bool _mia_compiler_tidy_round(_mia_compiler_function_s* const f) {
	mia_bytecode_function_s* const function = _mia_compiler_target(f);
	mia_bytecode_instruction_s* const code = function->code.data;
	const size_t count = function->code.count;
	bool changed = false;
	for (size_t i = 0; i < count; ++i) {
		const int64_t first = _mia_compiler_jump_target(&code[i]);
		int64_t to = first;
		if (to < 0) continue;
		for (size_t hops = 0; hops < count && (uint64_t)to < count && mia_opcode_JUMP == code[to].opcode && code[to].a != to; ++hops) to = code[to].a;
		if (to != first) {
			_mia_compiler_set_jump_target(&code[i], to);
			changed = true;
		}
		// the stack holds the same at the jump and at the return it leads to
		if (mia_opcode_JUMP == code[i].opcode && (uint64_t)to < count && mia_opcode_RETURN == code[to].opcode) {
			code[i] = code[to];
			changed = true;
		}
	}
	bool* const targeted = mia_calloc(count + 1, sizeof(bool));
	bool* const removed = mia_calloc(count + 1, sizeof(bool));
	bool* const reached = mia_calloc(count + 1, sizeof(bool));
	size_t* const renumbered = mia_calloc(count + 1, sizeof(size_t));
	for (size_t i = 0; i < count; ++i) {
		const int64_t to = _mia_compiler_jump_target(&code[i]);
		if (to >= 0 && (uint64_t)to <= count) targeted[to] = true;
	}
	// 'if c then break;': a jump over a jump, when nothing else jumps to the second one
	for (size_t i = 0; i + 1 < count; ++i) {
		const mia_opcode_e opcode = code[i].opcode;
		if ((opcode != mia_opcode_JUMP_IF_FALSE && opcode != mia_opcode_JUMP_IF_TRUE) || code[i].a != (int64_t)i + 2) continue;
		if (code[i + 1].opcode != mia_opcode_JUMP || targeted[i + 1] || removed[i]) continue;
		code[i].opcode = (mia_opcode_JUMP_IF_FALSE == opcode) ? mia_opcode_JUMP_IF_TRUE : mia_opcode_JUMP_IF_FALSE;
		code[i].a = code[i + 1].a;
		removed[i + 1] = true;
		changed = true;
	}
	// what runs: from the first instruction, along jumps and from each instruction to the next
	mia_vector_of(size_t) work = {0};
	if (count > 0) {
		reached[0] = true;
		mia_vector_push(&work, 0);
	}
	while (work.count > 0) {
		const size_t at = mia_vector_pop(&work);
		const mia_opcode_e opcode = code[at].opcode;
		const int64_t to = removed[at] ? -1 : _mia_compiler_jump_target(&code[at]);
		const bool falls = removed[at] || (opcode != mia_opcode_JUMP && opcode != mia_opcode_RETURN && opcode != mia_opcode_ASSERT_FAIL);
		if (to >= 0 && (uint64_t)to < count && !reached[to]) {
			reached[to] = true;
			mia_vector_push(&work, (size_t)to);
		}
		if (falls && at + 1 < count && !reached[at + 1]) {
			reached[at + 1] = true;
			mia_vector_push(&work, at + 1);
		}
	}
	mia_vector_free(&work);
	for (size_t i = 0; i < count; ++i) {
		if (!reached[i] && !removed[i]) {
			removed[i] = true;
			changed = true;
		}
	}
	// a jump to the next instruction that stays (a removed instruction's number is that of the next one that stays)
	for (bool again = true; again;) {
		again = false;
		size_t kept = 0;
		for (size_t i = 0; i <= count; ++i) {
			renumbered[i] = kept;
			if (i < count && !removed[i]) kept += 1;
		}
		for (size_t i = 0; i < count; ++i) {
			if (removed[i] || code[i].opcode != mia_opcode_JUMP || code[i].a < 0 || (uint64_t)code[i].a > count) continue;
			if (renumbered[code[i].a] == renumbered[i] + 1) {
				removed[i] = true;
				again = changed = true;
			}
		}
	}
	if (changed) {
		size_t kept = 0;
		for (size_t i = 0; i < count; ++i) {
			if (removed[i]) continue;
			mia_bytecode_instruction_s instruction = code[i];
			const int64_t to = _mia_compiler_jump_target(&instruction);
			if (to >= 0 && (uint64_t)to <= count) _mia_compiler_set_jump_target(&instruction, (int64_t)renumbered[to]);
			code[kept] = instruction;
			function->positions.data[kept] = function->positions.data[i];
			kept += 1;
		}
		function->code.count = kept;
		function->positions.count = kept;
	}
	mia_free(targeted);
	mia_free(removed);
	mia_free(reached);
	mia_free(renumbered);
	return changed;
}

static void _mia_compiler_tidy(_mia_compiler_function_s* const f) {
	for (int pass = 0; pass < 8 && _mia_compiler_tidy_round(f); ++pass) {}
}

static void _mia_compiler_function_free(_mia_compiler_function_s* const f) {
	for (size_t i = 0; i < f->loops.count; ++i) {
		mia_vector_free(&f->loops.data[i].breaks);
		mia_vector_free(&f->loops.data[i].continues);
	}
	mia_vector_free(&f->loops);
	mia_map_index_free(&f->slots);
}

static void _mia_compiler_compile_function(_mia_compiler_s* const c, _mia_compiler_function_s* const f, const _mia_compiler_pending_s pending) {
	const mia_ast_node_s* body = NULL;
	const mia_ast_nodes_s* parameters = NULL;
	const mia_type_s* result = NULL;
	if (pending.lambda) {
		const mia_ast_node_s* const node = pending.target;
		parameters = &node->as.lambda.parameters;
		body = node->as.lambda.body;
		const mia_type_s* const type = node->note.type;
		result = (type != NULL && mia_type_function == type->kind) ? type->as.function.result : NULL;
	}
	else {
		const mia_program_instance_s* const instance = pending.target;
		const mia_ast_node_s* const node = (instance->node != NULL) ? instance->node : instance->function->node;
		parameters = &node->as.function.parameters;
		body = node->as.function.body;
		result = instance->result;
	}
	*f = (_mia_compiler_function_s) { .c = c, .index = pending.index, };
	_mia_compiler_target(f)->returns = result != NULL;
	_mia_compiler_target(f)->result = (uint32_t)_mia_compiler_words(c, result);
	for (size_t i = 0; i < parameters->count; ++i) {
		const mia_program_variable_s* const variable = parameters->data[i]->note.variable;
		const int64_t offset = _mia_compiler_slot(f, variable);
		const mia_type_s* const type = variable->reference ? NULL : variable->type;
		const mia_bytecode_slot_s slot = {
			.code = (NULL == type) ? mia_code_slice : _mia_compiler_memory_code(c, type),
			.offset = (uint64_t)offset,
			.size = (NULL == type) ? 16 : _mia_compiler_size(c, type, NULL),
		};
		mia_vector_push(&_mia_compiler_target(f)->slots, slot);
	}
	if (NULL == body) _mia_compiler_fail(c, "`%s` has no body", _mia_compiler_target(f)->name);
	f->span = body->span;
	f->has_span = true;
	_mia_compiler_block(f, body);
	f->span = (mia_span_s) { .file = body->span.file, .begin = body->span.end - 1, .end = body->span.end, };
	_mia_compiler_emit_a(f, mia_opcode_RETURN, 0);
	_mia_compiler_tidy(f);
	if (f->frame > mia_bytecode_frame_size_limit) {
		_mia_compiler_fail(c, "the locals of `%s` take %llu bytes, more than the %llu a function may have", _mia_compiler_target(f)->name,
			(unsigned long long)f->frame, (unsigned long long)mia_bytecode_frame_size_limit);
	}
	_mia_compiler_target(f)->frame = (f->frame + 7) / 8 * 8;
	_mia_compiler_function_free(f);
}

/// compiles 'main' and every function it reaches.
static void _mia_compiler_compile_all(_mia_compiler_s* const c, _mia_compiler_function_s* const f) {
	const mia_program_symbol_s* const symbol = mia_map_get_cstr(&c->program->entry->symbols, "main");
	const mia_program_function_s* const entry_function = (symbol != NULL && mia_program_symbol_function == symbol->kind) ? (const mia_program_function_s*)symbol : NULL;
	const mia_program_instance_s* entry = NULL;
	for (size_t i = 0; entry_function != NULL && i < entry_function->instances.count; ++i) if (0 == entry_function->instances.data[i]->argument_count) entry = entry_function->instances.data[i];
	if (NULL == entry) _mia_compiler_fail(c, "the program has no `main` function");
	c->module->entry = _mia_compiler_function(c, entry, false);
	c->module->entry_returns = entry->result != NULL;
	while (c->next < c->functions.count) {
		const _mia_compiler_pending_s pending = c->functions.data[c->next++];
		_mia_compiler_compile_function(c, f, pending);
	}
}

bool mia_compiler_compile(mia_program_s* const program, mia_bytecode_module_s* const module, char** const error) {
	*module = (mia_bytecode_module_s) {0};
	jmp_buf stop;
	_mia_compiler_s c = { .program = program, .checker = program->checker, .module = module, .stop = &stop, };
	_mia_compiler_function_s f = {0};
	if (0 == setjmp(stop)) _mia_compiler_compile_all(&c, &f);
	_mia_compiler_function_free(&f);
	mia_vector_free(&c.functions);
	mia_vector_free(&c.same_hash);
	mia_map_index_free(&c.function_of);
	mia_map_index_free(&c.type_of);
	mia_map_index_free(&c.file_of);
	mia_map_index_free(&c.constant_of);
	if (c.error != NULL) {
		*error = c.error;
		mia_bytecode_free(module);
		return false;
	}
	return true;
}
