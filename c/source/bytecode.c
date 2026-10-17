
/// bytecode: instruction names, the binary '.mcb' file format (documented
/// in bytecode.h) and the disassembler.

#include "mc/bytecode.h"
#include "mc/format.h"
#include "mc/source.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static const char _mc_magic[4] = { 'M', 'C', 'B', '\0', };

typedef struct {
	const char* name    ;
	int         types   ;
	int         operands;
} _mc_opcode_info_s;

static const _mc_opcode_info_s _mc_opcodes[] = {
#define mc_opcode(_name, _types, _operands) { #_name, _types, _operands, },
#include "mc/opcodes.def"
#undef mc_opcode
};

static const char* const _mc_type_codes[] = {
	"S8", "S16", "S32", "S64", "U8", "U16", "U32", "U64", "INT", "UINT", "F32", "F64", "FLOAT", "BOOL", "FN", "SLICE",
};

const char* mc_opcode_name(const mc_opcode_e opcode) {
	return _mc_opcodes[opcode].name;
}

int mc_opcode_types(const mc_opcode_e opcode) {
	return _mc_opcodes[opcode].types;
}

int mc_opcode_operands(const mc_opcode_e opcode) {
	return _mc_opcodes[opcode].operands;
}

const char* mc_type_code_name(const mc_type_code_e code) {
	return _mc_type_codes[code];
}

uint64_t mc_type_code_size(const mc_type_code_e code) {
	return (mc_code_slice == code) ? 16 : 8;
}

void mc_instruction_name(const mc_instruction_s* const instruction, mc_string_s* const out) {
	const _mc_opcode_info_s* const info = &_mc_opcodes[instruction->opcode];
	mc_string_append_cstr(out, info->name);
	if (info->types >= 1) mc_string_appendf(out, "_%s", _mc_type_codes[instruction->type]);
	if (info->types >= 2) mc_string_appendf(out, "_%s", _mc_type_codes[instruction->target]);
}

/// the type code at the start of 'text'; its length, or 0.
static size_t _mc_type_code(const char* const text, mc_type_code_e* const code) {
	size_t best = 0;
	for (size_t i = 0; i < mc_count_of(_mc_type_codes); ++i) {
		const size_t length = strlen(_mc_type_codes[i]);
		if (length > best && 0 == strncmp(_mc_type_codes[i], text, length) && ('\0' == text[length] || '_' == text[length])) {
			*code = (mc_type_code_e)i;
			best = length;
		}
	}
	return best;
}

bool mc_instruction_parse(const char* const name, mc_instruction_s* const instruction) {
	for (size_t i = 0; i < mc_count_of(_mc_opcodes); ++i) {
		const _mc_opcode_info_s* const info = &_mc_opcodes[i];
		const size_t length = strlen(info->name);
		if (0 != strncmp(name, info->name, length)) continue;
		*instruction = (mc_instruction_s) { .opcode = (mc_opcode_e)i, };
		const char* rest = name + length;
		bool good = true;
		for (int t = 0; t < info->types && good; ++t) {
			mc_type_code_e code = mc_code_int;
			const size_t taken = ('_' == rest[0]) ? _mc_type_code(rest + 1, &code) : 0;
			good = taken > 0;
			if (0 == t) instruction->type = code;
			else instruction->target = code;
			rest += good ? taken + 1 : 0;
		}
		if (good && '\0' == rest[0]) return true;
	}
	*instruction = (mc_instruction_s) {0};
	return false;
}

void mc_bc_constant_free(mc_bc_constant_s* const constant) {
	for (size_t i = 0; i < constant->items.count; ++i) mc_bc_constant_free(&constant->items.data[i]);
	mc_vector_free(&constant->items);
	mc_free(constant->text);
	constant->text = NULL;
}

