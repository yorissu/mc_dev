
/// the machine's memory: heap objects, the garbage collector, copying
/// values, locations and slices.

#include "machine.h"
#include "mc/format.h"

#include <string.h>

// --- objects -----------------------------------------------------------------------------

static void* _mc_object(mc_vm_s* const vm, const size_t size, const mc_object_kind_e kind, const size_t weight) {
	mc_object_s* const object = mc_calloc(1, size);
	object->kind = kind;
	object->next = vm->objects;
	vm->objects = object;
	vm->allocated += 1 + weight;
	return object;
}

mc_list_s* mc_vm_list(mc_vm_s* const vm, const size_t count) {
	mc_list_s* const list = _mc_object(vm, sizeof(mc_list_s), mc_object_list, count);
	list->count = count;
	list->items = (count > 0) ? mc_calloc(count, sizeof(mc_value_s)) : NULL;
	return list;
}

mc_block_s* mc_vm_block(mc_vm_s* const vm, mc_list_s* const data, mc_object_s* const owner, const mc_block_kind_e kind) {
	mc_block_s* const block = _mc_object(vm, sizeof(mc_block_s), mc_object_block, 0);
	block->data = data;
	block->owner = owner;
	block->kind = kind;
	return block;
}

mc_place_s* mc_vm_place(mc_vm_s* const vm, mc_list_s* const container, const int64_t key, mc_object_s* const owner,
						mc_block_s* const block, const bool read_only) {
	mc_place_s* const place = _mc_object(vm, sizeof(mc_place_s), mc_object_location, 0);
	place->container = container;
	place->key = key;
	place->owner = owner;
	place->block = block;
	place->read_only = read_only;
	return place;
}

mc_frame_record_s* mc_vm_frame_record(mc_vm_s* const vm, const size_t locals) {
	mc_list_s* const list = mc_vm_list(vm, locals);
	mc_frame_record_s* const record = _mc_object(vm, sizeof(mc_frame_record_s), mc_object_frame, 0);
	record->locals = list;
	record->alive = true;
	return record;
}

static void _mc_free_object(mc_object_s* const object) {
	if (mc_object_list == object->kind) mc_free(((mc_list_s*)object)->items);
	mc_free(object);
}

void mc_vm_free_objects(mc_vm_s* const vm) {
	mc_object_s* object = vm->objects;
	while (object != NULL) {
		mc_object_s* const next = object->next;
		_mc_free_object(object);
		object = next;
	}
	vm->objects = NULL;
	mc_vector_free(&vm->gray);
}

// --- the collector -------------------------------------------------------------------------

static void _mc_mark_object(mc_vm_s* const vm, mc_object_s* const object) {
	if (NULL == object || object->marked) return;
	object->marked = true;
	mc_vector_push(&vm->gray, object);
}

static void _mc_mark_value(mc_vm_s* const vm, const mc_value_s* const value) {
	switch (value->kind) {
		case mc_value_list:     _mc_mark_object(vm, &value->as.list->object); break;
		case mc_value_slice:    if (value->as.slice.block != NULL) _mc_mark_object(vm, &value->as.slice.block->object); break;
		case mc_value_location: _mc_mark_object(vm, &value->as.location->object); break;
		default:                break;
	}
}

static void _mc_trace(mc_vm_s* const vm, mc_object_s* const object) {
	switch (object->kind) {
		case mc_object_list: {
			const mc_list_s* const list = (const mc_list_s*)object;
			for (size_t i = 0; i < list->count; ++i) _mc_mark_value(vm, &list->items[i]);
			break;
		}
		case mc_object_block: {
			mc_block_s* const block = (mc_block_s*)object;
			_mc_mark_object(vm, &block->data->object);
			_mc_mark_object(vm, block->owner);
			break;
		}
		case mc_object_location: {
			mc_place_s* const place = (mc_place_s*)object;
			_mc_mark_object(vm, &place->container->object);
			_mc_mark_object(vm, place->owner);
			if (place->block != NULL) _mc_mark_object(vm, &place->block->object);
			break;
		}
		case mc_object_frame:
			_mc_mark_object(vm, &((mc_frame_record_s*)object)->locals->object);
			break;
	}
}

