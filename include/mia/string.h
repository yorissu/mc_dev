
#ifndef __mia__string_h__
#define __mia__string_h__

/// text that grows: a builder with its own buffer.

#include "mia/view.h"

#include <stdarg.h>

typedef struct {
	char*  data    ;
	size_t length  ;
	size_t capacity;
} mia_string_s;

/// appends to a string (which grows as needed).
void mia_string_append(mia_string_s* const string, const mia_view_s view);

void mia_string_append_cstr(mia_string_s* const string, const char* const cstr);

void mia_string_append_char(mia_string_s* const string, const char symbol);

void mia_string_append_repeat(mia_string_s* const string, const char symbol, const size_t count);

void mia_string_appendf(mia_string_s* const string, const char* const format, ...) mia_printf_like(2, 3);

void mia_string_vappendf(mia_string_s* const string, const char* const format, va_list args) mia_printf_like(2, 0);

/// the text so far; never null.
const char* mia_string_cstr(const mia_string_s* const string);

mia_view_s mia_string_view(const mia_string_s* const string);

void mia_string_clear(mia_string_s* const string);

/// hands the buffer over to the caller (who frees it) and empties the string.
char* mia_string_take(mia_string_s* const string);

void mia_string_free(mia_string_s* const string);

/// a formatted text owned by the caller.
char* mia_string_format(const char* const format, ...) mia_printf_like(1, 2);

char* mia_string_vformat(const char* const format, va_list args) mia_printf_like(1, 0);

#endif