void mc_module_bc_free(mc_module_bc_s* const module) {
	for (size_t i = 0; i < module->functions.count; ++i) {
		mc_bc_function_s* const function = &module->functions.data[i];
		mc_free(function->name);
		mc_vector_free(&function->slots);
		mc_vector_free(&function->code);
		mc_vector_free(&function->positions);
	}
	mc_vector_free(&module->functions);
	for (size_t i = 0; i < module->constants.count; ++i) mc_bc_constant_free(&module->constants.data[i]);
	mc_vector_free(&module->constants);
	for (size_t i = 0; i < module->types.count; ++i) {
		mc_bc_type_s* const type = &module->types.data[i];
		mc_free(type->name);
		for (size_t f = 0; f < type->field_names.count; ++f) mc_free(type->field_names.data[f]);
		mc_vector_free(&type->field_names);
		mc_vector_free(&type->field_types);
		mc_vector_free(&type->field_offsets);
	}
	mc_vector_free(&module->types);
	for (size_t i = 0; i < module->intrinsics.count; ++i) {
		mc_bc_intrinsic_s* const intrinsic = &module->intrinsics.data[i];
		mc_free(intrinsic->name);
		mc_vector_free(&intrinsic->arguments);
		mc_vector_free(&intrinsic->parameters);
		mc_vector_free(&intrinsic->variadic);
		mc_vector_free(&intrinsic->flags);
	}
	mc_vector_free(&module->intrinsics);
	for (size_t i = 0; i < module->files.count; ++i) mc_free(module->files.data[i]);
	mc_vector_free(&module->files);
	*module = (mc_module_bc_s) {0};
}

// --- writing ---------------------------------------------------------------------------

static void _mc_put(mc_string_s* const out, uint64_t value, const size_t bytes) {
	for (size_t i = 0; i < bytes; ++i) {
		mc_string_append_char(out, (char)(uint8_t)(value & 0xFF));
		value >>= 8;
	}
}

static void _mc_put_u8(mc_string_s* const out, const uint64_t value) {
	_mc_put(out, value, 1);
}

static void _mc_put_u32(mc_string_s* const out, const uint64_t value) {
	_mc_put(out, value, 4);
}

static void _mc_put_u64(mc_string_s* const out, const uint64_t value) {
	_mc_put(out, value, 8);
}

static void _mc_put_s64(mc_string_s* const out, const int64_t value) {
	_mc_put(out, (uint64_t)value, 8);
}

static void _mc_put_string(mc_string_s* const out, const char* const text) {
	const size_t length = (text != NULL) ? strlen(text) : 0;
	_mc_put_u32(out, length);
	if (length > 0) mc_string_append(out, mc_view_new(text, length));
}

static uint64_t _mc_real_bits(const double value) {
	uint64_t bits = 0;
	memcpy(&bits, &value, sizeof(bits));
	return bits;
}

static void _mc_put_constant(mc_string_s* const out, const mc_bc_constant_s* const constant) {
	_mc_put_u8(out, (uint64_t)constant->kind);
	_mc_put_s64(out, constant->type);
	switch (constant->kind) {
		case mc_constant_none: break;
		case mc_constant_integer:
			_mc_put_u64(out, constant->integer);
			_mc_put_u8(out, constant->signed_);
			break;
		case mc_constant_real:      _mc_put_u64(out, _mc_real_bits(constant->real)); break;
		case mc_constant_boolean:   _mc_put_u8(out, constant->boolean); break;
		case mc_constant_text:      _mc_put_string(out, constant->text); break;
		case mc_constant_function:
		case mc_constant_intrinsic: _mc_put_u64(out, constant->integer); break;
		case mc_constant_slice:
			_mc_put_u8(out, constant->null);
			_mc_put_u8(out, constant->read_only);
			_mc_put_s64(out, constant->start);
			_mc_put_s64(out, constant->length);
			break;
		case mc_constant_list:
			break;
	}
	if (mc_constant_slice == constant->kind || mc_constant_list == constant->kind) {
		_mc_put_u32(out, constant->items.count);
		for (size_t i = 0; i < constant->items.count; ++i) _mc_put_constant(out, &constant->items.data[i]);
	}
}

static void _mc_put_indexes(mc_string_s* const out, const int64_t* const items, const size_t count) {
	for (size_t i = 0; i < count; ++i) _mc_put_u32(out, (uint64_t)items[i]);
}

