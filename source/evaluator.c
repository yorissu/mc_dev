
/// compile-time evaluation of checked expressions.
///
/// constants ('NAME :: value;'), array lengths, default arguments, field
/// defaults and format strings must be known at compile time. the evaluator
/// computes them from the checked tree, and it can run user functions with
/// constant arguments (pure code: plugin functions cannot run at compile
/// time). evaluation stops after a number of steps ('--eval-steps'); the
/// error points at the innermost loop or call still running then.
///
/// values: integers (128 bits, so untyped constants are exact), floats and
/// bools; arrays and structs are lists (copied on assignment, like every
/// value); slices are views of a list; reference arguments point at the
/// cell they refer to. everything lives in the program's arena.

#include "mia/checker.h"
#include "mia/format.h"

#include <string.h>
#include <math.h>

#define _mia_evaluator_call_depth_limit 100

typedef struct {
	const mia_program_variable_s* variable;
	mia_evaluator_value_s*        cell    ;
} _mia_evaluator_slot_s;

struct mia_evaluator_frame_s {
	mia_vector_of(_mia_evaluator_slot_s) slots;
};

typedef enum {
	_mia_evaluator_flow_normal  ,
	_mia_evaluator_flow_return  ,
	_mia_evaluator_flow_break   ,
	_mia_evaluator_flow_continue,
} _mia_evaluator_flow_e;

typedef struct {
	_mia_evaluator_flow_e flow ;
	mia_evaluator_value_s value;
} _mia_evaluator_signal_s;

static mia_evaluator_value_s _mia_evaluator_expression(mia_evaluator_s* const e, const mia_ast_node_s* const node);
static _mia_evaluator_signal_s _mia_evaluator_block(mia_evaluator_s* const e, const mia_ast_node_s* const node);

// --- values --------------------------------------------------------------------------

static mia_arena_s* _mia_evaluator_arena(const mia_evaluator_s* const e) {
	return e->checker->arena;
}

static mia_evaluator_value_s _mia_evaluator_integer(const mia_int128_s value) {
	return (mia_evaluator_value_s) { .kind = mia_evaluator_value_integer, .as.integer = value, };
}

static mia_evaluator_value_s _mia_evaluator_int(const int64_t value) {
	return _mia_evaluator_integer(mia_int128_from_int(value));
}

static mia_evaluator_value_s _mia_evaluator_real(const double value) {
	return (mia_evaluator_value_s) { .kind = mia_evaluator_value_real, .as.real = value, };
}

static mia_evaluator_value_s _mia_evaluator_boolean(const bool value) {
	return (mia_evaluator_value_s) { .kind = mia_evaluator_value_boolean, .as.boolean = value, };
}

mia_noreturn static void _mia_evaluator_fail(mia_evaluator_s* const e, const mia_diag_code_e code, const mia_ast_node_s* const node, const char* const label,
											 const char* const format, ...) mia_printf_like(5, 6);

/// a list of 'count' values, for 'node' (which is blamed when values grow too many).
static mia_evaluator_list_s* _mia_evaluator_list_new(mia_evaluator_s* const e, const size_t count, const mia_ast_node_s* const node) {
	e->cells += count;
	if (e->cells > mia_evaluator_cell_limit) {
		_mia_evaluator_fail(e, mia_diag_E0705, node, "too large to compute here", "compile-time values take more than %llu elements",
			(unsigned long long)mia_evaluator_cell_limit);
	}
	mia_evaluator_list_s* const list = mia_arena_alloc(_mia_evaluator_arena(e), sizeof(mia_evaluator_list_s) + count * sizeof(mia_evaluator_value_s));
	list->count = count;
	return list;
}

static mia_evaluator_value_s _mia_evaluator_list_value(mia_evaluator_list_s* const list) {
	return (mia_evaluator_value_s) { .kind = mia_evaluator_value_list, .as.list = list, };
}

static mia_evaluator_value_s _mia_evaluator_slice_value(mia_evaluator_list_s* const list, const int64_t start, const int64_t length) {
	return (mia_evaluator_value_s) { .kind = mia_evaluator_value_slice, .as.slice = { .list = list, .start = start, .length = length, }, };
}

mia_evaluator_value_s mia_evaluator_value_copy(mia_checker_s* const checker, const mia_evaluator_value_s value) {
	if (mia_evaluator_value_list != value.kind) return value;
	mia_evaluator_list_s* const list = mia_arena_alloc(checker->arena, sizeof(mia_evaluator_list_s) + value.as.list->count * sizeof(mia_evaluator_value_s));
	list->count = value.as.list->count;
	for (size_t i = 0; i < list->count; ++i) list->items[i] = mia_evaluator_value_copy(checker, value.as.list->items[i]);
	return _mia_evaluator_list_value(list);
}

static mia_evaluator_value_s _mia_evaluator_copy(mia_evaluator_s* const e, const mia_evaluator_value_s value) {
	return mia_evaluator_value_copy(e->checker, value);
}

mia_int128_s mia_evaluator_integer_wrap(const mia_int128_s value, const mia_type_s* const type) {
	const uint32_t bits = mia_scalar_bits(type->as.scalar);
	if (bits >= 128) return value;
	const mia_int128_s top = mia_int128_shift_left(mia_int128_from_uint(1), bits);
	mia_int128_s wrapped = mia_int128_and(value, mia_int128_sub(top, mia_int128_from_uint(1)));
	const bool sign = !mia_int128_is_zero(mia_int128_shift_right_unsigned(wrapped, bits - 1));
	if (mia_scalar_signed(type->as.scalar) && sign) wrapped = mia_int128_sub(wrapped, top);
	return wrapped;
}

double mia_evaluator_float_wrap(const double value, const mia_type_s* const type) {
	if (32 == mia_scalar_bits(type->as.scalar) && isfinite(value)) {
		const float rounded = (float)value;
		return (double)rounded;
	}
	return value;
}

/// 'value' as a value of 'type': integers wrap around, f32 rounds.
static mia_evaluator_value_s _mia_evaluator_wrap(const mia_evaluator_value_s value, const mia_type_s* const type) {
	if (NULL == type || type->kind != mia_type_scalar) return value;
	const char kind = mia_scalar_kind(type->as.scalar);
	if ('i' == kind && mia_evaluator_value_integer == value.kind) return _mia_evaluator_integer(mia_evaluator_integer_wrap(value.as.integer, type));
	if ('f' == kind) {
		const double real = (mia_evaluator_value_integer == value.kind) ? mia_int128_to_double(value.as.integer) : value.as.real;
		return _mia_evaluator_real(mia_evaluator_float_wrap(real, type));
	}
	return value;
}

