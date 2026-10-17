
#include "mc/map.h"

static size_t _mc_map_slot(const mc_map_entry_s* const entries, const size_t capacity,
						   const mc_view_s key, const uint64_t hash) {
	size_t index = (size_t)hash & (capacity - 1);
	while (entries[index].occupied) {
		if ((entries[index].hash == hash) && mc_view_eq(entries[index].key, key)) break;
		index = (index + 1) & (capacity - 1);
	}
	return index;
}

static void _mc_map_grow(mc_map_s* const map) {
	const size_t capacity = map->capacity > 0 ? map->capacity * 2 : 16;
	mc_map_entry_s* const entries = mc_calloc(capacity, sizeof(*entries));
	for (size_t i = 0; i < map->capacity; ++i) {
		const mc_map_entry_s* const entry = &map->entries[i];
		if (entry->occupied) entries[_mc_map_slot(entries, capacity, entry->key, entry->hash)] = *entry;
	}
	mc_free(map->entries);
	map->entries  = entries ;
	map->capacity = capacity;
}

bool mc_map_find(const mc_map_s* const map, const mc_view_s key, void** const value) {
	mc_assert(map != NULL);
	if (0 == map->count) return false;
	const mc_map_entry_s* const entry = &map->entries[_mc_map_slot(map->entries, map->capacity, key, mc_view_hash(key))];
	if (!entry->occupied) return false;
	if (value != NULL) *value = entry->value;
	return true;
}

void* mc_map_get(const mc_map_s* const map, const mc_view_s key) {
	void* value = NULL;
	return mc_map_find(map, key, &value) ? value : NULL;
}

void* mc_map_get_cstr(const mc_map_s* const map, const char* const key) {
	return mc_map_get(map, mc_view_cstr(key));
}

bool mc_map_set(mc_map_s* const map, const mc_view_s key, void* const value) {
	mc_assert(map != NULL);
	if (((map->count + 1) * 2) > map->capacity) _mc_map_grow(map);
	const uint64_t hash = mc_view_hash(key);
	mc_map_entry_s* const entry = &map->entries[_mc_map_slot(map->entries, map->capacity, key, hash)];
	if (entry->occupied) {
		entry->value = value;
		return true;
	}
	*entry = (const mc_map_entry_s) { .key = key, .value = value, .hash = hash, .occupied = true, };
	map->count++;
	return false;
}

void mc_map_free(mc_map_s* const map) {
	mc_free(map->entries);
	*map = (const mc_map_s) {0};
}