void mc_module_bc_write(const mc_module_bc_s* const module, mc_string_s* const out) {
	mc_string_append(out, mc_view_new(_mc_magic, sizeof(_mc_magic)));
	_mc_put_u32(out, mc_bytecode_version);
	_mc_put_u32(out, module->files.count);
	for (size_t i = 0; i < module->files.count; ++i) _mc_put_string(out, module->files.data[i]);
	_mc_put_u32(out, module->types.count);
	for (size_t i = 0; i < module->types.count; ++i) {
		const mc_bc_type_s* const type = &module->types.data[i];
		_mc_put_string(out, type->name);
		_mc_put_u8(out, (uint64_t)type->kind);
		_mc_put_u8(out, (uint64_t)type->code);
		_mc_put_u64(out, type->size);
		_mc_put_u64(out, type->alignment);
		_mc_put_u32(out, (uint64_t)type->element);
		_mc_put_s64(out, type->length);
		_mc_put_s64(out, type->zero);
		_mc_put_u32(out, type->field_names.count);
		for (size_t f = 0; f < type->field_names.count; ++f) {
			_mc_put_string(out, type->field_names.data[f]);
			_mc_put_u32(out, (uint64_t)type->field_types.data[f]);
			_mc_put_u64(out, type->field_offsets.data[f]);
		}
	}
	_mc_put_u32(out, module->constants.count);
	for (size_t i = 0; i < module->constants.count; ++i) _mc_put_constant(out, &module->constants.data[i]);
	_mc_put_u32(out, module->intrinsics.count);
	for (size_t i = 0; i < module->intrinsics.count; ++i) {
		const mc_bc_intrinsic_s* const intrinsic = &module->intrinsics.data[i];
		_mc_put_string(out, intrinsic->name);
		_mc_put_s64(out, intrinsic->result);
		_mc_put_u32(out, intrinsic->arguments.count);
		_mc_put_u32(out, intrinsic->parameters.count);
		_mc_put_u32(out, intrinsic->variadic.count);
		_mc_put_indexes(out, intrinsic->arguments.data, intrinsic->arguments.count);
		_mc_put_indexes(out, intrinsic->parameters.data, intrinsic->parameters.count);
		_mc_put_indexes(out, intrinsic->variadic.data, intrinsic->variadic.count);
		_mc_put_u32(out, intrinsic->flags.count);
		for (size_t f = 0; f < intrinsic->flags.count; ++f) _mc_put_u32(out, intrinsic->flags.data[f]);
	}
	_mc_put_u32(out, module->functions.count);
	for (size_t i = 0; i < module->functions.count; ++i) {
		const mc_bc_function_s* const function = &module->functions.data[i];
		_mc_put_string(out, function->name);
		_mc_put_u32(out, function->parameters);
		_mc_put_u64(out, function->frame);
		_mc_put_u32(out, function->result);
		_mc_put_u8(out, function->returns);
		_mc_put_u32(out, function->slots.count);
		for (size_t s = 0; s < function->slots.count; ++s) {
			_mc_put_u8(out, (uint64_t)function->slots.data[s].code);
			_mc_put_u64(out, function->slots.data[s].offset);
			_mc_put_u64(out, function->slots.data[s].size);
		}
		_mc_put_u32(out, function->code.count);
		for (size_t c = 0; c < function->code.count; ++c) {
			const mc_instruction_s* const instruction = &function->code.data[c];
			_mc_put_u8(out, (uint64_t)instruction->opcode);
			_mc_put_u8(out, (uint64_t)instruction->type);
			_mc_put_u8(out, (uint64_t)instruction->target);
			const int operands = mc_opcode_operands(instruction->opcode);
			if (operands >= 1) _mc_put_s64(out, instruction->a);
			if (operands >= 2) _mc_put_s64(out, instruction->b);
			if (operands >= 3) _mc_put_s64(out, instruction->c);
		}
		for (size_t p = 0; p < function->code.count; ++p) {
			const mc_position_s position = (p < function->positions.count) ? function->positions.data[p] : (mc_position_s) { .file = -1, };
			_mc_put_u32(out, (uint64_t)(int64_t)position.file);
			_mc_put_u32(out, position.line);
			_mc_put_u32(out, position.column);
			_mc_put_u32(out, position.length);
		}
	}
	_mc_put_u32(out, (uint64_t)module->entry);
	_mc_put_u8(out, module->entry_returns);
}

