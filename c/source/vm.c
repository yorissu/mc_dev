
/// the machine: loading a module, the instruction loop and runtime errors.
/// it follows the python implementation, so both print the same output and
/// stop with the same runtime errors.

#include "machine.h"
#include "mc/format.h"

#include <math.h>
#include <stdarg.h>
#include <string.h>

void mc_vm_fail(mc_vm_s* const vm, const char* const format, ...) {
	va_list args; va_start(args, format);
	mc_free(vm->error);
	vm->error = mc_vformat(format, args);
	va_end(args);
	longjmp(*vm->stop, 1);
}

// --- type codes ------------------------------------------------------------------------------

typedef struct {
	char     kind  ;   // 'i' integer, 'f' float, 'b' bool, 'n' function
	uint32_t bits  ;
	bool     signed_;
	const char* name;
} _mc_code_info_s;

static const _mc_code_info_s _mc_codes[mc_type_code_count_] = {
	[mc_code_s8]    = { 'i', 8 , true , "s8"    },
	[mc_code_s16]   = { 'i', 16, true , "s16"   },
	[mc_code_s32]   = { 'i', 32, true , "s32"   },
	[mc_code_s64]   = { 'i', 64, true , "s64"   },
	[mc_code_u8]    = { 'i', 8 , false, "u8"    },
	[mc_code_u16]   = { 'i', 16, false, "u16"   },
	[mc_code_u32]   = { 'i', 32, false, "u32"   },
	[mc_code_u64]   = { 'i', 64, false, "u64"   },
	[mc_code_int]   = { 'i', 64, true , "int"   },
	[mc_code_uint]  = { 'i', 64, false, "uint"  },
	[mc_code_f32]   = { 'f', 32, true , "f32"   },
	[mc_code_f64]   = { 'f', 64, true , "f64"   },
	[mc_code_float] = { 'f', 64, true , "float" },
	[mc_code_bool]  = { 'b', 8 , false, "bool"  },
	[mc_code_fn]    = { 'n', 0 , false, "fn"    },
};

static __int128 _mc_wrap(const unsigned __int128 value, const mc_type_code_e code) {
	const uint32_t bits = _mc_codes[code].bits;
	unsigned __int128 wrapped = value & (((unsigned __int128)1 << bits) - 1);
	if (_mc_codes[code].signed_ && (wrapped >> (bits - 1)) != 0) wrapped -= (unsigned __int128)1 << bits;
	return (__int128)wrapped;
}

static double _mc_round(const double value, const mc_type_code_e code) {
	if (32 == _mc_codes[code].bits && isfinite(value)) return (double)(float)value;
	return value;
}

/// the exact integer part of 'value', wrapped to 'code' (like python's int()).
static __int128 _mc_truncate(const double value, const mc_type_code_e code) {
	const double whole = trunc(value);
	if (fabs(whole) < 1e38) return _mc_wrap((unsigned __int128)(__int128)whole, code);
	// only the low 64 bits matter: they are the remainder modulo 2^64
	const double low = fmod(whole, 18446744073709551616.0);
	return _mc_wrap((unsigned __int128)(__int128)low, code);
}

static bool _mc_truthy(const mc_value_s value) {
	switch (value.kind) {
		case mc_value_boolean: return value.as.boolean;
		case mc_value_integer: return value.as.integer != 0;
		case mc_value_real:    return value.as.real != 0.0;
		case mc_value_none:    return false;
		default:               return true;
	}
}

static double _mc_real_of(const mc_value_s value) {
	if (mc_value_real == value.kind) return value.as.real;
	if (mc_value_boolean == value.kind) return value.as.boolean ? 1.0 : 0.0;
	return (double)value.as.integer;
}

static __int128 _mc_integer_of(const mc_value_s value) {
	if (mc_value_boolean == value.kind) return value.as.boolean ? 1 : 0;
	if (mc_value_real == value.kind) return (__int128)value.as.real;
	return value.as.integer;
}

