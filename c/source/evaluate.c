
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

#include "check.h"
#include "mc/format.h"

#include <math.h>
#include <string.h>

#define _mc_call_depth_limit 100

typedef struct {
	const mc_variable_s* variable;
	mc_cvalue_s*         cell    ;
} _mc_slot_s;

struct mc_frame_s {
	mc_vector_of(_mc_slot_s) slots;
};

typedef enum {
	_mc_flow_normal  ,
	_mc_flow_return  ,
	_mc_flow_break   ,
	_mc_flow_continue,
} _mc_flow_e;

typedef struct {
	_mc_flow_e  flow ;
	mc_cvalue_s value;
} _mc_signal_s;

static mc_cvalue_s _mc_expression(mc_evaluator_s* const e, const mc_node_s* const node);
static _mc_signal_s _mc_block(mc_evaluator_s* const e, const mc_node_s* const node);

// --- values --------------------------------------------------------------------------

static mc_arena_s* _mc_arena(const mc_evaluator_s* const e) {
	return e->checker->arena;
}

static mc_cvalue_s _mc_integer(const __int128 value) {
	return (mc_cvalue_s) { .kind = mc_cvalue_integer, .as.integer = value, };
}

static mc_cvalue_s _mc_real(const double value) {
	return (mc_cvalue_s) { .kind = mc_cvalue_real, .as.real = value, };
}

static mc_cvalue_s _mc_boolean(const bool value) {
	return (mc_cvalue_s) { .kind = mc_cvalue_boolean, .as.boolean = value, };
}

static mc_clist_s* _mc_list_new(mc_evaluator_s* const e, const size_t count) {
	mc_clist_s* const list = mc_arena_alloc(_mc_arena(e), sizeof(mc_clist_s) + count * sizeof(mc_cvalue_s));
	list->count = count;
	return list;
}

static mc_cvalue_s _mc_list_value(mc_clist_s* const list) {
	return (mc_cvalue_s) { .kind = mc_cvalue_list, .as.list = list, };
}

static mc_cvalue_s _mc_slice_value(mc_clist_s* const list, const int64_t start, const int64_t length) {
	return (mc_cvalue_s) { .kind = mc_cvalue_slice, .as.slice = { .list = list, .start = start, .length = length, }, };
}

mc_cvalue_s mc_cvalue_copy(mc_checker_s* const checker, const mc_cvalue_s value) {
	if (mc_cvalue_list != value.kind) return value;
	mc_clist_s* const list = mc_arena_alloc(checker->arena, sizeof(mc_clist_s) + value.as.list->count * sizeof(mc_cvalue_s));
	list->count = value.as.list->count;
	for (size_t i = 0; i < list->count; ++i) list->items[i] = mc_cvalue_copy(checker, value.as.list->items[i]);
	return _mc_list_value(list);
}

static mc_cvalue_s _mc_copy(mc_evaluator_s* const e, const mc_cvalue_s value) {
	return mc_cvalue_copy(e->checker, value);
}

__int128 mc_integer_wrap(const __int128 value, const mc_type_s* const type) {
	const uint32_t bits = mc_scalar_bits(type->as.scalar);
	const unsigned __int128 mask = (bits >= 128) ? ~(unsigned __int128)0 : (((unsigned __int128)1 << bits) - 1);
	unsigned __int128 wrapped = (unsigned __int128)value & mask;
	if (mc_scalar_signed(type->as.scalar) && bits < 128 && (wrapped >> (bits - 1)) != 0) wrapped -= (unsigned __int128)1 << bits;
	return (__int128)wrapped;
}

double mc_float_wrap(const double value, const mc_type_s* const type) {
	if (32 == mc_scalar_bits(type->as.scalar) && isfinite(value)) {
		const float rounded = (float)value;
		return (double)rounded;
	}
	return value;
}

/// 'value' as a value of 'type': integers wrap around, f32 rounds.
static mc_cvalue_s _mc_wrap(const mc_cvalue_s value, const mc_type_s* const type) {
	if (NULL == type || type->kind != mc_type_scalar) return value;
	const char kind = mc_scalar_kind(type->as.scalar);
	if ('i' == kind && mc_cvalue_integer == value.kind) return _mc_integer(mc_integer_wrap(value.as.integer, type));
	if ('f' == kind) {
		const double real = (mc_cvalue_integer == value.kind) ? (double)value.as.integer : value.as.real;
		return _mc_real(mc_float_wrap(real, type));
	}
	return value;
}

static __int128 _mc_divide(const __int128 left, const __int128 right) {
	const __int128 a = left < 0 ? -left : left, b = right < 0 ? -right : right;
	const __int128 quotient = a / b;
	return ((left >= 0) == (right >= 0)) ? quotient : -quotient;
}

bool mc_integer_arithmetic(const mc_operator_e operator_, const __int128 left, const __int128 right, __int128* const result) {
	switch (operator_) {
		case mc_operator_add:       *result = left + right; return true;
		case mc_operator_subtract:  *result = left - right; return true;
		case mc_operator_multiply:  *result = left * right; return true;
		case mc_operator_divide:
			if (0 == right) return false;
			*result = _mc_divide(left, right);
			return true;
		case mc_operator_remainder:
			if (0 == right) return false;
			*result = left - right * _mc_divide(left, right);
			return true;
		case mc_operator_bit_and:   *result = left & right; return true;
		case mc_operator_bit_or:    *result = left | right; return true;
		case mc_operator_bit_xor:   *result = left ^ right; return true;
		case mc_operator_shift_left:
			*result = (right >= 120) ? 0 : (__int128)((unsigned __int128)left << right);
			return true;
		case mc_operator_shift_right:
			*result = (right >= 127) ? (left < 0 ? -1 : 0) : (left >> right);
			return true;
		default:
			return false;
	}
}

