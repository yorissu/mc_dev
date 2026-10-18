
/// the plugin 'memory': the functions of the module memory (memory.mia):
/// alloc, realloc, free and copy. the machine keeps the books of heap
/// memory (mia_plugin_heap_*), so slices of it are checked for bounds and
/// for use after free.

#include "mia/plugin.h"

#include <string.h>

static uint64_t _mia_memory_element_size(mia_plugin_call_s* const call, const size_t index) {
	return mia_plugin_type_size(call, mia_plugin_type_element(call, mia_plugin_argument_type(call, index)));
}

static mia_plugin_value_s _mia_memory_alloc(mia_plugin_call_s* const call) {
	return mia_plugin_heap_alloc(call, mia_plugin_type_argument(call, 0), mia_plugin_get_uint(call, 0), NULL);
}

/// stops unless slice argument 0 is a whole allocation (or null, when 'null' allows it).
static mia_plugin_heap_e _mia_memory_whole(mia_plugin_call_s* const call, const char* const what) {
	const mia_plugin_heap_e heap = mia_plugin_heap_of(call, 0);
	if (mia_plugin_heap_other == heap) mia_plugin_fail(call, "%s of memory that `alloc` did not return", what);
	if (mia_plugin_heap_part == heap) mia_plugin_fail(call, "%s needs the whole allocation, not a part of it", what);
	return heap;
}

static mia_plugin_value_s _mia_memory_realloc(mia_plugin_call_s* const call) {
	const mia_plugin_type_s* const element = mia_plugin_type_argument(call, 0);
	const uint64_t count = mia_plugin_get_uint(call, 1);
	if (mia_plugin_heap_none == _mia_memory_whole(call, "`realloc`")) return mia_plugin_heap_alloc(call, element, count, NULL);
	const mia_plugin_slice_s old = mia_plugin_get_slice(call, 0);
	void* data = NULL;
	const mia_plugin_value_s fresh = mia_plugin_heap_alloc(call, element, count, &data);
	const uint64_t kept = (count < old.length) ? count : old.length;
	if (kept > 0) memcpy(data, old.data, (size_t)(kept * mia_plugin_type_size(call, element)));
	mia_plugin_heap_free(call, 0);
	return fresh;
}

static mia_plugin_value_s _mia_memory_free(mia_plugin_call_s* const call) {
	if (mia_plugin_heap_whole == _mia_memory_whole(call, "`free`")) mia_plugin_heap_free(call, 0);
	return mia_plugin_make_empty(call);
}

static mia_plugin_value_s _mia_memory_copy(mia_plugin_call_s* const call) {
	const uint64_t length = mia_plugin_slice_length(call, 0);
	if (mia_plugin_slice_read_only(call, 0) && length != 0) mia_plugin_fail(call, "`copy` into a read-only slice");
	const mia_plugin_slice_s source = mia_plugin_get_slice(call, 1);
	const uint64_t copied = (length < source.length) ? length : source.length;
	if (copied > 0) {
		const mia_plugin_slice_s target = mia_plugin_get_slice(call, 0);
		// the source may overlap the target
		memmove(target.data, source.data, (size_t)(copied * _mia_memory_element_size(call, 0)));
	}
	return mia_plugin_make_uint(call, copied);
}

// --- registration ------------------------------------------------------------

mia_plugin_export bool mia_plugin_init(mia_plugin_api_s* const api) {
	if (!mia_plugin_compatible(api, mia_plugin_api_version)) return false;
	return mia_plugin_register(api, "memory", "memory.alloc", _mia_memory_alloc, NULL)
		&& mia_plugin_register(api, "memory", "memory.realloc", _mia_memory_realloc, NULL)
		&& mia_plugin_register(api, "memory", "memory.free", _mia_memory_free, NULL)
		&& mia_plugin_register(api, "memory", "memory.copy", _mia_memory_copy, NULL);
}
