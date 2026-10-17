
/// the machine: loading a module (checking and decoding every function),
/// the instruction loop and runtime errors.
///
/// loading verifies each function before it can run: every jump lands on
/// an instruction, the operand stack has the same depth on every path into
/// an instruction and never underflows, every offset into the frame and
/// into values on the stack is inside them, and calls and returns move the
/// words their functions take and give. that is what lets the loop skip
/// those checks, and what keeps a damaged '.mcb' file from corrupting
/// memory (everything else, like an access through a slice, is checked
/// when it runs). the maximal depth of the stack found there is reserved on
/// every call, so pushing never has to check for room.
///
/// the loop runs decoded instructions: the frequent ones (word loads and
/// stores, arithmetic on 64-bit integers and doubles, comparisons, jumps,
/// slice elements) get their own cases with the operand stack in a local
/// pointer; the rest go through the general _mc_step.

#include "machine.h"
#include "mc/format.h"

#include <math.h>
#include <stdarg.h>
#include <string.h>

void mc_vm_vfail(mc_vm_s* const vm, const char* const format, va_list args) {
	mc_free(vm->error);
	vm->error = mc_vformat(format, args);
	longjmp(*vm->stop, 1);
}

void mc_vm_fail(mc_vm_s* const vm, const char* const format, ...) {
	va_list args; va_start(args, format);
	mc_vm_vfail(vm, format, args);
}

/// the decoded operations: the general one, and fast cases.
typedef enum {
	_mc_x_step,                                        // anything: _mc_step on the instruction
	_mc_x_const1, _mc_x_load8, _mc_x_store8, _mc_x_load16, _mc_x_store16,
	_mc_x_add, _mc_x_sub, _mc_x_mul,                   // 64-bit integers (no wrapping needed)
	_mc_x_fadd, _mc_x_fsub, _mc_x_fmul,                // doubles
	_mc_x_eq, _mc_x_ne,                                // integer and bool words
	_mc_x_slt, _mc_x_sle, _mc_x_sgt, _mc_x_sge,        // signed
	_mc_x_ult, _mc_x_ule, _mc_x_ugt, _mc_x_uge,        // unsigned
	_mc_x_flt, _mc_x_fle, _mc_x_fgt, _mc_x_fge,        // floats
	_mc_x_jump, _mc_x_jump_false, _mc_x_jump_true,
	_mc_x_range_test, _mc_x_range_step,
	_mc_x_index_slice8, _mc_x_index_slice1,            // elements of 8 bytes, of u8 or bool
	_mc_x_loc_index_slice, _mc_x_load_loc8, _mc_x_store_loc8,
	_mc_x_dup1, _mc_x_pop,
	_mc_x_call, _mc_x_return,
	_mc_x_load, _mc_x_store,                           // other scalars, converted
	_mc_x_load_bytes, _mc_x_store_bytes, _mc_x_zero, _mc_x_const,
	_mc_x_f32add, _mc_x_f32sub, _mc_x_f32mul,          // computed as doubles, rounded to f32
	_mc_x_field, _mc_x_set_field,                      // scalars in a struct on the stack
	_mc_x_not,
	_mc_x_load_loc, _mc_x_store_loc, _mc_x_index_slice, // other scalars, converted
} _mc_x_e;

struct mc_exec_s {
	uint8_t                 x          ;
	const mc_instruction_s* instruction;
	int64_t                 a          ;
	int64_t                 b          ;
	uint64_t                word       ;   // const1: the constant's word
};

// --- arithmetic --------------------------------------------------------------------------------

static bool _mc_is_float(const mc_type_code_e code) {
	return mc_code_f32 == code || mc_code_f64 == code || mc_code_float == code;
}

static mc_word_u _mc_real(const double value) {
	return (mc_word_u) { .f = value, };
}

static mc_word_u _mc_boolean(const bool value) {
	return (mc_word_u) { .u = value ? 1 : 0, };
}

/// the integer part of 'value' modulo 2^64 (like python's int(), then wrapped).
static uint64_t _mc_truncate(const double value) {
	const double whole = trunc(value);
	if (fabs(whole) < 9223372036854775808.0) return (uint64_t)(int64_t)whole;
	double low = fmod(whole, 18446744073709551616.0);
	if (low < 0) low += 18446744073709551616.0;
	return (low >= 18446744073709551616.0) ? 0 : (uint64_t)low;
}

static mc_word_u _mc_arithmetic(mc_vm_s* const vm, const mc_instruction_s* const instruction, const mc_word_u left, const mc_word_u right) {
	const mc_type_code_e code = instruction->type;
	const mc_opcode_e opcode = instruction->opcode;
	if (_mc_is_float(code)) {
		const double a = left.f, b = right.f;
		switch (opcode) {
			case mc_op_ADD: return _mc_real(mc_vm_round(code, a + b));
			case mc_op_SUB: return _mc_real(mc_vm_round(code, a - b));
			case mc_op_MUL: return _mc_real(mc_vm_round(code, a * b));
			case mc_op_DIV:
				if (0.0 == b) return _mc_real((0.0 == a || isnan(a)) ? NAN : copysign(INFINITY, a) * copysign(1.0, b));
				return _mc_real(mc_vm_round(code, a / b));
			default: break;
		}
		mc_vm_fail(vm, "`%s` on floats", mc_opcode_name(opcode));
	}
	const bool signed_ = mc_vm_signed(code);
	const uint64_t ua = left.u, ub = right.u;
	switch (opcode) {
		case mc_op_ADD: return mc_vm_wrap(code, ua + ub);
		case mc_op_SUB: return mc_vm_wrap(code, ua - ub);
		case mc_op_MUL: return mc_vm_wrap(code, ua * ub);
		case mc_op_DIV:
		case mc_op_MOD: {
			if (0 == ub) mc_vm_fail(vm, "division by zero");
			if (!signed_) return mc_vm_wrap(code, mc_op_DIV == opcode ? ua / ub : ua % ub);
			if (INT64_MIN == left.i && -1 == right.i) return mc_vm_wrap(code, mc_op_DIV == opcode ? ua : 0);
			return mc_vm_wrap(code, (uint64_t)(mc_op_DIV == opcode ? left.i / right.i : left.i % right.i));
		}
		case mc_op_AND: return mc_vm_wrap(code, ua & ub);
		case mc_op_OR:  return mc_vm_wrap(code, ua | ub);
		case mc_op_XOR: return mc_vm_wrap(code, ua ^ ub);
		case mc_op_SHL:
		case mc_op_SHR: {
			if (mc_vm_signed(instruction->target) && right.i < 0) {
				char text[48];
				mc_vm_fail(vm, "negative shift count %s", mc_vm_int_text(text, right.i));
			}
			const uint64_t count = ub;
			if (mc_op_SHL == opcode) return mc_vm_wrap(code, count >= 64 ? 0 : ua << count);
			if (signed_) {
				if (count >= 64) return mc_vm_wrap(code, left.i < 0 ? UINT64_MAX : 0);
				// an arithmetic shift, written so it does not depend on the c implementation
				return mc_vm_wrap(code, left.i < 0 ? ~(~ua >> count) : ua >> count);
			}
			return mc_vm_wrap(code, count >= 64 ? 0 : ua >> count);
		}
		default: break;
	}
	mc_vm_fail(vm, "`%s` on integers", mc_opcode_name(opcode));
}

/// -1, 0 or 1; 2 when unordered (nan).
static int _mc_compare(const mc_type_code_e code, const mc_word_u left, const mc_word_u right) {
	if (_mc_is_float(code)) return (left.f < right.f) ? -1 : (left.f > right.f) ? 1 : (left.f == right.f) ? 0 : 2;
	if (mc_vm_signed(code)) return (left.i < right.i) ? -1 : (left.i > right.i) ? 1 : 0;
	return (left.u < right.u) ? -1 : (left.u > right.u) ? 1 : 0;
}