bool mc_module_bc_save(const mc_module_bc_s* const module, const char* const path) {
	mc_string_s out = {0};
	mc_module_bc_write(module, &out);
	FILE* const file = fopen(path, "wb");
	bool written = false;
	if (file != NULL) {
		written = fwrite(out.data, 1, out.length, file) == out.length;
		written = (0 == fclose(file)) && written;
	}
	mc_string_free(&out);
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
} _mc_reader_s;

static void _mc_reader_fail(_mc_reader_s* const r, const char* const error) {
	if (NULL == r->error) r->error = error;
}

static uint64_t _mc_get(_mc_reader_s* const r, const size_t bytes) {
	if (r->error != NULL) return 0;
	if (r->length - r->at < bytes) {
		_mc_reader_fail(r, "the file is truncated");
		return 0;
	}
	uint64_t value = 0;
	for (size_t i = 0; i < bytes; ++i) value |= (uint64_t)r->data[r->at + i] << (8 * i);
	r->at += bytes;
	return value;
}

static uint8_t _mc_get_u8(_mc_reader_s* const r) {
	return (uint8_t)_mc_get(r, 1);
}

static uint32_t _mc_get_u32(_mc_reader_s* const r) {
	return (uint32_t)_mc_get(r, 4);
}

static uint64_t _mc_get_u64(_mc_reader_s* const r) {
	return _mc_get(r, 8);
}

static int64_t _mc_get_s64(_mc_reader_s* const r) {
	return (int64_t)_mc_get(r, 8);
}

/// a count of entries that each take at least 'minimum' bytes (so a broken
/// count cannot make the reader allocate more than the file could hold).
static uint32_t _mc_get_count(_mc_reader_s* const r, const size_t minimum) {
	const uint32_t count = _mc_get_u32(r);
	if (r->error == NULL && (uint64_t)count * minimum > r->length - r->at) {
		_mc_reader_fail(r, "the file is corrupted (a count is larger than the file)");
		return 0;
	}
	return count;
}

static char* _mc_get_string(_mc_reader_s* const r) {
	const uint32_t length = _mc_get_count(r, 1);
	if (r->error != NULL) return mc_strdup("");
	char* const text = mc_malloc((size_t)length + 1);
	memcpy(text, r->data + r->at, length);
	text[length] = '\0';
	r->at += length;
	if (strlen(text) != length) _mc_reader_fail(r, "the file is corrupted (a NUL in a name)");
	return text;
}

static void _mc_get_constant(_mc_reader_s* const r, mc_bc_constant_s* const constant, const int depth) {
	*constant = (mc_bc_constant_s) {0};
	const uint8_t kind = _mc_get_u8(r);
	constant->type = _mc_get_s64(r);
	if (kind > mc_constant_intrinsic || depth > 1000) {
		_mc_reader_fail(r, "the file is corrupted (an unknown constant)");
		return;
	}
	constant->kind = (mc_constant_kind_e)kind;
	switch (constant->kind) {
		case mc_constant_none: break;
		case mc_constant_integer:
			constant->integer = _mc_get_u64(r);
			constant->signed_ = _mc_get_u8(r) != 0;
			break;
		case mc_constant_real: {
			const uint64_t bits = _mc_get_u64(r);
			memcpy(&constant->real, &bits, sizeof(bits));
			break;
		}
		case mc_constant_boolean:   constant->boolean = _mc_get_u8(r) != 0; break;
		case mc_constant_text:      constant->text = _mc_get_string(r); break;
		case mc_constant_function:
		case mc_constant_intrinsic: constant->integer = _mc_get_u64(r); break;
		case mc_constant_slice:
			constant->null = _mc_get_u8(r) != 0;
			constant->read_only = _mc_get_u8(r) != 0;
			constant->start = _mc_get_s64(r);
			constant->length = _mc_get_s64(r);
			break;
		case mc_constant_list:
			break;
	}
	if (mc_constant_slice == constant->kind || mc_constant_list == constant->kind) {
		const uint32_t count = _mc_get_count(r, 9);
		for (uint32_t i = 0; i < count && NULL == r->error; ++i) {
			mc_bc_constant_s item = {0};
			_mc_get_constant(r, &item, depth + 1);
			mc_vector_push(&constant->items, item);
		}
	}
}

