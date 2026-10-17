
/// the machine's memory: blocks and their handles, the byte areas of
/// frames, scalars in memory, and the images of constants.

#include "machine.h"

#include <math.h>
#include <string.h>

#define _mc_chunk_size ((size_t)1 << 20)

// --- blocks ------------------------------------------------------------------------------

uint64_t mc_vm_block_new(mc_vm_s* const vm, const mc_block_kind_e kind, uint8_t* const data, const uint64_t size,
						 void (*release)(void* const memory)) {
	mc_blocks_s* const table = &vm->blocks[kind];
	uint32_t index = table->free;
	if (index != 0) {
		table->free = table->items[index].next_free;
	}
	else {
		if (0 == table->count) table->count = 1;   // index 0 is no block
		if (table->count >= table->capacity) {
			table->capacity = (table->capacity > 0) ? table->capacity * 2 : 64;
			table->items = mc_realloc(table->items, table->capacity * sizeof(*table->items));
		}
		index = table->count++;
		table->items[index] = (mc_block_s) { .generation = 1, };
	}
	mc_block_s* const block = &table->items[index];
	block->data = data;
	block->size = size;
	block->alive = true;
	block->next_free = 0;
	block->release = release;
	return mc_handle_make(kind, index, block->generation);
}

void mc_vm_block_end(mc_vm_s* const vm, const uint64_t handle) {
	const mc_block_kind_e kind = mc_handle_kind(handle);
	mc_blocks_s* const table = &vm->blocks[kind];
	const uint32_t index = mc_handle_index(handle);
	if (mc_block_none == kind || index >= table->count) return;
	mc_block_s* const block = &table->items[index];
	if (!block->alive) return;
	if (block->release != NULL) block->release(block->data);
	block->alive = false;
	block->data = NULL;
	block->size = 0;
	block->release = NULL;
	block->generation = (block->generation + 1) & mc_handle_generation_mask;
	if (0 == block->generation) block->generation = 1;
	block->next_free = table->free;
	table->free = index;
}

mc_block_s* mc_vm_block_of(mc_vm_s* const vm, const uint64_t handle) {
	const mc_block_kind_e kind = mc_handle_kind(handle);
	mc_blocks_s* const table = &vm->blocks[kind];
	const uint32_t index = mc_handle_index(handle);
	if (mc_block_none == kind || 0 == index || index >= table->count) return NULL;
	mc_block_s* const block = &table->items[index];
	if (!block->alive || block->generation != mc_handle_generation(handle)) return NULL;
	return block;
}

void mc_vm_bad_access(mc_vm_s* const vm, const uint64_t handle) {
	if (mc_vm_block_of(vm, handle) != NULL) mc_vm_fail(vm, "an access outside of its memory (the module is broken)");
	switch (mc_handle_kind(handle)) {
		case mc_block_heap:  mc_vm_fail(vm, "use of freed heap memory");
		case mc_block_frame: mc_vm_fail(vm, "use of a local array of a function that has returned");
		default:             mc_vm_fail(vm, "use of memory that does not exist");
	}
}

uint8_t* mc_vm_resolve(mc_vm_s* const vm, const uint64_t handle) {
	mc_block_s* const block = mc_vm_block_of(vm, handle);
	if (NULL == block) mc_vm_bad_access(vm, handle);
	return block->data;
}

void mc_vm_reserve(mc_vm_s* const vm, const size_t count) {
	if (vm->capacity - vm->sp >= count) return;
	size_t capacity = (vm->capacity > 0) ? vm->capacity : 1024;
	while (capacity - vm->sp < count) capacity *= 2;
	vm->stack = mc_realloc(vm->stack, capacity * sizeof(*vm->stack));
	vm->capacity = capacity;
}

// --- frames ------------------------------------------------------------------------------

