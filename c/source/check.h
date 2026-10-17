
#ifndef __mc__check_h__
#define __mc__check_h__

/// the checker's internals, shared by its source files:
///
///   check.c              programs, modules, imports, lookup, declarations, instances
///   check_types.c        resolving types, struct layouts, sizes
///   check_statements.c   statements, assignments, places, flow
///   check_expressions.c  expressions, calls, operators, literals, conversions
///   evaluate.c           compile-time evaluation
///
/// generics are blueprints: every use with new type arguments creates an
/// instance (a copy of the declaration with the type parameters bound) that is
/// checked like ordinary code. operators on non-scalar operands call functions
/// marked '[op._+_]' etc. that are visible from the module, from its imports
/// or from the modules that define the operand types.

#include "mc/program.h"

#include <setjmp.h>

typedef struct mc_checker_s mc_checker_s;

// --- compile-time values -----------------------------------------------------

typedef enum {
	mc_cvalue_nothing ,
	mc_cvalue_integer ,
	mc_cvalue_real    ,
	mc_cvalue_boolean ,
	mc_cvalue_list    ,   // an array or a struct (fields in declaration order)
	mc_cvalue_slice   ,
	mc_cvalue_function,
	mc_cvalue_cell    ,   // a reference argument: the place it refers to
} mc_cvalue_kind_e;

typedef struct mc_clist_s mc_clist_s;

struct mc_cvalue_s {
	mc_cvalue_kind_e kind;
	union {
		mc_i128_s integer;
		double   real;
		bool     boolean;
		mc_clist_s* list;
		struct { mc_clist_s* list; int64_t start; int64_t length; } slice;
		struct { mc_instance_s* instance; mc_node_s* lambda; } function;
		mc_cvalue_s* cell;
	} as;
};

struct mc_clist_s {
	size_t      count;
	mc_cvalue_s items[];
};

typedef enum {
	mc_evaluation_ok          ,
	mc_evaluation_not_constant,   // 'node' and 'reason' say why
	mc_evaluation_failed      ,   // an error was reported already (unless quiet)
} mc_evaluation_status_e;

typedef struct {
	mc_evaluation_status_e status;
	mc_cvalue_s            value ;
	const mc_node_s*       node  ;
	char*                  reason;   // owned by the program arena
} mc_evaluation_s;

typedef struct mc_frame_s mc_frame_s;

/// a loop or a call that is running in an evaluation (for E0705).
typedef struct {
	const mc_node_s* node    ;   // the while, the for or the call
	const char*      function;   // calls: the function called; loops: null
} mc_running_s;

typedef struct {
	mc_checker_s*            checker;
	mc_vector_of(mc_frame_s*) frames;
	mc_vector_of(mc_running_s) running;   // innermost last
	uint64_t                 steps  ;
	uint64_t                 limit  ;   // of steps
	bool                     quiet  ;
	bool                     active ;
	jmp_buf*                 stop   ;
	mc_evaluation_s          problem;
} mc_evaluator_s;

/// the compile-time value of the checked expression 'node'. quiet evaluation
/// runs no functions and reports nothing; it only probes for a known value.
mc_evaluation_s mc_evaluate(mc_evaluator_s* const evaluator, const mc_node_s* const node, const bool quiet);

/// a deep copy (arrays and structs are values).
mc_cvalue_s mc_cvalue_copy(mc_checker_s* const checker, const mc_cvalue_s value);

/// the exact result of an arithmetic or bit operator on integers; false on division by zero.
bool mc_integer_arithmetic(const mc_operator_e operator_, const mc_i128_s left, const mc_i128_s right, mc_i128_s* const result);

/// 'value' wrapped around to fit the integer scalar 'type'.
mc_i128_s mc_integer_wrap(const mc_i128_s value, const mc_type_s* const type);

/// 'value' rounded as the float scalar 'type' (f32 rounds).
double mc_float_wrap(const double value, const mc_type_s* const type);

// --- contexts ------------------------------------------------------------------

/// a set of variables (the ones that may not be assigned yet).
typedef mc_vector_of(mc_variable_s*) mc_variable_set_s;