static mc_word_u _mc_cast(mc_vm_s* const vm, const mc_instruction_s* const instruction, const mc_word_u value) {
	static const char* const names[] = { "s8", "s16", "s32", "s64", "u8", "u16", "u32", "u64", "int", "uint", "f32", "f64", "float", "bool", "fn", "slice", };
	const mc_type_code_e source = instruction->type, target = instruction->target;
	if (_mc_is_float(target)) {
		if (_mc_is_float(source)) return _mc_real(mc_vm_round(target, value.f));
		return _mc_real(mc_vm_round(target, mc_vm_signed(source) ? (double)value.i : (double)value.u));
	}
	if (_mc_is_float(source)) {
		if (!isfinite(value.f)) {
			mc_string_s text = {0};
			mc_format_float_value(&text, value.f, 64);
			char buffer[64];
			(void)snprintf(buffer, sizeof(buffer), "%s", mc_string_cstr(&text));
			mc_string_free(&text);
			mc_vm_fail(vm, "cannot convert %s to `%s`", buffer, names[target]);
		}
		return mc_vm_wrap(target, _mc_truncate(value.f));
	}
	return mc_vm_wrap(target, value.u);
}

// --- the stack -------------------------------------------------------------------------------

#define _mc_top(_vm) ((_vm)->stack[(_vm)->sp - 1])

static inline void _mc_push(mc_vm_s* const vm, const mc_word_u word) {
	vm->stack[vm->sp++] = word;
}

static inline mc_word_u _mc_pop(mc_vm_s* const vm) {
	return vm->stack[--vm->sp];
}

static uint64_t _mc_words(const uint64_t size) {
	return (size + 7) / 8;
}

/// pushes 'size' bytes of memory as words.
static void _mc_push_bytes(mc_vm_s* const vm, const uint8_t* const memory, const uint64_t size) {
	const size_t words = (size_t)_mc_words(size);
	if (words > 0) {
		vm->stack[vm->sp + words - 1].u = 0;
		memcpy(vm->stack + vm->sp, memory, (size_t)size);
	}
	vm->sp += words;
}

/// pushes the value of type 'code' that is in memory.
static void _mc_push_memory(mc_vm_s* const vm, const mc_type_code_e code, const uint8_t* const memory) {
	if (mc_code_slice == code) {
		_mc_push_bytes(vm, memory, 16);
		return;
	}
	_mc_push(vm, mc_vm_read(code, memory));
}

/// pops a value of type 'code' into memory.
static void _mc_pop_memory(mc_vm_s* const vm, const mc_type_code_e code, uint8_t* const memory) {
	if (mc_code_slice == code) {
		vm->sp -= 2;
		memcpy(memory, vm->stack + vm->sp, 16);
		return;
	}
	mc_vm_write(code, memory, _mc_pop(vm));
}

static uint64_t _mc_code_words(const mc_type_code_e code) {
	return (mc_code_slice == code) ? 2 : 1;
}

static mc_frame_s* _mc_frame(mc_vm_s* const vm) {
	return &vm->frames.data[vm->frames.count - 1];
}

/// stops: an index out of bounds.
mc_noreturn static void _mc_out_of_bounds(mc_vm_s* const vm, const mc_type_code_e code, const mc_word_u word, const uint64_t length, const bool slice) {
	char text[48];
	mc_vm_int_text(text, mc_vm_integer(code, word));
	if (slice) mc_vm_fail(vm, "index %s is out of bounds (length %lld)", text, (long long)length);
	mc_vm_fail(vm, "index %s is out of bounds (length %llu)", text, (unsigned long long)length);
}

/// an index of type 'code' in [0, length), or stops. negative indexes are
/// large as unsigned words, so one comparison checks both ends.
static inline uint64_t _mc_index(mc_vm_s* const vm, const mc_type_code_e code, const mc_word_u word, const uint64_t length, const bool slice) {
	if (word.u >= length) _mc_out_of_bounds(vm, code, word, length, slice);
	return word.u;
}

/// pops a location: the memory of 'size' bytes there (alive and in bounds) and its handle.
static uint8_t* _mc_pop_location(mc_vm_s* const vm, const uint64_t size, uint64_t* const handle) {
	vm->sp -= 2;
	*handle = vm->stack[vm->sp].u;
	return mc_vm_memory(vm, *handle, vm->stack[vm->sp + 1].u, size);
}

/// the bounds [low, high) of a slice of something of 'length' elements: pops them.
static void _mc_slice_bounds(mc_vm_s* const vm, const mc_instruction_s* const instruction, const uint64_t length, uint64_t* const low_out,
							 uint64_t* const high_out) {
	const int64_t flags = instruction->a;
	const bool has_high = (flags & mc_slice_high) != 0, has_low = (flags & mc_slice_low) != 0;
	const __int128 high_value = has_high ? mc_vm_integer(instruction->target, _mc_pop(vm)) : 0;
	const __int128 low = has_low ? mc_vm_integer(instruction->type, _mc_pop(vm)) : 0;
	const __int128 high = has_high ? high_value + ((flags & mc_slice_inclusive) ? 1 : 0) : (__int128)length;
	if (!(0 <= low && low <= high && high <= (__int128)length)) {
		char low_text[48], high_text[48];
		mc_vm_fail(vm, "slice %s..%s is out of bounds (length %lld)", mc_vm_int_text(low_text, low), mc_vm_int_text(high_text, high), (long long)length);
	}
	*low_out = (uint64_t)low;
	*high_out = (uint64_t)high;
}

/// pushes a slice; its offset and length must fit the slice format.
static void _mc_push_slice(mc_vm_s* const vm, const uint64_t handle, const uint64_t offset, const uint64_t length) {
	if (offset > 0xFFFFFFFFu || length > 0xFFFFFFFFu) mc_vm_fail(vm, "a slice of more than 4 GiB");
	vm->stack[vm->sp++].u = handle;
	vm->stack[vm->sp++].u = mc_slice_word(offset, length);
}

// --- calls -----------------------------------------------------------------------------------

static void _mc_call(mc_vm_s* const vm, const int64_t index) {
	if (vm->frames.count >= vm->call_depth) mc_vm_fail(vm, "stack overflow: more than %llu nested calls", (unsigned long long)vm->call_depth);
	const mc_bc_function_s* const function = &vm->module->functions.data[index];
	const mc_prepared_s* const prepared = &vm->prepared.data[index];
	if (vm->frames.count == vm->frames.capacity) mc_vector_reserve(&vm->frames, vm->frames.capacity * 2 + 16);
	mc_frame_s* const frame = &vm->frames.data[vm->frames.count];
	*frame = (mc_frame_s) { .function = index, .code = prepared->code, };
	// the byte area: from the chunk in use when it fits
	mc_chunk_s* const chunk = (vm->chunk < vm->chunks.count) ? &vm->chunks.data[vm->chunk] : NULL;
	const size_t rounded = (size_t)((function->frame + 15) / 16 * 16);
	if (chunk != NULL && chunk->size - chunk->used >= rounded) {
		frame->chunk = vm->chunk;
		frame->used = chunk->used;
		frame->base = chunk->data + chunk->used;
		chunk->used += rounded;
		memset(frame->base, 0, rounded);
	}
	else {
		frame->base = mc_vm_frame_alloc(vm, frame, function->frame);
	}
	// the arguments, first to last, from the operand stack into the frame
	const mc_word_u* argument = vm->stack + vm->sp - prepared->arguments;
	if (prepared->packed) {
		memcpy(frame->base, argument, (size_t)prepared->arguments * sizeof(mc_word_u));
	}
	else {
		for (size_t i = 0; i < function->slots.count; ++i) {
			const mc_bc_slot_s* const slot = &function->slots.data[i];
			uint8_t* const memory = frame->base + slot->offset;
			if (mc_type_code_count_ == slot->code) {
				memcpy(memory, argument, (size_t)slot->size);
				argument += _mc_words(slot->size);
			}
			else if (mc_code_slice == slot->code) {
				memcpy(memory, argument, 16);
				argument += 2;
			}
			else {
				mc_vm_write(slot->code, memory, *argument);
				argument += 1;
			}
		}
	}
	vm->sp -= (size_t)prepared->arguments;
	frame->stack_base = vm->sp;
	vm->frames.count += 1;
	if (vm->capacity - vm->sp < prepared->max_stack + 2) mc_vm_reserve(vm, prepared->max_stack + 2);
}