/// -1, 0 or 1; 2 when unordered (nan, or different functions).
static int _mc_compare(const mc_value_s left, const mc_value_s right) {
	if (mc_value_function == left.kind || mc_value_function == right.kind) {
		const bool same = left.kind == right.kind && left.as.function.builtin == right.as.function.builtin
			&& left.as.function.index == right.as.function.index;
		return same ? 0 : 2;
	}
	if (mc_value_real == left.kind || mc_value_real == right.kind) {
		const double a = _mc_real_of(left), b = _mc_real_of(right);
		return (a < b) ? -1 : (a > b) ? 1 : (a == b) ? 0 : 2;
	}
	const __int128 a = _mc_integer_of(left), b = _mc_integer_of(right);
	return (a < b) ? -1 : (a > b) ? 1 : 0;
}

/// python's float floor division.
static double _mc_floor_divide(const double a, const double b) {
	double mod = fmod(a, b);
	double div = (a - mod) / b;
	if (mod != 0.0) {
		if ((b < 0) != (mod < 0)) div -= 1.0;
	}
	if (div != 0.0) {
		double floored = floor(div);
		if (div - floored > 0.5) floored += 1.0;
		return floored;
	}
	return copysign(0.0, a / b);
}

static double _mc_float_divide(const double a, const double b) {
	return _mc_floor_divide(fabs(a), fabs(b)) * (((a >= 0) == (b >= 0)) ? 1.0 : -1.0);
}

static __int128 _mc_integer_divide(const __int128 a, const __int128 b) {
	const __int128 quotient = (a < 0 ? -a : a) / (b < 0 ? -b : b);
	return ((a >= 0) == (b >= 0)) ? quotient : -quotient;
}

static mc_value_s _mc_arithmetic(mc_vm_s* const vm, const mc_instruction_s* const instruction, const mc_value_s left, const mc_value_s right) {
	const mc_type_code_e code = instruction->type;
	const mc_opcode_e opcode = instruction->opcode;
	if ('f' == _mc_codes[code].kind) {
		const double a = _mc_real_of(left), b = _mc_real_of(right);
		switch (opcode) {
			case mc_op_ADD: return mc_make_real(_mc_round(a + b, code));
			case mc_op_SUB: return mc_make_real(_mc_round(a - b, code));
			case mc_op_MUL: return mc_make_real(_mc_round(a * b, code));
			case mc_op_DIV:
				if (0.0 == b) return mc_make_real((0.0 == a || isnan(a)) ? NAN : copysign(INFINITY, a) * copysign(1.0, b));
				return mc_make_real(_mc_round(a / b, code));
			case mc_op_MOD:
				if (0.0 == b) mc_vm_fail(vm, "division by zero");
				return mc_make_real(_mc_round(a - b * _mc_float_divide(a, b), code));
			default: break;
		}
		mc_vm_fail(vm, "`%s` on floats", mc_opcode_name(opcode));
	}
	const __int128 a = _mc_integer_of(left), b = _mc_integer_of(right);
	const unsigned __int128 ua = (unsigned __int128)a, ub = (unsigned __int128)b;
	switch (opcode) {
		case mc_op_ADD: return mc_make_integer(_mc_wrap(ua + ub, code));
		case mc_op_SUB: return mc_make_integer(_mc_wrap(ua - ub, code));
		case mc_op_MUL: return mc_make_integer(_mc_wrap(ua * ub, code));
		case mc_op_DIV:
			if (0 == b) mc_vm_fail(vm, "division by zero");
			return mc_make_integer(_mc_wrap((unsigned __int128)_mc_integer_divide(a, b), code));
		case mc_op_MOD:
			if (0 == b) mc_vm_fail(vm, "division by zero");
			return mc_make_integer(_mc_wrap(ua - ub * (unsigned __int128)_mc_integer_divide(a, b), code));
		case mc_op_AND: return mc_make_integer(_mc_wrap(ua & ub, code));
		case mc_op_OR:  return mc_make_integer(_mc_wrap(ua | ub, code));
		case mc_op_XOR: return mc_make_integer(_mc_wrap(ua ^ ub, code));
		case mc_op_SHL:
		case mc_op_SHR: {
			if (b < 0) {
				char text[48];
				mc_vm_fail(vm, "negative shift count %s", mc_vm_int_text(text, b));
			}
			if (mc_op_SHL == opcode) return mc_make_integer(_mc_wrap(b >= 128 ? 0 : ua << b, code));
			const __int128 shifted = (b >= 127) ? (a < 0 ? -1 : 0) : (a >> b);
			return mc_make_integer(_mc_wrap((unsigned __int128)shifted, code));
		}
		default: break;
	}
	mc_vm_fail(vm, "`%s` on integers", mc_opcode_name(opcode));
}

