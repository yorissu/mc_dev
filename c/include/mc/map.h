
#ifndef __mc__map_h__
#define __mc__map_h__

/// a hash map from text to pointers. keys are views; their text must outlive
/// the map (keys point into sources or arenas).

#include "mc/string.h"

typedef struct {
	mc_view_s key     ;
	void*     value   ;
	uint64_t  hash    ;
	bool      occupied;
} mc_map_entry_s;

typedef struct {
	mc_map_entry_s* entries ;
	size_t          count   ;
	size_t          capacity;
} mc_map_s;

bool mc_map_find(const mc_map_s* const map, const mc_view_s key, void** const value);

/// the value of 'key', or null.
void* mc_map_get(const mc_map_s* const map, const mc_view_s key);

void* mc_map_get_cstr(const mc_map_s* const map, const char* const key);

/// sets 'key'; returns whether it was set before.
bool mc_map_set(mc_map_s* const map, const mc_view_s key, void* const value);

void mc_map_free(mc_map_s* const map);

/// iteration: for (size_t i = 0; i < map.capacity; ++i) if (map.entries[i].occupied) ...
#define mc_map_each(_map, _entry)                                              \
	for (mc_map_entry_s* _entry = (_map)->entries;                             \
		 (_map)->entries != NULL && _entry < (_map)->entries + (_map)->capacity; ++_entry) \
		if (_entry->occupied)

#endif
