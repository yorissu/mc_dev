
#include "mia/type.h"

#include <string.h>

static uint64_t _mia_type_mix(const uint64_t hash, const uint64_t value) {
	return hash ^ (value + 0x9e3779b97f4a7c15u + (hash << 6) + (hash >> 2));
}

static uint64_t _mia_type_hash(const mia_type_s* const type) {
	uint64_t hash = _mia_type_mix(0x84222325u, (uint64_t)type->kind);
	switch (type->kind) {
		case mia_type_scalar:    return _mia_type_mix(hash, (uint64_t)type->as.scalar);
		case mia_type_array:     return _mia_type_mix(_mia_type_mix(hash, (uint64_t)(uintptr_t)type->as.array.element), (uint64_t)type->as.array.length);
		case mia_type_slice:     return _mia_type_mix(hash, (uint64_t)(uintptr_t)type->as.slice.element);
		case mia_type_reference: return _mia_type_mix(hash, (uint64_t)(uintptr_t)type->as.reference.element);
		case mia_type_function:
			for (size_t i = 0; i < type->as.function.count; ++i) hash = _mia_type_mix(hash, (uint64_t)(uintptr_t)type->as.function.parameters[i]);
			return _mia_type_mix(_mia_type_mix(hash, type->as.function.count), (uint64_t)(uintptr_t)type->as.function.result);
		case mia_type_struct:
			for (size_t i = 0; i < type->as.structure.count; ++i) hash = _mia_type_mix(hash, (uint64_t)(uintptr_t)type->as.structure.arguments[i]);
			return _mia_type_mix(_mia_type_mix(hash, type->as.structure.count), (uint64_t)(uintptr_t)type->as.structure.info);
		case mia_type_parameter: return _mia_type_mix(_mia_type_mix(hash, mia_view_hash(type->as.parameter.name)), (uint64_t)(uintptr_t)type->as.parameter.owner);
		case mia_type_special:   return _mia_type_mix(hash, (uint64_t)type->as.special);
	}
	mia_unreachable();
}

static bool _mia_type_equal(const mia_type_s* const a, const mia_type_s* const b) {
	if (a->kind != b->kind) return false;
	switch (a->kind) {
		case mia_type_scalar:    return a->as.scalar == b->as.scalar;
		case mia_type_array:     return a->as.array.element == b->as.array.element && a->as.array.length == b->as.array.length;
		case mia_type_slice:     return a->as.slice.element == b->as.slice.element;
		case mia_type_reference: return a->as.reference.element == b->as.reference.element;
		case mia_type_function:
			if (a->as.function.count != b->as.function.count || a->as.function.result != b->as.function.result) return false;
			for (size_t i = 0; i < a->as.function.count; ++i) if (a->as.function.parameters[i] != b->as.function.parameters[i]) return false;
			return true;
		case mia_type_struct:
			if (a->as.structure.info != b->as.structure.info || a->as.structure.count != b->as.structure.count) return false;
			for (size_t i = 0; i < a->as.structure.count; ++i) if (a->as.structure.arguments[i] != b->as.structure.arguments[i]) return false;
			return true;
		case mia_type_parameter: return mia_view_eq(a->as.parameter.name, b->as.parameter.name) && a->as.parameter.owner == b->as.parameter.owner;
		case mia_type_special:   return a->as.special == b->as.special;
	}
	return false;
}

static void _mia_type_grow(mia_type_table_s* const types) {
	const size_t capacity = types->capacity > 0 ? types->capacity * 2 : 256;
	const mia_type_s** const table = mia_calloc(capacity, sizeof(*table));
	for (size_t i = 0; i < types->capacity; ++i) {
		const mia_type_s* const type = types->table[i];
		if (NULL == type) continue;
		size_t index = (size_t)type->hash & (capacity - 1);
		while (table[index] != NULL) index = (index + 1) & (capacity - 1);
		table[index] = type;
	}
	mia_free(types->table);
	types->table = table;
	types->capacity = capacity;
}