static mc_value_s _mc_cast(mc_vm_s* const vm, const mc_instruction_s* const instruction, const mc_value_s value) {
	const char source = _mc_codes[instruction->type].kind;
	const mc_type_code_e target = instruction->target;
	if ('i' == _mc_codes[target].kind) {
		if ('f' == source) {
			const double real = _mc_real_of(value);
			if (!isfinite(real)) {
				mc_string_s text = {0};
				mc_vm_format_float(&text, real, 64);
				char buffer[64];
				(void)snprintf(buffer, sizeof(buffer), "%s", mc_string_cstr(&text));
				mc_string_free(&text);
				mc_vm_fail(vm, "cannot convert %s to `%s`", buffer, _mc_codes[target].name);
			}
			return mc_make_integer(_mc_truncate(real, target));
		}
		return mc_make_integer(_mc_wrap((unsigned __int128)_mc_integer_of(value), target));
	}
	if ('f' == _mc_codes[target].kind) return mc_make_real(_mc_round(_mc_real_of(value), target));
	return value;
}

// --- the stack -------------------------------------------------------------------------------

static void _mc_push(mc_vm_s* const vm, const mc_value_s value) {
	mc_vector_push(&vm->stack, value);
}

static mc_value_s _mc_pop(mc_vm_s* const vm) {
	return mc_vector_pop(&vm->stack);
}

static mc_value_s* _mc_top(mc_vm_s* const vm) {
	return &vm->stack.data[vm->stack.count - 1];
}

/// the top 'count' values as arguments (copies; locations stay), popped.
static mc_value_s* _mc_arguments(mc_vm_s* const vm, const int64_t count, mc_value_s* const scratch) {
	mc_value_s* const first = vm->stack.data + vm->stack.count - (size_t)count;
	for (int64_t i = 0; i < count; ++i) scratch[i] = (mc_value_location == first[i].kind) ? first[i] : mc_vm_copy(vm, first[i]);
	vm->stack.count -= (size_t)count;
	return scratch;
}

static void _mc_call(mc_vm_s* const vm, const int64_t index, const int64_t count) {
	if (vm->frames.count >= mc_call_depth_limit) mc_vm_fail(vm, "stack overflow: more than %d nested calls", mc_call_depth_limit);
	const mc_bc_function_s* const function = &vm->module->functions.data[index];
	const size_t locals = (size_t)((function->locals > count) ? function->locals : count);
	mc_frame_record_s* const record = mc_vm_frame_record(vm, locals);
	mc_value_s* const first = vm->stack.data + vm->stack.count - (size_t)count;
	for (int64_t i = 0; i < count; ++i) {
		record->locals->items[i] = (mc_value_location == first[i].kind) ? first[i] : mc_vm_copy(vm, first[i]);
	}
	vm->stack.count -= (size_t)count;
	const mc_frame_s frame = { .function = index, .pc = 0, .record = record, };
	mc_vector_push(&vm->frames, frame);
}

/// the arguments of a builtin or native call, in the machine's scratch room.
static mc_value_s* _mc_scratch_arguments(mc_vm_s* const vm, const int64_t count) {
	vm->arguments.count = 0;
	mc_vector_reserve(&vm->arguments, (size_t)count + 1);
	return _mc_arguments(vm, count, vm->arguments.data);
}

static mc_value_s _mc_builtin(mc_vm_s* const vm, const int64_t index, const int64_t count) {
	mc_value_s* const arguments = _mc_scratch_arguments(vm, count);
	return vm->builtins.data[index](vm, arguments, (size_t)count, &vm->module->builtins.data[index]);
}

// --- running ---------------------------------------------------------------------------------