static void _mc_return(mc_vm_s* const vm, const int64_t words) {
	mc_frame_s* const frame = _mc_frame(vm);
	const size_t base = frame->stack_base;
	if (1 == vm->frames.count && words > 0) {
		vm->returned = true;
		vm->result = _mc_top(vm);
	}
	memmove(vm->stack + base, vm->stack + vm->sp - words, (size_t)words * sizeof(mc_word_u));
	vm->sp = base + (size_t)words;
	mc_vm_frame_release(vm, frame);
	vm->frames.count -= 1;
}

/// a call of a function value: the value sits below the arguments.
static void _mc_call_value(mc_vm_s* const vm, const int64_t words, const int64_t result) {
	const size_t slot = vm->sp - 1 - (size_t)words;
	const mc_word_u function = vm->stack[slot];
	memmove(vm->stack + slot, vm->stack + slot + 1, (size_t)words * sizeof(mc_word_u));
	vm->sp -= 1;
	const uint64_t index = function.u & ~((uint64_t)1 << 63);
	if (function.u >> 63) {
		if (index >= vm->links.count) mc_vm_fail(vm, "a call of a function value that is not one");
		const mc_bc_intrinsic_s* const intrinsic = &vm->module->intrinsics.data[index];
		if (mc_vm_intrinsic_arguments(vm, intrinsic) != (uint64_t)words || mc_vm_intrinsic_result(vm, intrinsic) != (uint64_t)result) {
			mc_vm_fail(vm, "a call of a function value of another type (the module is broken)");
		}
		mc_vm_call_plugin(vm, (int64_t)index, words);
		return;
	}
	if (index >= vm->module->functions.count) mc_vm_fail(vm, "a call of a function value that is not one");
	if (vm->prepared.data[index].arguments != (uint64_t)words || vm->module->functions.data[index].result != (uint64_t)result) {
		mc_vm_fail(vm, "a call of a function value of another type (the module is broken)");
	}
	_mc_call(vm, (int64_t)index);
}

// --- the general step --------------------------------------------------------------------------