// --- stopping ------------------------------------------------------------------------

mc_noreturn static void _mc_not_constant(mc_evaluator_s* const e, const mc_node_s* const node, const char* const format, ...) mc_printf_like(3, 4);

mc_noreturn static void _mc_not_constant(mc_evaluator_s* const e, const mc_node_s* const node, const char* const format, ...) {
	va_list args; va_start(args, format);
	char* const reason = mc_vformat(format, args);
	va_end(args);
	e->problem = (mc_evaluation_s) {
		.status = mc_evaluation_not_constant, .node = node, .reason = mc_arena_strndup(_mc_arena(e), reason, strlen(reason)),
	};
	mc_free(reason);
	longjmp(*e->stop, 1);
}

/// reports an error (unless quiet) and stops the evaluation.
mc_noreturn static void _mc_fail(mc_evaluator_s* const e, const mc_code_e code, const mc_node_s* const node, const char* const label,
								 const char* const format, ...) mc_printf_like(5, 6);

mc_noreturn static void _mc_fail(mc_evaluator_s* const e, const mc_code_e code, const mc_node_s* const node, const char* const label,
								 const char* const format, ...) {
	if (!e->quiet) {
		va_list args; va_start(args, format);
		char* const message = mc_vformat(format, args);
		va_end(args);
		mc_label_main(mc_error(e->checker->diag, code, node->span, "%s", message), "%s", label);
		mc_free(message);
	}
	e->problem = (mc_evaluation_s) { .status = mc_evaluation_failed, };
	longjmp(*e->stop, 1);
}

/// a value of an unexpected kind: code with (reported) errors.
mc_noreturn static void _mc_broken(mc_evaluator_s* const e) {
	e->problem = (mc_evaluation_s) { .status = mc_evaluation_failed, };
	longjmp(*e->stop, 1);
}

/// stops: the evaluation took too many steps. the error points at the
/// innermost loop or call that is still running.
mc_noreturn static void _mc_too_long(mc_evaluator_s* const e, const mc_node_s* const node) {
	if (!e->quiet) {
		const mc_running_s* const innermost = (e->running.count > 0) ? &mc_vector_last(&e->running) : NULL;
		const mc_node_s* const at = (innermost != NULL) ? innermost->node : node;
		mc_diagnostic_s* const diagnostic = mc_error(e->checker->diag, mc_E0705, at->span, "compile-time evaluation takes too long");
		const unsigned long long limit = (unsigned long long)e->limit;
		if (NULL == innermost) {
			mc_label_main(diagnostic, "stopped after %llu steps", limit);
		}
		else if (innermost->function != NULL) {
			mc_label_main(diagnostic, "this call runs more than %llu steps", limit);
			mc_note(diagnostic, "the call of %s runs more than %llu steps", innermost->function, limit);
		}
		else {
			// the function the loop is in: the innermost call around it
			const char* function = NULL;
			for (size_t i = e->running.count; i-- > 0 && NULL == function;) function = e->running.data[i].function;
			mc_label_main(diagnostic, "this loop runs more than %llu steps", limit);
			if (function != NULL) mc_note(diagnostic, "the loop in %s runs more than %llu steps", function, limit);
			else mc_note(diagnostic, "the loop runs more than %llu steps", limit);
		}
		mc_help(diagnostic, "raise the limit with `--eval-steps <n>` if the evaluation is meant to take this long");
	}
	e->problem = (mc_evaluation_s) { .status = mc_evaluation_failed, };
	longjmp(*e->stop, 1);
}

static void _mc_step(mc_evaluator_s* const e, const mc_node_s* const node) {
	e->steps += 1;
	if (e->steps > e->limit) _mc_too_long(e, node);
}

/// a loop or a call starts running (and stops with _mc_stopped).
static void _mc_running(mc_evaluator_s* const e, const mc_node_s* const node, const char* const function) {
	const mc_running_s running = { .node = node, .function = function, };
	mc_vector_push(&e->running, running);
}

static void _mc_stopped(mc_evaluator_s* const e) {
	e->running.count -= 1;
}

// --- frames -------------------------------------------------------------------------

static mc_frame_s* _mc_frame(const mc_evaluator_s* const e) {
	return (e->frames.count > 0) ? e->frames.data[e->frames.count - 1] : NULL;
}

static mc_cvalue_s* _mc_slot(const mc_frame_s* const frame, const mc_variable_s* const variable) {
	if (NULL == frame) return NULL;
	for (size_t i = frame->slots.count; i > 0; --i) if (frame->slots.data[i - 1].variable == variable) return frame->slots.data[i - 1].cell;
	return NULL;
}

static void _mc_bind(mc_evaluator_s* const e, mc_frame_s* const frame, const mc_variable_s* const variable, const mc_cvalue_s value) {
	mc_cvalue_s* const existing = _mc_slot(frame, variable);
	if (existing != NULL) {
		*existing = value;
		return;
	}
	mc_cvalue_s* const cell = mc_arena_new(_mc_arena(e), mc_cvalue_s);
	*cell = value;
	const _mc_slot_s slot = { .variable = variable, .cell = cell, };
	mc_arena_push(_mc_arena(e), &frame->slots, slot);
}