static void _mc_step(mc_vm_s* const vm, mc_frame_s* const frame, const mc_instruction_s* const instruction) {
	mc_list_s* const locals = frame->record->locals;
	const int64_t a = instruction->a;
	switch (instruction->opcode) {
		case mc_op_CONST: _mc_push(vm, mc_vm_copy(vm, vm->constants.data[a])); break;
		case mc_op_NONE:  _mc_push(vm, mc_make_none()); break;
		case mc_op_POP:   (void)_mc_pop(vm); break;
		case mc_op_DUP:   _mc_push(vm, *_mc_top(vm)); break;
		case mc_op_LOAD:  _mc_push(vm, locals->items[a]); break;
		case mc_op_LOAD_DEREF: _mc_push(vm, mc_vm_get(vm, locals->items[a].as.location)); break;
		case mc_op_STORE: {
			const mc_value_s value = _mc_pop(vm);
			locals->items[a] = mc_vm_assign(vm, locals->items[a], value);
			break;
		}
		case mc_op_STORE_NEW: locals->items[a] = mc_vm_copy(vm, _mc_pop(vm)); break;
		case mc_op_LOC_LOCAL:
			_mc_push(vm, mc_make_location(mc_vm_place(vm, locals, a, &frame->record->object, NULL, false)));
			break;
		case mc_op_LOC_FIELD: {
			const mc_place_s* const parent = _mc_top(vm)->as.location;
			const mc_value_s value = mc_vm_get(vm, parent);
			*_mc_top(vm) = mc_make_location(mc_vm_place(vm, value.as.list, a, parent->owner, parent->block, parent->read_only));
			break;
		}
		case mc_op_LOC_INDEX_ARRAY: {
			const __int128 index = _mc_pop(vm).as.integer;
			const mc_place_s* const parent = _mc_top(vm)->as.location;
			mc_list_s* const array = mc_vm_get(vm, parent).as.list;
			if (index < 0 || index >= (__int128)array->count) {
				char text[48];
				mc_vm_fail(vm, "index %s is out of bounds (length %zu)", mc_vm_int_text(text, index), array->count);
			}
			*_mc_top(vm) = mc_make_location(mc_vm_place(vm, array, (int64_t)index, parent->owner, parent->block, parent->read_only));
			break;
		}
		case mc_op_LOC_INDEX_SLICE: {
			const __int128 index = _mc_pop(vm).as.integer;
			const mc_value_s value = *_mc_top(vm);
			if (index < 0 || index >= value.as.slice.length) {
				char text[48];
				mc_vm_fail(vm, "index %s is out of bounds (length %lld)", mc_vm_int_text(text, index), (long long)value.as.slice.length);
			}
			mc_block_s* const block = value.as.slice.block;
			mc_vm_check(vm, block);
			*_mc_top(vm) = mc_make_location(mc_vm_place(vm, block->data, value.as.slice.start + (int64_t)index, &block->object, block,
				value.as.slice.read_only));
			break;
		}
		case mc_op_TEMP_LOC: {
			mc_list_s* const holder = mc_vm_list(vm, 1);
			holder->items[0] = *_mc_top(vm);
			*_mc_top(vm) = mc_make_location(mc_vm_place(vm, holder, 0, &frame->record->object, NULL, false));
			break;
		}
		case mc_op_LOAD_LOC: *_mc_top(vm) = mc_vm_get(vm, _mc_top(vm)->as.location); break;
		case mc_op_STORE_LOC: {
			const mc_value_s value = _mc_pop(vm);
			const mc_value_s place = _mc_pop(vm);
			mc_vm_set(vm, place.as.location, value);
			break;
		}
		case mc_op_FIELD: *_mc_top(vm) = _mc_top(vm)->as.list->items[a]; break;
		case mc_op_SET_FIELD: {
			const mc_value_s value = _mc_pop(vm);
			_mc_top(vm)->as.list->items[a] = mc_vm_copy(vm, value);
			break;
		}
		case mc_op_MAKE_ARRAY: {
			mc_list_s* const list = mc_vm_list(vm, (size_t)a);
			const mc_value_s* const first = vm->stack.data + vm->stack.count - (size_t)a;
			for (int64_t i = 0; i < a; ++i) list->items[i] = mc_vm_copy(vm, first[i]);
			vm->stack.count -= (size_t)a;
			_mc_push(vm, mc_make_list(list));
			break;
		}
		case mc_op_INDEX_ARRAY: {
			const __int128 index = _mc_pop(vm).as.integer;
			const mc_list_s* const array = _mc_top(vm)->as.list;
			if (index < 0 || index >= (__int128)array->count) {
				char text[48];
				mc_vm_fail(vm, "index %s is out of bounds (length %zu)", mc_vm_int_text(text, index), array->count);
			}
			*_mc_top(vm) = array->items[index];
			break;
		}
		case mc_op_INDEX_SLICE: {
			const __int128 index = _mc_pop(vm).as.integer;
			const mc_value_s value = *_mc_top(vm);
			if (index < 0 || index >= value.as.slice.length) {
				char text[48];
				mc_vm_fail(vm, "index %s is out of bounds (length %lld)", mc_vm_int_text(text, index), (long long)value.as.slice.length);
			}
			mc_vm_check(vm, value.as.slice.block);
			*_mc_top(vm) = value.as.slice.block->data->items[value.as.slice.start + (int64_t)index];
			break;
		}
		case mc_op_LEN_SLICE: *_mc_top(vm) = mc_make_integer(_mc_top(vm)->as.slice.length); break;
		case mc_op_SLICE_LOC:
		case mc_op_SLICE_SLICE: {
			const bool has_high = (a & mc_slice_high) != 0, has_low = (a & mc_slice_low) != 0;
			const __int128 high = has_high ? _mc_pop(vm).as.integer : 0;
			const __int128 low = has_low ? _mc_pop(vm).as.integer : 0;
			const mc_value_s value = _mc_pop(vm);
			if (mc_op_SLICE_LOC == instruction->opcode) {
				const mc_place_s* const place = value.as.location;
				mc_list_s* const array = mc_vm_get(vm, place).as.list;
				mc_object_s* owner = (place->block != NULL) ? &place->block->object : place->owner;
				if (a & mc_slice_static) owner = NULL;
				const bool read_only = (a & mc_slice_read_only) != 0 || place->read_only;
				_mc_push(vm, mc_vm_slice_of(vm, array, owner, read_only, a, has_low, low, has_high, high, 0, (int64_t)array->count, NULL));
				break;
			}
			mc_block_s* const block = value.as.slice.block;
			if (NULL == block) {
				if (low != 0 || high != 0) _mc_push(vm, mc_vm_slice_of(vm, mc_vm_list(vm, 0), NULL, value.as.slice.read_only, a, has_low, low, has_high, high, 0, 0, NULL));
				else _mc_push(vm, value);
				break;
			}
			mc_vm_check(vm, block);
			_mc_push(vm, mc_vm_slice_of(vm, block->data, NULL, value.as.slice.read_only, a, has_low, low, has_high, high,
				value.as.slice.start, value.as.slice.length, block));
			break;
		}
		case mc_op_SLICE_IS_NULL: *_mc_top(vm) = mc_make_boolean(0 == _mc_top(vm)->as.slice.length); break;
		case mc_op_JUMP: frame->pc = a; break;
		case mc_op_JUMP_IF_FALSE: if (!_mc_truthy(_mc_pop(vm))) frame->pc = a; break;
		case mc_op_JUMP_IF_TRUE:  if (_mc_truthy(_mc_pop(vm))) frame->pc = a; break;
		case mc_op_NOT: *_mc_top(vm) = mc_make_boolean(!_mc_truthy(*_mc_top(vm))); break;
		case mc_op_RANGE_INIT: {
			const __int128 end = _mc_pop(vm).as.integer;
			const __int128 start = _mc_pop(vm).as.integer;
			const __int128 step = (end >= start) ? 1 : -1;
			locals->items[a] = mc_make_integer(start);
			locals->items[a + 1] = mc_make_integer(step);
			locals->items[a + 2] = mc_make_integer(instruction->b ? end + step : end);
			break;
		}
		case mc_op_RANGE_TEST:
			if (locals->items[a].as.integer == locals->items[a + 2].as.integer) frame->pc = instruction->b;
			break;
		case mc_op_RANGE_STEP: locals->items[a].as.integer += locals->items[a + 1].as.integer; break;
		case mc_op_CALL: _mc_call(vm, a, instruction->b); break;
		case mc_op_CALL_VALUE: {
			const mc_value_s function = vm->stack.data[vm->stack.count - 1 - (size_t)a];
			if (function.as.function.builtin) {
				const mc_value_s result = _mc_builtin(vm, function.as.function.index, a);
				*_mc_top(vm) = result;
				break;
			}
			// the function value sits below the arguments: move them down over it
			mc_value_s* const slot = vm->stack.data + vm->stack.count - 1 - (size_t)a;
			memmove(slot, slot + 1, (size_t)a * sizeof(mc_value_s));
			vm->stack.count -= 1;
			_mc_call(vm, function.as.function.index, a);
			break;
		}
		case mc_op_CALL_BUILTIN: _mc_push(vm, _mc_builtin(vm, a, instruction->b)); break;
		case mc_op_CALL_NATIVE: {
			mc_value_s* const arguments = _mc_scratch_arguments(vm, instruction->b);
			_mc_push(vm, vm->natives.data[a](vm, arguments, (size_t)instruction->b));
			break;
		}
		case mc_op_FUNCTION:
		case mc_op_FUNCTION_BUILTIN: {
			mc_value_s value = { .kind = mc_value_function, };
			value.as.function.builtin = mc_op_FUNCTION_BUILTIN == instruction->opcode;
			value.as.function.index = a;
			_mc_push(vm, value);
			break;
		}
		case mc_op_RETURN: {
			const mc_value_s value = _mc_pop(vm);
			frame->record->alive = false;
			vm->frames.count -= 1;
			if (vm->frames.count > 0) _mc_push(vm, value);
			else vm->result = value;
			break;
		}
		case mc_op_ASSERT_FAIL: {
			const mc_bc_constant_s* const message = &vm->module->constants.data[a];
			mc_vm_fail(vm, "%s", mc_constant_text == message->kind ? message->text : "assertion failed");
		}
		case mc_op_EQ: case mc_op_NE: case mc_op_LT: case mc_op_GT: case mc_op_LE: case mc_op_GE: {
			const mc_value_s right = _mc_pop(vm);
			const int order = _mc_compare(*_mc_top(vm), right);
			bool result = false;
			switch (instruction->opcode) {
				case mc_op_EQ: result = 0 == order; break;
				case mc_op_NE: result = 0 != order; break;
				case mc_op_LT: result = -1 == order; break;
				case mc_op_GT: result = 1 == order; break;
				case mc_op_LE: result = -1 == order || 0 == order; break;
				default:       result = 1 == order || 0 == order; break;
			}
			*_mc_top(vm) = mc_make_boolean(result);
			break;
		}
		case mc_op_ADD: case mc_op_SUB: case mc_op_MUL: case mc_op_DIV: case mc_op_MOD:
		case mc_op_AND: case mc_op_OR: case mc_op_XOR: case mc_op_SHL: case mc_op_SHR: {
			const mc_value_s right = _mc_pop(vm);
			*_mc_top(vm) = _mc_arithmetic(vm, instruction, *_mc_top(vm), right);
			break;
		}
		case mc_op_NEG: {
			mc_value_s* const top = _mc_top(vm);
			if ('f' == _mc_codes[instruction->type].kind) *top = mc_make_real(_mc_round(-_mc_real_of(*top), instruction->type));
			else *top = mc_make_integer(_mc_wrap((unsigned __int128)0 - (unsigned __int128)_mc_integer_of(*top), instruction->type));
			break;
		}
		case mc_op_BITNOT: {
			mc_value_s* const top = _mc_top(vm);
			*top = mc_make_integer(_mc_wrap(~(unsigned __int128)_mc_integer_of(*top), instruction->type));
			break;
		}
		case mc_op_CAST: *_mc_top(vm) = _mc_cast(vm, instruction, *_mc_top(vm)); break;
		case mc_op_count_: mc_vm_fail(vm, "an invalid instruction");
	}
}

