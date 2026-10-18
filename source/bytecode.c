
/// bytecode: the binary '.miab' file format (documented in bytecode.h).

#include "mia/bytecode.h"
#include "mia/format.h"
#include "mia/source.h"

#include <inttypes.h>
#include <string.h>
#include <stdio.h>

static const char _mia_bytecode_magic[4] = { 'M', 'I', 'A', '\0', };

void mia_bytecode_constant_free(mia_bytecode_constant_s* const constant) {
	for (size_t i = 0; i < constant->items.count; ++i) mia_bytecode_constant_free(&constant->items.data[i]);
	mia_vector_free(&constant->items);
	mia_free(constant->text);
	constant->text = NULL;
}

void mia_bytecode_free(mia_bytecode_module_s* const module) {
	for (size_t i = 0; i < module->functions.count; ++i) {
		mia_bytecode_function_s* const function = &module->functions.data[i];
		mia_free(function->name);
		mia_vector_free(&function->slots);
		mia_vector_free(&function->code);
		mia_vector_free(&function->positions);
	}
	mia_vector_free(&module->functions);
	for (size_t i = 0; i < module->constants.count; ++i) mia_bytecode_constant_free(&module->constants.data[i]);
	mia_vector_free(&module->constants);
	for (size_t i = 0; i < module->types.count; ++i) {
		mia_bytecode_type_s* const type = &module->types.data[i];
		mia_free(type->name);
		for (size_t f = 0; f < type->field_names.count; ++f) mia_free(type->field_names.data[f]);
		mia_vector_free(&type->field_names);
		mia_vector_free(&type->field_types);
		mia_vector_free(&type->field_offsets);
	}
	mia_vector_free(&module->types);
	for (size_t i = 0; i < module->intrinsics.count; ++i) {
		mia_bytecode_intrinsic_s* const intrinsic = &module->intrinsics.data[i];
		mia_free(intrinsic->name);
		mia_free(intrinsic->plugin);
		mia_vector_free(&intrinsic->arguments);
		mia_vector_free(&intrinsic->parameters);
		mia_vector_free(&intrinsic->variadic);
		mia_vector_free(&intrinsic->flags);
	}
	mia_vector_free(&module->intrinsics);
	for (size_t i = 0; i < module->files.count; ++i) mia_free(module->files.data[i]);
	mia_vector_free(&module->files);
	*module = (mia_bytecode_module_s) {0};
}

// --- writing ---------------------------------------------------------------------------

static void _mia_bytecode_put(mia_string_s* const out, const uint64_t value, const size_t bytes) {
	for (size_t i = 0; i < bytes; ++i) mia_string_append_char(out, (char)(uint8_t)(value >> (8 * i)));
}

static void _mia_bytecode_put_u8(mia_string_s* const out, const uint64_t value) {
	_mia_bytecode_put(out, value, 1);
}

static void _mia_bytecode_put_u32(mia_string_s* const out, const uint64_t value) {
	_mia_bytecode_put(out, value, 4);
}

static void _mia_bytecode_put_u64(mia_string_s* const out, const uint64_t value) {
	_mia_bytecode_put(out, value, 8);
}

static void _mia_bytecode_put_s64(mia_string_s* const out, const int64_t value) {
	_mia_bytecode_put(out, (uint64_t)value, 8);
}

static void _mia_bytecode_put_string(mia_string_s* const out, const char* const text) {
	const size_t length = (text != NULL) ? strlen(text) : 0;
	_mia_bytecode_put_u32(out, length);
	if (length > 0) mia_string_append(out, mia_view_new(text, length));
}

static uint64_t _mia_bytecode_real_bits(const double value) {
	uint64_t bits = 0;
	memcpy(&bits, &value, sizeof(bits));
	return bits;
}

