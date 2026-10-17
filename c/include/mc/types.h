
#ifndef __mc__types_h__
#define __mc__types_h__

/// semantic types. types are interned: each distinct type exists once, so
/// two types are equal exactly when their pointers are. a struct type is its
/// declaration plus its type arguments (one per generic instance).

#include "mc/arena.h"
#include "mc/lexer.h"
#include "mc/string.h"
#include "mc/vector.h"

typedef enum {
	mc_type_scalar   ,
	mc_type_array    ,
	mc_type_slice    ,
	mc_type_reference,   // only the type of a parameter; reading it gives the element
	mc_type_function ,
	mc_type_struct   ,
	mc_type_parameter,   // a type parameter while a generic signature is matched
	mc_type_special  ,
} mc_type_kind_e;

typedef enum {
	mc_special_untyped_integer,   // '{integer}': an integer literal before it gets a type
	mc_special_untyped_float  ,   // '{float}'
	mc_special_null           ,
	mc_special_nothing        ,   // what a function without '->' returns
	mc_special_error          ,   // after an error; silences follow-up errors
	mc_special_variadic       ,   // '...': the last parameter of a variadic builtin
	mc_special_count_         ,
} mc_special_e;

typedef struct mc_type_s mc_type_s;

struct mc_type_s {
	mc_type_kind_e kind;
	uint64_t       hash;
	union {
		mc_scalar_e scalar;
		struct { const mc_type_s* element; int64_t length; } array;          // length -1 while '[_]' waits for its initializer
		struct { const mc_type_s* element; } slice;
		struct { const mc_type_s* element; bool mutable_; } reference;
		struct { const mc_type_s** parameters; size_t count; const mc_type_s* result; } function;   // result null: nothing
		struct { void* info; mc_view_s name; const mc_type_s** arguments; size_t count; } struct_;
		struct { mc_view_s name; const void* owner; } parameter;
		mc_special_e special;
	} as;
};

typedef struct {
	mc_arena_s        arena   ;
	const mc_type_s** table   ;
	size_t            count   ;
	size_t            capacity;
	const mc_type_s*  scalars[mc_scalar_count_];
	const mc_type_s*  specials[mc_special_count_];
} mc_types_s;

/// type parameters bound to types (by name).
typedef struct {
	mc_view_s        name;
	const mc_type_s* type;
} mc_binding_s;

typedef mc_vector_of(mc_binding_s) mc_bindings_s;

void mc_types_init(mc_types_s* const types);

void mc_types_free(mc_types_s* const types);

const mc_type_s* mc_type_of_scalar(mc_types_s* const types, const mc_scalar_e scalar);

const mc_type_s* mc_type_of_special(mc_types_s* const types, const mc_special_e special);

const mc_type_s* mc_type_array_of(mc_types_s* const types, const mc_type_s* const element, const int64_t length);

const mc_type_s* mc_type_slice_of(mc_types_s* const types, const mc_type_s* const element);

const mc_type_s* mc_type_reference_to(mc_types_s* const types, const mc_type_s* const element, const bool mutable_);

const mc_type_s* mc_type_function_of(mc_types_s* const types, const mc_type_s* const* const parameters, const size_t count,
									 const mc_type_s* const result);

const mc_type_s* mc_type_struct_of(mc_types_s* const types, void* const info, const mc_view_s name,
								   const mc_type_s* const* const arguments, const size_t count);

const mc_type_s* mc_type_parameter_of(mc_types_s* const types, const mc_view_s name, const void* const owner);

/// the type as text: 'int', '[]u8', 'Vector<int>', '(int) -> int', '{integer}'.
void mc_type_text(mc_string_s* const out, const mc_type_s* const type);

/// a caller-owned text of the type (for messages).
char* mc_type_name(const mc_type_s* const type);

bool mc_type_is_special(const mc_type_s* const type, const mc_special_e special);

bool mc_type_is_untyped(const mc_type_s* const type);

/// a scalar of kind 'i' or 'f'.
bool mc_type_is_numeric(const mc_type_s* const type);

/// an integer scalar or an untyped integer.
bool mc_type_is_integer(const mc_type_s* const type);

bool mc_type_is_float(const mc_type_s* const type);

bool mc_type_is_signed(const mc_type_s* const type);

bool mc_type_is_bool(const mc_type_s* const type);

/// the smallest and largest value of an integer scalar.
mc_i128_s mc_type_minimum(const mc_type_s* const type);

mc_i128_s mc_type_maximum(const mc_type_s* const type);

/// whether an integer scalar holds 'value'.
bool mc_type_holds(const mc_type_s* const type, const mc_i128_s value);

bool mc_type_contains_parameter(const mc_type_s* const type);

const mc_type_s* mc_bindings_get(const mc_bindings_s* const bindings, const mc_view_s name);

void mc_bindings_set(mc_bindings_s* const bindings, const mc_view_s name, const mc_type_s* const type);

/// 'type' with its type parameters replaced by their bindings.
const mc_type_s* mc_type_substitute(mc_types_s* const types, const mc_type_s* const type, const mc_bindings_s* const bindings);

/// matches 'pattern' (which may contain type parameters) against 'actual',
/// binding the parameters; false when they cannot match.
bool mc_type_unify(const mc_type_s* const pattern, const mc_type_s* const actual, mc_bindings_s* const bindings);

#endif