/// a shift count: negative counts never get here (the checker rejects
/// them), large ones shift everything out.
static unsigned _mia_evaluator_shift_count(const mia_int128_s right) {
	if (mia_int128_negative(right) || !mia_int128_less(right, mia_int128_from_uint(128))) return 128;
	return (unsigned)mia_int128_low(right);
}

bool mia_evaluator_integer_arithmetic(const mia_ast_operator_e operation, const mia_int128_s left, const mia_int128_s right, mia_int128_s* const result,
									  bool* const overflow) {
	*overflow = false;
	const mia_int128_s minimum = mia_int128_from_halves((uint64_t)1 << 63, 0);
	switch (operation) {
		case mia_ast_operator_add:
			*result = mia_int128_add(left, right);
			*overflow = mia_int128_negative(left) == mia_int128_negative(right) && mia_int128_negative(*result) != mia_int128_negative(left);
			return true;
		case mia_ast_operator_subtract:
			*result = mia_int128_sub(left, right);
			*overflow = mia_int128_negative(left) != mia_int128_negative(right) && mia_int128_negative(*result) != mia_int128_negative(left);
			return true;
		case mia_ast_operator_multiply: {
			*result = mia_int128_mul(left, right);
			if (mia_int128_is_zero(left) || mia_int128_is_zero(right)) return true;
			const mia_int128_s minus_one = mia_int128_from_int(-1);
			if ((mia_int128_equal(left, minus_one) && mia_int128_equal(right, minimum)) || (mia_int128_equal(right, minus_one) && mia_int128_equal(left, minimum))) {
				*overflow = true;
				return true;
			}
			mia_int128_s back = mia_int128_zero();
			(void)mia_int128_divide(*result, left, &back, NULL);
			*overflow = !mia_int128_equal(back, right);
			return true;
		}
		case mia_ast_operator_divide:
			*overflow = mia_int128_equal(left, minimum) && mia_int128_equal(right, mia_int128_from_int(-1));
			return mia_int128_divide(left, right, result, NULL);
		case mia_ast_operator_remainder: return mia_int128_divide(left, right, NULL, result);
		case mia_ast_operator_bit_and:   *result = mia_int128_and(left, right); return true;
		case mia_ast_operator_bit_or:    *result = mia_int128_or(left, right); return true;
		case mia_ast_operator_bit_xor:   *result = mia_int128_xor(left, right); return true;
		case mia_ast_operator_shift_left: {
			// exact when shifting back gives the value again (bits lost or the sign changed: overflow)
			const unsigned count = _mia_evaluator_shift_count(right);
			*result = (count >= 128) ? mia_int128_zero() : mia_int128_shift_left(left, count);
			*overflow = (count >= 128) ? !mia_int128_is_zero(left) : !mia_int128_equal(mia_int128_shift_right(*result, count), left);
			return true;
		}
		case mia_ast_operator_shift_right:
			*result = mia_int128_shift_right(left, _mia_evaluator_shift_count(right));
			return true;
		default:
			return false;
	}
}

// --- stopping ------------------------------------------------------------------------

mia_noreturn static void _mia_evaluator_not_constant(mia_evaluator_s* const e, const mia_ast_node_s* const node, const char* const format, ...) mia_printf_like(3, 4);

mia_noreturn static void _mia_evaluator_not_constant(mia_evaluator_s* const e, const mia_ast_node_s* const node, const char* const format, ...) {
	va_list args; va_start(args, format);
	char* const reason = mia_string_vformat(format, args);
	va_end(args);
	e->problem = (mia_evaluator_result_s) {
		.status = mia_evaluator_not_constant, .node = node, .reason = mia_arena_strndup(_mia_evaluator_arena(e), reason, strlen(reason)),
	};
	mia_free(reason);
	longjmp(*e->stop, 1);
}

/// reports an error (unless quiet) and stops the evaluation.

mia_noreturn static void _mia_evaluator_fail(mia_evaluator_s* const e, const mia_diag_code_e code, const mia_ast_node_s* const node, const char* const label,
											 const char* const format, ...) {
	if (!e->quiet) {
		va_list args; va_start(args, format);
		char* const message = mia_string_vformat(format, args);
		va_end(args);
		mia_diag_label_main(mia_diag_error(e->checker->diag, code, node->span, "%s", message), "%s", label);
		mia_free(message);
	}
	e->problem = (mia_evaluator_result_s) { .status = mia_evaluator_failed, };
	longjmp(*e->stop, 1);
}

/// a value of an unexpected kind: code with (reported) errors.
mia_noreturn static void _mia_evaluator_broken(mia_evaluator_s* const e) {
	e->problem = (mia_evaluator_result_s) { .status = mia_evaluator_failed, };
	longjmp(*e->stop, 1);
}

/// stops: the evaluation took too many steps. the error points at the
/// innermost loop or call that is still running.
mia_noreturn static void _mia_evaluator_too_long(mia_evaluator_s* const e, const mia_ast_node_s* const node) {
	if (!e->quiet) {
		const mia_evaluator_running_s* const innermost = (e->running.count > 0) ? &mia_vector_last(&e->running) : NULL;
		const mia_ast_node_s* const at = (innermost != NULL) ? innermost->node : node;
		mia_diag_entry_s* const diagnostic = mia_diag_error(e->checker->diag, mia_diag_E0705, at->span, "compile-time evaluation takes too long");
		const unsigned long long limit = (unsigned long long)e->limit;
		if (NULL == innermost) {
			mia_diag_label_main(diagnostic, "stopped after %llu steps", limit);
		}
		else if (innermost->function != NULL) {
			mia_diag_label_main(diagnostic, "this call runs more than %llu steps", limit);
			mia_diag_note(diagnostic, "the call of %s runs more than %llu steps", innermost->function, limit);
		}
		else {
			// the function the loop is in: the innermost call around it
			const char* function = NULL;
			for (size_t i = e->running.count; i-- > 0 && NULL == function;) function = e->running.data[i].function;
			mia_diag_label_main(diagnostic, "this loop runs more than %llu steps", limit);
			if (function != NULL) mia_diag_note(diagnostic, "the loop in %s runs more than %llu steps", function, limit);
			else mia_diag_note(diagnostic, "the loop runs more than %llu steps", limit);
		}
		mia_diag_help(diagnostic, "raise the limit with `--eval-steps <n>` if the evaluation is meant to take this long");
	}
	e->problem = (mia_evaluator_result_s) { .status = mia_evaluator_failed, };
	longjmp(*e->stop, 1);
}

