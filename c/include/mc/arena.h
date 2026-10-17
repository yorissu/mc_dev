
#ifndef __mc__arena_h__
#define __mc__arena_h__

/// arena allocation: many small zeroed allocations freed all at once. syntax
/// trees, types and symbols of one compilation live in one arena.

#include "mc/global.h"

#include <string.h>

typedef struct mc_arena_chunk_s mc_arena_chunk_s;

typedef struct {
	mc_arena_chunk_s* head;
} mc_arena_s;

/// 'size' zeroed bytes, aligned for any type.
void* mc_arena_alloc(mc_arena_s* const arena, const size_t size);

#define mc_arena_new(_arena, _type) ((_type*)mc_arena_alloc((_arena), sizeof(_type)))

/// a copy of 'length' bytes of 'data' with a null terminator.
char* mc_arena_strndup(mc_arena_s* const arena, const char* const data, const size_t length);

void mc_arena_free(mc_arena_s* const arena);

/// pushes onto a vector (data, count, capacity) whose memory lives in 'arena':
/// nothing to free, so syntax trees can be abandoned at any point.
#define mc_arena_push(_arena, _vector, _item) do {                             \
		__typeof__(*(_vector)->data) _mc_item = (_item);                       \
		if ((_vector)->count == (_vector)->capacity) {                         \
			const size_t _capacity = (_vector)->capacity ? (_vector)->capacity * 2 : 4; \
			void* const _data = mc_arena_alloc((_arena), _capacity * sizeof(*(_vector)->data)); \
			if ((_vector)->count > 0) memcpy(_data, (_vector)->data, (_vector)->count * sizeof(*(_vector)->data)); \
			(_vector)->data = _data;                                           \
			(_vector)->capacity = _capacity;                                   \
		}                                                                      \
		(_vector)->data[(_vector)->count++] = _mc_item;                        \
	} while (0)

#endif
