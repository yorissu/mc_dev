
#include "mc/types.h"

#include <string.h>

static uint64_t _mc_mix(uint64_t hash, const uint64_t value) {
	hash ^= value + 0x9e3779b97f4a7c15u + (hash << 6) + (hash >> 2);
	return hash;
}

static uint64_t _mc_hash(const mc_type_s* const type) {
	uint64_t hash = _mc_mix(0x84222325u, (uint64_t)type->kind);
	switch (type->kind) {
		case mc_type_scalar:    return _mc_mix(hash, (uint64_t)type->as.scalar);
		case mc_type_array:     return _mc_mix(_mc_mix(hash, (uint64_t)(uintptr_t)type->as.array.element), (uint64_t)type->as.array.length);
		case mc_type_slice:     return _mc_mix(hash, (uint64_t)(uintptr_t)type->as.slice.element);
		case mc_type_reference: return _mc_mix(_mc_mix(hash, (uint64_t)(uintptr_t)type->as.reference.element), type->as.reference.mutable_);
		case mc_type_function:
			for (size_t i = 0; i < type->as.function.count; ++i) hash = _mc_mix(hash, (uint64_t)(uintptr_t)type->as.function.parameters[i]);
			return _mc_mix(_mc_mix(hash, type->as.function.count), (uint64_t)(uintptr_t)type->as.function.result);
		case mc_type_struct:
			for (size_t i = 0; i < type->as.struct_.count; ++i) hash = _mc_mix(hash, (uint64_t)(uintptr_t)type->as.struct_.arguments[i]);
			return _mc_mix(_mc_mix(hash, type->as.struct_.count), (uint64_t)(uintptr_t)type->as.struct_.info);
		case mc_type_parameter: return _mc_mix(_mc_mix(hash, mc_view_hash(type->as.parameter.name)), (uint64_t)(uintptr_t)type->as.parameter.owner);
		case mc_type_special:   return _mc_mix(hash, (uint64_t)type->as.special);
	}
	mc_unreachable();
}

static bool _mc_equal(const mc_type_s* const a, const mc_type_s* const b) {
	if (a->kind != b->kind) return false;
	switch (a->kind) {
		case mc_type_scalar:    return a->as.scalar == b->as.scalar;
		case mc_type_array:     return a->as.array.element == b->as.array.element && a->as.array.length == b->as.array.length;
		case mc_type_slice:     return a->as.slice.element == b->as.slice.element;
		case mc_type_reference: return a->as.reference.element == b->as.reference.element && a->as.reference.mutable_ == b->as.reference.mutable_;
		case mc_type_function:
			if (a->as.function.count != b->as.function.count || a->as.function.result != b->as.function.result) return false;
			for (size_t i = 0; i < a->as.function.count; ++i) if (a->as.function.parameters[i] != b->as.function.parameters[i]) return false;
			return true;
		case mc_type_struct:
			if (a->as.struct_.info != b->as.struct_.info || a->as.struct_.count != b->as.struct_.count) return false;
			for (size_t i = 0; i < a->as.struct_.count; ++i) if (a->as.struct_.arguments[i] != b->as.struct_.arguments[i]) return false;
			return true;
		case mc_type_parameter: return mc_view_eq(a->as.parameter.name, b->as.parameter.name) && a->as.parameter.owner == b->as.parameter.owner;
		case mc_type_special:   return a->as.special == b->as.special;
	}
	return false;
}

static void _mc_grow(mc_types_s* const types) {
	const size_t capacity = types->capacity > 0 ? types->capacity * 2 : 256;
	const mc_type_s** const table = mc_calloc(capacity, sizeof(*table));
	for (size_t i = 0; i < types->capacity; ++i) {
		const mc_type_s* const type = types->table[i];
		if (NULL == type) continue;
		size_t index = (size_t)type->hash & (capacity - 1);
		while (table[index] != NULL) index = (index + 1) & (capacity - 1);
		table[index] = type;
	}
	mc_free(types->table);
	types->table = table;
	types->capacity = capacity;
}