static void _mc_loop(mc_vm_s* const vm) {
	const uint64_t limit = vm->options->limit;
	while (vm->frames.count > 0) {
		if (vm->allocated >= vm->threshold) mc_vm_collect(vm);
		mc_frame_s* const frame = &mc_vector_last(&vm->frames);
		const mc_bc_function_s* const function = &vm->module->functions.data[frame->function];
		if (limit > 0 && vm->steps >= limit) mc_vm_fail(vm, "stopped after %llu instructions", (unsigned long long)limit);
		if (frame->pc < 0 || (size_t)frame->pc >= function->code.count) mc_vm_fail(vm, "`%s` runs past its last instruction", function->name);
		const mc_instruction_s* const instruction = &function->code.data[frame->pc++];
		vm->steps++;
		_mc_step(vm, frame, instruction);
	}
}

// --- loading -------------------------------------------------------------------------------

static mc_value_s _mc_constant_value(mc_vm_s* const vm, const mc_bc_constant_s* const constant) {
	switch (constant->kind) {
		case mc_constant_integer: return mc_make_integer(constant->integer);
		case mc_constant_real:    return mc_make_real(constant->real);
		case mc_constant_boolean: return mc_make_boolean(constant->boolean);
		case mc_constant_list: {
			mc_list_s* const list = mc_vm_list(vm, constant->items.count);
			for (size_t i = 0; i < constant->items.count; ++i) list->items[i] = _mc_constant_value(vm, &constant->items.data[i]);
			return mc_make_list(list);
		}
		case mc_constant_slice: {
			if (constant->null) return mc_make_slice(NULL, 0, 0, false);
			mc_list_s* const data = mc_vm_list(vm, constant->items.count);
			for (size_t i = 0; i < constant->items.count; ++i) data->items[i] = _mc_constant_value(vm, &constant->items.data[i]);
			return mc_make_slice(mc_vm_block(vm, data, NULL, mc_block_static), constant->start, constant->length, constant->read_only);
		}
		case mc_constant_function:
		case mc_constant_builtin: {
			mc_value_s value = { .kind = mc_value_function, };
			value.as.function.builtin = mc_constant_builtin == constant->kind;
			value.as.function.index = (int64_t)constant->integer;
			return value;
		}
		default: return mc_make_none();
	}
}

