
#ifndef __mia__vector_h__
#define __mia__vector_h__

/// growable arrays: any struct with 'data', 'count' and 'capacity' members.
///
///     typedef mia_vector_of(int) ints_s;
///     ints_s numbers = {0};
///     mia_vector_push(&numbers, 42);
///     mia_vector_free(&numbers);

#include "mia/global.h"

#define mia_vector_of(_type) struct {                                          \
		_type* data    ;                                                       \
		size_t count   ;                                                       \
		size_t capacity;                                                       \
	}

#define mia_vector_reserve(_vector, _capacity) do {                            \
		if ((_vector)->capacity < (_capacity)) {                               \
			(_vector)->capacity = (_capacity);                                 \
			(_vector)->data = mia_realloc((_vector)->data,                     \
				(_vector)->capacity * sizeof(*(_vector)->data));               \
		}                                                                      \
	} while (0)

#define mia_vector_grow(_vector) do {                                          \
		if ((_vector)->count == (_vector)->capacity) {                         \
			(_vector)->capacity = (                                            \
				((_vector)->capacity > 0) ? (_vector)->capacity * 2 : 8);      \
			(_vector)->data = mia_realloc((_vector)->data,                     \
				(_vector)->capacity * sizeof(*(_vector)->data));               \
		}                                                                      \
	} while (0)

/// with __typeof__ the item is evaluated before the vector grows, so it may
/// read the vector's own elements; standard c evaluates it into the new slot
/// (the item must then not push onto the same vector, which no code does).
#if mia_has_typeof
#	define mia_vector_push(_vector, _item) do {                                       \
		__typeof__(*(_vector)->data) _mia_vector_item = (_item);                      \
		mia_vector_grow(_vector);                                                     \
		(_vector)->data[(_vector)->count++] = _mia_vector_item;                       \
	} while (0)
#else
#	define mia_vector_push(_vector, _item) do {                                \
		mia_vector_grow(_vector);                                              \
		(_vector)->data[(_vector)->count] = (_item);                           \
		(_vector)->count += 1;                                                 \
	} while (0)
#endif

#define mia_vector_pop(_vector) ((_vector)->data[--(_vector)->count])

#define mia_vector_last(_vector) ((_vector)->data[(_vector)->count - 1])

#define mia_vector_free(_vector) do {                                          \
		mia_free((_vector)->data);                                             \
		(_vector)->data     = NULL;                                            \
		(_vector)->count    = 0   ;                                            \
		(_vector)->capacity = 0   ;                                            \
	} while (0)

typedef mia_vector_of(void*) mia_vector_pointers_s;

#endif