static void _mc_get_indexes(_mc_reader_s* const r, const uint32_t count, const size_t limit, mc_bc_indexes_s* const items) {
	for (uint32_t i = 0; i < count && NULL == r->error; ++i) {
		const uint32_t index = _mc_get_u32(r);
		if (index >= limit) _mc_reader_fail(r, "the file is corrupted (a type index is out of range)");
		mc_vector_push(items, (int64_t)index);
	}
}

/// whether every index a constant holds is in range.
static bool _mc_constant_valid(const mc_module_bc_s* const module, const mc_bc_constant_s* const constant) {
	if (constant->type < -1 || constant->type >= (int64_t)module->types.count) return false;
	if (mc_constant_function == constant->kind && constant->integer >= module->functions.count) return false;
	if (mc_constant_intrinsic == constant->kind && constant->integer >= module->intrinsics.count) return false;
	for (size_t i = 0; i < constant->items.count; ++i) if (!_mc_constant_valid(module, &constant->items.data[i])) return false;
	return true;
}

/// whether the operands of an instruction refer to things that exist.
static bool _mc_instruction_valid(const mc_module_bc_s* const module, const mc_bc_function_s* const function, const mc_instruction_s* const instruction) {
	const int64_t a = instruction->a;
	switch (instruction->opcode) {
		case mc_op_CONST: case mc_op_LOC_CONST:
			return a >= 0 && a < (int64_t)module->constants.count;
		case mc_op_ASSERT_FAIL:
			return a >= 0 && a < (int64_t)module->constants.count && mc_constant_text == module->constants.data[a].kind;
		case mc_op_CALL: case mc_op_FUNCTION:
			return a >= 0 && a < (int64_t)module->functions.count;
		case mc_op_CALL_INTRINSIC: case mc_op_INTRINSIC:
			return a >= 0 && a < (int64_t)module->intrinsics.count;
		case mc_op_JUMP: case mc_op_JUMP_IF_FALSE: case mc_op_JUMP_IF_TRUE:
			return a >= 0 && a <= (int64_t)function->code.count;
		case mc_op_RANGE_TEST:
			return instruction->b >= 0 && instruction->b <= (int64_t)function->code.count;
		default:
			return true;
	}
}