/// the cell of a variable (following a reference argument).
static mc_cvalue_s* _mc_variable_cell(mc_evaluator_s* const e, const mc_node_s* const node, const mc_variable_s* const variable) {
	mc_cvalue_s* const cell = _mc_slot(_mc_frame(e), variable);
	if (NULL == cell) _mc_not_constant(e, node, "uses the variable `" mc_view_fmt "`", mc_view_arg(variable->name));
	return (mc_cvalue_cell == cell->kind) ? cell->as.cell : cell;
}

// --- storage ---------------------------------------------------------------------------

typedef struct {
	mc_clist_s* list  ;
	int64_t     start ;
	int64_t     length;
} _mc_storage_s;

static _mc_storage_s _mc_storage(mc_evaluator_s* const e, const mc_cvalue_s value) {
	if (mc_cvalue_slice == value.kind) return (_mc_storage_s) { value.as.slice.list, value.as.slice.start, value.as.slice.length, };
	if (mc_cvalue_list == value.kind) return (_mc_storage_s) { value.as.list, 0, (int64_t)value.as.list->count, };
	_mc_broken(e);
}

static mc_cvalue_s _mc_location_value(mc_evaluator_s* const e, const mc_node_s* const node);

/// a formatted text in the arena (it survives a stop).
static const char* _mc_text(mc_evaluator_s* const e, const char* const format, ...) mc_printf_like(2, 3);

static const char* _mc_text(mc_evaluator_s* const e, const char* const format, ...) {
	va_list args; va_start(args, format);
	char* const text = mc_vformat(format, args);
	va_end(args);
	const char* const kept = mc_arena_strndup(_mc_arena(e), text, strlen(text));
	mc_free(text);
	return kept;
}

static mc_cvalue_s* _mc_element(mc_evaluator_s* const e, const mc_node_s* const index_node, const mc_cvalue_s target) {
	const _mc_storage_s storage = _mc_storage(e, target);
	const mc_cvalue_s index = _mc_expression(e, index_node);
	if (index.kind != mc_cvalue_integer) _mc_broken(e);
	if (index.as.integer < 0 || index.as.integer >= storage.length) {
		_mc_fail(e, mc_E0706, index_node, _mc_text(e, "the length is %lld", (long long)storage.length), "index %lld is out of bounds",
			(long long)index.as.integer);
	}
	return &storage.list->items[storage.start + (int64_t)index.as.integer];
}

/// the value at a place without copying it (so slices share it).
static mc_cvalue_s _mc_location_value(mc_evaluator_s* const e, const mc_node_s* const node) {
	if (mc_node_paren == node->kind) return _mc_location_value(e, node->as.paren.inner);
	if (mc_node_name == node->kind && node->note.variable != NULL) return *_mc_variable_cell(e, node, node->note.variable);
	if (mc_node_member == node->kind && node->note.field != NULL) {
		const mc_cvalue_s target = _mc_location_value(e, node->as.member.target);
		if (target.kind != mc_cvalue_list || node->note.field->index >= target.as.list->count) _mc_broken(e);
		return target.as.list->items[node->note.field->index];
	}
	if (mc_node_index == node->kind && NULL == node->note.operator_call) {
		return *_mc_element(e, node->as.index.index, _mc_location_value(e, node->as.index.target));
	}
	return _mc_expression(e, node);
}

/// the cell of the place 'node' (a new temporary for other values).
static mc_cvalue_s* _mc_locate(mc_evaluator_s* const e, const mc_node_s* const node) {
	if (mc_node_paren == node->kind) return _mc_locate(e, node->as.paren.inner);
	if (mc_node_name == node->kind && node->note.variable != NULL) return _mc_variable_cell(e, node, node->note.variable);
	if (mc_node_member == node->kind && node->note.field != NULL) {
		const mc_cvalue_s target = _mc_location_value(e, node->as.member.target);
		if (target.kind != mc_cvalue_list || node->note.field->index >= target.as.list->count) _mc_broken(e);
		return &target.as.list->items[node->note.field->index];
	}
	if (mc_node_index == node->kind && NULL == node->note.operator_call) {
		return _mc_element(e, node->as.index.index, _mc_location_value(e, node->as.index.target));
	}
	mc_cvalue_s* const cell = mc_arena_new(_mc_arena(e), mc_cvalue_s);
	*cell = _mc_expression(e, node);
	return cell;
}

// --- zero values -------------------------------------------------------------------------

static mc_cvalue_s _mc_constant_default(mc_evaluator_s* const e, const mc_node_s* const node);

static mc_cvalue_s _mc_zero(mc_evaluator_s* const e, const mc_type_s* const type, const mc_node_s* const node) {
	switch (type->kind) {
		case mc_type_scalar: {
			const char kind = mc_scalar_kind(type->as.scalar);
			if ('b' == kind) return _mc_boolean(false);
			if ('f' == kind) return _mc_real(0.0);
			return _mc_integer(0);
		}
		case mc_type_slice:
			return _mc_slice_value(NULL, 0, 0);
		case mc_type_array: {
			const int64_t length = type->as.array.length > 0 ? type->as.array.length : 0;
			mc_clist_s* const list = _mc_list_new(e, (size_t)length);
			for (int64_t i = 0; i < length; ++i) list->items[i] = _mc_zero(e, type->as.array.element, node);
			return _mc_list_value(list);
		}
		case mc_type_struct: {
			const mc_layout_s* const layout = mc_layout(e->checker, type);
			mc_clist_s* const list = _mc_list_new(e, layout->fields.count);
			for (size_t i = 0; i < layout->fields.count; ++i) {
				const mc_field_s* const field = layout->fields.data[i];
				list->items[i] = (field->default_ != NULL) ? _mc_copy(e, _mc_constant_default(e, field->default_)) : _mc_zero(e, field->type, node);
			}
			return _mc_list_value(list);
		}
		default:
			if (mc_type_is_special(type, mc_special_null)) return _mc_slice_value(NULL, 0, 0);
			_mc_not_constant(e, node, "`%s` has no zero value", mc_name_of(e->checker, type));
	}
}