static void _mia_evaluator_step(mia_evaluator_s* const e, const mia_ast_node_s* const node) {
	e->steps += 1;
	if (e->steps > e->limit) _mia_evaluator_too_long(e, node);
}

/// a loop or a call starts running (and stops with _mia_evaluator_stopped).
static void _mia_evaluator_running(mia_evaluator_s* const e, const mia_ast_node_s* const node, const char* const function) {
	const mia_evaluator_running_s running = { .node = node, .function = function, };
	mia_vector_push(&e->running, running);
}

static void _mia_evaluator_stopped(mia_evaluator_s* const e) {
	e->running.count -= 1;
}

// --- frames -------------------------------------------------------------------------

static mia_evaluator_frame_s* _mia_evaluator_frame(const mia_evaluator_s* const e) {
	return (e->frames.count > 0) ? e->frames.data[e->frames.count - 1] : NULL;
}

static mia_evaluator_value_s* _mia_evaluator_slot(const mia_evaluator_frame_s* const frame, const mia_program_variable_s* const variable) {
	if (NULL == frame) return NULL;
	for (size_t i = frame->slots.count; i > 0; --i) if (frame->slots.data[i - 1].variable == variable) return frame->slots.data[i - 1].cell;
	return NULL;
}

static void _mia_evaluator_bind(mia_evaluator_s* const e, mia_evaluator_frame_s* const frame, const mia_program_variable_s* const variable, const mia_evaluator_value_s value) {
	mia_evaluator_value_s* const existing = _mia_evaluator_slot(frame, variable);
	if (existing != NULL) {
		*existing = value;
		return;
	}
	mia_evaluator_value_s* const cell = mia_arena_new(_mia_evaluator_arena(e), mia_evaluator_value_s);
	*cell = value;
	const _mia_evaluator_slot_s slot = { .variable = variable, .cell = cell, };
	mia_arena_push(_mia_evaluator_arena(e), &frame->slots, slot);
}

/// the cell of a variable (following a reference argument).
static mia_evaluator_value_s* _mia_evaluator_variable_cell(mia_evaluator_s* const e, const mia_ast_node_s* const node, const mia_program_variable_s* const variable) {
	mia_evaluator_value_s* const cell = _mia_evaluator_slot(_mia_evaluator_frame(e), variable);
	if (NULL == cell) _mia_evaluator_not_constant(e, node, "uses the variable `" mia_view_fmt "`", mia_view_arg(variable->name));
	return (mia_evaluator_value_cell == cell->kind) ? cell->as.cell : cell;
}

// --- storage ---------------------------------------------------------------------------

typedef struct {
	mia_evaluator_list_s* list  ;
	int64_t               start ;
	int64_t               length;
} _mia_evaluator_storage_s;

static _mia_evaluator_storage_s _mia_evaluator_storage(mia_evaluator_s* const e, const mia_evaluator_value_s value) {
	if (mia_evaluator_value_slice == value.kind) return (_mia_evaluator_storage_s) { value.as.slice.list, value.as.slice.start, value.as.slice.length, };
	if (mia_evaluator_value_list == value.kind) return (_mia_evaluator_storage_s) { value.as.list, 0, (int64_t)value.as.list->count, };
	_mia_evaluator_broken(e);
}

static mia_evaluator_value_s _mia_evaluator_location_value(mia_evaluator_s* const e, const mia_ast_node_s* const node);

/// a formatted text in the arena (it survives a stop).
static const char* _mia_evaluator_text(mia_evaluator_s* const e, const char* const format, ...) mia_printf_like(2, 3);

static const char* _mia_evaluator_text(mia_evaluator_s* const e, const char* const format, ...) {
	va_list args; va_start(args, format);
	char* const text = mia_string_vformat(format, args);
	va_end(args);
	const char* const kept = mia_arena_strndup(_mia_evaluator_arena(e), text, strlen(text));
	mia_free(text);
	return kept;
}

static mia_evaluator_value_s* _mia_evaluator_element(mia_evaluator_s* const e, const mia_ast_node_s* const index_node, const mia_evaluator_value_s target) {
	const _mia_evaluator_storage_s storage = _mia_evaluator_storage(e, target);
	const mia_evaluator_value_s index = _mia_evaluator_expression(e, index_node);
	if (index.kind != mia_evaluator_value_integer) _mia_evaluator_broken(e);
	if (mia_int128_negative(index.as.integer) || !mia_int128_less(index.as.integer, mia_int128_from_int(storage.length))) {
		_mia_evaluator_fail(e, mia_diag_E0706, index_node, _mia_evaluator_text(e, "the length is %lld", (long long)storage.length), "index %lld is out of bounds",
			(long long)mia_int128_to_int(index.as.integer));
	}
	return &storage.list->items[storage.start + mia_int128_to_int(index.as.integer)];
}

/// the value at a place without copying it (so slices share it).
static mia_evaluator_value_s _mia_evaluator_location_value(mia_evaluator_s* const e, const mia_ast_node_s* const node) {
	if (mia_ast_node_paren == node->kind) return _mia_evaluator_location_value(e, node->as.paren.inner);
	if (mia_ast_node_name == node->kind && node->note.variable != NULL) return *_mia_evaluator_variable_cell(e, node, node->note.variable);
	if (mia_ast_node_member == node->kind && node->note.field != NULL) {
		const mia_evaluator_value_s target = _mia_evaluator_location_value(e, node->as.member.target);
		if (target.kind != mia_evaluator_value_list || node->note.field->index >= target.as.list->count) _mia_evaluator_broken(e);
		return target.as.list->items[node->note.field->index];
	}
	if (mia_ast_node_index == node->kind) {
		return *_mia_evaluator_element(e, node->as.index.index, _mia_evaluator_location_value(e, node->as.index.target));
	}
	return _mia_evaluator_expression(e, node);
}

/// the cell of the place 'node' (a new temporary for other values).
static mia_evaluator_value_s* _mia_evaluator_locate(mia_evaluator_s* const e, const mia_ast_node_s* const node) {
	if (mia_ast_node_paren == node->kind) return _mia_evaluator_locate(e, node->as.paren.inner);
	if (mia_ast_node_name == node->kind && node->note.variable != NULL) return _mia_evaluator_variable_cell(e, node, node->note.variable);
	if (mia_ast_node_member == node->kind && node->note.field != NULL) {
		const mia_evaluator_value_s target = _mia_evaluator_location_value(e, node->as.member.target);
		if (target.kind != mia_evaluator_value_list || node->note.field->index >= target.as.list->count) _mia_evaluator_broken(e);
		return &target.as.list->items[node->note.field->index];
	}
	if (mia_ast_node_index == node->kind) {
		return _mia_evaluator_element(e, node->as.index.index, _mia_evaluator_location_value(e, node->as.index.target));
	}
	mia_evaluator_value_s* const cell = mia_arena_new(_mia_evaluator_arena(e), mia_evaluator_value_s);
	*cell = _mia_evaluator_expression(e, node);
	return cell;
}

