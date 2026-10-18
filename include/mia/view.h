
#ifndef __mia__view_h__
#define __mia__view_h__

/// views of text: a pointer and a length, not owned.

#include "mia/arena.h"
#include "mia/global.h"
#include "mia/vector.h"

typedef struct {
	const char* data  ;
	size_t      length;
} mia_view_s;

#define mia_view_fmt "%.*s"
#define mia_view_arg(_view) (int)(_view).length, (_view).data
#define mia_view_literal(_literal) ((const mia_view_s) { .data = "" _literal, .length = sizeof(_literal) - 1, })

/// views: made from memory or a c string, compared, sliced, hashed.
mia_view_s mia_view_new(const char* const data, const size_t length);

mia_view_s mia_view_cstr(const char* const cstr);

bool mia_view_eq(const mia_view_s lhs, const mia_view_s rhs);

bool mia_view_eq_cstr(const mia_view_s lhs, const char* const rhs);

mia_view_s mia_view_slice(const mia_view_s view, const size_t begin, const size_t end);

uint64_t mia_view_hash(const mia_view_s view);

typedef mia_vector_of(mia_view_s) mia_view_list_s;

#endif
