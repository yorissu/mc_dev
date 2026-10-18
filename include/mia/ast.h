
#ifndef __mia__ast_h__
#define __mia__ast_h__

/// the syntax tree. every node has a kind, a span, a payload for its kind
/// and annotations that the checker fills in (types, the symbols names refer
/// to, chosen generic instances).

#include "mia/arena.h"
#include "mia/lexer.h"
#include "mia/source.h"
#include "mia/string.h"
#include "mia/vector.h"

typedef enum {
#define mia_ast_node(_kind, _name) mia_ast_node_##_kind,
#include "mia/ast.def"
#undef mia_ast_node
	mia_ast_node_count,
} mia_ast_node_kind_e;

/// operators of unary, binary and compound assignment nodes.
typedef enum {
	mia_ast_operator_none,
	mia_ast_operator_add, mia_ast_operator_subtract, mia_ast_operator_multiply, mia_ast_operator_divide, mia_ast_operator_remainder,
	mia_ast_operator_bit_and, mia_ast_operator_bit_or, mia_ast_operator_bit_xor, mia_ast_operator_shift_left, mia_ast_operator_shift_right,
	mia_ast_operator_equal, mia_ast_operator_not_equal, mia_ast_operator_less, mia_ast_operator_greater, mia_ast_operator_less_equal,
	mia_ast_operator_greater_equal, mia_ast_operator_and, mia_ast_operator_or,
	mia_ast_operator_negate, mia_ast_operator_not, mia_ast_operator_bit_not,   // unary
	mia_ast_operator_assign,                                          // '=' of assignments
	mia_ast_operator_count,
} mia_ast_operator_e;

/// the spelling of an operator ("+", "<=", "=").
const char* mia_ast_operator_text(const mia_ast_operator_e operation);

typedef enum {
	mia_ast_attribute_value_none   ,
	mia_ast_attribute_value_string ,
	mia_ast_attribute_value_integer,
	mia_ast_attribute_value_name   ,   // a name or a keyword
} mia_ast_attribute_value_e;

typedef struct mia_ast_node_s mia_ast_node_s;
typedef mia_vector_of(mia_ast_node_s*) mia_ast_nodes_s;

struct mia_type_s;
struct mia_program_variable_s;
struct mia_program_constant_s;
struct mia_program_instance_s;
struct mia_program_field_s;
typedef mia_vector_of(const struct mia_type_s*) mia_type_list_s;

/// what the checker learns about a node.
typedef struct {
	const struct mia_type_s*       type      ;   // of an expression
	struct mia_program_variable_s* variable  ;   // a name of a variable; lets, parameters, loops
	struct mia_program_constant_s* constant  ;   // a name of a constant
	struct mia_program_instance_s* instance  ;   // the function a name, call or specialization refers to
	struct mia_program_field_s*    field     ;   // a member that reads a field
	bool                           prechecked;   // checked already while inferring generic arguments
	bool                           negative  ;   // an integer literal under a unary minus
	bool                           format    ;   // the format string of a variadic call
	mia_type_list_s*               variadic  ;   // the types of the values of a variadic call
	uint32_t*                      flags     ;   // a plugin function's call: what its check marked on each argument
	uint32_t                       flag_count;
	uint64_t                       size      ;   // sizeof
	void*                          symbol    ;   // a local declaration's symbol
} mia_ast_annotation_s;

