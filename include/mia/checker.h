
#ifndef __mia__checker_h__
#define __mia__checker_h__

/// the checker's internals, shared with the compile-time evaluator
/// (evaluator.c); the compiler reads the checked program through them too.
/// a program's users need only program.h.
///
/// generics are blueprints: every use with new type arguments creates an
/// instance (a copy of the declaration with the type parameters bound) that is
/// checked like ordinary code. operators are built in: they apply to numbers,
/// 'bool' and functions ('==' and '!='), never to slices or structs.

#include "mia/evaluator.h"
#include "mia/plugin.h"
#include "mia/program.h"

#include <setjmp.h>

typedef struct mia_checker_s mia_checker_s;

// --- contexts ------------------------------------------------------------------

/// a set of variables (the ones that may not be assigned yet).
typedef mia_vector_of(mia_program_variable_s*) mia_checker_variable_set_s;

/// where an expression is checked: a module, type parameter bindings and,
/// inside a function, its scopes. lambdas and local functions get their own
/// context whose parent is the enclosing one.
struct mia_checker_context_s {
	mia_program_module_s*                  module    ;
	mia_type_bindings_s                    bindings  ;
	const mia_type_s*                      result    ;
	mia_checker_context_s*                 parent    ;
	mia_program_instance_s*                instance  ;
	bool                                   function  ;
	mia_vector_of(mia_map_s*)              scopes    ;
	int                                    loops     ;
	mia_checker_variable_set_s             unassigned;
	mia_vector_of(mia_program_variable_s*) variables ;
	bool                                   wrap      ;   // '[wrap]' on the function (lambdas and local functions inherit it)
	int                                    wrapping  ;   // statements marked '[wrap]' being checked (not inherited)
	int                                    level     ;
	int                                    depth     ;
};

// --- the checker -----------------------------------------------------------------

struct mia_plugin_type_s;

/// a checker type and its handle for plugin checks.
typedef struct {
	const mia_type_s*         type  ;
	struct mia_plugin_type_s* handle;
} mia_checker_plugin_type_s;

struct mia_checker_s {
	mia_program_s*                           program                ;
	mia_arena_s*                             arena                  ;
	mia_type_table_s*                        types                  ;
	mia_diag_s*                              diag                   ;
	mia_source_table_s*                      sources                ;
	mia_program_instances_s                  queue                  ;
	size_t                                   queue_head             ;
	mia_evaluator_s                          evaluator              ;
	mia_vector_of(const mia_type_s*)         sizing                 ;   // struct types whose size is being computed
	mia_vector_of(mia_program_struct_s*)     recursive_reported     ;
	mia_vector_of(const mia_type_s*)         large_reported         ;   // struct types reported too large
	mia_vector_of(mia_checker_context_s*)    contexts               ;   // every context, freed at the end
	const mia_program_check_options_s*       options                ;
	mia_vector_of(const char*)               roots                  ;   // module search directories
	mia_vector_of(mia_checker_plugin_type_s) plugin_types           ;   // type handles given to plugin checks
	int                                      constant_depth         ;   // constants being checked inside each other
	bool                                     constant_depth_reported;
	bool                                     struct_depth_reported  ;
	int                                      layout_depth           ;   // layouts being made inside each other
};

/// the attributes a declaration or statement has (null when absent).
typedef struct {
	const mia_ast_node_s* plugin;
	const mia_ast_node_s* wrap  ;
	const mia_ast_node_s* uninit;
} mia_checker_attribute_set_s;

/// loads the entry module at 'path' and every module it imports (null when it cannot be read).
mia_program_module_s* mia_checker_load(mia_checker_s* const c, const char* const path);

/// checks the attributes of a declaration or statement ('where' says what it is: "function",
/// "while"...) and finds the known ones.
mia_checker_attribute_set_s mia_checker_attributes(mia_checker_s* const c, const mia_ast_nodes_s* const attributes, const char* const where);

/// a new context of 'module': its type parameter bindings, the result type of its function (null:
/// none), its parent (local functions and lambdas) and the instance it checks.
mia_checker_context_s* mia_checker_context_new(mia_checker_s* const c, mia_program_module_s* const module, const mia_type_bindings_s* const bindings,
											   const mia_type_s* const result, mia_checker_context_s* const parent, mia_program_instance_s* const instance,
											   const bool function);