static void _mc_step(mc_vm_s* const vm, mc_frame_s* const frame, const mc_instruction_s* const instruction) {
	uint8_t* const base = frame->base;
	const int64_t a = instruction->a;
	const mc_type_code_e code = instruction->type;
	switch (instruction->opcode) {
		case mc_op_CONST: {
			const mc_vm_constant_s* const constant = &vm->constants.data[a];
			memcpy(vm->stack + vm->sp, constant->words, constant->count * sizeof(mc_word_u));
			vm->sp += constant->count;
			break;
		}
		case mc_op_ZERO:
			memset(vm->stack + vm->sp, 0, (size_t)a * sizeof(mc_word_u));
			vm->sp += (size_t)a;
			break;
		case mc_op_POP: vm->sp -= (size_t)a; break;
		case mc_op_DUP:
			memcpy(vm->stack + vm->sp, vm->stack + vm->sp - a, (size_t)a * sizeof(mc_word_u));
			vm->sp += (size_t)a;
			break;
		case mc_op_LOAD:        _mc_push_memory(vm, code, base + a); break;
		case mc_op_LOAD_BYTES:  _mc_push_bytes(vm, base + a, (uint64_t)instruction->b); break;
		case mc_op_STORE:       _mc_pop_memory(vm, code, base + a); break;
		case mc_op_STORE_BYTES:
			vm->sp -= (size_t)_mc_words((uint64_t)instruction->b);
			memcpy(base + a, vm->stack + vm->sp, (size_t)instruction->b);
			break;
		case mc_op_LOC_LOCAL:
			vm->stack[vm->sp++].u = mc_vm_frame_handle(vm, frame);
			vm->stack[vm->sp++].u = (uint64_t)a;
			break;
		case mc_op_LOC_CONST:
			vm->stack[vm->sp++].u = mc_vm_constant_handle(vm, a);
			vm->stack[vm->sp++].u = 0;
			break;
		case mc_op_LOC_FIELD: _mc_top(vm).u += (uint64_t)a; break;
		case mc_op_LOC_INDEX: {
			const uint64_t index = _mc_index(vm, code, _mc_pop(vm), (uint64_t)instruction->b, false);
			_mc_top(vm).u += index * (uint64_t)a;
			break;
		}
		case mc_op_LOC_INDEX_SLICE: {
			const mc_word_u index_word = _mc_pop(vm);
			const uint64_t slice = _mc_top(vm).u;
			const uint64_t handle = vm->stack[vm->sp - 2].u;
			const uint64_t index = _mc_index(vm, code, index_word, mc_slice_length(slice), true);
			const uint64_t offset = mc_slice_offset(slice) + index * (uint64_t)a;
			(void)mc_vm_memory(vm, handle, offset, (uint64_t)a);
			_mc_top(vm).u = offset;
			break;
		}
		case mc_op_TEMP_LOC:
		case mc_op_TEMP_LOC_BYTES:
			if (mc_op_TEMP_LOC == instruction->opcode) {
				_mc_pop_memory(vm, code, base + a);
			}
			else {
				vm->sp -= (size_t)_mc_words((uint64_t)instruction->b);
				memcpy(base + a, vm->stack + vm->sp, (size_t)instruction->b);
			}
			vm->stack[vm->sp++].u = mc_vm_frame_handle(vm, frame);
			vm->stack[vm->sp++].u = (uint64_t)a;
			break;
		case mc_op_LOAD_LOC: {
			uint64_t handle = 0;
			_mc_push_memory(vm, code, _mc_pop_location(vm, mc_vm_code_size(code), &handle));
			break;
		}
		case mc_op_LOAD_LOC_BYTES: {
			uint64_t handle = 0;
			_mc_push_bytes(vm, _mc_pop_location(vm, (uint64_t)a, &handle), (uint64_t)a);
			break;
		}
		case mc_op_STORE_LOC:
		case mc_op_STORE_LOC_BYTES: {
			const bool bytes = mc_op_STORE_LOC_BYTES == instruction->opcode;
			const uint64_t size = bytes ? (uint64_t)a : mc_vm_code_size(code);
			const size_t value = vm->sp - (bytes ? (size_t)_mc_words(size) : (size_t)_mc_code_words(code));
			vm->sp = value;
			uint64_t handle = 0;
			uint8_t* const memory = _mc_pop_location(vm, size, &handle);
			if (handle & mc_handle_read_only) mc_vm_fail(vm, "write through a read-only slice");
			if (bytes || mc_code_slice == code) memmove(memory, vm->stack + value, (size_t)size);
			else mc_vm_write(code, memory, vm->stack[value]);
			break;
		}
		case mc_op_FIELD:
		case mc_op_FIELD_BYTES: {
			const size_t start = vm->sp - (size_t)instruction->b;
			const uint8_t* const memory = (const uint8_t*)(vm->stack + start) + a;
			if (mc_op_FIELD == instruction->opcode && code != mc_code_slice) {
				const mc_word_u word = mc_vm_read(code, memory);
				vm->sp = start;
				_mc_push(vm, word);
				break;
			}
			const uint64_t size = (mc_op_FIELD == instruction->opcode) ? 16 : (uint64_t)instruction->c;
			const size_t field_words = (size_t)_mc_words(size);
			memmove(vm->stack + start, memory, (size_t)size);
			if (size % 8 != 0) memset((uint8_t*)(vm->stack + start) + size, 0, (size_t)(field_words * 8 - size));
			vm->sp = start + field_words;
			break;
		}
		case mc_op_SET_FIELD:
		case mc_op_SET_FIELD_BYTES: {
			const bool bytes = mc_op_SET_FIELD_BYTES == instruction->opcode;
			const size_t value = vm->sp - (bytes ? (size_t)_mc_words((uint64_t)instruction->c) : (size_t)_mc_code_words(code));
			uint8_t* const memory = (uint8_t*)(vm->stack + value - instruction->b) + a;
			if (bytes) memmove(memory, vm->stack + value, (size_t)instruction->c);
			else if (mc_code_slice == code) memmove(memory, vm->stack + value, 16);
			else mc_vm_write(code, memory, vm->stack[value]);
			vm->sp = value;
			break;
		}
		case mc_op_MAKE_ARRAY:
		case mc_op_MAKE_ARRAY_BYTES: {
			const bool bytes = mc_op_MAKE_ARRAY_BYTES == instruction->opcode;
			const uint64_t size = bytes ? (uint64_t)instruction->b : mc_vm_code_size(code);
			const size_t element_words = bytes ? (size_t)_mc_words(size) : (size_t)_mc_code_words(code);
			const size_t start = vm->sp - (size_t)a * element_words;
			mc_string_s* const scratch = &vm->bytes;
			mc_string_clear(scratch);
			mc_string_append_repeat(scratch, '\0', (size_t)(size * (uint64_t)a) + 8);
			uint8_t* const packed = (uint8_t*)scratch->data;
			for (int64_t i = 0; i < a; ++i) {
				const mc_word_u* const item = vm->stack + start + (size_t)i * element_words;
				uint8_t* const memory = packed + (uint64_t)i * size;
				if (bytes || mc_code_slice == code) memcpy(memory, item, (size_t)size);
				else mc_vm_write(code, memory, *item);
			}
			vm->sp = start;
			_mc_push_bytes(vm, packed, size * (uint64_t)a);
			break;
		}
		case mc_op_INDEX:
		case mc_op_INDEX_BYTES: {
			const bool bytes = mc_op_INDEX_BYTES == instruction->opcode;
			const mc_type_code_e index_code = bytes ? code : instruction->target;
			const uint64_t index = _mc_index(vm, index_code, _mc_pop(vm), (uint64_t)a, false);
			const size_t start = vm->sp - (size_t)instruction->b;
			const uint64_t size = bytes ? (uint64_t)instruction->c : mc_vm_code_size(code);
			const uint8_t* const memory = (const uint8_t*)(vm->stack + start) + index * size;
			if (!bytes && code != mc_code_slice) {
				const mc_word_u word = mc_vm_read(code, memory);
				vm->sp = start;
				_mc_push(vm, word);
				break;
			}
			const size_t words = (size_t)_mc_words(size);
			memmove(vm->stack + start, memory, (size_t)size);
			if (size % 8 != 0) memset((uint8_t*)(vm->stack + start) + size, 0, (size_t)(words * 8 - size));
			vm->sp = start + words;
			break;
		}
		case mc_op_INDEX_SLICE:
		case mc_op_INDEX_SLICE_BYTES: {
			const bool bytes = mc_op_INDEX_SLICE_BYTES == instruction->opcode;
			const mc_type_code_e index_code = bytes ? code : instruction->target;
			const mc_word_u index_word = _mc_pop(vm);
			vm->sp -= 2;
			const uint64_t handle = vm->stack[vm->sp].u, slice = vm->stack[vm->sp + 1].u;
			const uint64_t index = _mc_index(vm, index_code, index_word, mc_slice_length(slice), true);
			const uint64_t size = bytes ? (uint64_t)a : mc_vm_code_size(code);
			const uint8_t* const memory = mc_vm_memory(vm, handle, mc_slice_offset(slice) + index * size, size);
			if (bytes) _mc_push_bytes(vm, memory, size);
			else _mc_push_memory(vm, code, memory);
			break;
		}
		case mc_op_LEN_SLICE: {
			const uint64_t slice = _mc_pop(vm).u;
			_mc_top(vm).u = mc_slice_length(slice);
			break;
		}
		case mc_op_SLICE_IS_NULL: {
			const uint64_t slice = _mc_pop(vm).u;
			_mc_top(vm).u = 0 == mc_slice_length(slice);
			break;
		}
		case mc_op_SLICE_LOC: {
			uint64_t low = 0, high = 0;
			_mc_slice_bounds(vm, instruction, (uint64_t)instruction->b, &low, &high);
			vm->sp -= 2;
			uint64_t handle = vm->stack[vm->sp].u;
			const uint64_t offset = vm->stack[vm->sp + 1].u;
			if (a & mc_slice_read_only) handle |= mc_handle_read_only;
			_mc_push_slice(vm, handle, offset + low * (uint64_t)instruction->c, high - low);
			break;
		}
		case mc_op_SLICE_SLICE: {
			const size_t below = vm->sp - (((a & mc_slice_low) != 0) ? 1 : 0) - (((a & mc_slice_high) != 0) ? 1 : 0);
			const uint64_t handle = vm->stack[below - 2].u, slice = vm->stack[below - 1].u;
			uint64_t low = 0, high = 0;
			_mc_slice_bounds(vm, instruction, mc_slice_length(slice), &low, &high);
			vm->sp -= 2;
			if (0 == (handle & ~mc_handle_read_only)) {
				_mc_push_slice(vm, 0, 0, 0);
				break;
			}
			(void)mc_vm_resolve(vm, handle);
			_mc_push_slice(vm, handle, mc_slice_offset(slice) + low * (uint64_t)instruction->b, high - low);
			break;
		}
		case mc_op_JUMP: frame->pc = a; break;
		case mc_op_JUMP_IF_FALSE: if (0 == _mc_pop(vm).u) frame->pc = a; break;
		case mc_op_JUMP_IF_TRUE:  if (0 != _mc_pop(vm).u) frame->pc = a; break;
		case mc_op_NOT: _mc_top(vm).u = 0 == _mc_top(vm).u; break;
		case mc_op_RANGE_INIT: {
			const mc_word_u end = _mc_pop(vm), start = _mc_pop(vm);
			const bool up = mc_vm_signed(code) ? end.i >= start.i : end.u >= start.u;
			const uint64_t step = up ? 1 : UINT64_MAX;
			const uint64_t state[3] = { start.u, step, instruction->b ? end.u + step : end.u, };
			memcpy(base + a, state, sizeof(state));
			break;
		}
		case mc_op_RANGE_TEST: {
			uint64_t state[3];
			memcpy(state, base + a, sizeof(state));
			if (state[0] == state[2]) frame->pc = instruction->b;
			break;
		}
		case mc_op_RANGE_STEP: {
			uint64_t state[2];
			memcpy(state, base + a, sizeof(state));
			state[0] += state[1];
			memcpy(base + a, state, sizeof(state[0]));
			break;
		}
		case mc_op_CALL:           _mc_call(vm, a); break;
		case mc_op_CALL_VALUE:     _mc_call_value(vm, a, instruction->b); break;
		case mc_op_CALL_INTRINSIC: mc_vm_call_plugin(vm, a, instruction->b); break;
		case mc_op_FUNCTION:       _mc_push(vm, (mc_word_u) { .u = (uint64_t)a, }); break;
		case mc_op_INTRINSIC:      _mc_push(vm, (mc_word_u) { .u = (uint64_t)a | ((uint64_t)1 << 63), }); break;
		case mc_op_RETURN:         _mc_return(vm, a); break;
		case mc_op_ASSERT_FAIL: {
			const mc_bc_constant_s* const message = &vm->module->constants.data[a];
			mc_vm_fail(vm, "%s", mc_constant_text == message->kind ? message->text : "assertion failed");
		}
		case mc_op_EQ: case mc_op_NE: case mc_op_LT: case mc_op_GT: case mc_op_LE: case mc_op_GE: {
			const mc_word_u right = _mc_pop(vm);
			const int order = _mc_compare(code, _mc_top(vm), right);
			bool result = false;
			switch (instruction->opcode) {
				case mc_op_EQ: result = 0 == order; break;
				case mc_op_NE: result = 0 != order; break;
				case mc_op_LT: result = -1 == order; break;
				case mc_op_GT: result = 1 == order; break;
				case mc_op_LE: result = -1 == order || 0 == order; break;
				default:       result = 1 == order || 0 == order; break;
			}
			_mc_top(vm) = _mc_boolean(result);
			break;
		}
		case mc_op_ADD: case mc_op_SUB: case mc_op_MUL: case mc_op_DIV: case mc_op_MOD:
		case mc_op_AND: case mc_op_OR: case mc_op_XOR: case mc_op_SHL: case mc_op_SHR: {
			const mc_word_u right = _mc_pop(vm);
			_mc_top(vm) = _mc_arithmetic(vm, instruction, _mc_top(vm), right);
			break;
		}
		case mc_op_NEG:
			if (_mc_is_float(code)) _mc_top(vm) = _mc_real(mc_vm_round(code, -_mc_top(vm).f));
			else _mc_top(vm) = mc_vm_wrap(code, (uint64_t)0 - _mc_top(vm).u);
			break;
		case mc_op_BITNOT: _mc_top(vm) = mc_vm_wrap(code, ~_mc_top(vm).u); break;
		case mc_op_CAST: _mc_top(vm) = _mc_cast(vm, instruction, _mc_top(vm)); break;
		case mc_op_count_: mc_vm_fail(vm, "an invalid instruction");
	}
}