struct mia_ast_node_s {
	mia_ast_node_kind_e  kind;
	mia_span_s           span;
	mia_ast_annotation_s note;
	union {
		struct { mia_view_list_s path; mia_ast_nodes_s arguments; } type_name;
		struct { mia_ast_node_s* element; } type_slice;
		struct { mia_ast_node_s* length; mia_ast_node_s* element; } type_array;          // length null for '[_]'
		struct { mia_ast_node_s* element; } type_reference;
		struct { mia_ast_nodes_s parameters; mia_ast_node_s* result; } type_function;
		struct { mia_int128_s value; mia_scalar_e suffix; } integer;
		struct { double value; mia_scalar_e suffix; } floating;
		struct { uint8_t value; } character;
		struct { mia_view_s data; bool c; } string;
		struct { bool value; } boolean;
		struct { mia_view_s name; } name;
		struct { mia_ast_node_s* target; mia_view_s name; mia_span_s name_span; } member;
		struct { mia_ast_node_s* callee; bool has_type_arguments; mia_ast_nodes_s type_arguments; mia_ast_nodes_s arguments; bool piped; } call;   // piped: the first argument came through '~'
		struct { mia_ast_node_s* target; mia_ast_nodes_s type_arguments; } specialize;
		struct { mia_ast_node_s* target; mia_ast_node_s* index; } index;
		struct { mia_ast_node_s* target; mia_ast_node_s* low; mia_ast_node_s* high; bool inclusive; } slice_of;
		struct { mia_ast_operator_e operation; mia_ast_node_s* operand; } unary;
		struct { mia_ast_operator_e operation; mia_ast_node_s* left; mia_ast_node_s* right; mia_span_s operation_span; } binary;
		struct { mia_ast_node_s* inner; } paren;
		struct { mia_ast_node_s* type; mia_ast_node_s* operand; } cast;
		struct { mia_ast_node_s* operand; } reference;
		struct { bool named; mia_view_s name; mia_ast_node_s* value; } entry;
		struct { mia_ast_node_s* type; mia_ast_nodes_s entries; } composite;
		struct { mia_ast_nodes_s parameters; mia_ast_node_s* result; mia_ast_node_s* body; } lambda;
		struct { mia_ast_node_s* type; } size_of;
		struct { mia_ast_node_s* operand; } lenof;
		struct { mia_ast_node_s* condition; mia_ast_node_s* message; } assert_statement;
		struct { mia_view_s name; mia_ast_attribute_value_e value_kind; mia_view_s text; int64_t integer; } attribute;
		struct { mia_view_s name; mia_span_s name_span; mia_ast_node_s* type; mia_ast_node_s* value; mia_ast_nodes_s attributes; } let;
		struct { mia_ast_node_s* declaration; } declaration_statement;
		struct { mia_ast_node_s* condition; mia_ast_node_s* then; mia_ast_node_s* otherwise; mia_ast_nodes_s attributes; } if_statement;
		struct { mia_ast_node_s* condition; mia_ast_node_s* body; mia_ast_nodes_s attributes; } while_statement;
		struct { mia_view_s name; mia_span_s name_span; mia_ast_node_s* start; mia_ast_node_s* end; bool inclusive; mia_ast_node_s* body; mia_ast_nodes_s attributes; } for_statement;
		struct { mia_ast_node_s* value; mia_ast_nodes_s attributes; } return_statement;
		struct { mia_ast_nodes_s attributes; } jump;   // break, continue
		struct { mia_ast_nodes_s statements; bool braceless; mia_ast_nodes_s attributes; } block;
		struct { mia_ast_node_s* expression; mia_ast_nodes_s attributes; } expression_statement;
		struct { mia_ast_operator_e operation; mia_ast_node_s* target; mia_ast_node_s* value; mia_ast_nodes_s attributes; } assign;
		struct { mia_view_s name; } generic;
		struct { mia_view_s name; mia_ast_node_s* type; mia_ast_node_s* default_value; bool variadic; } param;
		struct { mia_view_s name; mia_ast_node_s* type; mia_ast_node_s* default_value; mia_ast_nodes_s attributes; } field;
		struct { mia_view_s name; mia_span_s name_span; mia_ast_nodes_s generics; mia_ast_nodes_s parameters; mia_ast_node_s* result;
				 mia_ast_node_s* body; mia_ast_nodes_s attributes; bool is_public; } function;
		struct { mia_view_s name; mia_span_s name_span; mia_ast_nodes_s generics; mia_ast_nodes_s fields; mia_ast_nodes_s attributes; bool is_public; } structure;
		struct { mia_view_s name; mia_span_s name_span; mia_ast_node_s* value; mia_ast_nodes_s attributes; bool is_public; } constant;
		struct { mia_view_s name; bool has_alias; mia_view_s alias; } import_name;
		struct { mia_view_list_s path; bool has_alias; mia_view_s alias; } import;
		struct { mia_view_list_s path; mia_ast_nodes_s names; bool star; } from_import;
		struct { mia_ast_nodes_s imports; mia_ast_nodes_s declarations; bool incomplete; } module;   // incomplete: declarations left out (E0207)
	} as;
};

/// the most a syntax tree nests: parentheses, operators (each one of a chain
/// like 1 + 2 + 3), statements and types. the passes that walk a tree
/// recurse, so deeper trees are refused (E0207) before they could overflow
/// the stack.
#define mia_ast_depth_limit 1000

/// the first node of the tree under 'root' that is more than 'limit' levels
/// below it, or null (an iterative walk: any depth is safe).
const mia_ast_node_s* mia_ast_node_too_deep(const mia_ast_node_s* const root, const size_t limit);

/// a new node of 'kind' in 'arena' (payload zeroed).
mia_ast_node_s* mia_ast_node_new(mia_arena_s* const arena, const mia_ast_node_kind_e kind, const mia_span_s span);

/// the name the tree printer uses for 'kind'.
const char* mia_ast_node_kind_name(const mia_ast_node_kind_e kind);

/// a deep copy of the syntax of 'node' (no annotations); generic instances
/// are checked on copies of their declaration.
mia_ast_node_s* mia_ast_node_clone(mia_arena_s* const arena, const mia_ast_node_s* const node);

/// the name and its span of a declaration.
mia_view_s mia_ast_declaration_name(const mia_ast_node_s* const node);

mia_span_s mia_ast_declaration_name_span(const mia_ast_node_s* const node);

/// the attributes written before a statement (an empty list for nodes that have none).
const mia_ast_nodes_s* mia_ast_statement_attributes(const mia_ast_node_s* const node);

/// the attribute 'name' of a list of attribute nodes, or null.
const mia_ast_node_s* mia_ast_attribute_find(const mia_ast_nodes_s* const attributes, const char* const name);

#endif