static void _mia_bytecode_put_constant(mia_string_s* const out, const mia_bytecode_constant_s* const constant) {
	_mia_bytecode_put_u8(out, (uint64_t)constant->kind);
	_mia_bytecode_put_s64(out, constant->type);
	switch (constant->kind) {
		case mia_bytecode_constant_none: break;
		case mia_bytecode_constant_integer:
			_mia_bytecode_put_u64(out, constant->integer);
			_mia_bytecode_put_u8(out, constant->is_signed);
			break;
		case mia_bytecode_constant_real:      _mia_bytecode_put_u64(out, _mia_bytecode_real_bits(constant->real)); break;
		case mia_bytecode_constant_boolean:   _mia_bytecode_put_u8(out, constant->boolean); break;
		case mia_bytecode_constant_text:      _mia_bytecode_put_string(out, constant->text); break;
		case mia_bytecode_constant_function:
		case mia_bytecode_constant_intrinsic: _mia_bytecode_put_u64(out, constant->integer); break;
		case mia_bytecode_constant_slice:
			_mia_bytecode_put_u8(out, constant->null);
			_mia_bytecode_put_u8(out, constant->read_only);
			_mia_bytecode_put_s64(out, constant->start);
			_mia_bytecode_put_s64(out, constant->length);
			break;
		case mia_bytecode_constant_list:
			break;
	}
	if (mia_bytecode_constant_slice == constant->kind || mia_bytecode_constant_list == constant->kind) {
		_mia_bytecode_put_u32(out, constant->items.count);
		for (size_t i = 0; i < constant->items.count; ++i) _mia_bytecode_put_constant(out, &constant->items.data[i]);
	}
}

static void _mia_bytecode_put_indexes(mia_string_s* const out, const int64_t* const items, const size_t count) {
	for (size_t i = 0; i < count; ++i) _mia_bytecode_put_u32(out, (uint64_t)items[i]);
}