bool mc_module_bc_read(const mc_view_s data, mc_module_bc_s* const module, char** const error) {
	*module = (mc_module_bc_s) {0};
	_mc_reader_s reader = { .data = (const uint8_t*)data.data, .length = data.length, };
	_mc_reader_s* const r = &reader;
	if (data.length < sizeof(_mc_magic) || 0 != memcmp(data.data, _mc_magic, sizeof(_mc_magic))) {
		*error = mc_strdup("not an mc bytecode file");
		return false;
	}
	r->at = sizeof(_mc_magic);
	const uint32_t version = _mc_get_u32(r);
	if (NULL == r->error && version != mc_bytecode_version) {
		*error = mc_format("bytecode format version %u, but this mc reads version %u; build the program again", version, mc_bytecode_version);
		return false;
	}
	const uint32_t files = _mc_get_count(r, 4);
	for (uint32_t i = 0; i < files && NULL == r->error; ++i) mc_vector_push(&module->files, _mc_get_string(r));
	const uint32_t types = _mc_get_count(r, 40);
	for (uint32_t i = 0; i < types && NULL == r->error; ++i) {
		mc_bc_type_s type = { .name = _mc_get_string(r), };
		const uint8_t kind = _mc_get_u8(r), code = _mc_get_u8(r);
		if (kind >= mc_descriptor_count_ || code > mc_type_code_count_) _mc_reader_fail(r, "the file is corrupted (an unknown type)");
		type.kind = (mc_descriptor_kind_e)(kind % mc_descriptor_count_);
		type.code = (mc_type_code_e)(code % (mc_type_code_count_ + 1));
		type.size = _mc_get_u64(r);
		type.alignment = _mc_get_u64(r);
		type.element = _mc_get_u32(r);
		type.length = _mc_get_s64(r);
		type.zero = _mc_get_s64(r);
		const uint32_t fields = _mc_get_count(r, 16);
		for (uint32_t f = 0; f < fields && NULL == r->error; ++f) {
			mc_vector_push(&type.field_names, _mc_get_string(r));
			mc_vector_push(&type.field_types, (int64_t)_mc_get_u32(r));
			mc_vector_push(&type.field_offsets, _mc_get_u64(r));
		}
		mc_vector_push(&module->types, type);
	}
	const uint32_t constants = _mc_get_count(r, 9);
	for (uint32_t i = 0; i < constants && NULL == r->error; ++i) {
		mc_bc_constant_s constant = {0};
		_mc_get_constant(r, &constant, 0);
		mc_vector_push(&module->constants, constant);
	}
	const uint32_t intrinsics = _mc_get_count(r, 28);
	for (uint32_t i = 0; i < intrinsics && NULL == r->error; ++i) {
		mc_bc_intrinsic_s intrinsic = { .name = _mc_get_string(r), };
		intrinsic.result = _mc_get_s64(r);
		if (intrinsic.result < -1 || intrinsic.result >= (int64_t)module->types.count) _mc_reader_fail(r, "the file is corrupted (a type index is out of range)");
		const uint32_t arguments = _mc_get_count(r, 4), parameters = _mc_get_count(r, 4), variadic = _mc_get_count(r, 4);
		_mc_get_indexes(r, arguments, module->types.count, &intrinsic.arguments);
		_mc_get_indexes(r, parameters, module->types.count, &intrinsic.parameters);
		_mc_get_indexes(r, variadic, module->types.count, &intrinsic.variadic);
		const uint32_t flags = _mc_get_count(r, 4);
		for (uint32_t f = 0; f < flags && NULL == r->error; ++f) mc_vector_push(&intrinsic.flags, _mc_get_u32(r));
		mc_vector_push(&module->intrinsics, intrinsic);
	}
	const uint32_t functions = _mc_get_count(r, 25);
	for (uint32_t i = 0; i < functions && NULL == r->error; ++i) {
		mc_bc_function_s function = { .name = _mc_get_string(r), };
		function.parameters = _mc_get_u32(r);
		function.frame = _mc_get_u64(r);
		function.result = _mc_get_u32(r);
		function.returns = _mc_get_u8(r) != 0;
		const uint32_t slots = _mc_get_count(r, 17);
		for (uint32_t s = 0; s < slots && NULL == r->error; ++s) {
			mc_bc_slot_s slot = {0};
			const uint8_t code = _mc_get_u8(r);
			if (code > mc_type_code_count_) _mc_reader_fail(r, "the file is corrupted (an unknown type code)");
			slot.code = (mc_type_code_e)(code % (mc_type_code_count_ + 1));
			slot.offset = _mc_get_u64(r);
			slot.size = _mc_get_u64(r);
			mc_vector_push(&function.slots, slot);
		}
		const uint32_t code = _mc_get_count(r, 3 + 16);
		for (uint32_t c = 0; c < code && NULL == r->error; ++c) {
			mc_instruction_s instruction = {0};
			const uint8_t opcode = _mc_get_u8(r), type = _mc_get_u8(r), target = _mc_get_u8(r);
			if (opcode >= mc_op_count_ || type >= mc_type_code_count_ || target >= mc_type_code_count_) {
				_mc_reader_fail(r, "the file is corrupted (an unknown instruction)");
				break;
			}
			instruction.opcode = (mc_opcode_e)opcode;
			instruction.type = (mc_type_code_e)type;
			instruction.target = (mc_type_code_e)target;
			const int operands = mc_opcode_operands(instruction.opcode);
			if (operands >= 1) instruction.a = _mc_get_s64(r);
			if (operands >= 2) instruction.b = _mc_get_s64(r);
			if (operands >= 3) instruction.c = _mc_get_s64(r);
			mc_vector_push(&function.code, instruction);
		}
		for (uint32_t p = 0; p < code && NULL == r->error; ++p) {
			mc_position_s position = {0};
			position.file = (int32_t)_mc_get_u32(r);
			position.line = _mc_get_u32(r);
			position.column = _mc_get_u32(r);
			position.length = _mc_get_u32(r);
			if (position.file < -1 || position.file >= (int32_t)module->files.count) _mc_reader_fail(r, "the file is corrupted (a position names no file)");
			mc_vector_push(&function.positions, position);
		}
		mc_vector_push(&module->functions, function);
	}
	module->entry = _mc_get_u32(r);
	module->entry_returns = _mc_get_u8(r) != 0;
	if (NULL == r->error && r->at != r->length) _mc_reader_fail(r, "the file is corrupted (bytes after the end)");
	// cross references, now that every section is known
	for (size_t i = 0; i < module->types.count && NULL == r->error; ++i) {
		const mc_bc_type_s* const type = &module->types.data[i];
		bool good = type->element < (int64_t)module->types.count && type->zero >= -1 && type->zero < (int64_t)module->constants.count;
		for (size_t f = 0; f < type->field_types.count; ++f) good = good && type->field_types.data[f] < (int64_t)module->types.count;
		if (!good) _mc_reader_fail(r, "the file is corrupted (a type refers to nothing)");
	}
	for (size_t i = 0; i < module->constants.count && NULL == r->error; ++i) {
		if (!_mc_constant_valid(module, &module->constants.data[i])) _mc_reader_fail(r, "the file is corrupted (a constant refers to nothing)");
	}
	for (size_t f = 0; f < module->functions.count && NULL == r->error; ++f) {
		const mc_bc_function_s* const function = &module->functions.data[f];
		for (size_t c = 0; c < function->code.count; ++c) {
			if (!_mc_instruction_valid(module, function, &function->code.data[c])) {
				_mc_reader_fail(r, "the file is corrupted (an instruction refers to nothing)");
				break;
			}
		}
	}
	if (NULL == r->error && (module->entry < 0 || module->entry >= (int64_t)module->functions.count)) {
		_mc_reader_fail(r, "the file is corrupted (no entry function)");
	}
	if (r->error != NULL) {
		*error = mc_strdup(r->error);
		mc_module_bc_free(module);
		return false;
	}
	return true;
}