/// the interned copy of 'candidate' (copied into the arena when new).
static const mia_type_s* _mia_type_intern(mia_type_table_s* const types, const mia_type_s given) {
	mia_type_s candidate = given;
	candidate.hash = _mia_type_hash(&candidate);
	if ((types->count + 1) * 2 > types->capacity) _mia_type_grow(types);
	size_t index = (size_t)candidate.hash & (types->capacity - 1);
	while (types->table[index] != NULL) {
		const mia_type_s* const existing = types->table[index];
		if (existing->hash == candidate.hash && _mia_type_equal(existing, &candidate)) return existing;
		index = (index + 1) & (types->capacity - 1);
	}
	mia_type_s* const type = mia_arena_new(&types->arena, mia_type_s);
	*type = candidate;
	if (mia_type_function == type->kind && type->as.function.count > 0) {
		const mia_type_s** const parameters = mia_arena_alloc(&types->arena, type->as.function.count * sizeof(*parameters));
		memcpy(parameters, candidate.as.function.parameters, type->as.function.count * sizeof(*parameters));
		type->as.function.parameters = parameters;
	}
	if (mia_type_struct == type->kind && type->as.structure.count > 0) {
		const mia_type_s** const arguments = mia_arena_alloc(&types->arena, type->as.structure.count * sizeof(*arguments));
		memcpy(arguments, candidate.as.structure.arguments, type->as.structure.count * sizeof(*arguments));
		type->as.structure.arguments = arguments;
	}
	types->table[index] = type;
	types->count++;
	return type;
}

void mia_type_table_init(mia_type_table_s* const types) {
	*types = (const mia_type_table_s) {0};
	for (int scalar = 0; scalar < mia_scalar_count; ++scalar) {
		types->scalars[scalar] = _mia_type_intern(types, (mia_type_s) { .kind = mia_type_scalar, .as.scalar = (mia_scalar_e)scalar, });
	}
	for (int special = 0; special < mia_type_special_count; ++special) {
		types->specials[special] = _mia_type_intern(types, (mia_type_s) { .kind = mia_type_special, .as.special = (mia_type_special_e)special, });
	}
}

void mia_type_table_free(mia_type_table_s* const types) {
	mia_free(types->table);
	mia_arena_free(&types->arena);
	*types = (const mia_type_table_s) {0};
}

const mia_type_s* mia_type_of_scalar(mia_type_table_s* const types, const mia_scalar_e scalar) {
	return types->scalars[scalar];
}

const mia_type_s* mia_type_of_special(mia_type_table_s* const types, const mia_type_special_e special) {
	return types->specials[special];
}

const mia_type_s* mia_type_array_of(mia_type_table_s* const types, const mia_type_s* const element, const int64_t length) {
	return _mia_type_intern(types, (mia_type_s) { .kind = mia_type_array, .as.array = { .element = element, .length = length, }, });
}

const mia_type_s* mia_type_slice_of(mia_type_table_s* const types, const mia_type_s* const element) {
	return _mia_type_intern(types, (mia_type_s) { .kind = mia_type_slice, .as.slice = { .element = element, }, });
}

const mia_type_s* mia_type_reference_to(mia_type_table_s* const types, const mia_type_s* const element) {
	return _mia_type_intern(types, (mia_type_s) { .kind = mia_type_reference, .as.reference = { .element = element, }, });
}

const mia_type_s* mia_type_function_of(mia_type_table_s* const types, const mia_type_s* const* const parameters, const size_t count,
									   const mia_type_s* const result) {
	return _mia_type_intern(types, (mia_type_s) {
		.kind = mia_type_function,
		.as.function = { .parameters = (const mia_type_s**)(uintptr_t)parameters, .count = count, .result = result, },
	});
}

const mia_type_s* mia_type_struct_of(mia_type_table_s* const types, void* const info, const mia_view_s name,
									 const mia_type_s* const* const arguments, const size_t count) {
	return _mia_type_intern(types, (mia_type_s) {
		.kind = mia_type_struct,
		.as.structure = { .info = info, .name = name, .arguments = (const mia_type_s**)(uintptr_t)arguments, .count = count, },
	});
}

const mia_type_s* mia_type_parameter_of(mia_type_table_s* const types, const mia_view_s name, const void* const owner) {
	return _mia_type_intern(types, (mia_type_s) { .kind = mia_type_parameter, .as.parameter = { .name = name, .owner = owner, }, });
}

