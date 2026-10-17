
/// calls of plugin functions at run time: linking intrinsics to the plugin
/// functions that provide them, and the run-time half of the plugin api
/// (see mc/plugin.h). a call handle points at the arguments where they are,
/// on the operand stack; nothing is copied unless a plugin asks for the
/// memory of a scalar.

#include "machine.h"
#include "mc/format.h"

#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
	mc_plugin_call_s         public_  ;   // first: the handle the plugin gets
	mc_vm_s*                 vm       ;
	const mc_vm_link_s*      link     ;
	const mc_word_u*         arguments;
} _mc_call_s;

#define _mc_value_none  0
#define _mc_value_word  1
#define _mc_value_slice 2

static _mc_call_s* _mc_call_of(mc_plugin_call_s* const call) {
	return (_mc_call_s*)call;
}

// --- linking -----------------------------------------------------------------------------

uint64_t mc_vm_intrinsic_arguments(const mc_vm_s* const vm, const mc_bc_intrinsic_s* const intrinsic) {
	uint64_t words = 0;
	for (size_t i = 0; i < intrinsic->parameters.count; ++i) words += mc_vm_type_words(vm, intrinsic->parameters.data[i]);
	for (size_t i = 0; i < intrinsic->variadic.count; ++i) words += mc_vm_type_words(vm, intrinsic->variadic.data[i]);
	return words;
}

uint64_t mc_vm_intrinsic_result(const mc_vm_s* const vm, const mc_bc_intrinsic_s* const intrinsic) {
	return (intrinsic->result >= 0) ? mc_vm_type_words(vm, intrinsic->result) : 0;
}

void mc_vm_link(mc_vm_s* const vm, const mc_plugins_s* const plugins) {
	const mc_module_bc_s* const module = vm->module;
	vm->types = mc_plugin_types_new(module);
	for (size_t i = 0; i < module->intrinsics.count; ++i) {
		const mc_bc_intrinsic_s* const intrinsic = &module->intrinsics.data[i];
		mc_vm_link_s link = { .intrinsic = intrinsic, .function = mc_plugins_find(plugins, intrinsic->name), };
		link.fixed = intrinsic->parameters.count;
		link.count = link.fixed + intrinsic->variadic.count;
		link.types = mc_calloc(link.count + 1, sizeof(*link.types));
		link.offsets = mc_calloc(link.count + 1, sizeof(*link.offsets));
		size_t offset = 0;
		for (size_t a = 0; a < link.count; ++a) {
			const int64_t type = (a < link.fixed) ? intrinsic->parameters.data[a] : intrinsic->variadic.data[a - link.fixed];
			link.types[a] = &vm->types[type];
			link.offsets[a] = offset;
			offset += (size_t)mc_vm_type_words(vm, type);
		}
		link.result = (intrinsic->result >= 0) ? &vm->types[intrinsic->result] : NULL;
		link.result_words = mc_vm_intrinsic_result(vm, intrinsic);
		mc_vector_push(&vm->links, link);
		if (NULL == link.function) mc_vm_fail(vm, "no plugin provides `%s`", intrinsic->name);
	}
}

void mc_vm_unlink(mc_vm_s* const vm) {
	for (size_t i = 0; i < vm->links.count; ++i) {
		mc_free((void*)(uintptr_t)vm->links.data[i].types);
		mc_free(vm->links.data[i].offsets);
	}
	mc_vector_free(&vm->links);
	mc_plugin_types_free(vm->types, vm->module->types.count);
	vm->types = NULL;
	mc_vector_free(&vm->scratch);
}

void mc_vm_call_plugin(mc_vm_s* const vm, const int64_t index, const int64_t words) {
	const mc_vm_link_s* const link = &vm->links.data[index];
	_mc_call_s call = { .public_ = { .interface = mc_plugin_table(), }, .vm = vm, .link = link, .arguments = vm->stack + vm->sp - words, };
	if (vm->scratch.capacity < 8 * link->count) mc_vector_reserve(&vm->scratch, 8 * link->count);
	const mc_plugin_value_s value = link->function->run(&call.public_);
	const uint64_t given = (_mc_value_slice == value.kind) ? 2 : (_mc_value_word == value.kind) ? 1 : 0;
	if (given != link->result_words || (2 == given && link->result != NULL && link->result->kind != mc_plugin_kind_slice)) {
		mc_vm_fail(vm, "the plugin function `%s` returned %s", link->intrinsic->name, (0 == given) ? "no value" : "a value of another kind");
	}
	vm->sp -= (size_t)words;
	for (uint64_t i = 0; i < given; ++i) vm->stack[vm->sp++].u = value.words[i];
}