// --- zero values -------------------------------------------------------------------------

static mia_evaluator_value_s _mia_evaluator_constant_default(mia_evaluator_s* const e, const mia_ast_node_s* const node);

static mia_evaluator_value_s _mia_evaluator_zero(mia_evaluator_s* const e, const mia_type_s* const type, const mia_ast_node_s* const node) {
	switch (type->kind) {
		case mia_type_scalar: {
			const char kind = mia_scalar_kind(type->as.scalar);
			if ('b' == kind) return _mia_evaluator_boolean(false);
			if ('f' == kind) return _mia_evaluator_real(0.0);
			return _mia_evaluator_int(0);
		}
		case mia_type_slice:
			return _mia_evaluator_slice_value(NULL, 0, 0);
		case mia_type_array: {
			const int64_t length = type->as.array.length > 0 ? type->as.array.length : 0;
			mia_evaluator_list_s* const list = _mia_evaluator_list_new(e, (size_t)length, node);
			for (int64_t i = 0; i < length; ++i) list->items[i] = _mia_evaluator_zero(e, type->as.array.element, node);
			return _mia_evaluator_list_value(list);
		}
		case mia_type_struct: {
			const mia_program_layout_s* const layout = mia_checker_layout(e->checker, type);
			mia_evaluator_list_s* const list = _mia_evaluator_list_new(e, layout->fields.count, node);
			for (size_t i = 0; i < layout->fields.count; ++i) {
				const mia_program_field_s* const field = layout->fields.data[i];
				list->items[i] = (field->default_value != NULL) ? _mia_evaluator_copy(e, _mia_evaluator_constant_default(e, field->default_value)) : _mia_evaluator_zero(e, field->type, node);
			}
			return _mia_evaluator_list_value(list);
		}
		default:
			_mia_evaluator_not_constant(e, node, "`%s` has no zero value", mia_checker_name_of(e->checker, type));
	}
}

static mia_evaluator_value_s _mia_evaluator_constant_default(mia_evaluator_s* const e, const mia_ast_node_s* const node) {
	// evaluated outside of the current call: defaults use no variables.
	const size_t frames = e->frames.count;
	mia_vector_push(&e->frames, NULL);
	const mia_evaluator_value_s value = _mia_evaluator_expression(e, node);
	e->frames.count = frames;
	return value;
}

// --- calls -------------------------------------------------------------------------------

static mia_evaluator_value_s _mia_evaluator_call(mia_evaluator_s* const e, const mia_evaluator_value_s function, mia_evaluator_value_s* const arguments, const size_t count,
												 const mia_ast_node_s* const node) {
	if (e->quiet) _mia_evaluator_not_constant(e, node, "calls a function");
	if (function.kind != mia_evaluator_value_function) _mia_evaluator_broken(e);
	const mia_ast_nodes_s* parameters = NULL;
	const mia_ast_node_s* body = NULL;
	const char* name = NULL;
	bool returns_value = false;
	if (function.as.function.lambda != NULL) {
		const mia_ast_node_s* const lambda = function.as.function.lambda;
		parameters = &lambda->as.lambda.parameters;
		body = lambda->as.lambda.body;
		name = "a lambda";
		returns_value = lambda->as.lambda.result != NULL;
	}
	else {
		mia_program_instance_s* const instance = function.as.function.instance;
		const mia_program_function_s* const info = instance->function;
		name = _mia_evaluator_text(e, "`%s`", mia_checker_instance_name(e->checker, instance));
		if (mia_program_function_is_plugin(info)) {
			_mia_evaluator_not_constant(e, node, "calls %s, a plugin function, which cannot run at compile time", name);
		}
		if (mia_program_instance_pending == instance->state) mia_checker_check_instance(e->checker, instance);
		if (instance->state != mia_program_instance_checked) _mia_evaluator_not_constant(e, node, "calls %s while it is still being checked", name);
		parameters = &instance->node->as.function.parameters;
		body = instance->node->as.function.body;
		returns_value = instance->result != NULL;
		if (NULL == body) _mia_evaluator_not_constant(e, node, "calls %s, which has no body", name);
	}
	if (e->frames.count >= _mia_evaluator_call_depth_limit) {
		_mia_evaluator_fail(e, mia_diag_E0705, node, "more than 100 nested calls", "compile-time evaluation recurses too deeply");
	}
	mia_evaluator_frame_s* const frame = mia_arena_new(_mia_evaluator_arena(e), mia_evaluator_frame_s);
	for (size_t i = 0; i < parameters->count; ++i) {
		const mia_ast_node_s* const parameter = parameters->data[i];
		// a missing argument without a default: a call the checker reported
		if (NULL == parameter->note.variable || (i >= count && NULL == parameter->as.param.default_value)) _mia_evaluator_broken(e);
		const mia_evaluator_value_s value = (i < count) ? arguments[i] : _mia_evaluator_copy(e, _mia_evaluator_constant_default(e, parameter->as.param.default_value));
		_mia_evaluator_bind(e, frame, parameter->note.variable, value);
	}
	const size_t depth = e->frames.count;
	mia_vector_push(&e->frames, frame);
	_mia_evaluator_running(e, node, name);
	const _mia_evaluator_signal_s signal = _mia_evaluator_block(e, body);
	_mia_evaluator_stopped(e);
	e->frames.count = depth;
	if (_mia_evaluator_flow_return == signal.flow) return signal.value;
	if (_mia_evaluator_flow_break == signal.flow || _mia_evaluator_flow_continue == signal.flow) {
		_mia_evaluator_not_constant(e, node, "%s uses `break` or `continue` outside of a loop", name);
	}
	if (returns_value) _mia_evaluator_not_constant(e, node, "%s ended without returning a value", name);
	return (mia_evaluator_value_s) { .kind = mia_evaluator_value_nothing, };
}

// --- expressions ---------------------------------------------------------------------------

static bool _mia_evaluator_truthy(mia_evaluator_s* const e, const mia_evaluator_value_s value) {
	if (mia_evaluator_value_boolean == value.kind) return value.as.boolean;
	if (mia_evaluator_value_integer == value.kind) return !mia_int128_is_zero(value.as.integer);
	_mia_evaluator_broken(e);
}