void mia_type_text(mia_string_s* const out, const mia_type_s* const type) {
	switch (type->kind) {
		case mia_type_scalar:
			mia_string_append_cstr(out, mia_scalar_name(type->as.scalar));
			return;
		case mia_type_array:
			if (type->as.array.length < 0) mia_string_append_cstr(out, "[_]");
			else mia_string_appendf(out, "[%lld]", (long long)type->as.array.length);
			mia_type_text(out, type->as.array.element);
			return;
		case mia_type_slice:
			mia_string_append_cstr(out, "[]");
			mia_type_text(out, type->as.slice.element);
			return;
		case mia_type_reference:
			mia_string_append_cstr(out, "&");
			mia_type_text(out, type->as.reference.element);
			return;
		case mia_type_function:
			mia_string_append_char(out, '(');
			for (size_t i = 0; i < type->as.function.count; ++i) {
				if (i > 0) mia_string_append_cstr(out, ", ");
				mia_type_text(out, type->as.function.parameters[i]);
			}
			mia_string_append_char(out, ')');
			if (type->as.function.result != NULL) {
				mia_string_append_cstr(out, " -> ");
				mia_type_text(out, type->as.function.result);
			}
			return;
		case mia_type_struct:
			mia_string_append(out, type->as.structure.name);
			if (type->as.structure.count > 0) {
				mia_string_append_char(out, '<');
				for (size_t i = 0; i < type->as.structure.count; ++i) {
					if (i > 0) mia_string_append_cstr(out, ", ");
					mia_type_text(out, type->as.structure.arguments[i]);
				}
				mia_string_append_char(out, '>');
			}
			return;
		case mia_type_parameter:
			mia_string_append(out, type->as.parameter.name);
			return;
		case mia_type_special: {
			static const char* const names[] = { "{integer}", "{float}", "nothing", "{error}", "...", };
			mia_string_append_cstr(out, names[type->as.special]);
			return;
		}
	}
}

char* mia_type_name(const mia_type_s* const type) {
	mia_string_s out = {0};
	mia_type_text(&out, type);
	return mia_string_take(&out);
}

bool mia_type_is_special(const mia_type_s* const type, const mia_type_special_e special) {
	return type != NULL && mia_type_special == type->kind && type->as.special == special;
}

bool mia_type_is_untyped(const mia_type_s* const type) {
	return mia_type_is_special(type, mia_type_special_untyped_integer) || mia_type_is_special(type, mia_type_special_untyped_float);
}

bool mia_type_is_numeric(const mia_type_s* const type) {
	return type != NULL && mia_type_scalar == type->kind && 'b' != mia_scalar_kind(type->as.scalar);
}

bool mia_type_is_integer(const mia_type_s* const type) {
	return (type != NULL && mia_type_scalar == type->kind && 'i' == mia_scalar_kind(type->as.scalar))
		|| mia_type_is_special(type, mia_type_special_untyped_integer);
}

bool mia_type_is_float(const mia_type_s* const type) {
	return type != NULL && mia_type_scalar == type->kind && 'f' == mia_scalar_kind(type->as.scalar);
}

bool mia_type_is_signed(const mia_type_s* const type) {
	return type != NULL && mia_type_scalar == type->kind && mia_scalar_signed(type->as.scalar);
}

mia_int128_s mia_type_minimum(const mia_type_s* const type) {
	const uint32_t bits = mia_scalar_bits(type->as.scalar);
	if (!mia_scalar_signed(type->as.scalar)) return mia_int128_zero();
	return mia_int128_negate(mia_int128_shift_left(mia_int128_from_uint(1), bits - 1));
}

mia_int128_s mia_type_maximum(const mia_type_s* const type) {
	const uint32_t bits = mia_scalar_bits(type->as.scalar);
	const uint32_t magnitude_bits = mia_scalar_signed(type->as.scalar) ? bits - 1 : bits;
	return mia_int128_sub(mia_int128_shift_left(mia_int128_from_uint(1), magnitude_bits), mia_int128_from_uint(1));
}

bool mia_type_holds(const mia_type_s* const type, const mia_int128_s value) {
	return !mia_int128_less(value, mia_type_minimum(type)) && !mia_int128_less(mia_type_maximum(type), value);
}

bool mia_type_contains_parameter(const mia_type_s* const type) {
	switch (type->kind) {
		case mia_type_parameter: return true;
		case mia_type_array:     return mia_type_contains_parameter(type->as.array.element);
		case mia_type_slice:     return mia_type_contains_parameter(type->as.slice.element);
		case mia_type_reference: return mia_type_contains_parameter(type->as.reference.element);
		case mia_type_function:
			for (size_t i = 0; i < type->as.function.count; ++i) if (mia_type_contains_parameter(type->as.function.parameters[i])) return true;
			return type->as.function.result != NULL && mia_type_contains_parameter(type->as.function.result);
		case mia_type_struct:
			for (size_t i = 0; i < type->as.structure.count; ++i) if (mia_type_contains_parameter(type->as.structure.arguments[i])) return true;
			return false;
		default: return false;
	}
}

