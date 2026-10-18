
#include "mia/map.h"

static size_t _mia_map_slot(const mia_map_entry_s* const entries, const size_t capacity,
							const mia_view_s key, const uint64_t hash) {
	size_t index = (size_t)hash & (capacity - 1);
	while (entries[index].occupied) {
		if ((entries[index].hash == hash) && mia_view_eq(entries[index].key, key)) break;
		index = (index + 1) & (capacity - 1);
	}
	return index;
}

static void _mia_map_grow(mia_map_s* const map) {
	const size_t capacity = map->capacity > 0 ? map->capacity * 2 : 16;
	mia_map_entry_s* const entries = mia_calloc(capacity, sizeof(*entries));
	for (size_t i = 0; i < map->capacity; ++i) {
		const mia_map_entry_s* const entry = &map->entries[i];
		if (entry->occupied) entries[_mia_map_slot(entries, capacity, entry->key, entry->hash)] = *entry;
	}
	mia_free(map->entries);
	map->entries  = entries ;
	map->capacity = capacity;
}

bool mia_map_find(const mia_map_s* const map, const mia_view_s key, void** const value) {
	mia_assert(map != NULL);
	if (0 == map->count) return false;
	const mia_map_entry_s* const entry = &map->entries[_mia_map_slot(map->entries, map->capacity, key, mia_view_hash(key))];
	if (!entry->occupied) return false;
	if (value != NULL) *value = entry->value;
	return true;
}

void* mia_map_get(const mia_map_s* const map, const mia_view_s key) {
	void* value = NULL;
	return mia_map_find(map, key, &value) ? value : NULL;
}

void* mia_map_get_cstr(const mia_map_s* const map, const char* const key) {
	return mia_map_get(map, mia_view_cstr(key));
}

bool mia_map_set(mia_map_s* const map, const mia_view_s key, void* const value) {
	mia_assert(map != NULL);
	if (((map->count + 1) * 2) > map->capacity) _mia_map_grow(map);
	const uint64_t hash = mia_view_hash(key);
	mia_map_entry_s* const entry = &map->entries[_mia_map_slot(map->entries, map->capacity, key, hash)];
	if (entry->occupied) {
		entry->value = value;
		return true;
	}
	*entry = (const mia_map_entry_s) { .key = key, .value = value, .hash = hash, .occupied = true, };
	map->count++;
	return false;
}

void mia_map_free(mia_map_s* const map) {
	mia_free(map->entries);
	*map = (const mia_map_s) {0};
}

// --- the map from 64-bit keys ---------------------------------------------------------------

/// a mix of the bits of 'key' (pointers and small numbers have their bits in few places).
static uint64_t _mia_map_index_hash(const uint64_t key) {
	const uint64_t mixed = (key ^ (key >> 33)) * 0xFF51AFD7ED558CCDull;
	return mixed ^ (mixed >> 33);
}

static size_t _mia_map_index_slot(const mia_map_index_s* const map, const uint64_t key) {
	size_t index = (size_t)_mia_map_index_hash(key) & (map->capacity - 1);
	while (map->used[index] && map->keys[index] != key) index = (index + 1) & (map->capacity - 1);
	return index;
}

bool mia_map_index_find(const mia_map_index_s* const map, const uint64_t key, int64_t* const value) {
	if (0 == map->count) return false;
	const size_t slot = _mia_map_index_slot(map, key);
	if (!map->used[slot]) return false;
	*value = map->values[slot];
	return true;
}

void mia_map_index_set(mia_map_index_s* const map, const uint64_t key, const int64_t value) {
	if ((map->count + 1) * 2 > map->capacity) {
		const mia_map_index_s old = *map;
		map->capacity = (old.capacity > 0) ? old.capacity * 2 : 64;
		map->keys = mia_calloc(map->capacity, sizeof(*map->keys));
		map->values = mia_calloc(map->capacity, sizeof(*map->values));
		map->used = mia_calloc(map->capacity, sizeof(*map->used));
		map->count = 0;
		for (size_t i = 0; i < old.capacity; ++i) if (old.used[i]) mia_map_index_set(map, old.keys[i], old.values[i]);
		mia_free(old.keys);
		mia_free(old.values);
		mia_free(old.used);
	}
	const size_t slot = _mia_map_index_slot(map, key);
	if (!map->used[slot]) map->count += 1;
	map->used[slot] = true;
	map->keys[slot] = key;
	map->values[slot] = value;
}

void mia_map_index_free(mia_map_index_s* const map) {
	mia_free(map->keys);
	mia_free(map->values);
	mia_free(map->used);
	*map = (mia_map_index_s) {0};
}