static mia_evaluator_value_s _mia_evaluator_symbol(mia_evaluator_s* const e, const mia_ast_node_s* const node) {
	if (node->note.variable != NULL) return *_mia_evaluator_variable_cell(e, node, node->note.variable);
	mia_program_constant_s* const constant = node->note.constant;
	if (constant != NULL) {
		(void)mia_checker_constant_type(e->checker, constant);
		if (NULL == constant->value) {
			_mia_evaluator_not_constant(e, node, "`" mia_view_fmt "` has no compile-time value", mia_view_arg(constant->node->as.constant.name));
		}
		return _mia_evaluator_copy(e, *constant->value);
	}
	if (node->note.instance != NULL) return (mia_evaluator_value_s) { .kind = mia_evaluator_value_function, .as.function = { .instance = node->note.instance, }, };
	_mia_evaluator_not_constant(e, node, "not known at compile time");
}

static double _mia_evaluator_as_real(const mia_evaluator_value_s value) {
	return (mia_evaluator_value_integer == value.kind) ? mia_int128_to_double(value.as.integer) : value.as.real;
}

static mia_evaluator_value_s _mia_evaluator_binary_values(mia_evaluator_s* const e, const mia_ast_node_s* const node, const mia_ast_operator_e operation, const mia_evaluator_value_s left,
														  const mia_evaluator_value_s right, const mia_type_s* const operand_type) {
	if (mia_ast_operator_equal == operation || mia_ast_operator_not_equal == operation) {
		bool equal = false;
		if (mia_evaluator_value_function == left.kind && mia_evaluator_value_function == right.kind) {
			equal = left.as.function.instance == right.as.function.instance && left.as.function.lambda == right.as.function.lambda;
		}
		else if (mia_evaluator_value_real == left.kind || mia_evaluator_value_real == right.kind) {
			equal = _mia_evaluator_as_real(left) == _mia_evaluator_as_real(right);
		}
		else if (mia_evaluator_value_boolean == left.kind && mia_evaluator_value_boolean == right.kind) {
			equal = left.as.boolean == right.as.boolean;
		}
		else if (mia_evaluator_value_integer == left.kind && mia_evaluator_value_integer == right.kind) {
			equal = mia_int128_equal(left.as.integer, right.as.integer);
		}
		else {
			_mia_evaluator_broken(e);
		}
		return _mia_evaluator_boolean((mia_ast_operator_equal == operation) == equal);
	}
	if (operation >= mia_ast_operator_less && operation <= mia_ast_operator_greater_equal) {
		if (mia_evaluator_value_integer == left.kind && mia_evaluator_value_integer == right.kind) {
			const int order = mia_int128_compare(left.as.integer, right.as.integer);
			switch (operation) {
				case mia_ast_operator_less:    return _mia_evaluator_boolean(order < 0);
				case mia_ast_operator_greater: return _mia_evaluator_boolean(order > 0);
				case mia_ast_operator_less_equal: return _mia_evaluator_boolean(order <= 0);
				default:                  return _mia_evaluator_boolean(order >= 0);
			}
		}
		if ((mia_evaluator_value_integer != left.kind && mia_evaluator_value_real != left.kind) || (mia_evaluator_value_integer != right.kind && mia_evaluator_value_real != right.kind)) _mia_evaluator_broken(e);
		const double a = _mia_evaluator_as_real(left), b = _mia_evaluator_as_real(right);
		switch (operation) {
			case mia_ast_operator_less:    return _mia_evaluator_boolean(a < b);
			case mia_ast_operator_greater: return _mia_evaluator_boolean(a > b);
			case mia_ast_operator_less_equal: return _mia_evaluator_boolean(a <= b);
			default:                  return _mia_evaluator_boolean(a >= b);
		}
	}
	const mia_type_s* const node_type = node->note.type;
	const mia_type_s* const type = (node_type != NULL && mia_type_scalar == node_type->kind) ? node_type : operand_type;
	if (mia_evaluator_value_integer == left.kind && mia_evaluator_value_integer == right.kind) {
		if ((mia_ast_operator_divide == operation || mia_ast_operator_remainder == operation) && mia_int128_is_zero(right.as.integer)) {
			_mia_evaluator_fail(e, mia_diag_E0704, node, "the divisor is zero at compile time", "division by zero");
		}
		mia_int128_s result = mia_int128_zero();
		bool overflow = false;
		if (!mia_evaluator_integer_arithmetic(operation, left.as.integer, right.as.integer, &result, &overflow)) _mia_evaluator_broken(e);
		if (overflow && (NULL == type || mia_type_scalar != type->kind)) {
			// untyped constants are exact: one that leaves 128 bits has no value
			_mia_evaluator_fail(e, mia_diag_E0511, node, "more than 128 bits", "the constant does not fit in 128 bits (-2^127 to 2^127 - 1)");
		}
		if (type != NULL && mia_type_scalar == type->kind && 'f' == mia_scalar_kind(type->as.scalar)) return _mia_evaluator_wrap(_mia_evaluator_integer(result), type);
		return (type != NULL && mia_type_scalar == type->kind) ? _mia_evaluator_wrap(_mia_evaluator_integer(result), type) : _mia_evaluator_integer(result);
	}
	if ((mia_evaluator_value_integer != left.kind && mia_evaluator_value_real != left.kind) || (mia_evaluator_value_integer != right.kind && mia_evaluator_value_real != right.kind)) _mia_evaluator_broken(e);
	const double a = _mia_evaluator_as_real(left), b = _mia_evaluator_as_real(right);
	double result = 0;
	switch (operation) {
		case mia_ast_operator_add:      result = a + b; break;
		case mia_ast_operator_subtract: result = a - b; break;
		case mia_ast_operator_multiply: result = a * b; break;
		case mia_ast_operator_divide:
			if (0 == b) result = (0 == a || isnan(a)) ? NAN : copysign(INFINITY, a) * copysign(1.0, b);
			else result = a / b;
			break;
		default: _mia_evaluator_broken(e);
	}
	return (type != NULL && mia_type_scalar == type->kind) ? _mia_evaluator_wrap(_mia_evaluator_real(result), type) : _mia_evaluator_real(result);
}

