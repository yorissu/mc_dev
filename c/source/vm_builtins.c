
/// builtins ('[builtin]' functions), natives ('[extern = "name"]' functions)
/// and printing values.
///
/// to add a builtin or a native, write its function and list it in
/// _mc_builtins or _mc_natives below; the declaration in an mc module gives
/// its signature, which the checker enforces before the machine runs.

#include "machine.h"
#include "mc/format.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define mc_nesting_limit 256

// --- helpers -----------------------------------------------------------------------------

/// the elements a slice views (an empty range for a null slice).
static const mc_value_s* _mc_elements(mc_vm_s* const vm, const mc_value_s slice, size_t* const count) {
	*count = 0;
	const mc_block_s* const block = slice.as.slice.block;
	if (NULL == block) return NULL;
	mc_vm_check(vm, block);
	const int64_t size = (int64_t)block->data->count;
	int64_t start = slice.as.slice.start, end = slice.as.slice.start + slice.as.slice.length;
	if (start > size) start = size;
	if (end > size) end = size;
	if (start < 0) start = 0;
	if (end < start) end = start;
	*count = (size_t)(end - start);
	return block->data->items + start;
}

static void _mc_bytes_text(mc_string_s* const out, const mc_value_s* items, size_t count, const bool strip_nul) {
	if (strip_nul && count > 0 && mc_value_integer == items[count - 1].kind && 0 == items[count - 1].as.integer) --count;
	char chunk[256];
	while (count > 0) {
		// whole utf-8 sequences per chunk: cut before a lead byte
		size_t size = (count < sizeof(chunk)) ? count : sizeof(chunk);
		if (size < count) {
			size_t cut = size;
			while (cut > size - 4 && 0x80 == ((uint8_t)items[cut].as.integer & 0xC0)) --cut;
			size = cut;
		}
		for (size_t i = 0; i < size; ++i) chunk[i] = (char)(uint8_t)items[i].as.integer;
		mc_format_utf8(out, mc_view_new(chunk, size));
		items += size;
		count -= size;
	}
}

void mc_vm_text(mc_vm_s* const vm, mc_string_s* const out, const mc_value_s value) {
	size_t count = 0;
	const mc_value_s* items = NULL;
	if (mc_value_slice == value.kind) items = _mc_elements(vm, value, &count);
	else if (mc_value_list == value.kind) items = value.as.list->items, count = value.as.list->count;
	_mc_bytes_text(out, items, count, false);
}