uint8_t* mc_vm_frame_alloc(mc_vm_s* const vm, mc_frame_s* const frame, const uint64_t size) {
	const size_t rounded = (size_t)((size + 15) / 16 * 16);
	if (0 == vm->chunks.count) {
		const mc_chunk_s chunk = { .data = mc_malloc(_mc_chunk_size), .size = _mc_chunk_size, };
		mc_vector_push(&vm->chunks, chunk);
		vm->chunk = 0;
	}
	mc_chunk_s* chunk = &vm->chunks.data[vm->chunk];
	while (chunk->size - chunk->used < rounded) {
		// the next chunk (chunks never move, so pointers into frames stay valid)
		if (vm->chunk + 1 < vm->chunks.count && vm->chunks.data[vm->chunk + 1].size >= rounded) {
			vm->chunk += 1;
			vm->chunks.data[vm->chunk].used = 0;
		}
		else {
			const size_t size_needed = (rounded > _mc_chunk_size) ? rounded : _mc_chunk_size;
			const mc_chunk_s fresh = { .data = mc_malloc(size_needed), .size = size_needed, };
			// a chunk too small to come back to is replaced
			if (vm->chunk + 1 < vm->chunks.count) {
				mc_free(vm->chunks.data[vm->chunk + 1].data);
				vm->chunks.data[vm->chunk + 1] = fresh;
			}
			else {
				mc_vector_push(&vm->chunks, fresh);
			}
			vm->chunk += 1;
		}
		chunk = &vm->chunks.data[vm->chunk];
	}
	frame->chunk = vm->chunk;
	frame->used = chunk->used;
	uint8_t* const base = chunk->data + chunk->used;
	chunk->used += rounded;
	memset(base, 0, rounded);
	return base;
}

void mc_vm_frame_release(mc_vm_s* const vm, mc_frame_s* const frame) {
	if (frame->handle != 0) mc_vm_block_end(vm, frame->handle);
	frame->handle = 0;
	vm->chunk = frame->chunk;
	vm->chunks.data[frame->chunk].used = frame->used;
}

uint64_t mc_vm_frame_handle(mc_vm_s* const vm, mc_frame_s* const frame) {
	if (0 == frame->handle) {
		const mc_bc_function_s* const function = &vm->module->functions.data[frame->function];
		frame->handle = mc_vm_block_new(vm, mc_block_frame, frame->base, function->frame, NULL);
	}
	return frame->handle;
}

// --- scalars -----------------------------------------------------------------------------

uint64_t mc_vm_type_words(const mc_vm_s* const vm, const int64_t type) {
	const mc_bc_type_s* const descriptor = &vm->module->types.data[type];
	if (descriptor->code != mc_type_code_count_) return (mc_code_slice == descriptor->code) ? 2 : 1;
	return (descriptor->size + 7) / 8;
}

uint64_t mc_vm_code_size(const mc_type_code_e code) {
	switch (code) {
		case mc_code_s8: case mc_code_u8: case mc_code_bool: return 1;
		case mc_code_s16: case mc_code_u16:                   return 2;
		case mc_code_s32: case mc_code_u32: case mc_code_f32: return 4;
		case mc_code_slice:                                   return 16;
		default:                                              return 8;
	}
}

bool mc_vm_signed(const mc_type_code_e code) {
	switch (code) {
		case mc_code_s8: case mc_code_s16: case mc_code_s32: case mc_code_s64: case mc_code_int: return true;
		default: return false;
	}
}

__int128 mc_vm_integer(const mc_type_code_e code, const mc_word_u word) {
	return mc_vm_signed(code) ? (__int128)word.i : (__int128)word.u;
}

mc_word_u mc_vm_wrap(const mc_type_code_e code, const uint64_t value) {
	mc_word_u word = { .u = value, };
	switch (code) {
		case mc_code_s8:  word.i = (int8_t)(uint8_t)value; break;
		case mc_code_u8:  word.u = (uint8_t)value; break;
		case mc_code_s16: word.i = (int16_t)(uint16_t)value; break;
		case mc_code_u16: word.u = (uint16_t)value; break;
		case mc_code_s32: word.i = (int32_t)(uint32_t)value; break;
		case mc_code_u32: word.u = (uint32_t)value; break;
		case mc_code_bool: word.u = value != 0; break;
		default: break;
	}
	return word;
}

double mc_vm_round(const mc_type_code_e code, const double value) {
	if (mc_code_f32 == code && isfinite(value)) return (double)(float)value;
	return value;
}