static mia_evaluator_value_s _mia_evaluator_composite(mia_evaluator_s* const e, const mia_ast_node_s* const node) {
	const mia_type_s* const type = node->note.type;
	if (NULL == type) _mia_evaluator_broken(e);
	const mia_ast_nodes_s* const entries = &node->as.composite.entries;
	if (0 == entries->count) return _mia_evaluator_zero(e, type, node);
	if (mia_type_struct == type->kind) {
		mia_evaluator_value_s value = _mia_evaluator_zero(e, type, node);
		const mia_program_layout_s* const layout = mia_checker_layout(e->checker, type);
		for (size_t i = 0; i < entries->count; ++i) {
			const mia_program_field_s* const field = mia_checker_layout_field(layout, entries->data[i]->as.entry.name);
			if (NULL == field) _mia_evaluator_broken(e);
			value.as.list->items[field->index] = _mia_evaluator_copy(e, _mia_evaluator_expression(e, entries->data[i]->as.entry.value));
		}
		return value;
	}
	mia_evaluator_list_s* const list = _mia_evaluator_list_new(e, entries->count, node);
	for (size_t i = 0; i < entries->count; ++i) list->items[i] = _mia_evaluator_copy(e, _mia_evaluator_expression(e, entries->data[i]->as.entry.value));
	if (mia_type_slice == type->kind) return _mia_evaluator_slice_value(list, 0, (int64_t)list->count);
	return _mia_evaluator_list_value(list);
}

static mia_evaluator_value_s _mia_evaluator_call_expression(mia_evaluator_s* const e, const mia_ast_node_s* const node) {
	mia_program_instance_s* const instance = node->note.instance;
	const mia_evaluator_value_s function = (instance != NULL)
		? (mia_evaluator_value_s) { .kind = mia_evaluator_value_function, .as.function = { .instance = instance, }, }
		: _mia_evaluator_expression(e, node->as.call.callee);
	const mia_type_s* const* parameters = NULL;
	size_t count = 0;
	if (instance != NULL) {
		parameters = instance->parameters.data;
		count = instance->parameters.count;
	}
	else if (node->as.call.callee->note.type != NULL && mia_type_function == node->as.call.callee->note.type->kind) {
		parameters = node->as.call.callee->note.type->as.function.parameters;
		count = node->as.call.callee->note.type->as.function.count;
	}
	const mia_ast_nodes_s* const arguments = &node->as.call.arguments;
	mia_evaluator_value_s* const values = mia_arena_alloc(_mia_evaluator_arena(e), (arguments->count + 1) * sizeof(mia_evaluator_value_s));
	size_t used = 0;
	for (size_t i = 0; i < arguments->count && i < count; ++i) {
		const mia_ast_node_s* const argument = arguments->data[i];
		if (mia_ast_node_reference == argument->kind) {
			values[used++] = (mia_evaluator_value_s) { .kind = mia_evaluator_value_cell, .as.cell = _mia_evaluator_locate(e, argument->as.reference.operand), };
		}
		else {
			values[used++] = _mia_evaluator_copy(e, _mia_evaluator_expression(e, argument));
		}
	}
	mia_unused(parameters);
	return _mia_evaluator_call(e, function, values, used, node);
}