// --- the loop --------------------------------------------------------------------------------

static void _mc_loop(mc_vm_s* const vm) {
	const uint64_t stop_at = (vm->options->limit > 0) ? vm->options->limit : UINT64_MAX;
	uint64_t steps = vm->steps;
	mc_frame_s* frame = _mc_frame(vm);
	const mc_exec_s* code = frame->code;
	int64_t pc = frame->pc;
	uint8_t* base = frame->base;
	mc_word_u* sp = vm->stack + vm->sp;
	// the state lives in locals; these move it to and from the machine around general steps
#define _mc_save() do { vm->sp = (size_t)(sp - vm->stack); frame->pc = pc; vm->steps = steps; } while (0)
#define _mc_load() do {                                                        \
		if (0 == vm->frames.count) return;                                     \
		frame = _mc_frame(vm);                                                 \
		code = frame->code;                                                    \
		pc = frame->pc;                                                        \
		base = frame->base;                                                    \
		sp = vm->stack + vm->sp;                                               \
	} while (0)
	for (;;) {
		if (steps >= stop_at) {
			_mc_save();
			mc_vm_fail(vm, "stopped after %llu instructions", (unsigned long long)stop_at);
		}
		const mc_exec_s* const exec = &code[pc++];
		steps += 1;
		switch ((_mc_x_e)exec->x) {
			case _mc_x_const1:   (sp++)->u = exec->word; break;
			case _mc_x_load8:    memcpy(sp++, base + exec->a, 8); break;
			case _mc_x_store8:   memcpy(base + exec->a, --sp, 8); break;
			case _mc_x_load16:   memcpy(sp, base + exec->a, 16); sp += 2; break;
			case _mc_x_store16:  sp -= 2; memcpy(base + exec->a, sp, 16); break;
			case _mc_x_dup1:     *sp = sp[-1]; ++sp; break;
			case _mc_x_pop:      sp -= exec->a; break;
			case _mc_x_add:      --sp; sp[-1].u += sp->u; break;
			case _mc_x_sub:      --sp; sp[-1].u -= sp->u; break;
			case _mc_x_mul:      --sp; sp[-1].u *= sp->u; break;
			case _mc_x_fadd:     --sp; sp[-1].f += sp->f; break;
			case _mc_x_fsub:     --sp; sp[-1].f -= sp->f; break;
			case _mc_x_fmul:     --sp; sp[-1].f *= sp->f; break;
			case _mc_x_eq:       --sp; sp[-1].u = sp[-1].u == sp->u; break;
			case _mc_x_ne:       --sp; sp[-1].u = sp[-1].u != sp->u; break;
			case _mc_x_slt:      --sp; sp[-1].u = sp[-1].i < sp->i; break;
			case _mc_x_sle:      --sp; sp[-1].u = sp[-1].i <= sp->i; break;
			case _mc_x_sgt:      --sp; sp[-1].u = sp[-1].i > sp->i; break;
			case _mc_x_sge:      --sp; sp[-1].u = sp[-1].i >= sp->i; break;
			case _mc_x_ult:      --sp; sp[-1].u = sp[-1].u < sp->u; break;
			case _mc_x_ule:      --sp; sp[-1].u = sp[-1].u <= sp->u; break;
			case _mc_x_ugt:      --sp; sp[-1].u = sp[-1].u > sp->u; break;
			case _mc_x_uge:      --sp; sp[-1].u = sp[-1].u >= sp->u; break;
			case _mc_x_flt:      --sp; sp[-1].u = sp[-1].f < sp->f; break;
			case _mc_x_fle:      --sp; sp[-1].u = sp[-1].f <= sp->f; break;
			case _mc_x_fgt:      --sp; sp[-1].u = sp[-1].f > sp->f; break;
			case _mc_x_fge:      --sp; sp[-1].u = sp[-1].f >= sp->f; break;
			case _mc_x_jump:       pc = exec->a; break;
			case _mc_x_jump_false: if (0 == (--sp)->u) pc = exec->a; break;
			case _mc_x_jump_true:  if (0 != (--sp)->u) pc = exec->a; break;
			case _mc_x_range_test: {
				uint64_t current, stop;
				memcpy(&current, base + exec->a, 8);
				memcpy(&stop, base + exec->a + 16, 8);
				if (current == stop) pc = exec->b;
				break;
			}
			case _mc_x_range_step: {
				uint64_t state[2];
				memcpy(state, base + exec->a, sizeof(state));
				state[0] += state[1];
				memcpy(base + exec->a, state, 8);
				break;
			}
			case _mc_x_index_slice8:
			case _mc_x_index_slice1: {
				const mc_word_u index = sp[-1];
				const uint64_t handle = sp[-3].u, slice = sp[-2].u;
				if (index.u >= mc_slice_length(slice)) {
					_mc_save();
					_mc_out_of_bounds(vm, exec->instruction->target, index, mc_slice_length(slice), true);
				}
				sp -= 3;
				if (_mc_x_index_slice8 == exec->x) {
					_mc_save();
					memcpy(sp, mc_vm_memory(vm, handle, mc_slice_offset(slice) + index.u * 8, 8), 8);
				}
				else {
					_mc_save();
					sp->u = *mc_vm_memory(vm, handle, mc_slice_offset(slice) + index.u, 1);
					if (mc_code_bool == exec->instruction->type) sp->u = sp->u != 0;
				}
				sp += 1;
				break;
			}
			case _mc_x_loc_index_slice: {
				const mc_word_u index = sp[-1];
				const uint64_t handle = sp[-3].u, slice = sp[-2].u;
				if (index.u >= mc_slice_length(slice)) {
					_mc_save();
					_mc_out_of_bounds(vm, exec->instruction->type, index, mc_slice_length(slice), true);
				}
				const uint64_t offset = mc_slice_offset(slice) + index.u * (uint64_t)exec->a;
				_mc_save();
				(void)mc_vm_memory(vm, handle, offset, (uint64_t)exec->a);
				sp -= 1;
				sp[-1].u = offset;
				break;
			}
			case _mc_x_load_loc8: {
				_mc_save();
				memcpy(sp - 2, mc_vm_memory(vm, sp[-2].u, sp[-1].u, 8), 8);
				sp -= 1;
				break;
			}
			case _mc_x_store_loc8: {
				const uint64_t handle = sp[-3].u;
				_mc_save();
				uint8_t* const memory = mc_vm_memory(vm, handle, sp[-2].u, 8);
				if (handle & mc_handle_read_only) mc_vm_fail(vm, "write through a read-only slice");
				memcpy(memory, sp - 1, 8);
				sp -= 3;
				break;
			}
			case _mc_x_load:  *sp++ = mc_vm_read(exec->instruction->type, base + exec->a); break;
			case _mc_x_store: mc_vm_write(exec->instruction->type, base + exec->a, *--sp); break;
			case _mc_x_load_bytes: {
				const size_t words = (size_t)((exec->b + 7) / 8);
				sp[words - 1].u = 0;
				memcpy(sp, base + exec->a, (size_t)exec->b);
				sp += words;
				break;
			}
			case _mc_x_store_bytes:
				sp -= (exec->b + 7) / 8;
				memcpy(base + exec->a, sp, (size_t)exec->b);
				break;
			case _mc_x_zero:
				memset(sp, 0, (size_t)exec->a * sizeof(mc_word_u));
				sp += exec->a;
				break;
			case _mc_x_const: {
				const mc_vm_constant_s* const constant = &vm->constants.data[exec->a];
				memcpy(sp, constant->words, constant->count * sizeof(mc_word_u));
				sp += constant->count;
				break;
			}
			case _mc_x_f32add: --sp; sp[-1].f = (double)(float)(sp[-1].f + sp->f); break;
			case _mc_x_f32sub: --sp; sp[-1].f = (double)(float)(sp[-1].f - sp->f); break;
			case _mc_x_f32mul: --sp; sp[-1].f = (double)(float)(sp[-1].f * sp->f); break;
			case _mc_x_field: {
				mc_word_u* const start = sp - exec->b;
				*start = mc_vm_read(exec->instruction->type, (const uint8_t*)start + exec->a);
				sp = start + 1;
				break;
			}
			case _mc_x_set_field: {
				--sp;
				mc_vm_write(exec->instruction->type, (uint8_t*)(sp - exec->b) + exec->a, *sp);
				break;
			}
			case _mc_x_not: sp[-1].u = 0 == sp[-1].u; break;
			case _mc_x_load_loc: {
				const mc_type_code_e type = exec->instruction->type;
				_mc_save();
				sp[-2] = mc_vm_read(type, mc_vm_memory(vm, sp[-2].u, sp[-1].u, mc_vm_code_size(type)));
				sp -= 1;
				break;
			}
			case _mc_x_store_loc: {
				const mc_type_code_e type = exec->instruction->type;
				const uint64_t handle = sp[-3].u;
				_mc_save();
				uint8_t* const memory = mc_vm_memory(vm, handle, sp[-2].u, mc_vm_code_size(type));
				if (handle & mc_handle_read_only) mc_vm_fail(vm, "write through a read-only slice");
				mc_vm_write(type, memory, sp[-1]);
				sp -= 3;
				break;
			}
			case _mc_x_index_slice: {
				const mc_type_code_e type = exec->instruction->type;
				const mc_word_u index = sp[-1];
				const uint64_t handle = sp[-3].u, slice = sp[-2].u;
				_mc_save();
				if (index.u >= mc_slice_length(slice)) _mc_out_of_bounds(vm, exec->instruction->target, index, mc_slice_length(slice), true);
				const uint64_t size = mc_vm_code_size(type);
				sp -= 3;
				*sp++ = mc_vm_read(type, mc_vm_memory(vm, handle, mc_slice_offset(slice) + index.u * size, size));
				break;
			}
			case _mc_x_call:
				_mc_save();
				_mc_call(vm, exec->a);
				_mc_load();
				break;
			case _mc_x_return:
				_mc_save();
				_mc_return(vm, exec->a);
				_mc_load();
				break;
			case _mc_x_step:
				_mc_save();
				_mc_step(vm, frame, exec->instruction);
				_mc_load();
				break;
		}
	}
