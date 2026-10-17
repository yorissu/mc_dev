
/// intrinsics (the functions of the standard library that the machine
/// provides: memory and io) and printing values.

#include "machine.h"
#include "mc/format.h"

#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define mc_nesting_limit 256

// --- printing ----------------------------------------------------------------------------

void mc_vm_print(mc_vm_s* const vm, const mc_string_s* const text) {
	if (text->length > 0) (void)fwrite(text->data, 1, text->length, vm->out);
}

void mc_vm_format_float(mc_string_s* const out, const double value, const int bits) {
	if (isnan(value)) {
		mc_string_append_cstr(out, "nan");
		return;
	}
	if (isinf(value)) {
		mc_string_append_cstr(out, value > 0 ? "inf" : "-inf");
		return;
	}
	mc_string_s text = {0};
	if (32 == bits) {
		// the shortest text that reads back as the same f32
		char digits[64];
		for (int precision = 1; precision < 10; ++precision) {
			(void)snprintf(digits, sizeof(digits), "%.*g", precision, value);
			if ((double)(float)strtod(digits, NULL) == value) break;
		}
		mc_string_append_cstr(&text, digits);
	}
	else {
		mc_format_float_repr(&text, value);
	}
	const char* const cstr = mc_string_cstr(&text);
	mc_string_append_cstr(out, cstr);
	if (NULL == strpbrk(cstr, "e.n")) mc_string_append_cstr(out, ".0");
	mc_string_free(&text);
}

/// the elements a slice views (none for the null slice).
static const uint8_t* _mc_slice_memory(mc_vm_s* const vm, const uint8_t* const slice, const uint64_t element, uint64_t* const length) {
	uint64_t words[2];
	memcpy(words, slice, 16);
	*length = mc_slice_length(words[1]);
	if (0 == words[0]) {
		*length = 0;
		return NULL;
	}
	return mc_vm_memory(vm, words[0], mc_slice_offset(words[1]), *length * element);
}

static void _mc_bytes_text(mc_string_s* const out, const uint8_t* const bytes, uint64_t count, const bool strip_nul) {
	if (strip_nul && count > 0 && 0 == bytes[count - 1]) --count;
	if (count > 0) mc_format_utf8(out, mc_view_new((const char*)bytes, (size_t)count));
}

void mc_vm_format(mc_vm_s* const vm, mc_string_s* const out, const uint8_t* const memory, const int64_t type, const int depth) {
	if (depth > mc_nesting_limit) mc_vm_fail(vm, "values nest too deeply");
	const mc_module_bc_s* const module = vm->module;
	const mc_bc_type_s* const descriptor = &module->types.data[type];
	switch (descriptor->kind) {
		case mc_descriptor_int: {
			const mc_word_u word = mc_vm_read(descriptor->code, memory);
			if (mc_vm_signed(descriptor->code)) mc_string_appendf(out, "%" PRId64, word.i);
			else mc_string_appendf(out, "%" PRIu64, word.u);
			return;
		}
		case mc_descriptor_float:
			mc_vm_format_float(out, mc_vm_read(descriptor->code, memory).f, mc_code_f32 == descriptor->code ? 32 : 64);
			return;
		case mc_descriptor_bool:
			mc_string_append_cstr(out, mc_vm_read(mc_code_bool, memory).u ? "true" : "false");
			return;
		case mc_descriptor_function: {
			const uint64_t word = mc_vm_read(mc_code_fn, memory).u;
			const uint64_t index = word & ~((uint64_t)1 << 63);
			const char* name = "?";
			if (word >> 63) {
				if (index < module->intrinsics.count) {
					// intrinsics show their own name, without the module
					name = module->intrinsics.data[index].name;
					const char* const dot = strrchr(name, '.');
					if (dot != NULL) name = dot + 1;
				}
			}
			else if (index < module->functions.count) {
				name = module->functions.data[index].name;
			}
			mc_string_appendf(out, "<function %s>", name);
			return;
		}
		case mc_descriptor_array:
		case mc_descriptor_slice: {
			uint64_t count = 0;
			const uint8_t* items = NULL;
			const mc_bc_type_s* const element = &module->types.data[descriptor->element];
			if (mc_descriptor_slice == descriptor->kind) {
				items = _mc_slice_memory(vm, memory, element->size, &count);
			}
			else {
				items = memory;
				count = (uint64_t)descriptor->length;
			}
			if (0 == strcmp(element->name, "u8")) {
				_mc_bytes_text(out, items, count, true);
				return;
			}
			mc_string_append_char(out, '{');
			for (uint64_t i = 0; i < count; ++i) {
				if (i > 0) mc_string_append_cstr(out, ", ");
				mc_vm_format(vm, out, items + i * element->size, descriptor->element, depth + 1);
			}
			mc_string_append_char(out, '}');
			return;
		}
		case mc_descriptor_struct:
			mc_string_append_char(out, '{');
			for (size_t i = 0; i < descriptor->field_names.count; ++i) {
				if (i > 0) mc_string_append_cstr(out, ", ");
				mc_string_appendf(out, "%s: ", descriptor->field_names.data[i]);
				mc_vm_format(vm, out, memory + descriptor->field_offsets.data[i], descriptor->field_types.data[i], depth + 1);
			}
			mc_string_append_char(out, '}');
			return;
		default:
			return;
	}
}