bool mc_module_bc_load(const char* const path, mc_module_bc_s* const module, char** const error) {
	*module = (mc_module_bc_s) {0};
	mc_string_s content = {0};
	if (!mc_read_file(path, &content)) {
		*error = mc_strdup("cannot read the file");
		return false;
	}
	const bool loaded = mc_module_bc_read(mc_string_view(&content), module, error);
	mc_string_free(&content);
	return loaded;
}

// --- the disassembler --------------------------------------------------------------------

void mc_bc_constant_text(mc_string_s* const out, const mc_bc_constant_s* const constant) {
	switch (constant->kind) {
		case mc_constant_none:      mc_string_append_cstr(out, "None"); return;
		case mc_constant_integer:
			if (constant->signed_) mc_string_appendf(out, "%" PRId64, (int64_t)constant->integer);
			else mc_string_appendf(out, "%" PRIu64, constant->integer);
			return;
		case mc_constant_real:      mc_format_float_repr(out, constant->real); return;
		case mc_constant_boolean:   mc_string_append_cstr(out, constant->boolean ? "true" : "false"); return;
		case mc_constant_text:      mc_format_repr_text(out, mc_view_cstr(constant->text)); return;
		case mc_constant_function:  mc_string_appendf(out, "function %" PRIu64, constant->integer); return;
		case mc_constant_intrinsic: mc_string_appendf(out, "intrinsic %" PRIu64, constant->integer); return;
		case mc_constant_slice: {
			mc_string_append_cstr(out, "slice {");
			for (int64_t i = 0; i < constant->length && constant->start + i < (int64_t)constant->items.count; ++i) {
				if (i > 0) mc_string_append_cstr(out, ", ");
				mc_bc_constant_text(out, &constant->items.data[constant->start + i]);
			}
			mc_string_append_char(out, '}');
			return;
		}
		case mc_constant_list:
			mc_string_append_char(out, '{');
			for (size_t i = 0; i < constant->items.count; ++i) {
				if (i > 0) mc_string_append_cstr(out, ", ");
				mc_bc_constant_text(out, &constant->items.data[i]);
			}
			mc_string_append_char(out, '}');
			return;
	}
}

static const char* _mc_base_name(const char* const path) {
	const char* const slash = strrchr(path, '/');
	const char* const backslash = strrchr(path, '\\');
	const char* base = path;
	if (slash != NULL && slash + 1 > base) base = slash + 1;
	if (backslash != NULL && backslash + 1 > base) base = backslash + 1;
	return base;
}