/// where an expression is checked: a module, type parameter bindings and,
/// inside a function, its scopes. lambdas and local functions get their own
/// context whose parent is the enclosing one.
struct mc_context_s {
	mc_module_s*                module    ;
	mc_bindings_s               bindings  ;
	const mc_type_s*            result    ;
	mc_context_s*               parent    ;
	mc_instance_s*              instance  ;
	bool                        function  ;
	mc_vector_of(mc_map_s*)     scopes    ;
	int                         loops     ;
	mc_variable_set_s           unassigned;
	mc_vector_of(mc_variable_s*) variables;
	bool                        wrap      ;   // '[wrap]' on the function (lambdas and local functions inherit it)
	int                         wrapping  ;   // statements marked '[wrap]' being checked (not inherited)
	int                         level     ;
	int                         depth     ;
};

// --- the checker -----------------------------------------------------------------

struct mc_plugin_type_s;

/// a checker type and its handle for plugin checks.
typedef struct {
	const mc_type_s*         type  ;
	struct mc_plugin_type_s* handle;
} mc_checker_plugin_type_s;

struct mc_checker_s {
	mc_program_s*                program  ;
	mc_arena_s*                  arena    ;
	mc_types_s*                  types    ;
	mc_diag_s*                   diag     ;
	mc_sources_s*                sources  ;
	mc_instances_s               queue    ;
	size_t                       queue_head;
	mc_evaluator_s               evaluator;
	mc_vector_of(const mc_type_s*) sizing ;   // struct types whose size is being computed
	mc_vector_of(mc_struct_s*)   recursive_reported;
	mc_vector_of(mc_context_s*)  contexts ;   // every context, freed at the end
	const mc_check_options_s*    options  ;
	mc_vector_of(const char*)    roots    ;   // module search directories
	mc_vector_of(mc_checker_plugin_type_s) plugin_types;   // type handles given to plugin checks
};

/// a place an assignment writes to.
typedef struct {
	mc_variable_s* variable  ;
	bool           mutable_  ;
	const char*    reason    ;
	const char*    help      ;
	mc_vector_of(const char*) path;   // field names, element indexes ("3") or "*"
	bool           needs_root;
	bool           valid     ;
} mc_place_s;

/// the attributes a declaration or statement has (null when absent).
typedef struct {
	const mc_node_s* plugin ;
	const mc_node_s* op     ;
	const mc_node_s* wrap   ;
	const mc_node_s* uninit ;
} mc_attribute_set_s;

// load.c
mc_module_s* mc_load(mc_checker_s* const c, const char* const path);

// check.c
mc_attribute_set_s mc_attributes(mc_checker_s* const c, const mc_nodes_s* const attributes, const char* const where);
mc_context_s* mc_context_new(mc_checker_s* const c, mc_module_s* const module, const mc_bindings_s* const bindings,
							 const mc_type_s* const result, mc_context_s* const parent, mc_instance_s* const instance,
							 const bool function);
void mc_context_push_scope(mc_checker_s* const c, mc_context_s* const context);
void mc_context_pop_scope(mc_context_s* const context);
mc_symbol_s* mc_context_declare(mc_context_s* const context, const mc_view_s name, mc_symbol_s* const symbol);
mc_symbol_s* mc_lookup(mc_checker_s* const c, const mc_view_s name, mc_context_s* const context, const mc_span_s span, const bool report);
mc_symbol_s* mc_member_of(mc_checker_s* const c, mc_symbol_s* const base, const mc_view_s name, const mc_span_s span, mc_context_s* const context);
mc_symbol_s* mc_qualified(mc_checker_s* const c, const mc_node_s* const node, mc_context_s* const context, const bool report);
void mc_function_declaration(mc_checker_s* const c, mc_function_s* const info, mc_context_s* const context);
const mc_type_s* mc_constant_type(mc_checker_s* const c, mc_constant_s* const info);
bool mc_require_constant(mc_checker_s* const c, const mc_node_s* const node, mc_context_s* const context, const char* const what, mc_cvalue_s* const value);
bool mc_try_value(mc_checker_s* const c, const mc_node_s* const node, mc_cvalue_s* const value);
mc_instance_s* mc_instance(mc_checker_s* const c, mc_function_s* const info, const mc_type_s* const* const arguments, const size_t count,
						   mc_context_s* const context, const mc_span_s span);