/// opens a scope (a block) in 'context'.
void mia_checker_context_push_scope(mia_checker_s* const c, mia_checker_context_s* const context);

/// closes the innermost scope of 'context'.
void mia_checker_context_pop_scope(mia_checker_context_s* const context);

/// declares 'name' in the innermost scope; the symbol declared there before under that name, or
/// null.
mia_program_symbol_s* mia_checker_context_declare(mia_checker_context_s* const context, const mia_view_s name, mia_program_symbol_s* const symbol);

/// the symbol 'name' means in 'context' (scopes, the module, its imports); null, reported when
/// 'report', when there is none.
mia_program_symbol_s* mia_checker_lookup(mia_checker_s* const c, const mia_view_s name, mia_checker_context_s* const context, const mia_span_s span, const bool report);

/// the symbol 'name' in the module or the module prefix 'base'; null, reported, when there is none.
mia_program_symbol_s* mia_checker_member_of(mia_checker_s* const c, mia_program_symbol_s* const base, const mia_view_s name, const mia_span_s span, mia_checker_context_s* const context);

/// the symbol a name or a dotted name ('a.b.c') means; null, reported when 'report', when there is
/// none.
mia_program_symbol_s* mia_checker_qualified(mia_checker_s* const c, const mia_ast_node_s* const node, mia_checker_context_s* const context, const bool report);

/// checks a function's declaration (not its body): generics, parameters, result, attributes.
void mia_checker_function_declaration(mia_checker_s* const c, mia_program_function_s* const info, mia_checker_context_s* const context);

/// the type of a constant, checking it on its first use (cycles are reported).
const mia_type_s* mia_checker_constant_type(mia_checker_s* const c, mia_program_constant_s* const info);

/// the compile-time value of 'node', which must have one ('what' says what it is in the error
/// otherwise).
bool mia_checker_require_constant(mia_checker_s* const c, const mia_ast_node_s* const node, mia_checker_context_s* const context, const char* const what, mia_evaluator_value_s* const value);

/// the compile-time value of 'node' when it has one, without reporting anything (no functions run).
bool mia_checker_try_value(mia_checker_s* const c, const mia_ast_node_s* const node, mia_evaluator_value_s* const value);

/// the instance of a function for its type arguments (made, and checked later, when it is new).
mia_program_instance_s* mia_checker_instance(mia_checker_s* const c, mia_program_function_s* const info, const mia_type_s* const* const arguments, const size_t count,
											 mia_checker_context_s* const context, const mia_span_s span);

/// the parameter and result types of a function (result null: nothing).
void mia_checker_signature(mia_checker_s* const c, mia_program_function_s* const info, mia_type_list_s* const parameters, const mia_type_s** const result);

/// checks the body of an instance (once).
void mia_checker_check_instance(mia_checker_s* const c, mia_program_instance_s* const instance);

/// the text of a type ('[]u8'), kept in the program's arena.
const char* mia_checker_name_of(mia_checker_s* const c, const mia_type_s* const type);

/// the text of an instance ('max<int>'), kept in the program's arena.
const char* mia_checker_instance_name(mia_checker_s* const c, const mia_program_instance_s* const instance);

/// a null-terminated copy of 'view' in the program's arena.
const char* mia_checker_view_cstring(mia_checker_s* const c, const mia_view_s view);

/// the source text of a node.
mia_view_s mia_checker_node_text(mia_checker_s* const c, const mia_ast_node_s* const node);

/// whether a function has type parameters.
bool mia_checker_function_is_generic(const mia_program_function_s* const info);

/// the type a type expression means in 'context' ('reference': where a '&' type may be written;
/// 'infer': '[_]' takes its length from the value).
const mia_type_s* mia_checker_resolve_type(mia_checker_s* const c, const mia_ast_node_s* const node, mia_checker_context_s* const context,
										   const bool reference, const bool infer);

/// the type of a struct for its type arguments.
const mia_type_s* mia_checker_struct_type(mia_checker_s* const c, mia_program_struct_s* const info, const mia_type_s* const* const arguments,
										  const size_t count, const mia_span_s span);