#undef _mc_save
#undef _mc_load
}

// --- loading -------------------------------------------------------------------------------

/// the fast operation for an instruction (the general step when there is none).
static uint8_t _mc_decode(const mc_vm_s* const vm, const mc_instruction_s* const instruction, uint64_t* const word) {
	const mc_type_code_e code = instruction->type;
	const bool wide = mc_code_s64 == code || mc_code_u64 == code || mc_code_int == code || mc_code_uint == code;
	const bool double_ = mc_code_f64 == code || mc_code_float == code;
	const bool integer = code <= mc_code_uint || mc_code_bool == code || mc_code_fn == code;
	const bool signed_ = mc_vm_signed(code), unsigned_ = integer && !signed_;
	if (mc_op_CONST == instruction->opcode) {
		const mc_vm_constant_s* const constant = &vm->constants.data[instruction->a];
		if (constant->count != 1) return _mc_x_const;
		*word = constant->words[0].u;
		return _mc_x_const1;
	}
	switch (instruction->opcode) {
		case mc_op_ZERO:  return _mc_x_zero;
		case mc_op_LOAD:  return (wide || double_ || mc_code_fn == code) ? _mc_x_load8 : (mc_code_slice == code) ? _mc_x_load16 : _mc_x_load;
		case mc_op_STORE: return (wide || double_ || mc_code_fn == code) ? _mc_x_store8 : (mc_code_slice == code) ? _mc_x_store16 : _mc_x_store;
		case mc_op_LOAD_BYTES:  return (instruction->b > 0) ? _mc_x_load_bytes : _mc_x_step;
		case mc_op_STORE_BYTES: return _mc_x_store_bytes;
		case mc_op_FIELD:       return (code != mc_code_slice) ? _mc_x_field : _mc_x_step;
		case mc_op_SET_FIELD:   return (code != mc_code_slice) ? _mc_x_set_field : _mc_x_step;
		case mc_op_NOT:         return _mc_x_not;
		case mc_op_DUP:   return (1 == instruction->a) ? _mc_x_dup1 : _mc_x_step;
		case mc_op_POP:   return _mc_x_pop;
		case mc_op_ADD:   return wide ? _mc_x_add : double_ ? _mc_x_fadd : (mc_code_f32 == code) ? _mc_x_f32add : _mc_x_step;
		case mc_op_SUB:   return wide ? _mc_x_sub : double_ ? _mc_x_fsub : (mc_code_f32 == code) ? _mc_x_f32sub : _mc_x_step;
		case mc_op_MUL:   return wide ? _mc_x_mul : double_ ? _mc_x_fmul : (mc_code_f32 == code) ? _mc_x_f32mul : _mc_x_step;
		case mc_op_EQ:    return integer ? _mc_x_eq : _mc_x_step;
		case mc_op_NE:    return integer ? _mc_x_ne : _mc_x_step;
		case mc_op_LT:    return signed_ ? _mc_x_slt : unsigned_ ? _mc_x_ult : _mc_x_flt;
		case mc_op_LE:    return signed_ ? _mc_x_sle : unsigned_ ? _mc_x_ule : _mc_x_fle;
		case mc_op_GT:    return signed_ ? _mc_x_sgt : unsigned_ ? _mc_x_ugt : _mc_x_fgt;
		case mc_op_GE:    return signed_ ? _mc_x_sge : unsigned_ ? _mc_x_uge : _mc_x_fge;
		case mc_op_JUMP:  return _mc_x_jump;
		case mc_op_JUMP_IF_FALSE: return _mc_x_jump_false;
		case mc_op_JUMP_IF_TRUE:  return _mc_x_jump_true;
		case mc_op_RANGE_TEST:    return _mc_x_range_test;
		case mc_op_RANGE_STEP:    return _mc_x_range_step;
		case mc_op_INDEX_SLICE:
			return (wide || double_ || mc_code_fn == code) ? _mc_x_index_slice8 : (mc_code_u8 == code || mc_code_bool == code) ? _mc_x_index_slice1
				: (mc_code_slice == code) ? _mc_x_step : _mc_x_index_slice;
		case mc_op_LOC_INDEX_SLICE: return _mc_x_loc_index_slice;
		case mc_op_LOAD_LOC:  return (wide || double_ || mc_code_fn == code) ? _mc_x_load_loc8 : (mc_code_slice == code) ? _mc_x_step : _mc_x_load_loc;
		case mc_op_STORE_LOC: return (wide || double_ || mc_code_fn == code) ? _mc_x_store_loc8 : (mc_code_slice == code) ? _mc_x_step : _mc_x_store_loc;
		case mc_op_CALL:   return _mc_x_call;
		case mc_op_RETURN: return _mc_x_return;
		default:           return _mc_x_step;
	}
}