void mc_vm_write(mc_vm_s* const vm, const mc_string_s* const text) {
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

void mc_vm_format(mc_vm_s* const vm, mc_string_s* const out, const mc_value_s value, const int64_t type, const int depth) {
	if (depth > mc_nesting_limit) mc_vm_fail(vm, "values nest too deeply");
	const mc_bc_type_s* const descriptor = &vm->module->types.data[type];
	switch (vm->kinds.data[type]) {
		case mc_descriptor_int:
			mc_format_int128(out, value.as.integer);
			return;
		case mc_descriptor_float:
			mc_vm_format_float(out, value.as.real, 0 == strcmp(descriptor->name, "f32") ? 32 : 64);
			return;
		case mc_descriptor_bool:
			mc_string_append_cstr(out, value.as.boolean ? "true" : "false");
			return;
		case mc_descriptor_function: {
			const int64_t index = value.as.function.index;
			const char* const name = value.as.function.builtin ? vm->module->builtins.data[index].name : vm->module->functions.data[index].name;
			mc_string_appendf(out, "<function %s>", name);
			return;
		}
		case mc_descriptor_array:
		case mc_descriptor_slice: {
			size_t count = 0;
			const mc_value_s* items = NULL;
			if (mc_descriptor_slice == vm->kinds.data[type]) items = _mc_elements(vm, value, &count);
			else items = value.as.list->items, count = value.as.list->count;
			if (0 == strcmp(vm->module->types.data[descriptor->element].name, "u8")) {
				_mc_bytes_text(out, items, count, true);
				return;
			}
			mc_string_append_char(out, '{');
			for (size_t i = 0; i < count; ++i) {
				if (i > 0) mc_string_append_cstr(out, ", ");
				mc_vm_format(vm, out, items[i], descriptor->element, depth + 1);
			}
			mc_string_append_char(out, '}');
			return;
		}
		case mc_descriptor_struct: {
			mc_string_append_char(out, '{');
			for (size_t i = 0; i < descriptor->field_names.count && i < value.as.list->count; ++i) {
				if (i > 0) mc_string_append_cstr(out, ", ");
				mc_string_appendf(out, "%s: ", descriptor->field_names.data[i]);
				mc_vm_format(vm, out, value.as.list->items[i], descriptor->field_types.data[i], depth + 1);
			}
			mc_string_append_char(out, '}');
			return;
		}
	}
}

// --- builtins ----------------------------------------------------------------------------

#define mc_allocation_limit ((__int128)1 << 28)

static mc_value_s _mc_allocate(mc_vm_s* const vm, const __int128 count, const int64_t type) {
	if (count > mc_allocation_limit) {
		char text[48];
		mc_vm_fail(vm, "cannot allocate %s elements", mc_vm_int_text(text, count));
	}
	const size_t size = (count > 0) ? (size_t)count : 0;
	mc_list_s* const data = mc_vm_list(vm, size);
	for (size_t i = 0; i < size; ++i) data->items[i] = mc_vm_zero(vm, type);
	return mc_make_slice(mc_vm_block(vm, data, NULL, mc_block_heap), 0, (int64_t)count, false);
}

/// the heap block of a whole allocation, for realloc and free.
static mc_block_s* _mc_heap_allocation(mc_vm_s* const vm, const mc_value_s value, const char* const what) {
	mc_block_s* const block = value.as.slice.block;
	if (NULL == block || block->kind != mc_block_heap) mc_vm_fail(vm, "%s of memory that `alloc` did not return", what);
	mc_vm_check(vm, block);
	if (value.as.slice.start != 0 || value.as.slice.length != (int64_t)block->data->count) {
		mc_vm_fail(vm, "%s needs the whole allocation, not a part of it", what);
	}
	return block;
}

static mc_value_s _mc_builtin_alloc(mc_vm_s* const vm, mc_value_s* const arguments, const size_t count, const mc_bc_builtin_s* const builtin) {
	mc_unused(count);
	return _mc_allocate(vm, arguments[0].as.integer, builtin->types.data[0]);
}

static mc_value_s _mc_builtin_realloc(mc_vm_s* const vm, mc_value_s* const arguments, const size_t count, const mc_bc_builtin_s* const builtin) {
	mc_unused(count);
	const mc_value_s value = arguments[0];
	const __int128 size = arguments[1].as.integer;
	if (NULL == value.as.slice.block) return _mc_allocate(vm, size, builtin->types.data[0]);
	mc_block_s* const block = _mc_heap_allocation(vm, value, "`realloc`");
	if (size > mc_allocation_limit) {
		char text[48];
		mc_vm_fail(vm, "cannot allocate %s elements", mc_vm_int_text(text, size));
	}
	// python slicing: data[:size] (a negative size drops from the end)
	const __int128 old = (__int128)block->data->count;
	__int128 kept = (size >= 0) ? (size < old ? size : old) : (old + size > 0 ? old + size : 0);
	const __int128 added = (size > old) ? size - old : 0;
	mc_list_s* const data = mc_vm_list(vm, (size_t)(kept + added));
	for (size_t i = 0; i < (size_t)kept; ++i) data->items[i] = block->data->items[i];
	for (size_t i = 0; i < (size_t)added; ++i) data->items[(size_t)kept + i] = mc_vm_zero(vm, builtin->types.data[0]);
	block->freed = true;
	return mc_make_slice(mc_vm_block(vm, data, NULL, mc_block_heap), 0, (int64_t)size, false);
}

static mc_value_s _mc_builtin_free(mc_vm_s* const vm, mc_value_s* const arguments, const size_t count, const mc_bc_builtin_s* const builtin) {
	mc_unused(count);
	mc_unused(builtin);
	if (arguments[0].as.slice.block != NULL) _mc_heap_allocation(vm, arguments[0], "`free`")->freed = true;
	return mc_make_slice(NULL, 0, 0, false);
}

static mc_value_s _mc_builtin_copy(mc_vm_s* const vm, mc_value_s* const arguments, const size_t count, const mc_bc_builtin_s* const builtin) {
	mc_unused(count);
	mc_unused(builtin);
	const mc_value_s target = arguments[0];
	if (target.as.slice.read_only && target.as.slice.length != 0) mc_vm_fail(vm, "`copy` into a read-only slice");
	size_t available = 0;
	const mc_value_s* const items = _mc_elements(vm, arguments[1], &available);
	const int64_t length = target.as.slice.length;
	const size_t copied = (length <= 0) ? 0 : ((size_t)length < available ? (size_t)length : available);
	if (copied > 0) {
		mc_vm_check(vm, target.as.slice.block);
		// the source may overlap the target: take the values first
		mc_value_s* const saved = mc_malloc(copied * sizeof(mc_value_s));
		memcpy(saved, items, copied * sizeof(mc_value_s));
		for (size_t i = 0; i < copied; ++i) target.as.slice.block->data->items[target.as.slice.start + (int64_t)i] = mc_vm_copy(vm, saved[i]);
		mc_free(saved);
	}
	return mc_make_integer((__int128)copied);
}

/// the format string (the first argument) with each '{}' replaced by the
/// next value; '{{' and '}}' are braces. the checker matched them already.
static void _mc_formatted(mc_vm_s* const vm, mc_value_s* const arguments, const size_t count, const mc_bc_builtin_s* const builtin,
						  mc_string_s* const out) {
	size_t length = 0;
	const mc_value_s* const data = _mc_elements(vm, arguments[0], &length);
	size_t next = 1;
	mc_string_s* const text = &vm->bytes;
	mc_string_clear(text);
	size_t index = 0;
	while (index < length) {
		const uint8_t character = (uint8_t)data[index].as.integer;
		const int following = (index + 1 < length) ? (int)(uint8_t)data[index + 1].as.integer : -1;
		if ('{' == character && '}' == following) {
			mc_format_utf8(out, mc_string_view(text));
			mc_string_clear(text);
			if (next < count && next - 1 < builtin->types.count) {
				mc_vm_format(vm, out, arguments[next], builtin->types.data[next - 1], 0);
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

static mc_value_s _mc_builtin_print(mc_vm_s* const vm, mc_value_s* const arguments, const size_t count, const mc_bc_builtin_s* const builtin) {
	mc_string_clear(&vm->buffer);
	_mc_formatted(vm, arguments, count, builtin, &vm->buffer);
	mc_vm_write(vm, &vm->buffer);
	return mc_make_none();
}

static mc_value_s _mc_builtin_println(mc_vm_s* const vm, mc_value_s* const arguments, const size_t count, const mc_bc_builtin_s* const builtin) {
	mc_string_clear(&vm->buffer);
	_mc_formatted(vm, arguments, count, builtin, &vm->buffer);
	mc_string_append_char(&vm->buffer, '\n');
	mc_vm_write(vm, &vm->buffer);
	return mc_make_none();
}

static const struct {
	const char*  name    ;
	mc_builtin_f function;
} _mc_builtins[] = {
	{ "alloc"  , _mc_builtin_alloc   },
	{ "realloc", _mc_builtin_realloc },
	{ "free"   , _mc_builtin_free    },
	{ "copy"   , _mc_builtin_copy    },
	{ "print"  , _mc_builtin_print   },
	{ "println", _mc_builtin_println },
};

mc_builtin_f mc_vm_find_builtin(const char* const name) {
	for (size_t i = 0; i < mc_count_of(_mc_builtins); ++i) if (0 == strcmp(_mc_builtins[i].name, name)) return _mc_builtins[i].function;
	return NULL;
}

// --- natives -----------------------------------------------------------------------------

static mc_value_s _mc_native_write(mc_vm_s* const vm, mc_value_s* const arguments, const size_t count) {
	mc_unused(count);
	mc_string_clear(&vm->buffer);
	mc_vm_text(vm, &vm->buffer, arguments[0]);
	mc_vm_write(vm, &vm->buffer);
	return mc_make_none();
}

static mc_value_s _mc_native_write_line(mc_vm_s* const vm, mc_value_s* const arguments, const size_t count) {
	mc_unused(count);
	mc_string_clear(&vm->buffer);
	mc_vm_text(vm, &vm->buffer, arguments[0]);
	mc_string_append_char(&vm->buffer, '\n');
	mc_vm_write(vm, &vm->buffer);
	return mc_make_none();
}

static const struct {
	const char* name    ;
	mc_native_f function;
} _mc_natives[] = {
	{ "mc_write"     , _mc_native_write      },
	{ "mc_write_line", _mc_native_write_line },
};

mc_native_f mc_vm_find_native(const char* const name) {
	for (size_t i = 0; i < mc_count_of(_mc_natives); ++i) if (0 == strcmp(_mc_natives[i].name, name)) return _mc_natives[i].function;
	return NULL;
}