static mc_descriptor_kind_e _mc_descriptor_kind(mc_vm_s* const vm, const char* const kind) {
	static const char* const names[] = { "int", "float", "bool", "array", "slice", "struct", "function", };
	for (size_t i = 0; i < mc_count_of(names); ++i) if (0 == strcmp(names[i], kind)) return (mc_descriptor_kind_e)i;
	mc_vm_fail(vm, "unknown type kind `%s`", kind);
}

static void _mc_load(mc_vm_s* const vm) {
	const mc_module_bc_s* const module = vm->module;
	for (size_t i = 0; i < module->natives.count; ++i) {
		const mc_native_f native = mc_vm_find_native(module->natives.data[i]);
		if (NULL == native) mc_vm_fail(vm, "unknown extern function `%s`", module->natives.data[i]);
		mc_vector_push(&vm->natives, native);
	}
	for (size_t i = 0; i < module->builtins.count; ++i) {
		const mc_builtin_f builtin = mc_vm_find_builtin(module->builtins.data[i].name);
		if (NULL == builtin) mc_vm_fail(vm, "unknown builtin function `%s`", module->builtins.data[i].name);
		mc_vector_push(&vm->builtins, builtin);
	}
	for (size_t i = 0; i < module->types.count; ++i) mc_vector_push(&vm->kinds, _mc_descriptor_kind(vm, module->types.data[i].kind));
	for (size_t i = 0; i < module->constants.count; ++i) mc_vector_push(&vm->constants, _mc_constant_value(vm, &module->constants.data[i]));
	if (module->entry < 0 || (size_t)module->entry >= module->functions.count) mc_vm_fail(vm, "the module has no entry function");
}