/// the interned copy of 'candidate' (copied into the arena when new).
static const mc_type_s* _mc_intern(mc_types_s* const types, mc_type_s candidate) {
	candidate.hash = _mc_hash(&candidate);
	if ((types->count + 1) * 2 > types->capacity) _mc_grow(types);
	size_t index = (size_t)candidate.hash & (types->capacity - 1);
	while (types->table[index] != NULL) {
		const mc_type_s* const existing = types->table[index];
		if (existing->hash == candidate.hash && _mc_equal(existing, &candidate)) return existing;
		index = (index + 1) & (types->capacity - 1);
	}
	mc_type_s* const type = mc_arena_new(&types->arena, mc_type_s);
	*type = candidate;
	if (mc_type_function == type->kind && type->as.function.count > 0) {
		const mc_type_s** const parameters = mc_arena_alloc(&types->arena, type->as.function.count * sizeof(*parameters));
		memcpy(parameters, candidate.as.function.parameters, type->as.function.count * sizeof(*parameters));
		type->as.function.parameters = parameters;
	}
	if (mc_type_struct == type->kind && type->as.struct_.count > 0) {
		const mc_type_s** const arguments = mc_arena_alloc(&types->arena, type->as.struct_.count * sizeof(*arguments));
		memcpy(arguments, candidate.as.struct_.arguments, type->as.struct_.count * sizeof(*arguments));
		type->as.struct_.arguments = arguments;
	}
	types->table[index] = type;
	types->count++;
	return type;
}

void mc_types_init(mc_types_s* const types) {
	*types = (const mc_types_s) {0};
	for (int scalar = 0; scalar < mc_scalar_count_; ++scalar) {
		types->scalars[scalar] = _mc_intern(types, (mc_type_s) { .kind = mc_type_scalar, .as.scalar = (mc_scalar_e)scalar, });
	}
	for (int special = 0; special < mc_special_count_; ++special) {
		types->specials[special] = _mc_intern(types, (mc_type_s) { .kind = mc_type_special, .as.special = (mc_special_e)special, });
	}
}

void mc_types_free(mc_types_s* const types) {
	mc_free(types->table);
	mc_arena_free(&types->arena);
	*types = (const mc_types_s) {0};
}

const mc_type_s* mc_type_of_scalar(mc_types_s* const types, const mc_scalar_e scalar) {
	return types->scalars[scalar];
}

const mc_type_s* mc_type_of_special(mc_types_s* const types, const mc_special_e special) {
	return types->specials[special];
}

const mc_type_s* mc_type_array_of(mc_types_s* const types, const mc_type_s* const element, const int64_t length) {
	return _mc_intern(types, (mc_type_s) { .kind = mc_type_array, .as.array = { .element = element, .length = length, }, });
}

const mc_type_s* mc_type_slice_of(mc_types_s* const types, const mc_type_s* const element) {
	return _mc_intern(types, (mc_type_s) { .kind = mc_type_slice, .as.slice = { .element = element, }, });
}

const mc_type_s* mc_type_reference_to(mc_types_s* const types, const mc_type_s* const element, const bool mutable_) {
	return _mc_intern(types, (mc_type_s) { .kind = mc_type_reference, .as.reference = { .element = element, .mutable_ = mutable_, }, });
}

const mc_type_s* mc_type_function_of(mc_types_s* const types, const mc_type_s* const* const parameters, const size_t count,
									 const mc_type_s* const result) {
	return _mc_intern(types, (mc_type_s) {
		.kind = mc_type_function,
		.as.function = { .parameters = (const mc_type_s**)(uintptr_t)parameters, .count = count, .result = result, },
	});
}

const mc_type_s* mc_type_struct_of(mc_types_s* const types, void* const info, const mc_view_s name,
								   const mc_type_s* const* const arguments, const size_t count) {
	return _mc_intern(types, (mc_type_s) {
		.kind = mc_type_struct,
		.as.struct_ = { .info = info, .name = name, .arguments = (const mc_type_s**)(uintptr_t)arguments, .count = count, },
	});
}

const mc_type_s* mc_type_parameter_of(mc_types_s* const types, const mc_view_s name, const void* const owner) {
	return _mc_intern(types, (mc_type_s) { .kind = mc_type_parameter, .as.parameter = { .name = name, .owner = owner, }, });
}

