
#ifndef __mc__vector_h__
#define __mc__vector_h__

/// growable arrays: any struct with 'data', 'count' and 'capacity' members.
///
///     typedef mc_vector_of(int) ints_s;
///     ints_s numbers = {0};
///     mc_vector_push(&numbers, 42);
///     mc_vector_free(&numbers);

#include "mc/global.h"

#define mc_vector_of(_type) struct {                                           \
		_type* data    ;                                                       \
		size_t count   ;                                                       \
		size_t capacity;                                                       \
	}

#define mc_vector_reserve(_vector, _capacity) do {                             \
		if ((_vector)->capacity < (_capacity)) {                               \
			(_vector)->capacity = (_capacity);                                 \
			(_vector)->data = mc_realloc((_vector)->data,                      \
				(_vector)->capacity * sizeof(*(_vector)->data));               \
		}                                                                      \
	} while (0)

#define mc_vector_push(_vector, _item) do {                                    \
		__typeof__(*(_vector)->data) _mc_item = (_item);                       \
		if ((_vector)->count == (_vector)->capacity) {                         \
			(_vector)->capacity = (                                            \
				((_vector)->capacity > 0) ? (_vector)->capacity * 2 : 8);      \
			(_vector)->data = mc_realloc((_vector)->data,                      \
				(_vector)->capacity * sizeof(*(_vector)->data));               \
		}                                                                      \
		(_vector)->data[(_vector)->count++] = _mc_item;                        \
	} while (0)

#define mc_vector_pop(_vector) ((_vector)->data[--(_vector)->count])

#define mc_vector_last(_vector) ((_vector)->data[(_vector)->count - 1])

#define mc_vector_free(_vector) do {                                           \
		mc_free((_vector)->data);                                              \
		(_vector)->data     = NULL;                                            \
		(_vector)->count    = 0   ;                                            \
		(_vector)->capacity = 0   ;                                            \
	} while (0)

typedef mc_vector_of(void*) mc_pointers_s;

#endif