static void _mc_start(mc_vm_s* const vm) {
	_mc_load(vm);
	_mc_call(vm, vm->module->entry, 0);
	_mc_loop(vm);
}

bool mc_vm_run(const mc_module_bc_s* const module, const mc_vm_options_s* const options, mc_vm_result_s* const result) {
	*result = (mc_vm_result_s) {0};
	const mc_vm_options_s defaults = {0};
	jmp_buf stop;
	mc_vm_s vm = { .module = module, .options = (options != NULL) ? options : &defaults, .stop = &stop, .threshold = 32768, };
	vm.out = (vm.options->out != NULL) ? vm.options->out : stdout;
	if (0 == setjmp(stop)) _mc_start(&vm);
	(void)fflush(vm.out);
	result->steps = vm.steps;
	result->collections = vm.collections;
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
	else if (mc_value_integer == vm.result.kind) {
		result->has_result = true;
		result->result = vm.result.as.integer;
	}
	mc_vm_free_objects(&vm);
	mc_vector_free(&vm.stack);
	mc_vector_free(&vm.frames);
	mc_vector_free(&vm.constants);
	mc_vector_free(&vm.kinds);
	mc_vector_free(&vm.builtins);
	mc_vector_free(&vm.natives);
	mc_vector_free(&vm.arguments);
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
