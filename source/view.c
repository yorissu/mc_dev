
#include "mia/view.h"

#include <string.h>

mia_view_s mia_view_new(const char* const data, const size_t length) {
	return (const mia_view_s) { .data = data, .length = length, };
}

mia_view_s mia_view_cstr(const char* const cstr) {
	mia_assert(cstr != NULL);
	return mia_view_new(cstr, strlen(cstr));
}

bool mia_view_eq(const mia_view_s lhs, const mia_view_s rhs) {
	if (lhs.length != rhs.length) return false;
	if (0 == lhs.length)          return true;
	return 0 == memcmp(lhs.data, rhs.data, lhs.length);
}

bool mia_view_eq_cstr(const mia_view_s lhs, const char* const rhs) {
	return mia_view_eq(lhs, mia_view_cstr(rhs));
}

mia_view_s mia_view_slice(const mia_view_s view, const size_t begin, const size_t end) {
	const size_t last  = (end > view.length) ? view.length : end;
	const size_t first = (begin > last) ? last : begin;
	return mia_view_new(view.data + first, last - first);
}

uint64_t mia_view_hash(const mia_view_s view) {
	uint64_t hash = 0xcbf29ce484222325u;
	for (size_t index = 0; index < view.length; ++index) {
		hash ^= (uint8_t)view.data[index];
		hash *= 0x100000001b3u;
	}
	return hash;
}
