
#ifndef __mc__string_h__
#define __mc__string_h__

/// text: views (pointer and length, not owned) and a growable builder.

#include "mc/arena.h"
#include "mc/global.h"

#include <stdarg.h>

typedef struct {
	const char* data  ;
	size_t      length;
} mc_view_s;

#define mc_view_fmt "%.*s"
#define mc_view_arg(_view) (int)(_view).length, (_view).data
#define mc_view_literal(_literal) ((const mc_view_s) { .data = "" _literal, .length = sizeof(_literal) - 1, })

mc_view_s mc_view_new(const char* const data, const size_t length);

mc_view_s mc_view_cstr(const char* const cstr);

bool mc_view_eq(const mc_view_s lhs, const mc_view_s rhs);

bool mc_view_eq_cstr(const mc_view_s lhs, const char* const rhs);

bool mc_view_starts_with(const mc_view_s view, const mc_view_s prefix);

bool mc_view_ends_with(const mc_view_s view, const mc_view_s suffix);

mc_view_s mc_view_slice(const mc_view_s view, const size_t begin, const size_t end);

uint64_t mc_view_hash(const mc_view_s view);

/// a null-terminated copy in 'arena'.
const char* mc_view_dup(mc_arena_s* const arena, const mc_view_s view);

typedef struct {
	char*  data    ;
	size_t length  ;
	size_t capacity;
} mc_string_s;

void mc_string_append(mc_string_s* const string, const mc_view_s view);

void mc_string_append_cstr(mc_string_s* const string, const char* const cstr);

void mc_string_append_char(mc_string_s* const string, const char symbol);

void mc_string_append_repeat(mc_string_s* const string, const char symbol, const size_t count);

void mc_string_appendf(mc_string_s* const string, const char* const format, ...) mc_printf_like(2, 3);

void mc_string_vappendf(mc_string_s* const string, const char* const format, va_list args) mc_printf_like(2, 0);

/// the text so far; never null.
const char* mc_string_cstr(const mc_string_s* const string);

mc_view_s mc_string_view(const mc_string_s* const string);

void mc_string_clear(mc_string_s* const string);

/// hands the buffer over to the caller (who frees it) and empties the string.
char* mc_string_take(mc_string_s* const string);

void mc_string_free(mc_string_s* const string);

/// a formatted text owned by the caller.
char* mc_format(const char* const format, ...) mc_printf_like(1, 2);

char* mc_vformat(const char* const format, va_list args) mc_printf_like(1, 0);

#endif