void mia_bytecode_write(const mia_bytecode_module_s* const module, mia_string_s* const out) {
	mia_string_append(out, mia_view_new(_mia_bytecode_magic, sizeof(_mia_bytecode_magic)));
	_mia_bytecode_put_u32(out, mia_bytecode_version);
	_mia_bytecode_put_u32(out, module->files.count);
	for (size_t i = 0; i < module->files.count; ++i) _mia_bytecode_put_string(out, module->files.data[i]);
	_mia_bytecode_put_u32(out, module->types.count);
	for (size_t i = 0; i < module->types.count; ++i) {
		const mia_bytecode_type_s* const type = &module->types.data[i];
		_mia_bytecode_put_string(out, type->name);
		_mia_bytecode_put_u8(out, (uint64_t)type->kind);
		_mia_bytecode_put_u8(out, (uint64_t)type->code);
		_mia_bytecode_put_u64(out, type->size);
		_mia_bytecode_put_u64(out, type->alignment);
		_mia_bytecode_put_u32(out, (uint64_t)type->element);
		_mia_bytecode_put_s64(out, type->length);
		_mia_bytecode_put_s64(out, type->zero);
		_mia_bytecode_put_u32(out, type->field_names.count);
		for (size_t f = 0; f < type->field_names.count; ++f) {
			_mia_bytecode_put_string(out, type->field_names.data[f]);
			_mia_bytecode_put_u32(out, (uint64_t)type->field_types.data[f]);
			_mia_bytecode_put_u64(out, type->field_offsets.data[f]);
		}
	}
	_mia_bytecode_put_u32(out, module->constants.count);
	for (size_t i = 0; i < module->constants.count; ++i) _mia_bytecode_put_constant(out, &module->constants.data[i]);
	_mia_bytecode_put_u32(out, module->intrinsics.count);
	for (size_t i = 0; i < module->intrinsics.count; ++i) {
		const mia_bytecode_intrinsic_s* const intrinsic = &module->intrinsics.data[i];
		_mia_bytecode_put_string(out, intrinsic->name);
		_mia_bytecode_put_string(out, intrinsic->plugin);
		_mia_bytecode_put_s64(out, intrinsic->result);
		_mia_bytecode_put_u32(out, intrinsic->arguments.count);
		_mia_bytecode_put_u32(out, intrinsic->parameters.count);
		_mia_bytecode_put_u32(out, intrinsic->variadic.count);
		_mia_bytecode_put_indexes(out, intrinsic->arguments.data, intrinsic->arguments.count);
		_mia_bytecode_put_indexes(out, intrinsic->parameters.data, intrinsic->parameters.count);
		_mia_bytecode_put_indexes(out, intrinsic->variadic.data, intrinsic->variadic.count);
		_mia_bytecode_put_u32(out, intrinsic->flags.count);
		for (size_t f = 0; f < intrinsic->flags.count; ++f) _mia_bytecode_put_u32(out, intrinsic->flags.data[f]);
	}
	_mia_bytecode_put_u32(out, module->functions.count);
	for (size_t i = 0; i < module->functions.count; ++i) {
		const mia_bytecode_function_s* const function = &module->functions.data[i];
		_mia_bytecode_put_string(out, function->name);
		_mia_bytecode_put_u32(out, function->parameters);
		_mia_bytecode_put_u64(out, function->frame);
		_mia_bytecode_put_u32(out, function->result);
		_mia_bytecode_put_u8(out, function->returns);
		_mia_bytecode_put_u32(out, function->slots.count);
		for (size_t s = 0; s < function->slots.count; ++s) {
			_mia_bytecode_put_u8(out, (uint64_t)function->slots.data[s].code);
			_mia_bytecode_put_u64(out, function->slots.data[s].offset);
			_mia_bytecode_put_u64(out, function->slots.data[s].size);
		}
		_mia_bytecode_put_u32(out, function->code.count);
		for (size_t c = 0; c < function->code.count; ++c) {
			const mia_bytecode_instruction_s* const instruction = &function->code.data[c];
			_mia_bytecode_put_u8(out, (uint64_t)instruction->opcode);
			_mia_bytecode_put_u8(out, (uint64_t)instruction->type);
			_mia_bytecode_put_u8(out, (uint64_t)instruction->target);
			const int operands = mia_opcode_operands(instruction->opcode);
			if (operands >= 1) _mia_bytecode_put_s64(out, instruction->a);
			if (operands >= 2) _mia_bytecode_put_s64(out, instruction->b);
			if (operands >= 3) _mia_bytecode_put_s64(out, instruction->c);
		}
		for (size_t p = 0; p < function->code.count; ++p) {
			const mia_bytecode_position_s position = (p < function->positions.count) ? function->positions.data[p] : (mia_bytecode_position_s) { .file = -1, };
			_mia_bytecode_put_u32(out, (uint64_t)(int64_t)position.file);
			_mia_bytecode_put_u32(out, position.line);
			_mia_bytecode_put_u32(out, position.column);
			_mia_bytecode_put_u32(out, position.length);
		}
	}
	_mia_bytecode_put_u32(out, (uint64_t)module->entry);
	_mia_bytecode_put_u8(out, module->entry_returns);
}

bool mia_bytecode_save(const mia_bytecode_module_s* const module, const char* const path) {
	mia_string_s out = {0};
	mia_bytecode_write(module, &out);
	FILE* const file = fopen(path, "wb");
	bool written = false;
	if (file != NULL) {
		written = fwrite(out.data, 1, out.length, file) == out.length;
		written = (0 == fclose(file)) && written;
	}
	mia_string_free(&out);
	return written;
}

// --- reading ---------------------------------------------------------------------------

/// a bounds-checked cursor over the bytes of a file: any read past the end
/// (or any inconsistency) marks it failed, and then every read gives zero.
typedef struct {
	const uint8_t* data  ;
	size_t         length;
	size_t         at    ;
	const char*    error ;
} _mia_bytecode_reader_s;

static void _mia_bytecode_reader_fail(_mia_bytecode_reader_s* const r, const char* const error) {
	if (NULL == r->error) r->error = error;
}

static uint64_t _mia_bytecode_get(_mia_bytecode_reader_s* const r, const size_t bytes) {
	if (r->error != NULL) return 0;
	if (r->length - r->at < bytes) {
		_mia_bytecode_reader_fail(r, "the file is truncated");
		return 0;
	}
	uint64_t value = 0;
	for (size_t i = 0; i < bytes; ++i) value |= (uint64_t)r->data[r->at + i] << (8 * i);
	r->at += bytes;
	return value;
}