void mc_disassemble(const mc_module_bc_s* const module, mc_string_s* const out) {
	for (size_t i = 0; i < module->constants.count; ++i) {
		mc_string_appendf(out, "constant %zu: ", i);
		mc_bc_constant_text(out, &module->constants.data[i]);
		mc_string_append_char(out, '\n');
	}
	if (module->constants.count > 0) mc_string_append_char(out, '\n');
	for (size_t i = 0; i < module->intrinsics.count; ++i) {
		const mc_bc_intrinsic_s* const intrinsic = &module->intrinsics.data[i];
		mc_string_appendf(out, "intrinsic %zu: %s", i, intrinsic->name);
		if (intrinsic->arguments.count > 0) {
			mc_string_append_char(out, '<');
			for (size_t a = 0; a < intrinsic->arguments.count; ++a) {
				mc_string_appendf(out, "%s%s", a > 0 ? ", " : "", module->types.data[intrinsic->arguments.data[a]].name);
			}
			mc_string_append_char(out, '>');
		}
		if (intrinsic->variadic.count > 0) {
			mc_string_append_cstr(out, " (...:");
			for (size_t a = 0; a < intrinsic->variadic.count; ++a) mc_string_appendf(out, " %s", module->types.data[intrinsic->variadic.data[a]].name);
			mc_string_append_char(out, ')');
		}
		mc_string_append_char(out, '\n');
	}
	if (module->intrinsics.count > 0) mc_string_append_char(out, '\n');
	for (size_t f = 0; f < module->functions.count; ++f) {
		const mc_bc_function_s* const function = &module->functions.data[f];
		mc_string_appendf(out, "function %zu %s: %u parameter(s), %" PRIu64 " byte(s) of frame%s%s\n", f, function->name, function->parameters,
			function->frame, function->returns ? ", returns a value" : "", (int64_t)f == module->entry ? "  (entry)" : "");
		for (size_t i = 0; i < function->code.count; ++i) {
			const mc_instruction_s* const instruction = &function->code.data[i];
			mc_string_s text = {0}, comment = {0}, where = {0};
			mc_instruction_name(instruction, &text);
			const int operands = mc_opcode_operands(instruction->opcode);
			if (1 == operands) mc_string_appendf(&text, " %" PRId64, instruction->a);
			else if (2 == operands) mc_string_appendf(&text, " [%" PRId64 ", %" PRId64 "]", instruction->a, instruction->b);
			else if (3 == operands) mc_string_appendf(&text, " [%" PRId64 ", %" PRId64 ", %" PRId64 "]", instruction->a, instruction->b, instruction->c);
			const mc_opcode_e opcode = instruction->opcode;
			if (mc_op_CALL == opcode || mc_op_FUNCTION == opcode) {
				mc_string_append_cstr(&comment, module->functions.data[instruction->a].name);
			}
			else if (mc_op_CALL_INTRINSIC == opcode || mc_op_INTRINSIC == opcode) {
				mc_string_append_cstr(&comment, module->intrinsics.data[instruction->a].name);
			}
			else if (mc_op_CONST == opcode || mc_op_LOC_CONST == opcode || mc_op_ASSERT_FAIL == opcode) {
				mc_bc_constant_text(&comment, &module->constants.data[instruction->a]);
			}
			if (comment.length > 40) {
				comment.length = 37;
				comment.data[37] = '\0';
				mc_string_append_cstr(&comment, "...");
			}
			const mc_position_s* const position = (i < function->positions.count) ? &function->positions.data[i] : NULL;
			if (position != NULL && position->file >= 0) {
				mc_string_appendf(&where, "%s:%u", _mc_base_name(module->files.data[position->file]), position->line);
			}
			mc_string_s line = {0};
			mc_string_appendf(&line, "  %4zu  %-32s %-16s %s", i, mc_string_cstr(&text), mc_string_cstr(&where), mc_string_cstr(&comment));
			while (line.length > 0 && ' ' == line.data[line.length - 1]) line.data[--line.length] = '\0';
			mc_string_append(out, mc_string_view(&line));
			mc_string_append_char(out, '\n');
			mc_string_free(&line);
			mc_string_free(&text);
			mc_string_free(&comment);
			mc_string_free(&where);
		}
		mc_string_append_char(out, '\n');
	}
}
