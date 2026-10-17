
#ifndef __mc__program_h__
#define __mc__program_h__

/// a checked program: its modules and the symbols the checker found in them.
///
/// every symbol starts with an mc_symbol_s, so a pointer to any symbol can
/// be stored as mc_symbol_s* and recovered by its kind. generic functions and
/// structs have instances: one per set of type arguments, each checked like
/// ordinary code. constants carry their compile-time value.

#include "mc/ast.h"
#include "mc/diag.h"
#include "mc/map.h"
#include "mc/types.h"

typedef enum {
	mc_symbol_module  ,
	mc_symbol_prefix  ,   // the first parts of a dotted module name ('a' of 'import a.b')
	mc_symbol_function,
	mc_symbol_struct  ,
	mc_symbol_constant,
	mc_symbol_variable,
} mc_symbol_kind_e;

typedef struct {
	mc_symbol_kind_e kind;
} mc_symbol_s;

typedef struct mc_module_s mc_module_s;
typedef struct mc_function_s mc_function_s;
typedef struct mc_instance_s mc_instance_s;
typedef struct mc_struct_s mc_struct_s;
typedef struct mc_layout_s mc_layout_s;
typedef struct mc_field_s mc_field_s;
typedef struct mc_constant_s mc_constant_s;
typedef struct mc_variable_s mc_variable_s;
typedef struct mc_context_s mc_context_s;
typedef struct mc_cvalue_s mc_cvalue_s;

/// the operator patterns of '[op...]' functions.
typedef enum {
#define mc_operator_pattern(_name, _text, _arity) mc_pattern_##_name,
#include "mc/operators.def"
#undef mc_operator_pattern
	mc_pattern_count_,
} mc_pattern_e;

typedef struct {
	mc_node_s*   node  ;   // the import or from-import
	mc_module_s* target;
} mc_import_s;

typedef mc_vector_of(mc_function_s*) mc_functions_s;

struct mc_module_s {
	mc_symbol_s                 base      ;
	const char*                 name      ;   // dotted, as first imported ('main' for the entry)
	const char*                 path      ;
	const mc_source_s*          source    ;
	mc_node_s*                  tree      ;
	mc_vector_of(mc_import_s)   imports   ;
	mc_map_s                    symbols   ;   // own top-level declarations
	mc_vector_of(mc_symbol_s*)  ordered   ;   // the same, in declaration order
	mc_map_s                    imported  ;   // name -> symbol of from-imports
	mc_vector_of(mc_module_s*)  star      ;   // modules of 'from m import *'
	mc_map_s                    qualifiers;   // dotted alias -> module
	mc_functions_s              operators[mc_pattern_count_];
};

typedef struct {
	mc_symbol_s  base  ;
	mc_module_s* module;
	const char*  prefix;
} mc_prefix_s;

typedef mc_vector_of(mc_instance_s*) mc_instances_s;

struct mc_function_s {
	mc_symbol_s    base     ;
	mc_node_s*     node     ;
	mc_module_s*   module   ;
	mc_context_s*  parent   ;   // the enclosing context of a local function
	mc_instances_s instances;
	bool           operator_;   // registered as an operator function
	bool           plugin_checked;   // '[plugin]': whether a plugin provides it was checked
};

typedef enum {
	mc_instance_pending ,
	mc_instance_checking,
	mc_instance_checked ,
} mc_instance_state_e;

struct mc_instance_s {
	mc_function_s*                function ;
	const mc_type_s**             arguments;
	size_t                        argument_count;
	mc_bindings_s                 bindings ;
	mc_node_s*                    node     ;   // a copy for generic functions
	mc_type_list_s                parameters;
	const mc_type_s*              result   ;   // null: nothing
	int                           depth    ;
	mc_instance_state_e           state    ;
	mc_vector_of(mc_variable_s*)  variables;   // of the parameters
};

typedef mc_vector_of(mc_layout_s*) mc_layouts_s;

