
#ifndef __mc__ast_h__
#define __mc__ast_h__

/// the syntax tree. every node has a kind, a span, a payload for its kind
/// and annotations that the checker fills in (types, the symbols names refer
/// to, chosen generic instances and operator functions).

#include "mc/arena.h"
#include "mc/lexer.h"
#include "mc/source.h"
#include "mc/string.h"
#include "mc/vector.h"

typedef enum {
#define mc_node(_kind, _name) mc_node_##_kind,
#include "mc/ast.def"
#undef mc_node
	mc_node_count_,
} mc_node_kind_e;

/// operators of unary, binary and compound assignment nodes.
typedef enum {
	mc_operator_none_,
	mc_operator_add, mc_operator_subtract, mc_operator_multiply, mc_operator_divide, mc_operator_remainder,
	mc_operator_bit_and, mc_operator_bit_or, mc_operator_bit_xor, mc_operator_shift_left, mc_operator_shift_right,
	mc_operator_equal, mc_operator_not_equal, mc_operator_less, mc_operator_greater, mc_operator_less_equal,
	mc_operator_greater_equal, mc_operator_and, mc_operator_or,
	mc_operator_negate, mc_operator_not, mc_operator_bit_not,   // unary
	mc_operator_assign,                                          // '=' of assignments
	mc_operator_count_,
} mc_operator_e;

/// the spelling of an operator ("+", "<=", "=").
const char* mc_operator_text(const mc_operator_e operator_);

typedef enum {
	mc_attribute_value_none   ,
	mc_attribute_value_string ,
	mc_attribute_value_integer,
	mc_attribute_value_name   ,   // a name, a keyword or an operator pattern
} mc_attribute_value_e;

typedef struct mc_node_s mc_node_s;
typedef mc_vector_of(mc_node_s*) mc_nodes_s;
typedef mc_vector_of(mc_view_s) mc_views_s;

struct mc_type_s;
struct mc_variable_s;
struct mc_constant_s;
struct mc_instance_s;
struct mc_field_s;
typedef mc_vector_of(const struct mc_type_s*) mc_type_list_s;

/// what the checker learns about a node.
typedef struct {
	const struct mc_type_s* type         ;   // of an expression
	struct mc_variable_s*   variable     ;   // a name of a variable; lets, parameters, loops
	struct mc_constant_s*   constant     ;   // a name of a constant
	struct mc_instance_s*   instance     ;   // the function a name, call or specialization refers to
	struct mc_field_s*      field        ;   // a member that reads a field
	struct mc_instance_s*   operator_call;   // the operator function an operator calls
	bool                    swapped      ;   // the operator function takes the operands swapped
	bool                    negated      ;   // the result of the operator function is negated
	bool                    prechecked   ;   // checked already while inferring generic arguments
	bool                    negative     ;   // an integer literal under a unary minus
	bool                    format       ;   // the format string of a variadic call
	mc_type_list_s*         variadic     ;   // the types of the values of a variadic call
	uint32_t*               flags        ;   // a plugin function's call: what its check marked on each argument
	uint32_t                flag_count   ;
	uint64_t                size         ;   // sizeof
	void*                   symbol       ;   // a local declaration's symbol
} mc_annotation_s;

