
#include "mia/arena.h"

#include <stdalign.h>
#include <string.h>

#define _mia_arena_chunk_capacity ((size_t)64 * 1024)

struct mia_arena_chunk_s {
	mia_arena_chunk_s*           next    ;
	size_t                       capacity;
	size_t                       used    ;
	alignas(max_align_t) uint8_t data[]  ;
};

static mia_arena_chunk_s* _mia_arena_chunk_new(const size_t capacity) {
	mia_arena_chunk_s* const chunk = mia_malloc(sizeof(*chunk) + capacity);
	chunk->next     = NULL    ;
	chunk->capacity = capacity;
	chunk->used     = 0       ;
	return chunk;
}

static size_t _mia_arena_align_up(const size_t size) {
	const size_t alignment = alignof(max_align_t);
	return (size + alignment - 1) & ~(alignment - 1);
}

void* mia_arena_alloc(mia_arena_s* const arena, const size_t size) {
	mia_assert(arena != NULL);
	const size_t aligned = _mia_arena_align_up(size > 0 ? size : 1);
	mia_arena_chunk_s* chunk = arena->head;

	if (aligned > (_mia_arena_chunk_capacity / 4)) {
		mia_arena_chunk_s* const dedicated = _mia_arena_chunk_new(aligned);
		dedicated->used = aligned;
		if (NULL == chunk) {
			arena->head = dedicated;
		}
		else {
			dedicated->next = chunk->next;
			chunk->next = dedicated;
		}
		(void)memset(dedicated->data, 0, aligned);
		return dedicated->data;
	}

	if ((NULL == chunk) || ((chunk->capacity - chunk->used) < aligned)) {
		chunk = _mia_arena_chunk_new(_mia_arena_chunk_capacity);
		chunk->next = arena->head;
		arena->head = chunk;
	}

	void* const pointer = chunk->data + chunk->used;
	chunk->used += aligned;
	(void)memset(pointer, 0, aligned);
	return pointer;
}

char* mia_arena_strndup(mia_arena_s* const arena, const char* const data, const size_t length) {
	char* const copy = mia_arena_alloc(arena, length + 1);
	if (length > 0) (void)memcpy(copy, data, length);
	copy[length] = '\0';
	return copy;
}

void mia_arena_free(mia_arena_s* const arena) {
	mia_assert(arena != NULL);
	mia_arena_chunk_s* chunk = arena->head;
	while (chunk != NULL) {
		mia_arena_chunk_s* const next = chunk->next;
		mia_free(chunk);
		chunk = next;
	}
	arena->head = NULL;
}