static uint8_t _mia_bytecode_get_u8(_mia_bytecode_reader_s* const r) {
	return (uint8_t)_mia_bytecode_get(r, 1);
}

static uint32_t _mia_bytecode_get_u32(_mia_bytecode_reader_s* const r) {
	return (uint32_t)_mia_bytecode_get(r, 4);
}

static uint64_t _mia_bytecode_get_u64(_mia_bytecode_reader_s* const r) {
	return _mia_bytecode_get(r, 8);
}

static int64_t _mia_bytecode_get_s64(_mia_bytecode_reader_s* const r) {
	return (int64_t)_mia_bytecode_get(r, 8);
}

/// a count of entries that each take at least 'minimum' bytes (so a broken
/// count cannot make the reader allocate more than the file could hold).
static uint32_t _mia_bytecode_get_count(_mia_bytecode_reader_s* const r, const size_t minimum) {
	const uint32_t count = _mia_bytecode_get_u32(r);
	if (r->error == NULL && (uint64_t)count * minimum > r->length - r->at) {
		_mia_bytecode_reader_fail(r, "the file is corrupted (a count is larger than the file)");
		return 0;
	}
	return count;
}

static char* _mia_bytecode_get_string(_mia_bytecode_reader_s* const r) {
	const uint32_t length = _mia_bytecode_get_count(r, 1);
	if (r->error != NULL) return mia_strdup("");
	char* const text = mia_malloc((size_t)length + 1);
	memcpy(text, r->data + r->at, length);
	text[length] = '\0';
	r->at += length;
	if (strlen(text) != length) _mia_bytecode_reader_fail(r, "the file is corrupted (a NUL in a name)");
	return text;
}

static void _mia_bytecode_get_constant(_mia_bytecode_reader_s* const r, mia_bytecode_constant_s* const constant, const int depth) {
	*constant = (mia_bytecode_constant_s) {0};
	const uint8_t kind = _mia_bytecode_get_u8(r);
	constant->type = _mia_bytecode_get_s64(r);
	if (kind > mia_bytecode_constant_intrinsic || depth > mia_bytecode_nesting_limit) {
		_mia_bytecode_reader_fail(r, "the file is corrupted (an unknown constant)");
		return;
	}
	constant->kind = (mia_bytecode_constant_kind_e)kind;
	switch (constant->kind) {
		case mia_bytecode_constant_none: break;
		case mia_bytecode_constant_integer:
			constant->integer = _mia_bytecode_get_u64(r);
			constant->is_signed = _mia_bytecode_get_u8(r) != 0;
			break;
		case mia_bytecode_constant_real: {
			const uint64_t bits = _mia_bytecode_get_u64(r);
			memcpy(&constant->real, &bits, sizeof(bits));
			break;
		}
		case mia_bytecode_constant_boolean:   constant->boolean = _mia_bytecode_get_u8(r) != 0; break;
		case mia_bytecode_constant_text:      constant->text = _mia_bytecode_get_string(r); break;
		case mia_bytecode_constant_function:
		case mia_bytecode_constant_intrinsic: constant->integer = _mia_bytecode_get_u64(r); break;
		case mia_bytecode_constant_slice:
			constant->null = _mia_bytecode_get_u8(r) != 0;
			constant->read_only = _mia_bytecode_get_u8(r) != 0;
			constant->start = _mia_bytecode_get_s64(r);
			constant->length = _mia_bytecode_get_s64(r);
			break;
		case mia_bytecode_constant_list:
			break;
	}
	if (mia_bytecode_constant_slice == constant->kind || mia_bytecode_constant_list == constant->kind) {
		const uint32_t count = _mia_bytecode_get_count(r, 9);
		for (uint32_t i = 0; i < count && NULL == r->error; ++i) {
			mia_bytecode_constant_s item = {0};
			_mia_bytecode_get_constant(r, &item, depth + 1);
			mia_vector_push(&constant->items, item);
		}
	}
}

