
#ifndef __mia__type_h__
#define __mia__type_h__

/// semantic types. types are interned: each distinct type exists once, so
/// two types are equal exactly when their pointers are. a struct type is its
/// declaration plus its type arguments (one per generic instance).

#include "mia/arena.h"
#include "mia/lexer.h"
#include "mia/string.h"
#include "mia/vector.h"

typedef enum {
	mia_type_scalar   ,
	mia_type_array    ,
	mia_type_slice    ,
	mia_type_reference,   // only the type of a parameter; reading it gives the element
	mia_type_function ,
	mia_type_struct   ,
	mia_type_parameter,   // a type parameter while a generic signature is matched
	mia_type_special  ,
} mia_type_kind_e;

typedef enum {
	mia_type_special_untyped_integer,   // '{integer}': an integer literal before it gets a type
	mia_type_special_untyped_float  ,   // '{float}'
	mia_type_special_nothing        ,   // what a function without '->' returns
	mia_type_special_error          ,   // after an error; silences follow-up errors
	mia_type_special_variadic       ,   // '...': the last parameter of a variadic builtin
	mia_type_special_count         ,
} mia_type_special_e;

typedef struct mia_type_s mia_type_s;

struct mia_type_s {
	mia_type_kind_e kind;
	uint64_t        hash;
	union {
		mia_scalar_e scalar;
		struct { const mia_type_s* element; int64_t length; } array;          // length -1 while '[_]' waits for its initializer
		struct { const mia_type_s* element; } slice;
		struct { const mia_type_s* element; } reference;
		struct { const mia_type_s** parameters; size_t count; const mia_type_s* result; } function;   // result null: nothing
		struct { void* info; mia_view_s name; const mia_type_s** arguments; size_t count; } structure;
		struct { mia_view_s name; const void* owner; } parameter;
		mia_type_special_e special;
	} as;
};

typedef struct {
	mia_arena_s        arena                           ;
	const mia_type_s** table                           ;
	size_t             count                           ;
	size_t             capacity                        ;
	const mia_type_s*  scalars[mia_scalar_count]       ;
	const mia_type_s*  specials[mia_type_special_count];
} mia_type_table_s;

/// type parameters bound to types (by name).
typedef struct {
	mia_view_s        name;
	const mia_type_s* type;
} mia_type_binding_s;

typedef mia_vector_of(mia_type_binding_s) mia_type_bindings_s;

/// the table of interned types: every type is made once.
void mia_type_table_init(mia_type_table_s* const types);

void mia_type_table_free(mia_type_table_s* const types);

/// the types of each kind (made on first use, the same pointer after).
const mia_type_s* mia_type_of_scalar(mia_type_table_s* const types, const mia_scalar_e scalar);

const mia_type_s* mia_type_of_special(mia_type_table_s* const types, const mia_type_special_e special);

const mia_type_s* mia_type_array_of(mia_type_table_s* const types, const mia_type_s* const element, const int64_t length);

const mia_type_s* mia_type_slice_of(mia_type_table_s* const types, const mia_type_s* const element);

const mia_type_s* mia_type_reference_to(mia_type_table_s* const types, const mia_type_s* const element);

const mia_type_s* mia_type_function_of(mia_type_table_s* const types, const mia_type_s* const* const parameters, const size_t count,
									   const mia_type_s* const result);

/// the type of a struct declaration ('info') with type arguments, and a
/// type parameter of a generic ('owner' tells the generics apart).
const mia_type_s* mia_type_struct_of(mia_type_table_s* const types, void* const info, const mia_view_s name,
									 const mia_type_s* const* const arguments, const size_t count);

const mia_type_s* mia_type_parameter_of(mia_type_table_s* const types, const mia_view_s name, const void* const owner);

/// the type as text: 'int', '[]u8', 'Vector<int>', '(int) -> int', '{integer}'.
void mia_type_text(mia_string_s* const out, const mia_type_s* const type);

/// a caller-owned text of the type (for messages).
char* mia_type_name(const mia_type_s* const type);

bool mia_type_is_special(const mia_type_s* const type, const mia_type_special_e special);

bool mia_type_is_untyped(const mia_type_s* const type);

/// a scalar of kind 'i' or 'f'.
bool mia_type_is_numeric(const mia_type_s* const type);

/// an integer scalar or an untyped integer.
bool mia_type_is_integer(const mia_type_s* const type);

bool mia_type_is_float(const mia_type_s* const type);

bool mia_type_is_signed(const mia_type_s* const type);

/// the smallest and largest value of an integer scalar.
mia_int128_s mia_type_minimum(const mia_type_s* const type);

mia_int128_s mia_type_maximum(const mia_type_s* const type);

/// whether an integer scalar holds 'value'.
bool mia_type_holds(const mia_type_s* const type, const mia_int128_s value);

bool mia_type_contains_parameter(const mia_type_s* const type);

const mia_type_s* mia_type_bindings_get(const mia_type_bindings_s* const bindings, const mia_view_s name);

void mia_type_bindings_set(mia_type_bindings_s* const bindings, const mia_view_s name, const mia_type_s* const type);

/// 'type' with its type parameters replaced by their bindings.
const mia_type_s* mia_type_substitute(mia_type_table_s* const types, const mia_type_s* const type, const mia_type_bindings_s* const bindings);

/// matches 'pattern' (which may contain type parameters) against 'actual',
/// binding the parameters; false when they cannot match.
bool mia_type_unify(const mia_type_s* const pattern, const mia_type_s* const actual, mia_type_bindings_s* const bindings);

#endif