// --- arguments ---------------------------------------------------------------------------

/// the words of argument 'index' (stops when there is no such argument).
static const mc_word_u* _mc_argument(_mc_call_s* const call, const size_t index) {
	if (index >= call->link->count) {
		mc_vm_fail(call->vm, "the plugin function `%s` reads argument %zu, but it has %zu", call->link->intrinsic->name, index, call->link->count);
	}
	return call->arguments + call->link->offsets[index];
}

static const mc_plugin_type_s* _mc_argument_type(_mc_call_s* const call, const size_t index) {
	(void)_mc_argument(call, index);
	return call->link->types[index];
}

/// a scalar argument (any number or bool) as a word, and its type code.
static mc_word_u _mc_scalar(_mc_call_s* const call, const size_t index, mc_type_code_e* const code) {
	const mc_word_u* const words = _mc_argument(call, index);
	const mc_plugin_type_s* const type = call->link->types[index];
	if (type->kind != mc_plugin_kind_int && type->kind != mc_plugin_kind_float && type->kind != mc_plugin_kind_bool) {
		mc_vm_fail(call->vm, "the plugin function `%s` reads argument %zu (a `%s`) as a number", call->link->intrinsic->name, index, type->name);
	}
	*code = type->code;
	return words[0];
}

static bool _mc_is_float(const mc_type_code_e code) {
	return mc_code_f32 == code || mc_code_f64 == code || mc_code_float == code;
}

/// a double as a 64-bit integer, saturated (c leaves out-of-range conversions undefined).
static int64_t _mc_to_int(const double value) {
	if (isnan(value)) return 0;
	if (value >= 9223372036854775807.0) return INT64_MAX;
	if (value <= -9223372036854775808.0) return INT64_MIN;
	return (int64_t)value;
}

static size_t _mc_count(mc_plugin_call_s* const call) {
	return _mc_call_of(call)->link->count;
}

static size_t _mc_fixed_count(mc_plugin_call_s* const call) {
	return _mc_call_of(call)->link->fixed;
}

static const mc_plugin_type_s* _mc_get_argument_type(mc_plugin_call_s* const call, const size_t index) {
	return _mc_argument_type(_mc_call_of(call), index);
}

static size_t _mc_type_argument_count(mc_plugin_call_s* const call) {
	return _mc_call_of(call)->link->intrinsic->arguments.count;
}

static const mc_plugin_type_s* _mc_type_argument(mc_plugin_call_s* const call, const size_t index) {
	_mc_call_s* const self = _mc_call_of(call);
	const mc_bc_indexes_s* const arguments = &self->link->intrinsic->arguments;
	if (index >= arguments->count) {
		mc_vm_fail(self->vm, "the plugin function `%s` reads type argument %zu, but it has %zu", self->link->intrinsic->name, index, arguments->count);
	}
	return &self->vm->types[arguments->data[index]];
}

static uint32_t _mc_argument_flags(mc_plugin_call_s* const call, const size_t index) {
	const mc_bc_intrinsic_s* const intrinsic = _mc_call_of(call)->link->intrinsic;
	return (index < intrinsic->flags.count) ? intrinsic->flags.data[index] : 0;
}

static int64_t _mc_get_int(mc_plugin_call_s* const call, const size_t index) {
	mc_type_code_e code = mc_code_int;
	const mc_word_u word = _mc_scalar(_mc_call_of(call), index, &code);
	return _mc_is_float(code) ? _mc_to_int(word.f) : word.i;
}

