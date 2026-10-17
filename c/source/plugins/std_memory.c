
/// the 'std' plugin: memory.alloc, realloc, free and copy. the machine
/// keeps the books of heap memory (mc_plugin_heap_*), so slices of it are
/// checked for bounds and for use after free.

#include "std.h"

#include <string.h>

/// the size of the elements of slice argument 'index'.
static uint64_t _mc_element_size(mc_plugin_call_s* const call, const size_t index) {
	return mc_plugin_type_size(call, mc_plugin_type_element(call, mc_plugin_argument_type(call, index)));
}

mc_plugin_value_s mc_std_alloc(mc_plugin_call_s* const call) {
	return mc_plugin_heap_alloc(call, mc_plugin_type_argument(call, 0), mc_plugin_get_uint(call, 0), NULL);
}

/// stops unless slice argument 0 is a whole allocation (or null, when 'null' allows it).
static mc_plugin_heap_e _mc_whole(mc_plugin_call_s* const call, const char* const what) {
	const mc_plugin_heap_e heap = mc_plugin_heap_of(call, 0);
	if (mc_plugin_heap_other == heap) mc_plugin_fail(call, "%s of memory that `alloc` did not return", what);
	if (mc_plugin_heap_part == heap) mc_plugin_fail(call, "%s needs the whole allocation, not a part of it", what);
	return heap;
}

mc_plugin_value_s mc_std_realloc(mc_plugin_call_s* const call) {
	const mc_plugin_type_s* const element = mc_plugin_type_argument(call, 0);
	const uint64_t count = mc_plugin_get_uint(call, 1);
	if (mc_plugin_heap_null == _mc_whole(call, "`realloc`")) return mc_plugin_heap_alloc(call, element, count, NULL);
	const mc_plugin_slice_s old = mc_plugin_get_slice(call, 0);
	void* data = NULL;
	const mc_plugin_value_s fresh = mc_plugin_heap_alloc(call, element, count, &data);
	const uint64_t kept = (count < old.length) ? count : old.length;
	if (kept > 0) memcpy(data, old.data, (size_t)(kept * mc_plugin_type_size(call, element)));
	mc_plugin_heap_free(call, 0);
	return fresh;
}

mc_plugin_value_s mc_std_free(mc_plugin_call_s* const call) {
	if (mc_plugin_heap_whole == _mc_whole(call, "`free`")) mc_plugin_heap_free(call, 0);
	return mc_plugin_make_null(call);
}

mc_plugin_value_s mc_std_copy(mc_plugin_call_s* const call) {
	const uint64_t length = mc_plugin_slice_length(call, 0);
	if (mc_plugin_slice_read_only(call, 0) && length != 0) mc_plugin_fail(call, "`copy` into a read-only slice");
	const mc_plugin_slice_s source = mc_plugin_get_slice(call, 1);
	const uint64_t copied = (length < source.length) ? length : source.length;
	if (copied > 0) {
		const mc_plugin_slice_s target = mc_plugin_get_slice(call, 0);
		// the source may overlap the target
		memmove(target.data, source.data, (size_t)(copied * _mc_element_size(call, 0)));
	}
	return mc_plugin_make_uint(call, copied);
}
