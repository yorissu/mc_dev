
#include "mc/string.h"

#include <stdio.h>
#include <string.h>

mc_view_s mc_view_new(const char* const data, const size_t length) {
	return (const mc_view_s) { .data = data, .length = length, };
}

mc_view_s mc_view_cstr(const char* const cstr) {
	mc_assert(cstr != NULL);
	return mc_view_new(cstr, strlen(cstr));
}

bool mc_view_eq(const mc_view_s lhs, const mc_view_s rhs) {
	if (lhs.length != rhs.length) return false;
	if (0 == lhs.length)          return true;
	return 0 == memcmp(lhs.data, rhs.data, lhs.length);
}

bool mc_view_eq_cstr(const mc_view_s lhs, const char* const rhs) {
	return mc_view_eq(lhs, mc_view_cstr(rhs));
}

bool mc_view_starts_with(const mc_view_s view, const mc_view_s prefix) {
	if (prefix.length > view.length) return false;
	return mc_view_eq(mc_view_new(view.data, prefix.length), prefix);
}

bool mc_view_ends_with(const mc_view_s view, const mc_view_s suffix) {
	if (suffix.length > view.length) return false;
	return mc_view_eq(mc_view_new(view.data + view.length - suffix.length, suffix.length), suffix);
}

mc_view_s mc_view_slice(const mc_view_s view, const size_t begin, const size_t end) {
	const size_t last  = (end > view.length) ? view.length : end;
	const size_t first = (begin > last) ? last : begin;
	return mc_view_new(view.data + first, last - first);
}

uint64_t mc_view_hash(const mc_view_s view) {
	uint64_t hash = 0xcbf29ce484222325u;
	for (size_t index = 0; index < view.length; ++index) {
		hash ^= (uint8_t)view.data[index];
		hash *= 0x100000001b3u;
	}
	return hash;
}

const char* mc_view_dup(mc_arena_s* const arena, const mc_view_s view) {
	return mc_arena_strndup(arena, view.data, view.length);
}

static void _mc_string_reserve(mc_string_s* const string, const size_t extra) {
	const size_t needed = string->length + extra + 1;
	if (needed <= string->capacity) return;
	size_t capacity = string->capacity > 0 ? string->capacity : 64;
	while (capacity < needed) capacity *= 2;
	string->data = mc_realloc(string->data, capacity);
	string->capacity = capacity;
}

void mc_string_append(mc_string_s* const string, const mc_view_s view) {
	mc_assert(string != NULL);
	_mc_string_reserve(string, view.length);
	if (view.length > 0) (void)memmove(string->data + string->length, view.data, view.length);
	string->length += view.length;
	string->data[string->length] = '\0';
}

void mc_string_append_cstr(mc_string_s* const string, const char* const cstr) {
	mc_string_append(string, mc_view_cstr(cstr));
}

void mc_string_append_char(mc_string_s* const string, const char symbol) {
	_mc_string_reserve(string, 1);
	string->data[string->length++] = symbol;
	string->data[string->length] = '\0';
}

void mc_string_append_repeat(mc_string_s* const string, const char symbol, const size_t count) {
	mc_assert(string != NULL);
	_mc_string_reserve(string, count);
	if (count > 0) (void)memset(string->data + string->length, symbol, count);
	string->length += count;
	string->data[string->length] = '\0';
}

void mc_string_appendf(mc_string_s* const string, const char* const format, ...) {
	va_list args; va_start(args, format);
	mc_string_vappendf(string, format, args);
	va_end(args);
}

void mc_string_vappendf(mc_string_s* const string, const char* const format, va_list args) {
	mc_assert(string != NULL);
	mc_assert(format != NULL);
	va_list copy; va_copy(copy, args);
	const int length = vsnprintf(NULL, 0, format, copy);
	va_end(copy);
	if (length < 0) mc_panic("invalid format string '%s'", format);
	_mc_string_reserve(string, (size_t)length);
	(void)vsnprintf(string->data + string->length, (size_t)length + 1, format, args);
	string->length += (size_t)length;
}

const char* mc_string_cstr(const mc_string_s* const string) {
	return (string->data != NULL) ? string->data : "";
}

mc_view_s mc_string_view(const mc_string_s* const string) {
	return mc_view_new(mc_string_cstr(string), string->length);
}

void mc_string_clear(mc_string_s* const string) {
	string->length = 0;
	if (string->data != NULL) string->data[0] = '\0';
}

char* mc_string_take(mc_string_s* const string) {
	char* const data = (string->data != NULL) ? string->data : mc_strdup("");
	*string = (const mc_string_s) {0};
	return data;
}

void mc_string_free(mc_string_s* const string) {
	mc_free(string->data);
	*string = (const mc_string_s) {0};
}

char* mc_format(const char* const format, ...) {
	va_list args; va_start(args, format);
	char* const text = mc_vformat(format, args);
	va_end(args);
	return text;
}

char* mc_vformat(const char* const format, va_list args) {
	mc_string_s string = {0};
	mc_string_vappendf(&string, format, args);
	return mc_string_take(&string);
}