const mia_type_s* mia_type_bindings_get(const mia_type_bindings_s* const bindings, const mia_view_s name) {
	for (size_t i = 0; i < bindings->count; ++i) if (mia_view_eq(bindings->data[i].name, name)) return bindings->data[i].type;
	return NULL;
}

void mia_type_bindings_set(mia_type_bindings_s* const bindings, const mia_view_s name, const mia_type_s* const type) {
	for (size_t i = 0; i < bindings->count; ++i) {
		if (mia_view_eq(bindings->data[i].name, name)) {
			bindings->data[i].type = type;
			return;
		}
	}
	const mia_type_binding_s binding = { .name = name, .type = type, };
	mia_vector_push(bindings, binding);
}

const mia_type_s* mia_type_substitute(mia_type_table_s* const types, const mia_type_s* const type, const mia_type_bindings_s* const bindings) {
	switch (type->kind) {
		case mia_type_parameter: {
			const mia_type_s* const bound = mia_type_bindings_get(bindings, type->as.parameter.name);
			return (bound != NULL) ? bound : type;
		}
		case mia_type_array: return mia_type_array_of(types, mia_type_substitute(types, type->as.array.element, bindings), type->as.array.length);
		case mia_type_slice: return mia_type_slice_of(types, mia_type_substitute(types, type->as.slice.element, bindings));
		case mia_type_reference:
			return mia_type_reference_to(types, mia_type_substitute(types, type->as.reference.element, bindings));
		case mia_type_function: {
			mia_vector_of(const mia_type_s*) parameters = {0};
			for (size_t i = 0; i < type->as.function.count; ++i) mia_vector_push(&parameters, mia_type_substitute(types, type->as.function.parameters[i], bindings));
			const mia_type_s* const result = (type->as.function.result != NULL) ? mia_type_substitute(types, type->as.function.result, bindings) : NULL;
			const mia_type_s* const substituted = mia_type_function_of(types, parameters.data, parameters.count, result);
			mia_vector_free(&parameters);
			return substituted;
		}
		case mia_type_struct: {
			mia_vector_of(const mia_type_s*) arguments = {0};
			for (size_t i = 0; i < type->as.structure.count; ++i) mia_vector_push(&arguments, mia_type_substitute(types, type->as.structure.arguments[i], bindings));
			const mia_type_s* const substituted = mia_type_struct_of(types, type->as.structure.info, type->as.structure.name, arguments.data, arguments.count);
			mia_vector_free(&arguments);
			return substituted;
		}
		default: return type;
	}
}

bool mia_type_unify(const mia_type_s* const pattern, const mia_type_s* const actual, mia_type_bindings_s* const bindings) {
	if (mia_type_parameter == pattern->kind) {
		const mia_type_s* const bound = mia_type_bindings_get(bindings, pattern->as.parameter.name);
		if (NULL == bound) {
			mia_type_bindings_set(bindings, pattern->as.parameter.name, actual);
			return true;
		}
		return bound == actual;
	}
	if (pattern->kind != actual->kind) return pattern == actual;
	switch (pattern->kind) {
		case mia_type_array:
			return pattern->as.array.length == actual->as.array.length && mia_type_unify(pattern->as.array.element, actual->as.array.element, bindings);
		case mia_type_slice:
			return mia_type_unify(pattern->as.slice.element, actual->as.slice.element, bindings);
		case mia_type_reference:
			return mia_type_unify(pattern->as.reference.element, actual->as.reference.element, bindings);
		case mia_type_function:
			if (pattern->as.function.count != actual->as.function.count) return false;
			if ((NULL == pattern->as.function.result) != (NULL == actual->as.function.result)) return false;
			for (size_t i = 0; i < pattern->as.function.count; ++i) {
				if (!mia_type_unify(pattern->as.function.parameters[i], actual->as.function.parameters[i], bindings)) return false;
			}
			return NULL == pattern->as.function.result || mia_type_unify(pattern->as.function.result, actual->as.function.result, bindings);
		case mia_type_struct:
			if (pattern->as.structure.info != actual->as.structure.info) return false;
			for (size_t i = 0; i < pattern->as.structure.count; ++i) {
				if (!mia_type_unify(pattern->as.structure.arguments[i], actual->as.structure.arguments[i], bindings)) return false;
			}
			return true;
		default:
			return pattern == actual;
	}
}