static uint64_t _mc_get_uint(mc_plugin_call_s* const call, const size_t index) {
	mc_type_code_e code = mc_code_int;
	const mc_word_u word = _mc_scalar(_mc_call_of(call), index, &code);
	return _mc_is_float(code) ? (uint64_t)_mc_to_int(word.f) : word.u;
}

static double _mc_get_float(mc_plugin_call_s* const call, const size_t index) {
	mc_type_code_e code = mc_code_int;
	const mc_word_u word = _mc_scalar(_mc_call_of(call), index, &code);
	if (_mc_is_float(code)) return word.f;
	return mc_vm_signed(code) ? (double)word.i : (double)word.u;
}

static bool _mc_get_bool(mc_plugin_call_s* const call, const size_t index) {
	mc_type_code_e code = mc_code_int;
	const mc_word_u word = _mc_scalar(_mc_call_of(call), index, &code);
	return _mc_is_float(code) ? word.f != 0.0 : word.u != 0;
}

static const void* _mc_get_memory(mc_plugin_call_s* const call, const size_t index) {
	_mc_call_s* const self = _mc_call_of(call);
	const mc_word_u* const words = _mc_argument(self, index);
	const mc_plugin_type_s* const type = self->link->types[index];
	if (type->code != mc_type_code_count_ && type->code != mc_code_slice) {
		// a scalar is a word on the stack: written as it is in memory
		uint8_t* const memory = self->vm->scratch.data + 8 * index;
		mc_vm_write(type->code, memory, words[0]);
		return memory;
	}
	return words;
}

/// a slice from its two words.
static mc_plugin_slice_s _mc_slice(_mc_call_s* const call, const uint64_t handle, const uint64_t word, const mc_plugin_type_s* const type) {
	mc_plugin_slice_s slice = { .length = mc_slice_length(word), .element = type->element, .read_only = (handle & mc_handle_read_only) != 0, };
	if (0 == (handle & ~mc_handle_read_only)) {
		slice.length = 0;
		return slice;
	}
	const uint64_t size = (type->element != NULL) ? type->element->size : 1;
	slice.data = mc_vm_memory(call->vm, handle, mc_slice_offset(word), slice.length * size);
	return slice;
}

static const mc_word_u* _mc_slice_argument(_mc_call_s* const call, const size_t index) {
	const mc_word_u* const words = _mc_argument(call, index);
	if (call->link->types[index]->kind != mc_plugin_kind_slice) {
		mc_vm_fail(call->vm, "the plugin function `%s` reads argument %zu (a `%s`) as a slice", call->link->intrinsic->name, index,
			call->link->types[index]->name);
	}
	return words;
}

static mc_plugin_slice_s _mc_get_slice(mc_plugin_call_s* const call, const size_t index) {
	_mc_call_s* const self = _mc_call_of(call);
	const mc_word_u* const words = _mc_slice_argument(self, index);
	return _mc_slice(self, words[0].u, words[1].u, self->link->types[index]);
}

static uint64_t _mc_slice_length(mc_plugin_call_s* const call, const size_t index) {
	return mc_slice_length(_mc_slice_argument(_mc_call_of(call), index)[1].u);
}

static bool _mc_slice_read_only(mc_plugin_call_s* const call, const size_t index) {
	return (_mc_slice_argument(_mc_call_of(call), index)[0].u & mc_handle_read_only) != 0;
}

static mc_plugin_slice_s _mc_slice_at(mc_plugin_call_s* const call, const void* const memory, const mc_plugin_type_s* const type) {
	uint64_t words[2];
	memcpy(words, memory, sizeof(words));
	return _mc_slice(_mc_call_of(call), words[0], words[1], type);
}

static const char* _mc_function_name_at(mc_plugin_call_s* const call, const void* const memory) {
	const mc_module_bc_s* const module = _mc_call_of(call)->vm->module;
	uint64_t word = 0;
	memcpy(&word, memory, sizeof(word));
	const uint64_t index = word & ~((uint64_t)1 << 63);
	if (word >> 63) {
		if (index >= module->intrinsics.count) return "?";
		// plugin functions show their own name, without the module
		const char* const name = module->intrinsics.data[index].name;
		const char* const dot = strrchr(name, '.');
		return (dot != NULL) ? dot + 1 : name;
	}
	return (index < module->functions.count) ? module->functions.data[index].name : "?";
}