static mia_evaluator_value_s _mia_evaluator_expression(mia_evaluator_s* const e, const mia_ast_node_s* const node) {
	_mia_evaluator_step(e, node);
	switch (node->kind) {
		case mia_ast_node_integer: {
			const mia_type_s* const type = node->note.type;
			if (type != NULL && mia_type_is_float(type)) return _mia_evaluator_wrap(_mia_evaluator_real(mia_int128_to_double_unsigned(node->as.integer.value)), type);
			return _mia_evaluator_integer(node->as.integer.value);
		}
		case mia_ast_node_float:
			return _mia_evaluator_wrap(_mia_evaluator_real(node->as.floating.value), node->note.type);
		case mia_ast_node_char:
			return _mia_evaluator_int(node->as.character.value);
		case mia_ast_node_string: {
			const mia_view_s data = node->as.string.data;
			mia_evaluator_list_s* const list = _mia_evaluator_list_new(e, data.length, node);
			for (size_t i = 0; i < data.length; ++i) list->items[i] = _mia_evaluator_int((uint8_t)data.data[i]);
			return _mia_evaluator_list_value(list);
		}
		case mia_ast_node_bool:  return _mia_evaluator_boolean(node->as.boolean.value);
		case mia_ast_node_paren: return _mia_evaluator_expression(e, node->as.paren.inner);
		case mia_ast_node_name:  return _mia_evaluator_symbol(e, node);
		case mia_ast_node_member: {
			if (NULL == node->note.field) return _mia_evaluator_symbol(e, node);
			const mia_evaluator_value_s target = _mia_evaluator_expression(e, node->as.member.target);
			if (target.kind != mia_evaluator_value_list || node->note.field->index >= target.as.list->count) _mia_evaluator_broken(e);
			return target.as.list->items[node->note.field->index];
		}
		case mia_ast_node_specialize:
			if (NULL == node->note.instance) _mia_evaluator_broken(e);
			return (mia_evaluator_value_s) { .kind = mia_evaluator_value_function, .as.function = { .instance = node->note.instance, }, };
		case mia_ast_node_lambda:
			return (mia_evaluator_value_s) { .kind = mia_evaluator_value_function, .as.function = { .lambda = (mia_ast_node_s*)(uintptr_t)node, }, };
		case mia_ast_node_sizeof:
			return _mia_evaluator_int((int64_t)node->note.size);
		case mia_ast_node_lenof: {
			const mia_evaluator_value_s value = _mia_evaluator_expression(e, node->as.lenof.operand);
			if (mia_evaluator_value_slice == value.kind) return _mia_evaluator_int(value.as.slice.length);
			if (mia_evaluator_value_list == value.kind) return _mia_evaluator_int((int64_t)value.as.list->count);
			_mia_evaluator_broken(e);
		}
		case mia_ast_node_cast: {
			const mia_evaluator_value_s value = _mia_evaluator_expression(e, node->as.cast.operand);
			const mia_type_s* const target = node->note.type;
			if (NULL == target || target->kind != mia_type_scalar) return value;
			const char kind = mia_scalar_kind(target->as.scalar);
			if ('i' == kind) {
				if (mia_evaluator_value_real == value.kind) {
					if (!isfinite(value.as.real)) {
						mia_string_s shown = {0};
						mia_format_float_repr(&shown, value.as.real);
						const char* const text = _mia_evaluator_text(e, "cannot convert `%s` to `%s`", mia_string_cstr(&shown), mia_checker_name_of(e->checker, target));
						mia_string_free(&shown);
						_mia_evaluator_fail(e, mia_diag_E0510, node, "not a finite number", "%s", text);
					}
					return _mia_evaluator_wrap(_mia_evaluator_integer(mia_int128_from_double(value.as.real)), target);
				}
				if (mia_evaluator_value_integer == value.kind) return _mia_evaluator_wrap(value, target);
				if (mia_evaluator_value_boolean == value.kind) return _mia_evaluator_wrap(_mia_evaluator_int(value.as.boolean ? 1 : 0), target);
				_mia_evaluator_broken(e);
			}
			if ('f' == kind) return _mia_evaluator_wrap(_mia_evaluator_real(_mia_evaluator_as_real(value)), target);
			return value;
		}
		case mia_ast_node_unary: {
			const mia_evaluator_value_s value = _mia_evaluator_expression(e, node->as.unary.operand);
			const mia_type_s* const type = node->note.type;
			switch (node->as.unary.operation) {
				case mia_ast_operator_not: return _mia_evaluator_boolean(!_mia_evaluator_truthy(e, value));
				case mia_ast_operator_negate:
					if (mia_evaluator_value_integer == value.kind) {
						const bool untyped = NULL == type || mia_type_scalar != type->kind;
						if (untyped && mia_int128_equal(value.as.integer, mia_int128_from_halves((uint64_t)1 << 63, 0))) {
							_mia_evaluator_fail(e, mia_diag_E0511, node, "more than 128 bits", "the constant does not fit in 128 bits (-2^127 to 2^127 - 1)");
						}
						return _mia_evaluator_wrap(_mia_evaluator_integer(mia_int128_negate(value.as.integer)), type);
					}
					if (mia_evaluator_value_real == value.kind) return _mia_evaluator_wrap(_mia_evaluator_real(-value.as.real), type);
					_mia_evaluator_broken(e);
				default:
					if (mia_evaluator_value_integer != value.kind) _mia_evaluator_broken(e);
					return _mia_evaluator_wrap(_mia_evaluator_integer(mia_int128_not(value.as.integer)), type);
			}
		}
		case mia_ast_node_binary: {
			const mia_ast_operator_e operation = node->as.binary.operation;
			if (mia_ast_operator_and == operation) {
				if (!_mia_evaluator_truthy(e, _mia_evaluator_expression(e, node->as.binary.left))) return _mia_evaluator_boolean(false);
				return _mia_evaluator_boolean(_mia_evaluator_truthy(e, _mia_evaluator_expression(e, node->as.binary.right)));
			}
			if (mia_ast_operator_or == operation) {
				if (_mia_evaluator_truthy(e, _mia_evaluator_expression(e, node->as.binary.left))) return _mia_evaluator_boolean(true);
				return _mia_evaluator_boolean(_mia_evaluator_truthy(e, _mia_evaluator_expression(e, node->as.binary.right)));
			}
			const mia_evaluator_value_s left = _mia_evaluator_expression(e, node->as.binary.left);
			const mia_evaluator_value_s right = _mia_evaluator_expression(e, node->as.binary.right);
			return _mia_evaluator_binary_values(e, node, operation, left, right, node->as.binary.left->note.type);
		}
		case mia_ast_node_index: {
			const mia_evaluator_value_s target = _mia_evaluator_expression(e, node->as.index.target);
			return *_mia_evaluator_element(e, node->as.index.index, target);
		}
		case mia_ast_node_slice_of: {
			const mia_evaluator_value_s target = _mia_evaluator_location_value(e, node->as.slice_of.target);
			const _mia_evaluator_storage_s storage = _mia_evaluator_storage(e, target);
			int64_t low = 0, high = storage.length;
			if (node->as.slice_of.low != NULL) {
				const mia_evaluator_value_s value = _mia_evaluator_expression(e, node->as.slice_of.low);
				if (value.kind != mia_evaluator_value_integer) _mia_evaluator_broken(e);
				low = mia_int128_to_int(value.as.integer);
			}
			if (node->as.slice_of.high != NULL) {
				const mia_evaluator_value_s value = _mia_evaluator_expression(e, node->as.slice_of.high);
				if (value.kind != mia_evaluator_value_integer) _mia_evaluator_broken(e);
				high = mia_int128_to_int(value.as.integer) + (node->as.slice_of.inclusive ? 1 : 0);
			}
			if (!(0 <= low && low <= high && high <= storage.length)) {
				_mia_evaluator_fail(e, mia_diag_E0706, node, _mia_evaluator_text(e, "the length is %lld", (long long)storage.length), "slice %lld..%lld is out of bounds",
					(long long)low, (long long)high);
			}
			return _mia_evaluator_slice_value(storage.list, storage.start + low, high - low);
		}
		case mia_ast_node_composite:
			return _mia_evaluator_composite(e, node);
		case mia_ast_node_assert: {
			if (!_mia_evaluator_truthy(e, _mia_evaluator_expression(e, node->as.assert_statement.condition))) {
				const mia_ast_node_s* const message = node->as.assert_statement.message;
				if (message != NULL && mia_ast_node_string == message->kind) {
					_mia_evaluator_fail(e, mia_diag_E0703, node, "false at compile time", "assertion failed during compile-time evaluation: " mia_view_fmt,
						mia_view_arg(message->as.string.data));
				}
				_mia_evaluator_fail(e, mia_diag_E0703, node, "false at compile time", "assertion failed during compile-time evaluation");
			}
			return (mia_evaluator_value_s) { .kind = mia_evaluator_value_nothing, };
		}
		case mia_ast_node_reference:
			_mia_evaluator_not_constant(e, node, "a reference is not a value");
		case mia_ast_node_call:
			return _mia_evaluator_call_expression(e, node);
		default:
			_mia_evaluator_not_constant(e, node, "not known at compile time");
	}
}

// --- statements -----------------------------------------------------------------------------

static _mia_evaluator_signal_s _mia_evaluator_normal(void) {
	return (_mia_evaluator_signal_s) { .flow = _mia_evaluator_flow_normal, };
}

static _mia_evaluator_signal_s _mia_evaluator_statement(mia_evaluator_s* const e, const mia_ast_node_s* const node);

static _mia_evaluator_signal_s _mia_evaluator_block(mia_evaluator_s* const e, const mia_ast_node_s* const node) {
	for (size_t i = 0; i < node->as.block.statements.count; ++i) {
		const _mia_evaluator_signal_s signal = _mia_evaluator_statement(e, node->as.block.statements.data[i]);
		if (signal.flow != _mia_evaluator_flow_normal) return signal;
	}
	return _mia_evaluator_normal();
}

