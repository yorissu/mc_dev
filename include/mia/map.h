
#ifndef __mia__map_h__
#define __mia__map_h__

/// a hash map from text to pointers. keys are views; their text must outlive
/// the map (keys point into sources or arenas).

#include "mia/string.h"

typedef struct {
	mia_view_s key     ;
	void*      value   ;
	uint64_t   hash    ;
	bool       occupied;
} mia_map_entry_s;

typedef struct {
	mia_map_entry_s* entries ;
	size_t           count   ;
	size_t           capacity;
} mia_map_s;

/// the value of 'key' in '*value' (when not null); false when it has none.
bool mia_map_find(const mia_map_s* const map, const mia_view_s key, void** const value);

/// the value of 'key', or null.
void* mia_map_get(const mia_map_s* const map, const mia_view_s key);

void* mia_map_get_cstr(const mia_map_s* const map, const char* const key);

/// sets 'key'; returns whether it was set before.
bool mia_map_set(mia_map_s* const map, const mia_view_s key, void* const value);

void mia_map_free(mia_map_s* const map);

/// a hash map from 64-bit keys (numbers, or pointers through
/// mia_map_key_of) to 64-bit values: what the compiler uses to find what it
/// made for a function, a type or a variable.
typedef struct {
	uint64_t* keys    ;
	int64_t*  values  ;
	bool*     used    ;
	size_t    count   ;
	size_t    capacity;
} mia_map_index_s;

#define mia_map_key_of(_pointer) ((uint64_t)(uintptr_t)(_pointer))

/// the value of 'key' in '*value'; false when it has none.
bool mia_map_index_find(const mia_map_index_s* const map, const uint64_t key, int64_t* const value);

/// sets the value of 'key'.
void mia_map_index_set(mia_map_index_s* const map, const uint64_t key, const int64_t value);

void mia_map_index_free(mia_map_index_s* const map);

/// iteration: for (size_t i = 0; i < map.capacity; ++i) if (map.entries[i].occupied) ...
#define mia_map_each(_map, _entry)                                                         \
	for (mia_map_entry_s* _entry = (_map)->entries;                                        \
		 (_map)->entries != NULL && _entry < (_map)->entries + (_map)->capacity; ++_entry) \
		if (_entry->occupied)

#endif