// --- arguments ---------------------------------------------------------------------------

/// the memory form of an argument: in place for slices and aggregates,
/// written to 'scratch' (8 bytes) for scalars.
static const uint8_t* _mc_argument_memory(mc_vm_s* const vm, const int64_t type, const mc_word_u* const words, uint8_t* const scratch) {
	const mc_bc_type_s* const descriptor = &vm->module->types.data[type];
	if (descriptor->code != mc_type_code_count_ && descriptor->code != mc_code_slice) {
		mc_vm_write(descriptor->code, scratch, words[0]);
		return scratch;
	}
	return (const uint8_t*)words;
}

// --- memory ------------------------------------------------------------------------------

#define mc_allocation_limit ((uint64_t)1 << 28)

static void _mc_heap_release(void* const memory) {
	free(memory);
}

/// a new heap block of 'count' elements of 'type', set to its zero value.
static size_t _mc_allocate(mc_vm_s* const vm, const uint64_t count, const int64_t type, mc_word_u* const result) {
	const mc_bc_type_s* const descriptor = &vm->module->types.data[type];
	if (count > mc_allocation_limit || (descriptor->size > 0 && count > ((uint64_t)1 << 32) / descriptor->size)) {
		mc_vm_fail(vm, "cannot allocate %" PRIu64 " elements", count);
	}
	const uint64_t size = count * descriptor->size;
	uint8_t* const data = calloc(1, size > 0 ? (size_t)size : 1);
	if (NULL == data) mc_vm_fail(vm, "cannot allocate %" PRIu64 " elements", count);
	const uint8_t* const zero = mc_vm_zero(vm, type);
	if (zero != NULL) for (uint64_t i = 0; i < count; ++i) memcpy(data + i * descriptor->size, zero, (size_t)descriptor->size);
	vm->allocations += 1;
	result[0].u = mc_vm_block_new(vm, mc_block_heap, data, size, _mc_heap_release);
	result[1].u = mc_slice_word(0, count);
	return 2;
}

/// the heap block of a whole allocation, for realloc and free.
static mc_block_s* _mc_heap_allocation(mc_vm_s* const vm, const mc_word_u* const slice, const int64_t element, const char* const what) {
	const uint64_t handle = slice[0].u;
	if (mc_handle_kind(handle) != mc_block_heap) mc_vm_fail(vm, "%s of memory that `alloc` did not return", what);
	(void)mc_vm_resolve(vm, handle);
	mc_block_s* const block = mc_vm_block_of(vm, handle);
	const uint64_t size = vm->module->types.data[element].size;
	if (mc_slice_offset(slice[1].u) != 0 || mc_slice_length(slice[1].u) * size != block->size) {
		mc_vm_fail(vm, "%s needs the whole allocation, not a part of it", what);
	}
	return block;
}

static size_t _mc_alloc(mc_vm_s* const vm, const mc_bc_intrinsic_s* const intrinsic, const mc_word_u* const arguments, mc_word_u* const result) {
	return _mc_allocate(vm, arguments[0].u, intrinsic->arguments.data[0], result);
}

static size_t _mc_realloc(mc_vm_s* const vm, const mc_bc_intrinsic_s* const intrinsic, const mc_word_u* const arguments, mc_word_u* const result) {
	const int64_t element = intrinsic->arguments.data[0];
	const uint64_t count = arguments[2].u;
	if (0 == arguments[0].u) return _mc_allocate(vm, count, element, result);
	const mc_block_s* const block = _mc_heap_allocation(vm, arguments, element, "`realloc`");
	const uint8_t* const old = block->data;
	const uint64_t old_size = block->size;
	(void)_mc_allocate(vm, count, element, result);
	uint8_t* const fresh = mc_vm_resolve(vm, result[0].u);
	const uint64_t size = count * vm->module->types.data[element].size;
	memcpy(fresh, old, (size_t)(size < old_size ? size : old_size));
	mc_vm_block_end(vm, arguments[0].u);
	return 2;
}

static size_t _mc_free(mc_vm_s* const vm, const mc_bc_intrinsic_s* const intrinsic, const mc_word_u* const arguments, mc_word_u* const result) {
	if (arguments[0].u != 0) {
		(void)_mc_heap_allocation(vm, arguments, intrinsic->arguments.data[0], "`free`");
		mc_vm_block_end(vm, arguments[0].u);
	}
	result[0].u = 0;
	result[1].u = 0;
	return 2;
}