void mc_signature(mc_checker_s* const c, mc_function_s* const info, mc_type_list_s* const parameters, const mc_type_s** const result);
void mc_check_instance(mc_checker_s* const c, mc_instance_s* const instance);
void mc_unused_mut(mc_checker_s* const c, mc_context_s* const context);
const char* mc_name_of(mc_checker_s* const c, const mc_type_s* const type);
const char* mc_instance_name(mc_checker_s* const c, const mc_instance_s* const instance);
const char* mc_view_cstring(mc_checker_s* const c, const mc_view_s view);
mc_view_s mc_node_text(mc_checker_s* const c, const mc_node_s* const node);
bool mc_function_is_generic(const mc_function_s* const info);

// check_types.c
const mc_type_s* mc_resolve_type(mc_checker_s* const c, const mc_node_s* const node, mc_context_s* const context,
								 const bool reference, const bool infer);
const mc_type_s* mc_struct_type(mc_checker_s* const c, mc_struct_s* const info, const mc_type_s* const* const arguments,
								const size_t count, const mc_span_s span);
mc_layout_s* mc_layout(mc_checker_s* const c, const mc_type_s* const type);
mc_field_s* mc_layout_field(const mc_layout_s* const layout, const mc_view_s name);
uint64_t mc_size_of(mc_checker_s* const c, const mc_type_s* const type, uint64_t* const alignment);
bool mc_zeroable(mc_checker_s* const c, const mc_type_s* const type, mc_type_list_s* const seen);
const mc_type_s* mc_scalar_type(mc_checker_s* const c, const mc_scalar_e scalar);
const mc_type_s* mc_special_type(mc_checker_s* const c, const mc_special_e special);

// check_statements.c
void mc_block(mc_checker_s* const c, mc_node_s* const node, mc_context_s* const context, const bool new_scope);
mc_place_s mc_place_of(mc_checker_s* const c, const mc_node_s* const node, mc_context_s* const context);
void mc_place_free(mc_place_s* const place);
void mc_report_immutable(mc_checker_s* const c, const mc_place_s* const place, const mc_node_s* const node, const char* const action);
bool mc_terminates(const mc_node_s* const node);

// check_expressions.c
const mc_type_s* mc_expression(mc_checker_s* const c, mc_node_s* const node, mc_context_s* const context, const mc_type_s* const expected);
const mc_type_s* mc_symbol_value(mc_checker_s* const c, mc_symbol_s* const symbol, mc_node_s* const node, mc_context_s* const context,
								 const mc_type_s* const expected);
const mc_type_s* mc_binary_operation(mc_checker_s* const c, mc_node_s* const node, const mc_operator_e operator_, mc_node_s* const left_node,
									 const mc_type_s* left, mc_node_s* const right_node, const mc_type_s* right, mc_context_s* const context,
									 const mc_span_s span);
bool mc_coerce(mc_checker_s* const c, mc_node_s* const node, const mc_type_s* const actual, const mc_type_s* const target);
const mc_type_s* mc_settle(mc_checker_s* const c, mc_node_s* const node, const mc_type_s* const actual);
void mc_finalize(mc_checker_s* const c, mc_node_s* const node, const mc_type_s* const target);
bool mc_untyped_value(mc_checker_s* const c, const mc_node_s* const node, mc_cvalue_s* const value);

// check_plugins.c
/// a '[plugin]' function is used at 'span': reports (once) when no plugin provides it.
void mc_plugin_use(mc_checker_s* const c, mc_function_s* const function, const mc_span_s span);
/// a call of a '[plugin]' function, its arguments checked: runs the plugin's check.
void mc_plugin_check_call(mc_checker_s* const c, mc_node_s* const node, mc_instance_s* const instance);

// variable sets
bool mc_set_contains(const mc_variable_set_s* const set, const mc_variable_s* const variable);
void mc_set_add(mc_variable_set_s* const set, mc_variable_s* const variable);
void mc_set_discard(mc_variable_set_s* const set, const mc_variable_s* const variable);
void mc_set_assign(mc_variable_set_s* const set, const mc_variable_set_s* const other);
void mc_set_union(mc_variable_set_s* const set, const mc_variable_set_s* const other);

#endif