const char* mc_vm_int_text(char buffer[48], const __int128 value) {
	char digits[48];
	size_t count = 0;
	unsigned __int128 magnitude = (value < 0) ? (unsigned __int128)0 - (unsigned __int128)value : (unsigned __int128)value;
	do {
		digits[count++] = (char)('0' + (int)(magnitude % 10));
		magnitude /= 10;
	} while (magnitude > 0);
	size_t length = 0;
	if (value < 0) buffer[length++] = '-';
	while (count > 0) buffer[length++] = digits[--count];
	buffer[length] = '\0';
	return buffer;
}

// --- constants ---------------------------------------------------------------------------

/// the word of a scalar constant.
static mc_word_u _mc_scalar_word(const mc_bc_constant_s* const constant) {
	mc_word_u word = { .u = 0, };
	switch (constant->kind) {
		case mc_constant_integer:   word.u = constant->integer; break;
		case mc_constant_real:      word.f = constant->real; break;
		case mc_constant_boolean:   word.u = constant->boolean; break;
		case mc_constant_function:  word.u = constant->integer; break;
		case mc_constant_intrinsic: word.u = constant->integer | ((uint64_t)1 << 63); break;
		default: break;
	}
	return word;
}

static void _mc_image(mc_vm_s* const vm, const mc_bc_constant_s* const constant, const int64_t type, uint8_t* const memory, const int depth);

/// a read-only static block holding the items of a constant slice.
static uint64_t _mc_static_items(mc_vm_s* const vm, const mc_bc_constant_s* const constant, const int64_t element, const int depth) {
	const mc_bc_type_s* const descriptor = &vm->module->types.data[element];
	const uint64_t size = descriptor->size * constant->items.count;
	uint8_t* const data = mc_calloc(1, size > 0 ? (size_t)size : 1);
	// the block owns the memory before it is filled: filling may stop the machine
	const uint64_t handle = mc_vm_block_new(vm, mc_block_static, data, size, mc_free) | mc_handle_read_only;
	for (size_t i = 0; i < constant->items.count; ++i) _mc_image(vm, &constant->items.data[i], element, data + i * descriptor->size, depth + 1);
	return handle;
}

/// writes the memory image of 'constant' as a value of 'type'.
static void _mc_image(mc_vm_s* const vm, const mc_bc_constant_s* const constant, const int64_t type, uint8_t* const memory, const int depth) {
	const mc_module_bc_s* const module = vm->module;
	if (type < 0 || type >= (int64_t)module->types.count || depth > 256) mc_vm_fail(vm, "the module is broken (a constant has no type)");
	const mc_bc_type_s* const descriptor = &module->types.data[type];
	switch (descriptor->kind) {
		case mc_descriptor_int: case mc_descriptor_float: case mc_descriptor_bool: case mc_descriptor_function:
			mc_vm_write(descriptor->code, memory, _mc_scalar_word(constant));
			return;
		case mc_descriptor_array: {
			const mc_bc_type_s* const element = &module->types.data[descriptor->element];
			if (constant->kind != mc_constant_list || (int64_t)constant->items.count != descriptor->length
					|| element->size * (uint64_t)descriptor->length > descriptor->size) {
				mc_vm_fail(vm, "the module is broken (a constant does not fit its type)");
			}
			for (size_t i = 0; i < constant->items.count; ++i) _mc_image(vm, &constant->items.data[i], descriptor->element, memory + i * element->size, depth + 1);
			return;
		}
		case mc_descriptor_struct:
			if (constant->kind != mc_constant_list || constant->items.count != descriptor->field_types.count) {
				mc_vm_fail(vm, "the module is broken (a constant does not fit its type)");
			}
			for (size_t i = 0; i < constant->items.count; ++i) {
				const int64_t field = descriptor->field_types.data[i];
				if (descriptor->field_offsets.data[i] + module->types.data[field].size > descriptor->size) mc_vm_fail(vm, "the module is broken (a field is outside its struct)");
				_mc_image(vm, &constant->items.data[i], field, memory + descriptor->field_offsets.data[i], depth + 1);
			}
			return;
		case mc_descriptor_slice: {
			if (constant->kind != mc_constant_slice) mc_vm_fail(vm, "the module is broken (a constant does not fit its type)");
			memset(memory, 0, 16);
			if (constant->null) return;
			const uint64_t element = module->types.data[descriptor->element].size;
			if (constant->start < 0 || constant->length < 0 || constant->start + constant->length > (int64_t)constant->items.count) {
				mc_vm_fail(vm, "the module is broken (a constant slice is out of bounds)");
			}
			uint64_t handle = _mc_static_items(vm, constant, descriptor->element, depth);
			if (!constant->read_only) handle &= ~mc_handle_read_only;
			const uint64_t words[2] = { handle, mc_slice_word((uint64_t)constant->start * element, (uint64_t)constant->length), };
			memcpy(memory, words, 16);
			return;
		}
		default:
			mc_vm_fail(vm, "the module is broken (an unknown type)");
	}
}