struct mc_node_s {
	mc_node_kind_e  kind;
	mc_span_s       span;
	mc_annotation_s note;
	union {
		struct { mc_views_s path; mc_nodes_s arguments; } type_name;
		struct { mc_node_s* element; } type_slice;
		struct { mc_node_s* length; mc_node_s* element; } type_array;          // length null for '[_]'
		struct { mc_node_s* element; bool mutable_; } type_reference;
		struct { mc_nodes_s parameters; mc_node_s* result; } type_function;
		struct { unsigned __int128 value; mc_scalar_e suffix; } integer;
		struct { double value; mc_scalar_e suffix; } float_;
		struct { uint8_t value; } char_;
		struct { mc_view_s data; bool c; } string;
		struct { bool value; } bool_;
		struct { mc_view_s name; } name;
		struct { mc_node_s* target; mc_view_s name; mc_span_s name_span; } member;
		struct { mc_node_s* callee; bool has_type_arguments; mc_nodes_s type_arguments; mc_nodes_s arguments; } call;
		struct { mc_node_s* target; mc_nodes_s type_arguments; } specialize;
		struct { mc_node_s* target; mc_node_s* index; } index;
		struct { mc_node_s* target; mc_node_s* low; mc_node_s* high; bool inclusive; } slice_of;
		struct { mc_operator_e operator_; mc_node_s* operand; } unary;
		struct { mc_operator_e operator_; mc_node_s* left; mc_node_s* right; mc_span_s operator_span; } binary;
		struct { mc_node_s* inner; } paren;
		struct { mc_node_s* type; mc_node_s* operand; } cast;
		struct { mc_node_s* operand; bool mutable_; } reference;
		struct { bool named; mc_view_s name; mc_node_s* value; } entry;
		struct { mc_node_s* type; mc_nodes_s entries; } composite;
		struct { mc_nodes_s parameters; mc_node_s* result; mc_node_s* body; } lambda;
		struct { mc_node_s* type; } sizeof_;
		struct { mc_node_s* operand; } lenof;
		struct { mc_node_s* condition; mc_node_s* message; } assert_;
		struct { mc_view_s name; mc_attribute_value_e value_kind; mc_view_s text; int64_t integer; } attribute;
		struct { mc_view_s name; mc_span_s name_span; bool mutable_; mc_node_s* type; mc_node_s* value; mc_nodes_s attributes; } let;
		struct { mc_node_s* declaration; } declaration_statement;
		struct { mc_node_s* condition; mc_node_s* then; mc_node_s* otherwise; } if_;
		struct { mc_node_s* condition; mc_node_s* body; } while_;
		struct { mc_view_s name; mc_span_s name_span; mc_node_s* start; mc_node_s* end; bool inclusive; mc_node_s* body; } for_;
		struct { mc_node_s* value; } return_;
		struct { mc_nodes_s statements; bool braceless; } block;
		struct { mc_node_s* expression; mc_nodes_s attributes; } expression_statement;
		struct { mc_operator_e operator_; mc_node_s* target; mc_node_s* value; mc_nodes_s attributes; } assign;
		struct { mc_view_s name; } generic;
		struct { mc_view_s name; bool mutable_; mc_node_s* type; mc_node_s* default_; bool variadic; } param;
		struct { mc_view_s name; bool mutable_; mc_node_s* type; mc_node_s* default_; mc_nodes_s attributes; } field;
		struct { mc_view_s name; mc_span_s name_span; mc_nodes_s generics; mc_nodes_s parameters; mc_node_s* result;
				 mc_node_s* body; mc_nodes_s attributes; bool public_; } function;
		struct { mc_view_s name; mc_span_s name_span; mc_nodes_s generics; mc_nodes_s fields; mc_nodes_s attributes; bool public_; } struct_;
		struct { mc_view_s name; mc_span_s name_span; mc_node_s* value; mc_nodes_s attributes; bool public_; } constant;
		struct { mc_view_s name; bool has_alias; mc_view_s alias; } import_name;
		struct { mc_views_s path; bool has_alias; mc_view_s alias; } import;
		struct { mc_views_s path; mc_nodes_s names; bool star; } from_import;
		struct { mc_nodes_s imports; mc_nodes_s declarations; } module;
	} as;
};

/// a new node of 'kind' in 'arena' (payload zeroed).
mc_node_s* mc_node_new(mc_arena_s* const arena, const mc_node_kind_e kind, const mc_span_s span);

/// the name the tree printer uses for 'kind'.
const char* mc_node_kind_name(const mc_node_kind_e kind);

/// a deep copy of the syntax of 'node' (no annotations); generic instances
/// are checked on copies of their declaration.
mc_node_s* mc_node_clone(mc_arena_s* const arena, const mc_node_s* const node);

/// whether 'node' is a declaration (function, struct or constant).
bool mc_node_is_declaration(const mc_node_s* const node);

/// the name and its span of a declaration.
mc_view_s mc_declaration_name(const mc_node_s* const node);

mc_span_s mc_declaration_name_span(const mc_node_s* const node);

const mc_nodes_s* mc_declaration_attributes(const mc_node_s* const node);

bool mc_declaration_public(const mc_node_s* const node);

/// the attribute 'name' of a list of attribute nodes, or null.
const mc_node_s* mc_attribute_find(const mc_nodes_s* const attributes, const char* const name);

#endif