/// what an instruction takes from and leaves on the operand stack (words);
/// false when its operands are out of range.
static bool _mc_effect(const mc_vm_s* const vm, const mc_bc_function_s* const function, const mc_instruction_s* const instruction,
					   uint64_t* const pop, uint64_t* const push) {
	const int64_t a = instruction->a, b = instruction->b, c = instruction->c;
	const mc_type_code_e code = instruction->type;
	const int64_t limit = (int64_t)1 << 40;
	const uint64_t frame = function->frame;
	const uint64_t words = _mc_code_words(code), size = mc_vm_code_size(code);
	*pop = 0;
	*push = 0;
	if (a < -limit || a > limit || b < -limit || b > limit || c < -limit || c > limit) return false;
#define _mc_in_frame(_offset, _size) ((_offset) >= 0 && (_size) >= 0 && (uint64_t)(_offset) + (uint64_t)(_size) <= frame)
	switch (instruction->opcode) {
		case mc_op_CONST:      *push = vm->constants.data[a].count; return true;
		case mc_op_ZERO:       *push = (uint64_t)a; return a >= 0;
		case mc_op_POP:        *pop = (uint64_t)a; return a >= 0;
		case mc_op_DUP:        *pop = (uint64_t)a; *push = 2 * (uint64_t)a; return a >= 0;
		case mc_op_LOAD:       *push = words; return _mc_in_frame(a, (int64_t)size);
		case mc_op_LOAD_BYTES: *push = _mc_words((uint64_t)b); return _mc_in_frame(a, b);
		case mc_op_STORE:      *pop = words; return _mc_in_frame(a, (int64_t)size);
		case mc_op_STORE_BYTES: *pop = _mc_words((uint64_t)b); return _mc_in_frame(a, b);
		case mc_op_LOC_LOCAL:  *push = 2; return _mc_in_frame(a, 0);
		case mc_op_LOC_CONST:  *push = 2; return true;
		case mc_op_LOC_FIELD:  *pop = 2; *push = 2; return a >= 0;
		case mc_op_LOC_INDEX:  *pop = 3; *push = 2; return a >= 0 && b >= 0;
		case mc_op_LOC_INDEX_SLICE: *pop = 3; *push = 2; return a >= 0;
		case mc_op_TEMP_LOC:   *pop = words; *push = 2; return _mc_in_frame(a, (int64_t)size);
		case mc_op_TEMP_LOC_BYTES: *pop = _mc_words((uint64_t)b); *push = 2; return _mc_in_frame(a, b);
		case mc_op_LOAD_LOC:   *pop = 2; *push = words; return true;
		case mc_op_LOAD_LOC_BYTES: *pop = 2; *push = _mc_words((uint64_t)a); return a >= 0;
		case mc_op_STORE_LOC:  *pop = 2 + words; return true;
		case mc_op_STORE_LOC_BYTES: *pop = 2 + _mc_words((uint64_t)a); return a >= 0;
		case mc_op_FIELD:      *pop = (uint64_t)b; *push = words; return a >= 0 && b >= 0 && (uint64_t)a + size <= (uint64_t)b * 8;
		case mc_op_FIELD_BYTES: *pop = (uint64_t)b; *push = _mc_words((uint64_t)c); return a >= 0 && b >= 0 && c >= 0 && a + c <= b * 8;
		case mc_op_SET_FIELD:  *pop = (uint64_t)b + words; *push = (uint64_t)b; return a >= 0 && b >= 0 && (uint64_t)a + size <= (uint64_t)b * 8;
		case mc_op_SET_FIELD_BYTES:
			*pop = (uint64_t)b + _mc_words((uint64_t)c);
			*push = (uint64_t)b;
			return a >= 0 && b >= 0 && c >= 0 && a + c <= b * 8;
		case mc_op_MAKE_ARRAY:
			*pop = (uint64_t)a * words;
			*push = _mc_words((uint64_t)a * size);
			return a >= 0 && a <= ((int64_t)1 << 28);
		case mc_op_MAKE_ARRAY_BYTES:
			*pop = (uint64_t)a * _mc_words((uint64_t)b);
			*push = _mc_words((uint64_t)a * (uint64_t)b);
			return a >= 0 && a <= ((int64_t)1 << 28) && b >= 0 && b <= ((int64_t)1 << 32);
		case mc_op_INDEX:       *pop = (uint64_t)b + 1; *push = words; return a >= 0 && b >= 0 && (uint64_t)a * size <= (uint64_t)b * 8;
		case mc_op_INDEX_BYTES:
			*pop = (uint64_t)b + 1;
			*push = _mc_words((uint64_t)c);
			return a >= 0 && b >= 0 && c >= 0 && c <= ((int64_t)1 << 32) && (uint64_t)a * (uint64_t)c <= (uint64_t)b * 8;
		case mc_op_INDEX_SLICE: *pop = 3; *push = words; return true;
		case mc_op_INDEX_SLICE_BYTES: *pop = 3; *push = _mc_words((uint64_t)a); return a >= 0;
		case mc_op_LEN_SLICE:
		case mc_op_SLICE_IS_NULL: *pop = 2; *push = 1; return true;
		case mc_op_SLICE_LOC:
		case mc_op_SLICE_SLICE: {
			*pop = 2 + ((a & mc_slice_low) ? 1 : 0) + ((a & mc_slice_high) ? 1 : 0);
			*push = 2;
			return b >= 0 && c >= 0;
		}
		case mc_op_JUMP:      return true;
		case mc_op_JUMP_IF_FALSE:
		case mc_op_JUMP_IF_TRUE: *pop = 1; return true;
		case mc_op_NOT:       *pop = 1; *push = 1; return true;
		case mc_op_RANGE_INIT: *pop = 2; return _mc_in_frame(a, 24);
		case mc_op_RANGE_TEST:
		case mc_op_RANGE_STEP: return _mc_in_frame(a, 24);
		case mc_op_CALL:
			*pop = vm->prepared.data[a].arguments;
			*push = vm->module->functions.data[a].result;
			return true;
		case mc_op_CALL_VALUE: *pop = (uint64_t)a + 1; *push = (uint64_t)b; return a >= 0 && b >= 0;
		case mc_op_CALL_INTRINSIC: {
			const mc_bc_intrinsic_s* const intrinsic = &vm->module->intrinsics.data[a];
			*pop = (uint64_t)b;
			*push = mc_vm_intrinsic_result(vm, intrinsic);
			return b >= 0 && mc_vm_intrinsic_arguments(vm, intrinsic) == (uint64_t)b && *push <= 2;
		}
		case mc_op_FUNCTION:
		case mc_op_INTRINSIC: *push = 1; return true;
		case mc_op_RETURN:    *pop = (uint64_t)a; return a >= 0 && (uint64_t)a == function->result;
		case mc_op_ASSERT_FAIL: return true;
		case mc_op_ADD: case mc_op_SUB: case mc_op_MUL: case mc_op_DIV: case mc_op_MOD: case mc_op_AND: case mc_op_OR: case mc_op_XOR:
		case mc_op_SHL: case mc_op_SHR: case mc_op_EQ: case mc_op_NE: case mc_op_LT: case mc_op_GT: case mc_op_LE: case mc_op_GE:
			*pop = 2;
			*push = 1;
			return code != mc_code_slice;
		case mc_op_NEG: case mc_op_BITNOT: case mc_op_CAST:
			*pop = 1;
			*push = 1;
			return code != mc_code_slice && instruction->target != mc_code_slice;
		case mc_op_count_: return false;
	}
#undef _mc_in_frame
	return false;
}