/// marks what the stack, the frames and the constants reach, then frees the rest.
void mc_vm_collect(mc_vm_s* const vm) {
	for (size_t i = 0; i < vm->stack.count; ++i) _mc_mark_value(vm, &vm->stack.data[i]);
	for (size_t i = 0; i < vm->frames.count; ++i) _mc_mark_object(vm, &vm->frames.data[i].record->object);
	for (size_t i = 0; i < vm->constants.count; ++i) _mc_mark_value(vm, &vm->constants.data[i]);
	_mc_mark_value(vm, &vm->result);
	while (vm->gray.count > 0) _mc_trace(vm, mc_vector_pop(&vm->gray));
	size_t live = 0;
	mc_object_s** link = &vm->objects;
	while (*link != NULL) {
		mc_object_s* const object = *link;
		if (object->marked) {
			object->marked = false;
			live += 1 + ((mc_object_list == object->kind) ? ((mc_list_s*)object)->count : 0);
			link = &object->next;
		}
		else {
			*link = object->next;
			_mc_free_object(object);
		}
	}
	vm->live = live;
	vm->allocated = 0;
	vm->threshold = (live > 32768) ? live : 32768;
	vm->collections++;
}

// --- values --------------------------------------------------------------------------------

mc_value_s mc_vm_copy(mc_vm_s* const vm, const mc_value_s value) {
	if (value.kind != mc_value_list) return value;
	const mc_list_s* const source = value.as.list;
	mc_list_s* const list = mc_vm_list(vm, source->count);
	for (size_t i = 0; i < source->count; ++i) list->items[i] = mc_vm_copy(vm, source->items[i]);
	return mc_make_list(list);
}

mc_value_s mc_vm_assign(mc_vm_s* const vm, const mc_value_s old, const mc_value_s value) {
	if (mc_value_list == old.kind && mc_value_list == value.kind && old.as.list->count == value.as.list->count) {
		mc_list_s* const target = old.as.list;
		const mc_list_s* const source = value.as.list;
		for (size_t i = 0; i < target->count; ++i) target->items[i] = mc_vm_assign(vm, target->items[i], source->items[i]);
		return old;
	}
	return mc_vm_copy(vm, value);
}

mc_value_s mc_vm_zero(mc_vm_s* const vm, const int64_t type) {
	const int64_t zero = vm->module->types.data[type].zero;
	if (zero < 0) return mc_make_none();
	return mc_vm_copy(vm, vm->constants.data[zero]);
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

// --- memory checks ---------------------------------------------------------------------------

static bool _mc_owner_alive(const mc_object_s* owner) {
	while (owner != NULL) {
		if (mc_object_frame == owner->kind) return ((const mc_frame_record_s*)owner)->alive;
		const mc_block_s* const block = (const mc_block_s*)owner;
		if (block->freed) return false;
		owner = block->owner;
	}
	return true;
}

void mc_vm_check(mc_vm_s* const vm, const mc_block_s* const block) {
	if (NULL == block || (!block->freed && _mc_owner_alive(block->owner))) return;
	const bool heap_owner = block->owner != NULL && mc_object_block == block->owner->kind && mc_block_heap == ((const mc_block_s*)block->owner)->kind;
	if (mc_block_heap == block->kind || heap_owner) mc_vm_fail(vm, "use of freed heap memory");
	mc_vm_fail(vm, "use of a local array of a function that has returned");
}

mc_value_s mc_vm_get(mc_vm_s* const vm, const mc_place_s* const place) {
	if (place->block != NULL) mc_vm_check(vm, place->block);
	return place->container->items[place->key];
}

void mc_vm_set(mc_vm_s* const vm, const mc_place_s* const place, const mc_value_s value) {
	if (place->block != NULL) mc_vm_check(vm, place->block);
	if (place->read_only) mc_vm_fail(vm, "write through a read-only slice");
	mc_value_s* const target = &place->container->items[place->key];
	*target = mc_vm_assign(vm, *target, value);
}

mc_value_s mc_vm_slice_of(mc_vm_s* const vm, mc_list_s* const data, mc_object_s* const owner, const bool read_only, const int64_t flags,
						  const bool has_low, const __int128 low_value, const bool has_high, const __int128 high_value,
						  const int64_t start, const int64_t length, mc_block_s* block) {
	const __int128 low = has_low ? low_value : 0;
	const __int128 high = has_high ? high_value + ((flags & mc_slice_inclusive) ? 1 : 0) : length;
	if (!(0 <= low && low <= high && high <= length)) {
		char low_text[48], high_text[48];
		mc_vm_fail(vm, "slice %s..%s is out of bounds (length %lld)", mc_vm_int_text(low_text, low), mc_vm_int_text(high_text, high),
			(long long)length);
	}
	if (NULL == block) block = mc_vm_block(vm, data, owner, (owner != NULL) ? mc_block_stack : mc_block_static);
	return mc_make_slice(block, start + (int64_t)low, (int64_t)(high - low), read_only);
}