static mc_cvalue_s _mc_constant_default(mc_evaluator_s* const e, const mc_node_s* const node) {
	// evaluated outside of the current call: defaults use no variables.
	const size_t frames = e->frames.count;
	mc_vector_push(&e->frames, NULL);
	const mc_cvalue_s value = _mc_expression(e, node);
	e->frames.count = frames;
	return value;
}

// --- calls -------------------------------------------------------------------------------

static mc_cvalue_s _mc_call(mc_evaluator_s* const e, const mc_cvalue_s function, mc_cvalue_s* const arguments, const size_t count,
							const mc_node_s* const node) {
	if (e->quiet) _mc_not_constant(e, node, "calls a function");
	if (function.kind != mc_cvalue_function) _mc_broken(e);
	const mc_nodes_s* parameters = NULL;
	const mc_node_s* body = NULL;
	const char* name = NULL;
	bool returns_value = false;
	if (function.as.function.lambda != NULL) {
		const mc_node_s* const lambda = function.as.function.lambda;
		parameters = &lambda->as.lambda.parameters;
		body = lambda->as.lambda.body;
		name = "a lambda";
		returns_value = lambda->as.lambda.result != NULL;
	}
	else {
		mc_instance_s* const instance = function.as.function.instance;
		const mc_function_s* const info = instance->function;
		name = _mc_text(e, "`%s`", mc_instance_name(e->checker, instance));
		if (mc_function_is_plugin(info)) {
			_mc_not_constant(e, node, "calls %s, a plugin function, which cannot run at compile time", name);
		}
		if (mc_instance_pending == instance->state) mc_check_instance(e->checker, instance);
		if (instance->state != mc_instance_checked) _mc_not_constant(e, node, "calls %s while it is still being checked", name);
		parameters = &instance->node->as.function.parameters;
		body = instance->node->as.function.body;
		returns_value = instance->result != NULL;
		if (NULL == body) _mc_not_constant(e, node, "calls %s, which has no body", name);
	}
	if (e->frames.count >= _mc_call_depth_limit) {
		_mc_fail(e, mc_E0705, node, "more than 100 nested calls", "compile-time evaluation recurses too deeply");
	}
	mc_frame_s* const frame = mc_arena_new(_mc_arena(e), mc_frame_s);
	for (size_t i = 0; i < parameters->count; ++i) {
		const mc_node_s* const parameter = parameters->data[i];
		const mc_cvalue_s value = (i < count) ? arguments[i] : _mc_copy(e, _mc_constant_default(e, parameter->as.param.default_));
		if (NULL == parameter->note.variable) _mc_broken(e);
		_mc_bind(e, frame, parameter->note.variable, value);
	}
	const size_t depth = e->frames.count;
	mc_vector_push(&e->frames, frame);
	_mc_running(e, node, name);
	const _mc_signal_s signal = _mc_block(e, body);
	_mc_stopped(e);
	e->frames.count = depth;
	if (_mc_flow_return == signal.flow) return signal.value;
	if (_mc_flow_break == signal.flow || _mc_flow_continue == signal.flow) {
		_mc_not_constant(e, node, "%s uses `break` or `continue` outside of a loop", name);
	}
	if (returns_value) _mc_not_constant(e, node, "%s ended without returning a value", name);
	return (mc_cvalue_s) { .kind = mc_cvalue_nothing, };
}

// --- expressions ---------------------------------------------------------------------------

static bool _mc_truthy(mc_evaluator_s* const e, const mc_cvalue_s value) {
	if (mc_cvalue_boolean == value.kind) return value.as.boolean;
	if (mc_cvalue_integer == value.kind) return value.as.integer != 0;
	_mc_broken(e);
}

static mc_cvalue_s _mc_symbol(mc_evaluator_s* const e, const mc_node_s* const node) {
	if (node->note.variable != NULL) return *_mc_variable_cell(e, node, node->note.variable);
	mc_constant_s* const constant = node->note.constant;
	if (constant != NULL) {
		(void)mc_constant_type(e->checker, constant);
		if (NULL == constant->value) {
			_mc_not_constant(e, node, "`" mc_view_fmt "` has no compile-time value", mc_view_arg(constant->node->as.constant.name));
		}
		return _mc_copy(e, *constant->value);
	}
	if (node->note.instance != NULL) return (mc_cvalue_s) { .kind = mc_cvalue_function, .as.function = { .instance = node->note.instance, }, };
	_mc_not_constant(e, node, "not known at compile time");
}

static mc_cvalue_s _mc_operator_function(mc_evaluator_s* const e, const mc_node_s* const node, mc_cvalue_s* const operands, const size_t count) {
	const mc_cvalue_s function = { .kind = mc_cvalue_function, .as.function = { .instance = node->note.operator_call, }, };
	if (node->note.swapped && 2 == count) {
		const mc_cvalue_s first = operands[0];
		operands[0] = operands[1];
		operands[1] = first;
	}
	const mc_cvalue_s result = _mc_call(e, function, operands, count, node);
	if (node->note.negated) return _mc_boolean(!_mc_truthy(e, result));
	return result;
}