/// the layout of a struct type: its fields with their types and offsets (made once).
mia_program_layout_s* mia_checker_layout(mia_checker_s* const c, const mia_type_s* const type);

/// the field 'name' of a layout, or null.
mia_program_field_s* mia_checker_layout_field(const mia_program_layout_s* const layout, const mia_view_s name);

/// the size of a type in bytes, and its alignment in 'alignment' (c layout).
uint64_t mia_checker_size_of(mia_checker_s* const c, const mia_type_s* const type, uint64_t* const alignment);

/// whether a type has a zero value: no function values but in fields with a default ('seen' guards
/// against recursive structs).
bool mia_checker_zeroable(mia_checker_s* const c, const mia_type_s* const type, mia_type_list_s* const seen);

/// the type of a built-in scalar.
const mia_type_s* mia_checker_scalar_type(mia_checker_s* const c, const mia_scalar_e scalar);

/// a special type (untyped literals, nothing, null, the error type).
const mia_type_s* mia_checker_special_type(mia_checker_s* const c, const mia_type_special_e special);

/// checks a block ('new_scope': its own scope, or the one of the function it is the body of).
void mia_checker_block(mia_checker_s* const c, mia_ast_node_s* const node, mia_checker_context_s* const context, const bool new_scope);

/// whether a statement never finishes normally: it returns, or loops forever, on every path.
bool mia_checker_terminates(const mia_ast_node_s* const node);

/// checks an expression and returns its type ('expected': the type it should have, which literals
/// take; null: none).
const mia_type_s* mia_checker_expression(mia_checker_s* const c, mia_ast_node_s* const node, mia_checker_context_s* const context, const mia_type_s* const expected);

/// checks the use of 'symbol' as a value at 'node' and returns its type.
const mia_type_s* mia_checker_symbol_value(mia_checker_s* const c, mia_program_symbol_s* const symbol, mia_ast_node_s* const node, mia_checker_context_s* const context,
										   const mia_type_s* const expected);

/// checks a binary operator on checked operands and returns the type of the result.
const mia_type_s* mia_checker_binary_operation(mia_checker_s* const c, mia_ast_node_s* const node, const mia_ast_operator_e operation, mia_ast_node_s* const left_node,
											   const mia_type_s* const left, mia_ast_node_s* const right_node, const mia_type_s* const right, mia_checker_context_s* const context,
											   const mia_span_s span);

/// whether a value of 'actual' can be used where 'target' is expected (an untyped literal that fits
/// takes the type); reports when it cannot.
bool mia_checker_coerce(mia_checker_s* const c, mia_ast_node_s* const node, const mia_type_s* const actual, const mia_type_s* const target);

/// the type an untyped value takes where nothing else decides it (int, float); reports an
/// integer literal that does not fit, and a null whose type is unknown.
const mia_type_s* mia_checker_settle(mia_checker_s* const c, mia_ast_node_s* const node, const mia_type_s* const actual);

/// gives an untyped expression (and its parts) the type 'target'.
void mia_checker_finalize(mia_checker_s* const c, mia_ast_node_s* const node, const mia_type_s* const target);

/// a plugin function is used at 'span': reports (once) when its plugin does not provide it.
void mia_checker_plugin_use(mia_checker_s* const c, mia_program_function_s* const function, const mia_span_s span);
/// a call of a plugin function, its arguments checked: runs the plugin's check.
void mia_checker_plugin_check_call(mia_checker_s* const c, mia_ast_node_s* const node, mia_program_instance_s* const instance);

/// variable sets: the variables that may not be assigned yet, as flow analysis follows a function.
bool mia_checker_set_contains(const mia_checker_variable_set_s* const set, const mia_program_variable_s* const variable);
void mia_checker_set_add(mia_checker_variable_set_s* const set, mia_program_variable_s* const variable);
void mia_checker_set_discard(mia_checker_variable_set_s* const set, const mia_program_variable_s* const variable);
void mia_checker_set_assign(mia_checker_variable_set_s* const set, const mia_checker_variable_set_s* const other);
void mia_checker_set_union(mia_checker_variable_set_s* const set, const mia_checker_variable_set_s* const other);

/// the checker's part of the plugin api's function table (checks at compile time).
void mia_checker_plugin_table(mia_plugin_interface_s* const table);

#endif