// --- results -----------------------------------------------------------------------------

/// a word of the declared result type.
static mc_plugin_value_s _mc_result(_mc_call_s* const call, const bool real, const double number, const uint64_t integer) {
	const mc_plugin_type_s* const type = call->link->result;
	mc_plugin_value_s value = { .kind = _mc_value_word, };
	if (NULL == type || (type->kind != mc_plugin_kind_int && type->kind != mc_plugin_kind_float && type->kind != mc_plugin_kind_bool)) {
		mc_vm_fail(call->vm, "the plugin function `%s` returned a number, but its result is not one", call->link->intrinsic->name);
	}
	mc_word_u word = { .u = 0, };
	if (mc_plugin_kind_float == type->kind) word.f = mc_vm_round(type->code, real ? number : (double)(int64_t)integer);
	else if (mc_plugin_kind_bool == type->kind) word.u = real ? number != 0.0 : integer != 0;
	else word = mc_vm_wrap(type->code, real ? (uint64_t)_mc_to_int(number) : integer);
	value.words[0] = word.u;
	return value;
}

static mc_plugin_value_s _mc_make_int(mc_plugin_call_s* const call, const int64_t value) {
	return _mc_result(_mc_call_of(call), false, 0.0, (uint64_t)value);
}

static mc_plugin_value_s _mc_make_uint(mc_plugin_call_s* const call, const uint64_t value) {
	_mc_call_s* const self = _mc_call_of(call);
	if (self->link->result != NULL && mc_plugin_kind_float == self->link->result->kind) return _mc_result(self, true, (double)value, 0);
	return _mc_result(self, false, 0.0, value);
}

static mc_plugin_value_s _mc_make_float(mc_plugin_call_s* const call, const double value) {
	return _mc_result(_mc_call_of(call), true, value, 0);
}

static mc_plugin_value_s _mc_make_bool(mc_plugin_call_s* const call, const bool value) {
	return _mc_result(_mc_call_of(call), false, 0.0, value ? 1 : 0);
}

static mc_plugin_value_s _mc_make_none(mc_plugin_call_s* const call) {
	mc_unused(call);
	return (mc_plugin_value_s) { .kind = _mc_value_none, };
}

static mc_plugin_value_s _mc_make_null(mc_plugin_call_s* const call) {
	mc_unused(call);
	return (mc_plugin_value_s) { .kind = _mc_value_slice, };
}

// --- heap memory -------------------------------------------------------------------------

#define _mc_allocation_limit ((uint64_t)1 << 28)

static void _mc_release(void* const memory) {
	free(memory);
}

static mc_plugin_value_s _mc_heap_slice(_mc_call_s* const call, const uint64_t count, uint8_t* const data, const uint64_t size,
										void (*release)(void* const memory)) {
	call->vm->allocations += 1;
	const mc_plugin_value_s value = { .kind = _mc_value_slice, .words = {
		mc_vm_block_new(call->vm, mc_block_heap, data, size, release), mc_slice_word(0, count),
	}, };
	return value;
}

static void _mc_check_count(_mc_call_s* const call, const mc_plugin_type_s* const element, const uint64_t count) {
	if (count > _mc_allocation_limit || (element->size > 0 && count > ((uint64_t)1 << 32) / element->size)) {
		mc_vm_fail(call->vm, "cannot allocate %" PRIu64 " elements", count);
	}
}

static mc_plugin_value_s _mc_heap_alloc(mc_plugin_call_s* const call, const mc_plugin_type_s* const element, const uint64_t count,
										void** const data) {
	_mc_call_s* const self = _mc_call_of(call);
	_mc_check_count(self, element, count);
	const uint64_t size = count * element->size;
	uint8_t* const memory = calloc(1, size > 0 ? (size_t)size : 1);
	if (NULL == memory) mc_vm_fail(self->vm, "cannot allocate %" PRIu64 " elements", count);
	const uint8_t* const zero = (element->index >= 0) ? mc_vm_zero(self->vm, element->index) : NULL;
	if (zero != NULL) for (uint64_t i = 0; i < count; ++i) memcpy(memory + i * element->size, zero, (size_t)element->size);
	if (data != NULL) *data = memory;
	return _mc_heap_slice(self, count, memory, size, _mc_release);
}