void mc_type_text(mc_string_s* const out, const mc_type_s* const type) {
	switch (type->kind) {
		case mc_type_scalar:
			mc_string_append_cstr(out, mc_scalar_name(type->as.scalar));
			return;
		case mc_type_array:
			if (type->as.array.length < 0) mc_string_append_cstr(out, "[_]");
			else mc_string_appendf(out, "[%lld]", (long long)type->as.array.length);
			mc_type_text(out, type->as.array.element);
			return;
		case mc_type_slice:
			mc_string_append_cstr(out, "[]");
			mc_type_text(out, type->as.slice.element);
			return;
		case mc_type_reference:
			mc_string_append_cstr(out, type->as.reference.mutable_ ? "mut &" : "&");
			mc_type_text(out, type->as.reference.element);
			return;
		case mc_type_function:
			mc_string_append_char(out, '(');
			for (size_t i = 0; i < type->as.function.count; ++i) {
				if (i > 0) mc_string_append_cstr(out, ", ");
				mc_type_text(out, type->as.function.parameters[i]);
			}
			mc_string_append_char(out, ')');
			if (type->as.function.result != NULL) {
				mc_string_append_cstr(out, " -> ");
				mc_type_text(out, type->as.function.result);
			}
			return;
		case mc_type_struct:
			mc_string_append(out, type->as.struct_.name);
			if (type->as.struct_.count > 0) {
				mc_string_append_char(out, '<');
				for (size_t i = 0; i < type->as.struct_.count; ++i) {
					if (i > 0) mc_string_append_cstr(out, ", ");
					mc_type_text(out, type->as.struct_.arguments[i]);
				}
				mc_string_append_char(out, '>');
			}
			return;
		case mc_type_parameter:
			mc_string_append(out, type->as.parameter.name);
			return;
		case mc_type_special: {
			static const char* const names[] = { "{integer}", "{float}", "null", "nothing", "{error}", "...", };
			mc_string_append_cstr(out, names[type->as.special]);
			return;
		}
	}
}

char* mc_type_name(const mc_type_s* const type) {
	mc_string_s out = {0};
	mc_type_text(&out, type);
	return mc_string_take(&out);
}

bool mc_type_is_special(const mc_type_s* const type, const mc_special_e special) {
	return type != NULL && mc_type_special == type->kind && type->as.special == special;
}

bool mc_type_is_untyped(const mc_type_s* const type) {
	return mc_type_is_special(type, mc_special_untyped_integer) || mc_type_is_special(type, mc_special_untyped_float);
}

bool mc_type_is_numeric(const mc_type_s* const type) {
	return type != NULL && mc_type_scalar == type->kind && 'b' != mc_scalar_kind(type->as.scalar);
}

bool mc_type_is_integer(const mc_type_s* const type) {
	return (type != NULL && mc_type_scalar == type->kind && 'i' == mc_scalar_kind(type->as.scalar))
		|| mc_type_is_special(type, mc_special_untyped_integer);
}

bool mc_type_is_float(const mc_type_s* const type) {
	return type != NULL && mc_type_scalar == type->kind && 'f' == mc_scalar_kind(type->as.scalar);
}

bool mc_type_is_signed(const mc_type_s* const type) {
	return type != NULL && mc_type_scalar == type->kind && mc_scalar_signed(type->as.scalar);
}

bool mc_type_is_bool(const mc_type_s* const type) {
	return type != NULL && mc_type_scalar == type->kind && mc_scalar_bool == type->as.scalar;
}

__int128 mc_type_minimum(const mc_type_s* const type) {
	const uint32_t bits = mc_scalar_bits(type->as.scalar);
	return mc_scalar_signed(type->as.scalar) ? -((__int128)1 << (bits - 1)) : 0;
}

__int128 mc_type_maximum(const mc_type_s* const type) {
	const uint32_t bits = mc_scalar_bits(type->as.scalar);
	return mc_scalar_signed(type->as.scalar) ? ((__int128)1 << (bits - 1)) - 1 : ((__int128)1 << bits) - 1;
}

bool mc_type_contains_parameter(const mc_type_s* const type) {
	switch (type->kind) {
		case mc_type_parameter: return true;
		case mc_type_array:     return mc_type_contains_parameter(type->as.array.element);
		case mc_type_slice:     return mc_type_contains_parameter(type->as.slice.element);
		case mc_type_reference: return mc_type_contains_parameter(type->as.reference.element);
		case mc_type_function:
			for (size_t i = 0; i < type->as.function.count; ++i) if (mc_type_contains_parameter(type->as.function.parameters[i])) return true;
			return type->as.function.result != NULL && mc_type_contains_parameter(type->as.function.result);
		case mc_type_struct:
			for (size_t i = 0; i < type->as.struct_.count; ++i) if (mc_type_contains_parameter(type->as.struct_.arguments[i])) return true;
			return false;
		default: return false;
	}
}