/// checks a function (see the top of this file) and decodes it.
static void _mc_verify(mc_vm_s* const vm, const int64_t index) {
	const mc_bc_function_s* const function = &vm->module->functions.data[index];
	mc_prepared_s* const prepared = &vm->prepared.data[index];
	const size_t count = function->code.count;
	if (0 == count) mc_vm_fail(vm, "the module is broken (`%s` has no code)", function->name);
	int64_t* const depth = mc_malloc(count * sizeof(int64_t));
	for (size_t i = 0; i < count; ++i) depth[i] = -1;
	mc_vector_of(size_t) work = {0};
	depth[0] = 0;
	mc_vector_push(&work, (size_t)0);
	uint64_t highest = 0;
	const char* problem = NULL;
	while (work.count > 0 && NULL == problem) {
		const size_t at = mc_vector_pop(&work);
		const mc_instruction_s* const instruction = &function->code.data[at];
		uint64_t pop = 0, push = 0;
		if (!_mc_effect(vm, function, instruction, &pop, &push)) {
			problem = "an operand is out of range";
			break;
		}
		const uint64_t before = (uint64_t)depth[at];
		if (before < pop) {
			problem = "the operand stack underflows";
			break;
		}
		const uint64_t after = before - pop + push;
		if (after > highest) highest = after;
		if (before > highest) highest = before;
		if (after > ((uint64_t)1 << 30)) {
			problem = "the operand stack grows too large";
			break;
		}
		if (mc_op_RETURN == instruction->opcode && before != pop) {
			problem = "a return leaves values on the stack";
			break;
		}
		size_t next[2];
		size_t successors = 0;
		const mc_opcode_e opcode = instruction->opcode;
		if (mc_op_JUMP == opcode || mc_op_JUMP_IF_FALSE == opcode || mc_op_JUMP_IF_TRUE == opcode) next[successors++] = (size_t)instruction->a;
		if (mc_op_RANGE_TEST == opcode) next[successors++] = (size_t)instruction->b;
		if (opcode != mc_op_JUMP && opcode != mc_op_RETURN && opcode != mc_op_ASSERT_FAIL) next[successors++] = at + 1;
		for (size_t s = 0; s < successors; ++s) {
			if (next[s] >= count) {
				problem = "the code runs past its end";
				break;
			}
			if (depth[next[s]] < 0) {
				depth[next[s]] = (int64_t)after;
				mc_vector_push(&work, next[s]);
			}
			else if ((uint64_t)depth[next[s]] != after) {
				problem = "the operand stack differs between paths";
				break;
			}
		}
	}
	mc_vector_free(&work);
	mc_free(depth);
	if (problem != NULL) mc_vm_fail(vm, "the module is broken (`%s`: %s)", function->name, problem);
	prepared->max_stack = (size_t)highest;
	prepared->code = mc_malloc(count * sizeof(mc_exec_s));
	for (size_t i = 0; i < count; ++i) {
		const mc_instruction_s* const instruction = &function->code.data[i];
		mc_exec_s* const exec = &prepared->code[i];
		*exec = (mc_exec_s) { .instruction = instruction, .a = instruction->a, .b = instruction->b, };
		exec->x = _mc_decode(vm, instruction, &exec->word);
	}
}

static void _mc_load(mc_vm_s* const vm) {
	const mc_module_bc_s* const module = vm->module;
	mc_vm_load_constants(vm);
	mc_vm_link(vm, (vm->options->plugins != NULL) ? vm->options->plugins : mc_plugins_default());
	if (module->entry < 0 || (size_t)module->entry >= module->functions.count) mc_vm_fail(vm, "the module has no entry function");
	// the words of every function's arguments first: calls are checked against them
	for (size_t i = 0; i < module->functions.count; ++i) {
		const mc_bc_function_s* const function = &module->functions.data[i];
		if (function->frame > ((uint64_t)1 << 31)) mc_vm_fail(vm, "the module is broken (the frame of `%s` is too large)", function->name);
		mc_prepared_s prepared = {0};
		for (size_t s = 0; s < function->slots.count; ++s) {
			const mc_bc_slot_s* const slot = &function->slots.data[s];
			const uint64_t size = (mc_type_code_count_ == slot->code) ? slot->size : mc_vm_code_size(slot->code);
			if (slot->offset + size > function->frame || slot->size > function->frame) {
				mc_vm_fail(vm, "the module is broken (a parameter of `%s` is outside its frame)", function->name);
			}
			prepared.arguments += (mc_type_code_count_ == slot->code) ? _mc_words(slot->size) : _mc_code_words(slot->code);
		}
		prepared.packed = true;
		for (size_t s = 0; s < function->slots.count; ++s) {
			const mc_type_code_e code = function->slots.data[s].code;
			const bool word = mc_code_s64 == code || mc_code_u64 == code || mc_code_int == code || mc_code_uint == code || mc_code_fn == code
				|| mc_code_f64 == code || mc_code_float == code;
			prepared.packed = prepared.packed && word && function->slots.data[s].offset == 8 * s;
		}
		mc_vector_push(&vm->prepared, prepared);
	}
	for (size_t i = 0; i < module->functions.count; ++i) _mc_verify(vm, (int64_t)i);
	const mc_bc_function_s* const entry = &module->functions.data[module->entry];
	const bool command_line = 1 == entry->slots.count && mc_code_slice == entry->slots.data[0].code;
	if (vm->prepared.data[module->entry].arguments != 0 && !command_line) mc_vm_fail(vm, "the module is broken (the entry function takes arguments)");
}

/// the command line as a [][]u8 on the stack: read-only blocks the program
/// cannot free (they are not heap memory).
static void _mc_push_command_line(mc_vm_s* const vm) {
	const size_t count = vm->options->argument_count;
	uint64_t bytes = 0;
	for (size_t i = 0; i < count; ++i) bytes += strlen(vm->options->arguments[i]);
	uint8_t* const text = mc_calloc(1, bytes > 0 ? (size_t)bytes : 1);
	const uint64_t text_handle = mc_vm_block_new(vm, mc_block_static, text, bytes, mc_free) | mc_handle_read_only;
	uint64_t* const slices = mc_calloc(count > 0 ? count : 1, 16);
	const uint64_t slices_handle = mc_vm_block_new(vm, mc_block_static, (uint8_t*)slices, 16 * count, mc_free) | mc_handle_read_only;
	uint64_t offset = 0;
	for (size_t i = 0; i < count; ++i) {
		const size_t length = strlen(vm->options->arguments[i]);
		memcpy(text + offset, vm->options->arguments[i], length);
		slices[2 * i] = text_handle;
		slices[2 * i + 1] = mc_slice_word(offset, length);
		offset += length;
	}
	vm->stack[vm->sp++].u = (count > 0) ? slices_handle : 0;
	vm->stack[vm->sp++].u = mc_slice_word(0, count);
}

static void _mc_start(mc_vm_s* const vm) {
	_mc_load(vm);
	mc_vm_reserve(vm, 1024);
	if (vm->prepared.data[vm->module->entry].arguments > 0) _mc_push_command_line(vm);
	_mc_call(vm, vm->module->entry);
	_mc_loop(vm);
}

bool mc_vm_run(const mc_module_bc_s* const module, const mc_vm_options_s* const options, mc_vm_result_s* const result) {
	*result = (mc_vm_result_s) {0};
	const mc_vm_options_s defaults = {0};
	jmp_buf stop;
	mc_vm_s vm = { .module = module, .options = (options != NULL) ? options : &defaults, .stop = &stop, };
	vm.out = (vm.options->out != NULL) ? vm.options->out : stdout;
	vm.call_depth = (vm.options->call_depth > 0) ? vm.options->call_depth : mc_vm_call_depth_default;
	if (0 == setjmp(stop)) _mc_start(&vm);
	(void)fflush(vm.out);
	result->steps = vm.steps;
	result->allocations = vm.allocations;
	if (vm.error != NULL) {
		result->failed = true;
		result->message = vm.error;
		for (size_t i = vm.frames.count; i-- > 0;) {
			const mc_frame_s* const frame = &vm.frames.data[i];
			const mc_bc_function_s* const function = &module->functions.data[frame->function];
			const size_t index = (frame->pc > 0) ? (size_t)frame->pc - 1 : 0;
			mc_trace_entry_s entry = { .function = function->name, };
			if (index < function->positions.count && function->positions.data[index].file >= 0) {
				entry.has_position = true;
				entry.position = function->positions.data[index];
				if ((size_t)entry.position.file < module->files.count) entry.path = module->files.data[entry.position.file];
				else entry.has_position = false;
			}
			mc_vector_push(&result->trace, entry);
		}
	}
	else if (vm.returned && module->entry_returns) {
		result->has_result = true;
		result->result = vm.result.i;
	}
	mc_vm_free_memory(&vm);
	for (size_t i = 0; i < vm.prepared.count; ++i) mc_free(vm.prepared.data[i].code);
	mc_vector_free(&vm.prepared);
	mc_vector_free(&vm.frames);
	mc_vm_unlink(&vm);
	mc_string_free(&vm.buffer);
	mc_string_free(&vm.bytes);
	return !result->failed;
}

void mc_vm_result_free(mc_vm_result_s* const result) {
	mc_free(result->message);
	mc_vector_free(&result->trace);
	*result = (mc_vm_result_s) {0};
}

int mc_vm_exit_status(const mc_vm_result_s* const result) {
	return result->has_result ? (int)(result->result & 0xFF) : 0;
}