static bool _mc_slice_empty(const mc_cvalue_s value) {
	return mc_cvalue_slice == value.kind ? 0 == value.as.slice.length : true;
}

static double _mc_as_real(const mc_cvalue_s value) {
	return (mc_cvalue_integer == value.kind) ? (double)value.as.integer : value.as.real;
}

static mc_cvalue_s _mc_binary_values(mc_evaluator_s* const e, const mc_node_s* const node, const mc_operator_e operator_, mc_cvalue_s left,
									 mc_cvalue_s right, const mc_type_s* const operand_type) {
	if (node->note.operator_call != NULL) {
		mc_cvalue_s operands[2] = { left, right };
		return _mc_operator_function(e, node, operands, 2);
	}
	if (mc_operator_equal == operator_ || mc_operator_not_equal == operator_) {
		bool equal = false;
		if (mc_cvalue_slice == left.kind || mc_cvalue_slice == right.kind) {
			equal = _mc_slice_empty(left) && _mc_slice_empty(right);
		}
		else if (mc_cvalue_function == left.kind && mc_cvalue_function == right.kind) {
			equal = left.as.function.instance == right.as.function.instance && left.as.function.lambda == right.as.function.lambda;
		}
		else if (mc_cvalue_real == left.kind || mc_cvalue_real == right.kind) {
			equal = _mc_as_real(left) == _mc_as_real(right);
		}
		else if (mc_cvalue_boolean == left.kind && mc_cvalue_boolean == right.kind) {
			equal = left.as.boolean == right.as.boolean;
		}
		else if (mc_cvalue_integer == left.kind && mc_cvalue_integer == right.kind) {
			equal = left.as.integer == right.as.integer;
		}
		else {
			_mc_broken(e);
		}
		return _mc_boolean((mc_operator_equal == operator_) == equal);
	}
	if (operator_ >= mc_operator_less && operator_ <= mc_operator_greater_equal) {
		if (mc_cvalue_integer == left.kind && mc_cvalue_integer == right.kind) {
			const __int128 a = left.as.integer, b = right.as.integer;
			switch (operator_) {
				case mc_operator_less:    return _mc_boolean(a < b);
				case mc_operator_greater: return _mc_boolean(a > b);
				case mc_operator_less_equal: return _mc_boolean(a <= b);
				default:                  return _mc_boolean(a >= b);
			}
		}
		if ((mc_cvalue_integer != left.kind && mc_cvalue_real != left.kind) || (mc_cvalue_integer != right.kind && mc_cvalue_real != right.kind)) _mc_broken(e);
		const double a = _mc_as_real(left), b = _mc_as_real(right);
		switch (operator_) {
			case mc_operator_less:    return _mc_boolean(a < b);
			case mc_operator_greater: return _mc_boolean(a > b);
			case mc_operator_less_equal: return _mc_boolean(a <= b);
			default:                  return _mc_boolean(a >= b);
		}
	}
	const mc_type_s* const node_type = node->note.type;
	const mc_type_s* const type = (node_type != NULL && mc_type_scalar == node_type->kind) ? node_type : operand_type;
	if (mc_cvalue_integer == left.kind && mc_cvalue_integer == right.kind) {
		if ((mc_operator_divide == operator_ || mc_operator_remainder == operator_) && 0 == right.as.integer) {
			_mc_fail(e, mc_E0704, node, "the divisor is zero at compile time", "division by zero");
		}
		__int128 result = 0;
		if (!mc_integer_arithmetic(operator_, left.as.integer, right.as.integer, &result)) _mc_broken(e);
		if (type != NULL && mc_type_scalar == type->kind && 'f' == mc_scalar_kind(type->as.scalar)) return _mc_wrap(_mc_integer(result), type);
		return (type != NULL && mc_type_scalar == type->kind) ? _mc_wrap(_mc_integer(result), type) : _mc_integer(result);
	}
	if ((mc_cvalue_integer != left.kind && mc_cvalue_real != left.kind) || (mc_cvalue_integer != right.kind && mc_cvalue_real != right.kind)) _mc_broken(e);
	const double a = _mc_as_real(left), b = _mc_as_real(right);
	double result = 0;
	switch (operator_) {
		case mc_operator_add:      result = a + b; break;
		case mc_operator_subtract: result = a - b; break;
		case mc_operator_multiply: result = a * b; break;
		case mc_operator_divide:
			if (0 == b) result = (0 == a || isnan(a)) ? NAN : copysign(INFINITY, a) * copysign(1.0, b);
			else result = a / b;
			break;
		default: _mc_broken(e);
	}
	return (type != NULL && mc_type_scalar == type->kind) ? _mc_wrap(_mc_real(result), type) : _mc_real(result);
}