struct mc_struct_s {
	mc_symbol_s  base     ;
	mc_node_s*   node     ;
	mc_module_s* module   ;
	mc_layouts_s instances;
};

struct mc_field_s {
	mc_view_s        name   ;
	const mc_type_s* type   ;
	bool             mutable_;
	mc_node_s*       default_;   // a checked copy, or null
	mc_node_s*       node   ;
	size_t           index  ;
	uint64_t         offset ;   // in bytes, c layout (set with the struct's size)
};

struct mc_layout_s {
	const mc_type_s*          type  ;
	mc_vector_of(mc_field_s*) fields;
};

typedef enum {
	mc_constant_pending ,
	mc_constant_checking,
	mc_constant_done    ,
} mc_constant_state_e;

struct mc_constant_s {
	mc_symbol_s         base   ;
	mc_node_s*          node   ;
	mc_module_s*        module ;
	mc_context_s*       context;   // local constants: the enclosing context
	mc_constant_state_e state  ;
	const mc_type_s*    type   ;
	mc_cvalue_s*        value  ;   // null when it has none
};

typedef enum {
	mc_variable_let  ,
	mc_variable_param,
	mc_variable_for  ,
} mc_variable_kind_e;

struct mc_variable_s {
	mc_symbol_s        base     ;
	mc_view_s          name     ;
	const mc_type_s*   type     ;   // reference parameters: the referenced type
	mc_variable_kind_e kind     ;
	bool               mutable_ ;   // reference parameters: writable through
	mc_span_s          span     ;
	bool               reference;
	int                level    ;
	bool               mutated  ;
	int                loops    ;   // loops around the declaration
	const mc_node_s*   initializer;   // lets: the value it starts with, or null
};

typedef struct {
	mc_arena_s                  arena    ;   // symbols, copies of trees, values
	mc_types_s                  types    ;
	mc_sources_s*               sources  ;
	mc_diag_s*                  diag     ;
	mc_vector_of(mc_module_s*)  modules  ;   // dependencies before dependents
	mc_module_s*                entry    ;
	mc_instances_s              instances;
	mc_layouts_s                layouts  ;
	mc_vector_of(mc_constant_s*) constants;
	struct mc_checker_s*        checker  ;
} mc_program_s;

typedef struct {
	const char* const*         include;   // extra module directories
	size_t                     include_count;
	const char*                stdlib ;   // the standard library directory, or null
	const struct mc_plugins_s* plugins;   // the plugins loaded; null: only the built-in ones
} mc_check_options_s;

/// loads the program whose entry module is 'path' (and everything it
/// imports) and checks it. errors go to 'diag'; the program is returned in
/// any case and must be freed.
mc_program_s* mc_check(const char* const path, mc_sources_s* const sources, mc_diag_s* const diag,
					   const mc_check_options_s* const options);

void mc_program_free(mc_program_s* const program);

/// 'name<int, f32>' of an instance.
void mc_instance_text(mc_string_s* const out, const mc_instance_s* const instance);

/// the type of a function value of 'instance'.
const mc_type_s* mc_instance_type(mc_program_s* const program, const mc_instance_s* const instance);

/// whether the function is '[plugin]' (provided by a plugin).
bool mc_function_is_plugin(const mc_function_s* const function);

/// the qualified name of a function: its module's dotted name and its own
/// ('memory.alloc'); 'main' is the module name of the entry file.
char* mc_function_qualified_name(const mc_function_s* const function);

/// the layout (fields) of a struct type.
mc_layout_s* mc_program_layout(mc_program_s* const program, const mc_type_s* const type);

/// size and alignment of a type in bytes (c layout).
uint64_t mc_program_size_of(mc_program_s* const program, const mc_type_s* const type, uint64_t* const alignment);

/// the program with every generic instantiated and every expression typed
/// (the 'mono' command).
void mc_program_text(mc_program_s* const program, mc_string_s* const out);

#endif