static void _mia_bytecode_get_indexes(_mia_bytecode_reader_s* const r, const uint32_t count, const size_t limit, mia_bytecode_indexes_s* const items) {
	for (uint32_t i = 0; i < count && NULL == r->error; ++i) {
		const uint32_t index = _mia_bytecode_get_u32(r);
		if (index >= limit) _mia_bytecode_reader_fail(r, "the file is corrupted (a type index is out of range)");
		mia_vector_push(items, (int64_t)index);
	}
}

/// whether every index a constant holds is in range.
static bool _mia_bytecode_constant_valid(const mia_bytecode_module_s* const module, const mia_bytecode_constant_s* const constant) {
	if (constant->type < -1 || constant->type >= (int64_t)module->types.count) return false;
	if (mia_bytecode_constant_function == constant->kind && constant->integer >= module->functions.count) return false;
	if (mia_bytecode_constant_intrinsic == constant->kind && constant->integer >= module->intrinsics.count) return false;
	for (size_t i = 0; i < constant->items.count; ++i) if (!_mia_bytecode_constant_valid(module, &constant->items.data[i])) return false;
	return true;
}

/// whether the operands of an instruction refer to things that exist.
static bool _mia_bytecode_instruction_valid(const mia_bytecode_module_s* const module, const mia_bytecode_function_s* const function, const mia_bytecode_instruction_s* const instruction) {
	const int64_t a = instruction->a;
	switch (instruction->opcode) {
		case mia_opcode_CONST: case mia_opcode_LOC_CONST:
			return a >= 0 && a < (int64_t)module->constants.count;
		case mia_opcode_ASSERT_FAIL:
			return a >= 0 && a < (int64_t)module->constants.count && mia_bytecode_constant_text == module->constants.data[a].kind;
		case mia_opcode_CALL: case mia_opcode_FUNCTION:
			return a >= 0 && a < (int64_t)module->functions.count;
		case mia_opcode_CALL_INTRINSIC: case mia_opcode_INTRINSIC:
			return a >= 0 && a < (int64_t)module->intrinsics.count;
		case mia_opcode_JUMP: case mia_opcode_JUMP_IF_FALSE: case mia_opcode_JUMP_IF_TRUE:
			return a >= 0 && a <= (int64_t)function->code.count;
		case mia_opcode_RANGE_TEST:
			return instruction->b >= 0 && instruction->b <= (int64_t)function->code.count;
		default:
			return true;
	}
}