static mc_cvalue_s _mc_composite(mc_evaluator_s* const e, const mc_node_s* const node) {
	const mc_type_s* const type = node->note.type;
	if (NULL == type) _mc_broken(e);
	const mc_nodes_s* const entries = &node->as.composite.entries;
	if (0 == entries->count) return _mc_zero(e, type, node);
	if (mc_type_struct == type->kind) {
		mc_cvalue_s value = _mc_zero(e, type, node);
		const mc_layout_s* const layout = mc_layout(e->checker, type);
		for (size_t i = 0; i < entries->count; ++i) {
			const mc_field_s* const field = mc_layout_field(layout, entries->data[i]->as.entry.name);
			if (NULL == field) _mc_broken(e);
			value.as.list->items[field->index] = _mc_copy(e, _mc_expression(e, entries->data[i]->as.entry.value));
		}
		return value;
	}
	mc_clist_s* const list = _mc_list_new(e, entries->count);
	for (size_t i = 0; i < entries->count; ++i) list->items[i] = _mc_copy(e, _mc_expression(e, entries->data[i]->as.entry.value));
	if (mc_type_slice == type->kind) return _mc_slice_value(list, 0, (int64_t)list->count);
	return _mc_list_value(list);
}

static mc_cvalue_s _mc_call_expression(mc_evaluator_s* const e, const mc_node_s* const node) {
	mc_instance_s* const instance = node->note.instance;
	const mc_cvalue_s function = (instance != NULL)
		? (mc_cvalue_s) { .kind = mc_cvalue_function, .as.function = { .instance = instance, }, }
		: _mc_expression(e, node->as.call.callee);
	const mc_type_s* const* parameters = NULL;
	size_t count = 0;
	if (instance != NULL) {
		parameters = instance->parameters.data;
		count = instance->parameters.count;
	}
	else if (node->as.call.callee->note.type != NULL && mc_type_function == node->as.call.callee->note.type->kind) {
		parameters = node->as.call.callee->note.type->as.function.parameters;
		count = node->as.call.callee->note.type->as.function.count;
	}
	const mc_nodes_s* const arguments = &node->as.call.arguments;
	mc_cvalue_s* const values = mc_arena_alloc(_mc_arena(e), (arguments->count + 1) * sizeof(mc_cvalue_s));
	size_t used = 0;
	for (size_t i = 0; i < arguments->count && i < count; ++i) {
		const mc_node_s* const argument = arguments->data[i];
		if (mc_node_reference == argument->kind) {
			values[used++] = (mc_cvalue_s) { .kind = mc_cvalue_cell, .as.cell = _mc_locate(e, argument->as.reference.operand), };
		}
		else {
			values[used++] = _mc_copy(e, _mc_expression(e, argument));
		}
	}
	mc_unused(parameters);
	return _mc_call(e, function, values, used, node);
}

