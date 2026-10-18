
#ifndef __mia__arena_h__
#define __mia__arena_h__

/// arena allocation: many small zeroed allocations freed all at once. syntax
/// trees, types and symbols of one compilation live in one arena.

#include "mia/global.h"

#include <string.h>

typedef struct mia_arena_chunk_s mia_arena_chunk_s;

typedef struct {
	mia_arena_chunk_s* head;
} mia_arena_s;

/// 'size' zeroed bytes, aligned for any type.
void* mia_arena_alloc(mia_arena_s* const arena, const size_t size);

#define mia_arena_new(_arena, _type) ((_type*)mia_arena_alloc((_arena), sizeof(_type)))

/// a copy of 'length' bytes of 'data' with a null terminator.
char* mia_arena_strndup(mia_arena_s* const arena, const char* const data, const size_t length);

void mia_arena_free(mia_arena_s* const arena);

/// pushes onto a vector (data, count, capacity) whose memory lives in 'arena':
/// nothing to free, so syntax trees can be abandoned at any point.
#define mia_arena_grow(_arena, _vector) do {                                                                       \
		if ((_vector)->count == (_vector)->capacity) {                                                             \
			const size_t _capacity = (_vector)->capacity ? (_vector)->capacity * 2 : 4;                            \
			void* const _data = mia_arena_alloc((_arena), _capacity * sizeof(*(_vector)->data));                   \
			if ((_vector)->count > 0) memcpy(_data, (_vector)->data, (_vector)->count * sizeof(*(_vector)->data)); \
			(_vector)->data = _data;                                                                               \
			(_vector)->capacity = _capacity;                                                                       \
		}                                                                                                          \
	} while (0)

/// the item is evaluated like in mia_vector_push (see vector.h).
#if mia_has_typeof
#	define mia_arena_push(_arena, _vector, _item) do {                               \
		__typeof__(*(_vector)->data) _mia_arena_item = (_item);                      \
		mia_arena_grow(_arena, _vector);                                             \
		(_vector)->data[(_vector)->count++] = _mia_arena_item;                       \
	} while (0)
#else
#	define mia_arena_push(_arena, _vector, _item) do {                         \
		mia_arena_grow(_arena, _vector);                                       \
		(_vector)->data[(_vector)->count] = (_item);                           \
		(_vector)->count += 1;                                                 \
	} while (0)
#endif

#endif