static size_t _mc_copy(mc_vm_s* const vm, const mc_bc_intrinsic_s* const intrinsic, const mc_word_u* const arguments, mc_word_u* const result) {
	const uint64_t size = vm->module->types.data[intrinsic->arguments.data[0]].size;
	const uint64_t target = arguments[0].u, source = arguments[2].u;
	const uint64_t length = mc_slice_length(arguments[1].u), available = mc_slice_length(arguments[3].u);
	if ((target & mc_handle_read_only) && length != 0) mc_vm_fail(vm, "`copy` into a read-only slice");
	const uint8_t* const from = (source != 0) ? mc_vm_memory(vm, source, mc_slice_offset(arguments[3].u), available * size) : NULL;
	const uint64_t copied = (length < available) ? length : available;
	if (copied > 0) {
		uint8_t* const to = mc_vm_memory(vm, target, mc_slice_offset(arguments[1].u), copied * size);
		// the source may overlap the target
		memmove(to, from, (size_t)(copied * size));
	}
	result[0].u = copied;
	return 1;
}

// --- io ----------------------------------------------------------------------------------

/// the format string (the first argument) with each '{}' replaced by the
/// next value; '{{' and '}}' are braces. the checker matched them already.
static void _mc_formatted(mc_vm_s* const vm, const mc_bc_intrinsic_s* const intrinsic, const mc_word_u* const arguments, mc_string_s* const out) {
	uint64_t length = 0;
	const uint8_t* const data = _mc_slice_memory(vm, (const uint8_t*)arguments, 1, &length);
	const mc_word_u* value = arguments + 2;
	size_t next = 0;
	mc_string_s* const text = &vm->bytes;
	mc_string_clear(text);
	for (uint64_t index = 0; index < length;) {
		const uint8_t character = data[index];
		const int following = (index + 1 < length) ? (int)data[index + 1] : -1;
		if ('{' == character && '}' == following) {
			mc_format_utf8(out, mc_string_view(text));
			mc_string_clear(text);
			if (next < intrinsic->variadic.count) {
				const int64_t type = intrinsic->variadic.data[next];
				uint8_t scratch[8];
				mc_vm_format(vm, out, _mc_argument_memory(vm, type, value, scratch), type, 0);
				value += mc_vm_type_words(vm, type);
				++next;
			}
			index += 2;
			continue;
		}
		if (('{' == character || '}' == character) && following == character) ++index;
		mc_string_append_char(text, (char)character);
		++index;
	}
	mc_format_utf8(out, mc_string_view(text));
}

static size_t _mc_print(mc_vm_s* const vm, const mc_bc_intrinsic_s* const intrinsic, const mc_word_u* const arguments, mc_word_u* const result) {
	mc_unused(result);
	mc_string_clear(&vm->buffer);
	_mc_formatted(vm, intrinsic, arguments, &vm->buffer);
	mc_vm_print(vm, &vm->buffer);
	return 0;
}

static size_t _mc_println(mc_vm_s* const vm, const mc_bc_intrinsic_s* const intrinsic, const mc_word_u* const arguments, mc_word_u* const result) {
	mc_unused(result);
	mc_string_clear(&vm->buffer);
	_mc_formatted(vm, intrinsic, arguments, &vm->buffer);
	mc_string_append_char(&vm->buffer, '\n');
	mc_vm_print(vm, &vm->buffer);
	return 0;
}

static void _mc_write_bytes(mc_vm_s* const vm, const mc_word_u* const arguments, const bool line) {
	uint64_t length = 0;
	const uint8_t* const data = _mc_slice_memory(vm, (const uint8_t*)arguments, 1, &length);
	mc_string_clear(&vm->buffer);
	_mc_bytes_text(&vm->buffer, data, length, false);
	if (line) mc_string_append_char(&vm->buffer, '\n');
	mc_vm_print(vm, &vm->buffer);
}

static size_t _mc_write(mc_vm_s* const vm, const mc_bc_intrinsic_s* const intrinsic, const mc_word_u* const arguments, mc_word_u* const result) {
	mc_unused(intrinsic);
	mc_unused(result);
	_mc_write_bytes(vm, arguments, false);
	return 0;
}

static size_t _mc_write_line(mc_vm_s* const vm, const mc_bc_intrinsic_s* const intrinsic, const mc_word_u* const arguments, mc_word_u* const result) {
	mc_unused(intrinsic);
	mc_unused(result);
	_mc_write_bytes(vm, arguments, true);
	return 0;
}

static const struct {
	const char*    name    ;
	mc_intrinsic_f function;
} _mc_intrinsics[] = {
	{ "memory.alloc"  , _mc_alloc      },
	{ "memory.realloc", _mc_realloc    },
	{ "memory.free"   , _mc_free       },
	{ "memory.copy"   , _mc_copy       },
	{ "io.print"      , _mc_print      },
	{ "io.println"    , _mc_println    },
	{ "io.write"      , _mc_write      },
	{ "io.write_line" , _mc_write_line },
};

mc_intrinsic_f mc_vm_find_intrinsic(const char* const name) {
	for (size_t i = 0; i < mc_count_of(_mc_intrinsics); ++i) if (0 == strcmp(_mc_intrinsics[i].name, name)) return _mc_intrinsics[i].function;
	return NULL;
}
