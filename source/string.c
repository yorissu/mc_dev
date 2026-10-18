
#include "mia/string.h"

#include <string.h>
#include <stdio.h>

static void _mia_string_reserve(mia_string_s* const string, const size_t extra) {
	const size_t needed = string->length + extra + 1;
	if (needed <= string->capacity) return;
	size_t capacity = string->capacity > 0 ? string->capacity : 64;
	while (capacity < needed) capacity *= 2;
	string->data = mia_realloc(string->data, capacity);
	string->capacity = capacity;
}

void mia_string_append(mia_string_s* const string, const mia_view_s view) {
	mia_assert(string != NULL);
	_mia_string_reserve(string, view.length);
	if (view.length > 0) (void)memmove(string->data + string->length, view.data, view.length);
	string->length += view.length;
	string->data[string->length] = '\0';
}

void mia_string_append_cstr(mia_string_s* const string, const char* const cstr) {
	mia_string_append(string, mia_view_cstr(cstr));
}

void mia_string_append_char(mia_string_s* const string, const char symbol) {
	_mia_string_reserve(string, 1);
	string->data[string->length++] = symbol;
	string->data[string->length] = '\0';
}

void mia_string_append_repeat(mia_string_s* const string, const char symbol, const size_t count) {
	mia_assert(string != NULL);
	_mia_string_reserve(string, count);
	if (count > 0) (void)memset(string->data + string->length, symbol, count);
	string->length += count;
	string->data[string->length] = '\0';
}

void mia_string_appendf(mia_string_s* const string, const char* const format, ...) {
	va_list args; va_start(args, format);
	mia_string_vappendf(string, format, args);
	va_end(args);
}

void mia_string_vappendf(mia_string_s* const string, const char* const format, va_list args) {
	mia_assert(string != NULL);
	mia_assert(format != NULL);
	va_list copy; va_copy(copy, args);
	const int length = vsnprintf(NULL, 0, format, copy);
	va_end(copy);
	if (length < 0) mia_panic("invalid format string '%s'", format);
	_mia_string_reserve(string, (size_t)length);
	(void)vsnprintf(string->data + string->length, (size_t)length + 1, format, args);
	string->length += (size_t)length;
}

const char* mia_string_cstr(const mia_string_s* const string) {
	return (string->data != NULL) ? string->data : "";
}

mia_view_s mia_string_view(const mia_string_s* const string) {
	return mia_view_new(mia_string_cstr(string), string->length);
}

void mia_string_clear(mia_string_s* const string) {
	string->length = 0;
	if (string->data != NULL) string->data[0] = '\0';
}

char* mia_string_take(mia_string_s* const string) {
	char* const data = (string->data != NULL) ? string->data : mia_strdup("");
	*string = (const mia_string_s) {0};
	return data;
}

void mia_string_free(mia_string_s* const string) {
	mia_free(string->data);
	*string = (const mia_string_s) {0};
}

char* mia_string_format(const char* const format, ...) {
	va_list args; va_start(args, format);
	char* const text = mia_string_vformat(format, args);
	va_end(args);
	return text;
}

char* mia_string_vformat(const char* const format, va_list args) {
	mia_string_s string = {0};
	mia_string_vappendf(&string, format, args);
	return mia_string_take(&string);
}