void mc_vm_load_constants(mc_vm_s* const vm) {
	const mc_module_bc_s* const module = vm->module;
	for (size_t i = 0; i < module->types.count; ++i) {
		const mc_bc_type_s* const type = &module->types.data[i];
		if (type->size > ((uint64_t)1 << 32)) mc_vm_fail(vm, "the module is broken (a type is too large)");
	}
	for (size_t i = 0; i < module->constants.count; ++i) {
		const mc_bc_constant_s* const constant = &module->constants.data[i];
		mc_vm_constant_s ready = {0};
		if (constant->type >= 0) {
			const uint64_t size = module->types.data[constant->type].size;
			ready.count = (size_t)((size + 7) / 8);
			ready.words = mc_calloc(ready.count > 0 ? ready.count : 1, sizeof(mc_word_u));
			// kept before it is filled: filling may stop the machine
			mc_vector_push(&vm->constants, ready);
			_mc_image(vm, constant, constant->type, (uint8_t*)ready.words, 0);
			continue;
		}
		else if (mc_constant_slice == constant->kind) {
			ready.count = 2;
			ready.words = mc_calloc(2, sizeof(mc_word_u));
		}
		else if (constant->kind != mc_constant_none && constant->kind != mc_constant_text) {
			ready.count = 1;
			ready.words = mc_calloc(1, sizeof(mc_word_u));
			ready.words[0] = _mc_scalar_word(constant);
		}
		mc_vector_push(&vm->constants, ready);
	}
}

uint64_t mc_vm_constant_handle(mc_vm_s* const vm, const int64_t index) {
	mc_vm_constant_s* const constant = &vm->constants.data[index];
	if (0 == constant->handle) {
		const int64_t type = vm->module->constants.data[index].type;
		const uint64_t size = (type >= 0) ? vm->module->types.data[type].size : constant->count * 8;
		constant->handle = mc_vm_block_new(vm, mc_block_static, (uint8_t*)constant->words, size, NULL) | mc_handle_read_only;
	}
	return constant->handle;
}

const uint8_t* mc_vm_zero(mc_vm_s* const vm, const int64_t type) {
	const int64_t zero = vm->module->types.data[type].zero;
	if (zero < 0) return NULL;
	return (const uint8_t*)vm->constants.data[zero].words;
}

void mc_vm_free_memory(mc_vm_s* const vm) {
	for (int kind = 0; kind < mc_block_kind_count_; ++kind) {
		mc_blocks_s* const table = &vm->blocks[kind];
		for (uint32_t i = 1; i < table->count; ++i) {
			mc_block_s* const block = &table->items[i];
			if (block->alive && block->release != NULL) block->release(block->data);
		}
		mc_free(table->items);
		*table = (mc_blocks_s) {0};
	}
	for (size_t i = 0; i < vm->constants.count; ++i) mc_free(vm->constants.data[i].words);
	mc_vector_free(&vm->constants);
	for (size_t i = 0; i < vm->chunks.count; ++i) mc_free(vm->chunks.data[i].data);
	mc_vector_free(&vm->chunks);
	mc_free(vm->stack);
	vm->stack = NULL;
}