const mc_type_s* mc_bindings_get(const mc_bindings_s* const bindings, const mc_view_s name) {
	for (size_t i = 0; i < bindings->count; ++i) if (mc_view_eq(bindings->data[i].name, name)) return bindings->data[i].type;
	return NULL;
}

void mc_bindings_set(mc_bindings_s* const bindings, const mc_view_s name, const mc_type_s* const type) {
	for (size_t i = 0; i < bindings->count; ++i) {
		if (mc_view_eq(bindings->data[i].name, name)) {
			bindings->data[i].type = type;
			return;
		}
	}
	const mc_binding_s binding = { .name = name, .type = type, };
	mc_vector_push(bindings, binding);
}

const mc_type_s* mc_type_substitute(mc_types_s* const types, const mc_type_s* const type, const mc_bindings_s* const bindings) {
	switch (type->kind) {
		case mc_type_parameter: {
			const mc_type_s* const bound = mc_bindings_get(bindings, type->as.parameter.name);
			return (bound != NULL) ? bound : type;
		}
		case mc_type_array: return mc_type_array_of(types, mc_type_substitute(types, type->as.array.element, bindings), type->as.array.length);
		case mc_type_slice: return mc_type_slice_of(types, mc_type_substitute(types, type->as.slice.element, bindings));
		case mc_type_reference:
			return mc_type_reference_to(types, mc_type_substitute(types, type->as.reference.element, bindings), type->as.reference.mutable_);
		case mc_type_function: {
			mc_vector_of(const mc_type_s*) parameters = {0};
			for (size_t i = 0; i < type->as.function.count; ++i) mc_vector_push(&parameters, mc_type_substitute(types, type->as.function.parameters[i], bindings));
			const mc_type_s* const result = (type->as.function.result != NULL) ? mc_type_substitute(types, type->as.function.result, bindings) : NULL;
			const mc_type_s* const substituted = mc_type_function_of(types, parameters.data, parameters.count, result);
			mc_vector_free(&parameters);
			return substituted;
		}
		case mc_type_struct: {
			mc_vector_of(const mc_type_s*) arguments = {0};
			for (size_t i = 0; i < type->as.struct_.count; ++i) mc_vector_push(&arguments, mc_type_substitute(types, type->as.struct_.arguments[i], bindings));
			const mc_type_s* const substituted = mc_type_struct_of(types, type->as.struct_.info, type->as.struct_.name, arguments.data, arguments.count);
			mc_vector_free(&arguments);
			return substituted;
		}
		default: return type;
	}
}

bool mc_type_unify(const mc_type_s* const pattern, const mc_type_s* const actual, mc_bindings_s* const bindings) {
	if (mc_type_parameter == pattern->kind) {
		const mc_type_s* const bound = mc_bindings_get(bindings, pattern->as.parameter.name);
		if (NULL == bound) {
			mc_bindings_set(bindings, pattern->as.parameter.name, actual);
			return true;
		}
		return bound == actual;
	}
	if (pattern->kind != actual->kind) return pattern == actual;
	switch (pattern->kind) {
		case mc_type_array:
			return pattern->as.array.length == actual->as.array.length && mc_type_unify(pattern->as.array.element, actual->as.array.element, bindings);
		case mc_type_slice:
			return mc_type_unify(pattern->as.slice.element, actual->as.slice.element, bindings);
		case mc_type_reference:
			return mc_type_unify(pattern->as.reference.element, actual->as.reference.element, bindings);
		case mc_type_function:
			if (pattern->as.function.count != actual->as.function.count) return false;
			if ((NULL == pattern->as.function.result) != (NULL == actual->as.function.result)) return false;
			for (size_t i = 0; i < pattern->as.function.count; ++i) {
				if (!mc_type_unify(pattern->as.function.parameters[i], actual->as.function.parameters[i], bindings)) return false;
			}
			return NULL == pattern->as.function.result || mc_type_unify(pattern->as.function.result, actual->as.function.result, bindings);
		case mc_type_struct:
			if (pattern->as.struct_.info != actual->as.struct_.info) return false;
			for (size_t i = 0; i < pattern->as.struct_.count; ++i) {
				if (!mc_type_unify(pattern->as.struct_.arguments[i], actual->as.struct_.arguments[i], bindings)) return false;
			}
			return true;
		default:
			return pattern == actual;
	}
}