static mc_cvalue_s _mc_expression(mc_evaluator_s* const e, const mc_node_s* const node) {
	_mc_step(e, node);
	switch (node->kind) {
		case mc_node_integer: {
			const mc_type_s* const type = node->note.type;
			if (type != NULL && mc_type_is_float(type)) return _mc_wrap(_mc_real((double)node->as.integer.value), type);
			return _mc_integer((__int128)node->as.integer.value);
		}
		case mc_node_float:
			return _mc_wrap(_mc_real(node->as.float_.value), node->note.type);
		case mc_node_char:
			return _mc_integer(node->as.char_.value);
		case mc_node_string: {
			const mc_view_s data = node->as.string.data;
			mc_clist_s* const list = _mc_list_new(e, data.length);
			for (size_t i = 0; i < data.length; ++i) list->items[i] = _mc_integer((uint8_t)data.data[i]);
			return _mc_list_value(list);
		}
		case mc_node_bool:  return _mc_boolean(node->as.bool_.value);
		case mc_node_null:  return _mc_slice_value(NULL, 0, 0);
		case mc_node_paren: return _mc_expression(e, node->as.paren.inner);
		case mc_node_name:  return _mc_symbol(e, node);
		case mc_node_member: {
			if (NULL == node->note.field) return _mc_symbol(e, node);
			const mc_cvalue_s target = _mc_expression(e, node->as.member.target);
			if (target.kind != mc_cvalue_list || node->note.field->index >= target.as.list->count) _mc_broken(e);
			return target.as.list->items[node->note.field->index];
		}
		case mc_node_specialize:
			if (NULL == node->note.instance) _mc_broken(e);
			return (mc_cvalue_s) { .kind = mc_cvalue_function, .as.function = { .instance = node->note.instance, }, };
		case mc_node_lambda:
			return (mc_cvalue_s) { .kind = mc_cvalue_function, .as.function = { .lambda = (mc_node_s*)(uintptr_t)node, }, };
		case mc_node_sizeof:
			return _mc_integer((__int128)node->note.size);
		case mc_node_lenof: {
			const mc_cvalue_s value = _mc_expression(e, node->as.lenof.operand);
			if (mc_cvalue_slice == value.kind) return _mc_integer(value.as.slice.length);
			if (mc_cvalue_list == value.kind) return _mc_integer((__int128)value.as.list->count);
			_mc_broken(e);
		}
		case mc_node_cast: {
			const mc_cvalue_s value = _mc_expression(e, node->as.cast.operand);
			const mc_type_s* const target = node->note.type;
			if (NULL == target || target->kind != mc_type_scalar) return value;
			const char kind = mc_scalar_kind(target->as.scalar);
			if ('i' == kind) {
				if (mc_cvalue_real == value.kind) {
					if (!isfinite(value.as.real)) {
						mc_string_s shown = {0};
						mc_format_float_repr(&shown, value.as.real);
						const char* const text = _mc_text(e, "cannot convert `%s` to `%s`", mc_string_cstr(&shown), mc_name_of(e->checker, target));
						mc_string_free(&shown);
						_mc_fail(e, mc_E0510, node, "not a finite number", "%s", text);
					}
					return _mc_wrap(_mc_integer((__int128)value.as.real), target);
				}
				if (mc_cvalue_integer == value.kind) return _mc_wrap(value, target);
				if (mc_cvalue_boolean == value.kind) return _mc_wrap(_mc_integer(value.as.boolean ? 1 : 0), target);
				_mc_broken(e);
			}
			if ('f' == kind) return _mc_wrap(_mc_real(_mc_as_real(value)), target);
			return value;
		}
		case mc_node_unary: {
			if (node->note.operator_call != NULL) {
				mc_cvalue_s operands[1] = { _mc_expression(e, node->as.unary.operand) };
				return _mc_operator_function(e, node, operands, 1);
			}
			const mc_cvalue_s value = _mc_expression(e, node->as.unary.operand);
			const mc_type_s* const type = node->note.type;
			switch (node->as.unary.operator_) {
				case mc_operator_not: return _mc_boolean(!_mc_truthy(e, value));
				case mc_operator_negate:
					if (mc_cvalue_integer == value.kind) return _mc_wrap(_mc_integer(-value.as.integer), type);
					if (mc_cvalue_real == value.kind) return _mc_wrap(_mc_real(-value.as.real), type);
					_mc_broken(e);
				default:
					if (mc_cvalue_integer != value.kind) _mc_broken(e);
					return _mc_wrap(_mc_integer(~value.as.integer), type);
			}
		}
		case mc_node_binary: {
			const mc_operator_e operator_ = node->as.binary.operator_;
			if (mc_operator_and == operator_) {
				if (!_mc_truthy(e, _mc_expression(e, node->as.binary.left))) return _mc_boolean(false);
				return _mc_boolean(_mc_truthy(e, _mc_expression(e, node->as.binary.right)));
			}
			if (mc_operator_or == operator_) {
				if (_mc_truthy(e, _mc_expression(e, node->as.binary.left))) return _mc_boolean(true);
				return _mc_boolean(_mc_truthy(e, _mc_expression(e, node->as.binary.right)));
			}
			const mc_cvalue_s left = _mc_expression(e, node->as.binary.left);
			const mc_cvalue_s right = _mc_expression(e, node->as.binary.right);
			return _mc_binary_values(e, node, operator_, left, right, node->as.binary.left->note.type);
		}
		case mc_node_index: {
			if (node->note.operator_call != NULL) {
				mc_cvalue_s operands[2] = { _mc_expression(e, node->as.index.target), _mc_expression(e, node->as.index.index) };
				return _mc_operator_function(e, node, operands, 2);
			}
			const mc_cvalue_s target = _mc_expression(e, node->as.index.target);
			return *_mc_element(e, node->as.index.index, target);
		}
		case mc_node_slice_of: {
			const mc_cvalue_s target = _mc_location_value(e, node->as.slice_of.target);
			const _mc_storage_s storage = _mc_storage(e, target);
			int64_t low = 0, high = storage.length;
			if (node->as.slice_of.low != NULL) {
				const mc_cvalue_s value = _mc_expression(e, node->as.slice_of.low);
				if (value.kind != mc_cvalue_integer) _mc_broken(e);
				low = (int64_t)value.as.integer;
			}
			if (node->as.slice_of.high != NULL) {
				const mc_cvalue_s value = _mc_expression(e, node->as.slice_of.high);
				if (value.kind != mc_cvalue_integer) _mc_broken(e);
				high = (int64_t)value.as.integer + (node->as.slice_of.inclusive ? 1 : 0);
			}
			if (!(0 <= low && low <= high && high <= storage.length)) {
				_mc_fail(e, mc_E0706, node, _mc_text(e, "the length is %lld", (long long)storage.length), "slice %lld..%lld is out of bounds",
					(long long)low, (long long)high);
			}
			return _mc_slice_value(storage.list, storage.start + low, high - low);
		}
		case mc_node_composite:
			return _mc_composite(e, node);
		case mc_node_assert: {
			if (!_mc_truthy(e, _mc_expression(e, node->as.assert_.condition))) {
				const mc_node_s* const message = node->as.assert_.message;
				if (message != NULL && mc_node_string == message->kind) {
					_mc_fail(e, mc_E0703, node, "false at compile time", "assertion failed during compile-time evaluation: " mc_view_fmt,
						mc_view_arg(message->as.string.data));
				}
				_mc_fail(e, mc_E0703, node, "false at compile time", "assertion failed during compile-time evaluation");
			}
			return (mc_cvalue_s) { .kind = mc_cvalue_nothing, };
		}
		case mc_node_reference:
			_mc_not_constant(e, node, "a reference is not a value");
		case mc_node_call:
			return _mc_call_expression(e, node);
		default:
			_mc_not_constant(e, node, "not known at compile time");
	}
}

// --- statements -----------------------------------------------------------------------------

static _mc_signal_s _mc_normal(void) {
	return (_mc_signal_s) { .flow = _mc_flow_normal, };
}

static _mc_signal_s _mc_statement(mc_evaluator_s* const e, const mc_node_s* const node);

static _mc_signal_s _mc_block(mc_evaluator_s* const e, const mc_node_s* const node) {
	for (size_t i = 0; i < node->as.block.statements.count; ++i) {
		const _mc_signal_s signal = _mc_statement(e, node->as.block.statements.data[i]);
		if (signal.flow != _mc_flow_normal) return signal;
	}
	return _mc_normal();
}