static _mia_evaluator_signal_s _mia_evaluator_statement(mia_evaluator_s* const e, const mia_ast_node_s* const node) {
	_mia_evaluator_step(e, node);
	mia_evaluator_frame_s* const frame = _mia_evaluator_frame(e);
	switch (node->kind) {
		case mia_ast_node_let: {
			if (NULL == frame || NULL == node->note.variable) _mia_evaluator_broken(e);
			const mia_evaluator_value_s value = (node->as.let.value != NULL) ? _mia_evaluator_expression(e, node->as.let.value) : _mia_evaluator_zero(e, node->note.variable->type, node);
			_mia_evaluator_bind(e, frame, node->note.variable, _mia_evaluator_copy(e, value));
			return _mia_evaluator_normal();
		}
		case mia_ast_node_assign: {
			const mia_ast_node_s* const target = node->as.assign.target;
			if (mia_ast_node_name == target->kind && mia_view_eq_cstr(target->as.name.name, "_")) {
				(void)_mia_evaluator_expression(e, node->as.assign.value);
			}
			else if (mia_ast_operator_assign == node->as.assign.operation) {
				mia_evaluator_value_s* const cell = _mia_evaluator_locate(e, target);
				*cell = _mia_evaluator_copy(e, _mia_evaluator_expression(e, node->as.assign.value));
			}
			else {
				mia_evaluator_value_s* const cell = _mia_evaluator_locate(e, target);
				const mia_evaluator_value_s right = _mia_evaluator_expression(e, node->as.assign.value);
				*cell = _mia_evaluator_binary_values(e, node, node->as.assign.operation, *cell, right, target->note.type);
			}
			return _mia_evaluator_normal();
		}
		case mia_ast_node_expression_statement:
			(void)_mia_evaluator_expression(e, node->as.expression_statement.expression);
			return _mia_evaluator_normal();
		case mia_ast_node_if:
			if (_mia_evaluator_truthy(e, _mia_evaluator_expression(e, node->as.if_statement.condition))) return _mia_evaluator_block(e, node->as.if_statement.then);
			if (NULL == node->as.if_statement.otherwise) return _mia_evaluator_normal();
			if (mia_ast_node_if == node->as.if_statement.otherwise->kind) return _mia_evaluator_statement(e, node->as.if_statement.otherwise);
			return _mia_evaluator_block(e, node->as.if_statement.otherwise);
		case mia_ast_node_while: {
			_mia_evaluator_running(e, node, NULL);
			_mia_evaluator_signal_s result = _mia_evaluator_normal();
			while (_mia_evaluator_truthy(e, _mia_evaluator_expression(e, node->as.while_statement.condition))) {
				const _mia_evaluator_signal_s signal = _mia_evaluator_block(e, node->as.while_statement.body);
				if (_mia_evaluator_flow_break == signal.flow) break;
				if (_mia_evaluator_flow_return == signal.flow) {
					result = signal;
					break;
				}
			}
			_mia_evaluator_stopped(e);
			return result;
		}
		case mia_ast_node_for: {
			const mia_evaluator_value_s start = _mia_evaluator_expression(e, node->as.for_statement.start);
			const mia_evaluator_value_s end = _mia_evaluator_expression(e, node->as.for_statement.end);
			if (start.kind != mia_evaluator_value_integer || end.kind != mia_evaluator_value_integer || NULL == frame || NULL == node->note.variable) _mia_evaluator_broken(e);
			const mia_int128_s step = mia_int128_from_int(mia_int128_less(end.as.integer, start.as.integer) ? -1 : 1);
			const mia_int128_s stop = node->as.for_statement.inclusive ? mia_int128_add(end.as.integer, step) : end.as.integer;
			_mia_evaluator_running(e, node, NULL);
			_mia_evaluator_signal_s result = _mia_evaluator_normal();
			for (mia_int128_s value = start.as.integer; !mia_int128_equal(value, stop); value = mia_int128_add(value, step)) {
				_mia_evaluator_bind(e, frame, node->note.variable, _mia_evaluator_integer(value));
				const _mia_evaluator_signal_s signal = _mia_evaluator_block(e, node->as.for_statement.body);
				if (_mia_evaluator_flow_break == signal.flow) break;
				if (_mia_evaluator_flow_return == signal.flow) {
					result = signal;
					break;
				}
				_mia_evaluator_step(e, node);
			}
			_mia_evaluator_stopped(e);
			return result;
		}
		case mia_ast_node_return: {
			_mia_evaluator_signal_s signal = { .flow = _mia_evaluator_flow_return, };
			signal.value = (node->as.return_statement.value != NULL) ? _mia_evaluator_copy(e, _mia_evaluator_expression(e, node->as.return_statement.value))
				: (mia_evaluator_value_s) { .kind = mia_evaluator_value_nothing, };
			return signal;
		}
		case mia_ast_node_break:    return (_mia_evaluator_signal_s) { .flow = _mia_evaluator_flow_break, };
		case mia_ast_node_continue: return (_mia_evaluator_signal_s) { .flow = _mia_evaluator_flow_continue, };
		case mia_ast_node_block:    return _mia_evaluator_block(e, node);
		case mia_ast_node_declaration_statement: return _mia_evaluator_normal();
		default:
			_mia_evaluator_not_constant(e, node, "cannot run at compile time");
	}
}

// --- entry ------------------------------------------------------------------------------------

static mia_evaluator_result_s _mia_evaluator_run(mia_evaluator_s* const e, const mia_ast_node_s* const node) {
	jmp_buf here;
	jmp_buf* const outer = e->stop;
	const size_t frames = e->frames.count, running = e->running.count;
	// cppcheck-suppress autoVariables ; 'outer' is put back before it returns
	e->stop = &here;
	mia_evaluator_result_s result = { .status = mia_evaluator_ok, };
	if (0 == setjmp(here)) result.value = _mia_evaluator_expression(e, node);
	else result = e->problem;
	e->stop = outer;
	e->frames.count = frames;
	e->running.count = running;
	return result;
}

mia_evaluator_result_s mia_evaluator_evaluate(mia_evaluator_s* const e, const mia_ast_node_s* const node, const bool quiet) {
	if (e->active) {
		// a nested evaluation (a constant needed while evaluating another one).
		const bool saved_quiet = e->quiet;
		const size_t frames = e->frames.count;
		mia_vector_push(&e->frames, NULL);
		e->quiet = quiet;
		const mia_evaluator_result_s result = _mia_evaluator_run(e, node);
		e->quiet = saved_quiet;
		e->frames.count = frames;
		return result;
	}
	e->active = true;
	e->frames.count = 0;
	e->running.count = 0;
	e->cells = 0;
	e->steps = 0;
	e->quiet = quiet;
	const mia_evaluator_result_s result = _mia_evaluator_run(e, node);
	e->active = false;
	return result;
}