static mc_plugin_value_s _mc_heap_adopt(mc_plugin_call_s* const call, const mc_plugin_type_s* const element, const uint64_t count, void* const memory,
										void (*release)(void* const memory)) {
	_mc_call_s* const self = _mc_call_of(call);
	_mc_check_count(self, element, count);
	if (NULL == memory) mc_vm_fail(self->vm, "the plugin function `%s` gave no memory", self->link->intrinsic->name);
	return _mc_heap_slice(self, count, memory, count * element->size, release);
}

static mc_plugin_heap_e _mc_heap_of(mc_plugin_call_s* const call, const size_t index) {
	_mc_call_s* const self = _mc_call_of(call);
	const mc_word_u* const words = _mc_slice_argument(self, index);
	const uint64_t handle = words[0].u & ~mc_handle_read_only;
	if (0 == handle) return mc_plugin_heap_null;
	if (mc_handle_kind(handle) != mc_block_heap) return mc_plugin_heap_other;
	(void)mc_vm_resolve(self->vm, handle);
	const mc_block_s* const block = mc_vm_block_of(self->vm, handle);
	const mc_plugin_type_s* const element = self->link->types[index]->element;
	const uint64_t size = (element != NULL) ? element->size : 1;
	const bool whole = 0 == mc_slice_offset(words[1].u) && mc_slice_length(words[1].u) * size == block->size;
	return whole ? mc_plugin_heap_whole : mc_plugin_heap_part;
}

static void _mc_heap_free(mc_plugin_call_s* const call, const size_t index) {
	_mc_call_s* const self = _mc_call_of(call);
	if (_mc_heap_of(call, index) != mc_plugin_heap_whole) {
		mc_vm_fail(self->vm, "the plugin function `%s` frees memory that is not a whole heap allocation", self->link->intrinsic->name);
	}
	mc_vm_block_end(self->vm, _mc_argument(self, index)[0].u & ~mc_handle_read_only);
}

// --- the rest ----------------------------------------------------------------------------

static void _mc_write(mc_plugin_call_s* const call, const char* const data, const size_t length) {
	if (length > 0) (void)fwrite(data, 1, length, _mc_call_of(call)->vm->out);
}

static void _mc_fail(mc_plugin_call_s* const call, const char* const format, va_list args) mc_printf_like(2, 0);

static void _mc_fail(mc_plugin_call_s* const call, const char* const format, va_list args) {
	mc_vm_vfail(_mc_call_of(call)->vm, format, args);
}

void mc_vm_plugin_table(mc_plugin_interface_s* const table) {
	table->count = _mc_count;
	table->fixed_count = _mc_fixed_count;
	table->argument_type = _mc_get_argument_type;
	table->type_argument_count = _mc_type_argument_count;
	table->type_argument = _mc_type_argument;
	table->argument_flags = _mc_argument_flags;
	table->get_int = _mc_get_int;
	table->get_uint = _mc_get_uint;
	table->get_float = _mc_get_float;
	table->get_bool = _mc_get_bool;
	table->get_memory = _mc_get_memory;
	table->get_slice = _mc_get_slice;
	table->slice_length = _mc_slice_length;
	table->slice_read_only = _mc_slice_read_only;
	table->slice_at = _mc_slice_at;
	table->function_name_at = _mc_function_name_at;
	table->make_int = _mc_make_int;
	table->make_uint = _mc_make_uint;
	table->make_float = _mc_make_float;
	table->make_bool = _mc_make_bool;
	table->make_none = _mc_make_none;
	table->make_null = _mc_make_null;
	table->heap_alloc = _mc_heap_alloc;
	table->heap_adopt = _mc_heap_adopt;
	table->heap_of = _mc_heap_of;
	table->heap_free = _mc_heap_free;
	table->write = _mc_write;
	table->fail = _mc_fail;
}