static _mc_signal_s _mc_statement(mc_evaluator_s* const e, const mc_node_s* const node) {
	_mc_step(e, node);
	mc_frame_s* const frame = _mc_frame(e);
	switch (node->kind) {
		case mc_node_let: {
			if (NULL == frame || NULL == node->note.variable) _mc_broken(e);
			const mc_cvalue_s value = (node->as.let.value != NULL) ? _mc_expression(e, node->as.let.value) : _mc_zero(e, node->note.variable->type, node);
			_mc_bind(e, frame, node->note.variable, _mc_copy(e, value));
			return _mc_normal();
		}
		case mc_node_assign: {
			const mc_node_s* const target = node->as.assign.target;
			if (mc_node_name == target->kind && mc_view_eq_cstr(target->as.name.name, "_")) {
				(void)_mc_expression(e, node->as.assign.value);
			}
			else if (mc_operator_assign == node->as.assign.operator_) {
				mc_cvalue_s* const cell = _mc_locate(e, target);
				*cell = _mc_copy(e, _mc_expression(e, node->as.assign.value));
			}
			else {
				mc_cvalue_s* const cell = _mc_locate(e, target);
				const mc_cvalue_s right = _mc_expression(e, node->as.assign.value);
				*cell = _mc_binary_values(e, node, node->as.assign.operator_, *cell, right, target->note.type);
			}
			return _mc_normal();
		}
		case mc_node_expression_statement:
			(void)_mc_expression(e, node->as.expression_statement.expression);
			return _mc_normal();
		case mc_node_if:
			if (_mc_truthy(e, _mc_expression(e, node->as.if_.condition))) return _mc_block(e, node->as.if_.then);
			if (NULL == node->as.if_.otherwise) return _mc_normal();
			if (mc_node_if == node->as.if_.otherwise->kind) return _mc_statement(e, node->as.if_.otherwise);
			return _mc_block(e, node->as.if_.otherwise);
		case mc_node_while: {
			_mc_running(e, node, NULL);
			_mc_signal_s result = _mc_normal();
			while (_mc_truthy(e, _mc_expression(e, node->as.while_.condition))) {
				const _mc_signal_s signal = _mc_block(e, node->as.while_.body);
				if (_mc_flow_break == signal.flow) break;
				if (_mc_flow_return == signal.flow) {
					result = signal;
					break;
				}
			}
			_mc_stopped(e);
			return result;
		}
		case mc_node_for: {
			const mc_cvalue_s start = _mc_expression(e, node->as.for_.start);
			const mc_cvalue_s end = _mc_expression(e, node->as.for_.end);
			if (start.kind != mc_cvalue_integer || end.kind != mc_cvalue_integer || NULL == frame || NULL == node->note.variable) _mc_broken(e);
			const __int128 step = (end.as.integer >= start.as.integer) ? 1 : -1;
			const __int128 stop = node->as.for_.inclusive ? end.as.integer + step : end.as.integer;
			_mc_running(e, node, NULL);
			_mc_signal_s result = _mc_normal();
			for (__int128 value = start.as.integer; value != stop; value += step) {
				_mc_bind(e, frame, node->note.variable, _mc_integer(value));
				const _mc_signal_s signal = _mc_block(e, node->as.for_.body);
				if (_mc_flow_break == signal.flow) break;
				if (_mc_flow_return == signal.flow) {
					result = signal;
					break;
				}
				_mc_step(e, node);
			}
			_mc_stopped(e);
			return result;
		}
		case mc_node_return: {
			_mc_signal_s signal = { .flow = _mc_flow_return, };
			signal.value = (node->as.return_.value != NULL) ? _mc_copy(e, _mc_expression(e, node->as.return_.value))
				: (mc_cvalue_s) { .kind = mc_cvalue_nothing, };
			return signal;
		}
		case mc_node_break:    return (_mc_signal_s) { .flow = _mc_flow_break, };
		case mc_node_continue: return (_mc_signal_s) { .flow = _mc_flow_continue, };
		case mc_node_block:    return _mc_block(e, node);
		case mc_node_declaration_statement: return _mc_normal();
		default:
			_mc_not_constant(e, node, "cannot run at compile time");
	}
}

// --- entry ------------------------------------------------------------------------------------

static mc_evaluation_s _mc_run(mc_evaluator_s* const e, const mc_node_s* const node) {
	jmp_buf here;
	jmp_buf* const outer = e->stop;
	const size_t frames = e->frames.count, running = e->running.count;
	e->stop = &here;
	mc_evaluation_s result = { .status = mc_evaluation_ok, };
	if (0 == setjmp(here)) result.value = _mc_expression(e, node);
	else result = e->problem;
	e->stop = outer;
	e->frames.count = frames;
	e->running.count = running;
	return result;
}

mc_evaluation_s mc_evaluate(mc_evaluator_s* const e, const mc_node_s* const node, const bool quiet) {
	if (e->active) {
		// a nested evaluation (a constant needed while evaluating another one).
		const bool saved_quiet = e->quiet;
		const size_t frames = e->frames.count;
		mc_vector_push(&e->frames, NULL);
		e->quiet = quiet;
		const mc_evaluation_s result = _mc_run(e, node);
		e->quiet = saved_quiet;
		e->frames.count = frames;
		return result;
	}
	e->active = true;
	e->frames.count = 0;
	e->running.count = 0;
	e->steps = 0;
	e->quiet = quiet;
	const mc_evaluation_s result = _mc_run(e, node);
	e->active = false;
	return result;
}