bool mia_bytecode_read(const mia_view_s data, mia_bytecode_module_s* const module, char** const error) {
	*module = (mia_bytecode_module_s) {0};
	_mia_bytecode_reader_s reader = { .data = (const uint8_t*)data.data, .length = data.length, };
	_mia_bytecode_reader_s* const r = &reader;
	if (data.length < sizeof(_mia_bytecode_magic) || 0 != memcmp(data.data, _mia_bytecode_magic, sizeof(_mia_bytecode_magic))) {
		*error = mia_strdup("not a mia bytecode file");
		return false;
	}
	r->at = sizeof(_mia_bytecode_magic);
	const uint32_t version = _mia_bytecode_get_u32(r);
	if (NULL == r->error && version != mia_bytecode_version) {
		*error = mia_string_format("bytecode format version %u, but this mia reads version %u; build the program again", version, mia_bytecode_version);
		return false;
	}
	const uint32_t files = _mia_bytecode_get_count(r, 4);
	for (uint32_t i = 0; i < files && NULL == r->error; ++i) mia_vector_push(&module->files, _mia_bytecode_get_string(r));
	const uint32_t types = _mia_bytecode_get_count(r, 40);
	for (uint32_t i = 0; i < types && NULL == r->error; ++i) {
		mia_bytecode_type_s type = { .name = _mia_bytecode_get_string(r), };
		const uint8_t kind = _mia_bytecode_get_u8(r), code = _mia_bytecode_get_u8(r);
		if (kind >= mia_bytecode_descriptor_count || code >= mia_code_count) _mia_bytecode_reader_fail(r, "the file is corrupted (an unknown type)");
		type.kind = (mia_bytecode_descriptor_kind_e)(kind % mia_bytecode_descriptor_count);
		type.code = (mia_code_e)(code % mia_code_count);
		type.size = _mia_bytecode_get_u64(r);
		type.alignment = _mia_bytecode_get_u64(r);
		type.element = _mia_bytecode_get_u32(r);
		type.length = _mia_bytecode_get_s64(r);
		type.zero = _mia_bytecode_get_s64(r);
		const uint32_t fields = _mia_bytecode_get_count(r, 16);
		for (uint32_t f = 0; f < fields && NULL == r->error; ++f) {
			mia_vector_push(&type.field_names, _mia_bytecode_get_string(r));
			mia_vector_push(&type.field_types, (int64_t)_mia_bytecode_get_u32(r));
			mia_vector_push(&type.field_offsets, _mia_bytecode_get_u64(r));
		}
		mia_vector_push(&module->types, type);
	}
	const uint32_t constants = _mia_bytecode_get_count(r, 9);
	for (uint32_t i = 0; i < constants && NULL == r->error; ++i) {
		mia_bytecode_constant_s constant = {0};
		_mia_bytecode_get_constant(r, &constant, 0);
		mia_vector_push(&module->constants, constant);
	}
	const uint32_t intrinsics = _mia_bytecode_get_count(r, 28);
	for (uint32_t i = 0; i < intrinsics && NULL == r->error; ++i) {
		mia_bytecode_intrinsic_s intrinsic = { .name = _mia_bytecode_get_string(r), };
		intrinsic.plugin = _mia_bytecode_get_string(r);
		intrinsic.result = _mia_bytecode_get_s64(r);
		if (intrinsic.result < -1 || intrinsic.result >= (int64_t)module->types.count) _mia_bytecode_reader_fail(r, "the file is corrupted (a type index is out of range)");
		const uint32_t arguments = _mia_bytecode_get_count(r, 4), parameters = _mia_bytecode_get_count(r, 4), variadic = _mia_bytecode_get_count(r, 4);
		_mia_bytecode_get_indexes(r, arguments, module->types.count, &intrinsic.arguments);
		_mia_bytecode_get_indexes(r, parameters, module->types.count, &intrinsic.parameters);
		_mia_bytecode_get_indexes(r, variadic, module->types.count, &intrinsic.variadic);
		const uint32_t flags = _mia_bytecode_get_count(r, 4);
		for (uint32_t f = 0; f < flags && NULL == r->error; ++f) mia_vector_push(&intrinsic.flags, _mia_bytecode_get_u32(r));
		mia_vector_push(&module->intrinsics, intrinsic);
	}
	const uint32_t functions = _mia_bytecode_get_count(r, 25);
	for (uint32_t i = 0; i < functions && NULL == r->error; ++i) {
		mia_bytecode_function_s function = { .name = _mia_bytecode_get_string(r), };
		function.parameters = _mia_bytecode_get_u32(r);
		function.frame = _mia_bytecode_get_u64(r);
		function.result = _mia_bytecode_get_u32(r);
		function.returns = _mia_bytecode_get_u8(r) != 0;
		const uint32_t slots = _mia_bytecode_get_count(r, 17);
		for (uint32_t s = 0; s < slots && NULL == r->error; ++s) {
			mia_bytecode_slot_s slot = {0};
			const uint8_t code = _mia_bytecode_get_u8(r);
			if (code >= mia_code_count) _mia_bytecode_reader_fail(r, "the file is corrupted (an unknown type code)");
			slot.code = (mia_code_e)(code % mia_code_count);
			slot.offset = _mia_bytecode_get_u64(r);
			slot.size = _mia_bytecode_get_u64(r);
			mia_vector_push(&function.slots, slot);
		}
		const uint32_t code = _mia_bytecode_get_count(r, 3 + 16);
		for (uint32_t c = 0; c < code && NULL == r->error; ++c) {
			mia_bytecode_instruction_s instruction = {0};
			const uint8_t opcode = _mia_bytecode_get_u8(r), type = _mia_bytecode_get_u8(r), target = _mia_bytecode_get_u8(r);
			if (opcode >= mia_opcode_count || type >= mia_code_count || target >= mia_code_count) {
				_mia_bytecode_reader_fail(r, "the file is corrupted (an unknown instruction)");
				break;
			}
			instruction.opcode = (mia_opcode_e)opcode;
			instruction.type = (mia_code_e)type;
			instruction.target = (mia_code_e)target;
			const int operands = mia_opcode_operands(instruction.opcode);
			if (operands >= 1) instruction.a = _mia_bytecode_get_s64(r);
			if (operands >= 2) instruction.b = _mia_bytecode_get_s64(r);
			if (operands >= 3) instruction.c = _mia_bytecode_get_s64(r);
			mia_vector_push(&function.code, instruction);
		}
		for (uint32_t p = 0; p < code && NULL == r->error; ++p) {
			mia_bytecode_position_s position = {0};
			position.file = (int32_t)_mia_bytecode_get_u32(r);
			position.line = _mia_bytecode_get_u32(r);
			position.column = _mia_bytecode_get_u32(r);
			position.length = _mia_bytecode_get_u32(r);
			if (position.file < -1 || position.file >= (int32_t)module->files.count) _mia_bytecode_reader_fail(r, "the file is corrupted (a position names no file)");
			mia_vector_push(&function.positions, position);
		}
		mia_vector_push(&module->functions, function);
	}
	module->entry = _mia_bytecode_get_u32(r);
	module->entry_returns = _mia_bytecode_get_u8(r) != 0;
	if (NULL == r->error && r->at != r->length) _mia_bytecode_reader_fail(r, "the file is corrupted (bytes after the end)");
	// cross references, now that every section is known
	for (size_t i = 0; i < module->types.count && NULL == r->error; ++i) {
		const mia_bytecode_type_s* const type = &module->types.data[i];
		bool good = type->element < (int64_t)module->types.count && type->zero >= -1 && type->zero < (int64_t)module->constants.count;
		for (size_t f = 0; f < type->field_types.count; ++f) good = good && type->field_types.data[f] < (int64_t)module->types.count;
		if (!good) _mia_bytecode_reader_fail(r, "the file is corrupted (a type refers to nothing)");
	}
	for (size_t i = 0; i < module->constants.count && NULL == r->error; ++i) {
		if (!_mia_bytecode_constant_valid(module, &module->constants.data[i])) _mia_bytecode_reader_fail(r, "the file is corrupted (a constant refers to nothing)");
	}
	for (size_t f = 0; f < module->functions.count && NULL == r->error; ++f) {
		const mia_bytecode_function_s* const function = &module->functions.data[f];
		for (size_t c = 0; c < function->code.count; ++c) {
			if (!_mia_bytecode_instruction_valid(module, function, &function->code.data[c])) {
				_mia_bytecode_reader_fail(r, "the file is corrupted (an instruction refers to nothing)");
				break;
			}
		}
	}
	if (NULL == r->error && (module->entry < 0 || module->entry >= (int64_t)module->functions.count)) {
		_mia_bytecode_reader_fail(r, "the file is corrupted (no entry function)");
	}
	if (r->error != NULL) {
		*error = mia_strdup(r->error);
		mia_bytecode_free(module);
		return false;
	}
	return true;
}

bool mia_bytecode_load(const char* const path, mia_bytecode_module_s* const module, char** const error) {
	*module = (mia_bytecode_module_s) {0};
	mia_string_s content = {0};
	if (!mia_source_read_file(path, &content)) {
		*error = mia_strdup("cannot read the file");
		return false;
	}
	const bool loaded = mia_bytecode_read(mia_string_view(&content), module, error);
	mia_string_free(&content);
	return loaded;
}
