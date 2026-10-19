
#ifndef __mia__program_h__
#define __mia__program_h__

/// a checked program: its modules and the symbols the checker found in them.
///
/// every symbol starts with an mia_program_symbol_s, so a pointer to any symbol can
/// be stored as mia_program_symbol_s* and recovered by its kind. generic functions and
/// structs have instances: one per set of type arguments, each checked like
/// ordinary code. constants carry their compile-time value.

#include "mia/ast.h"
#include "mia/diag.h"
#include "mia/map.h"
#include "mia/type.h"

typedef enum {
	mia_program_symbol_module  ,
	mia_program_symbol_prefix  ,   // the first parts of a dotted module name ('a' of 'import a.b')
	mia_program_symbol_function,
	mia_program_symbol_struct  ,
	mia_program_symbol_constant,
	mia_program_symbol_variable,
} mia_program_symbol_kind_e;

typedef struct {
	mia_program_symbol_kind_e kind;
} mia_program_symbol_s;

typedef struct mia_program_module_s mia_program_module_s;
typedef struct mia_program_function_s mia_program_function_s;
typedef struct mia_program_instance_s mia_program_instance_s;
typedef struct mia_program_struct_s mia_program_struct_s;
typedef struct mia_program_layout_s mia_program_layout_s;
typedef struct mia_program_field_s mia_program_field_s;
typedef struct mia_program_constant_s mia_program_constant_s;
typedef struct mia_program_variable_s mia_program_variable_s;
typedef struct mia_checker_context_s mia_checker_context_s;
typedef struct mia_evaluator_value_s mia_evaluator_value_s;

typedef struct {
	mia_ast_node_s*       node  ;   // the import or from-import
	mia_program_module_s* target;
} mia_program_import_s;

typedef mia_vector_of(mia_program_function_s*) mia_program_functions_s;

struct mia_program_module_s {
	mia_program_symbol_s                 base      ;
	const char*                          name      ;   // dotted, as first imported ('main' for the entry)
	const char*                          path      ;
	const mia_source_s*                  source    ;
	mia_ast_node_s*                      tree      ;
	mia_vector_of(mia_program_import_s)  imports   ;
	mia_map_s                            symbols   ;   // own top-level declarations
	mia_vector_of(mia_program_symbol_s*) ordered   ;   // the same, in declaration order
	mia_map_s                            imported  ;   // name -> symbol of from-imports
	mia_vector_of(mia_program_module_s*) star      ;   // modules of 'from m import *'
	mia_map_s                            qualifiers;   // dotted alias -> module
};

typedef struct {
	mia_program_symbol_s  base  ;
	mia_program_module_s* module;
	const char*           prefix;
} mia_program_prefix_s;

typedef mia_vector_of(mia_program_instance_s*) mia_program_instances_s;

struct mia_program_function_s {
	mia_program_symbol_s    base          ;
	mia_ast_node_s*         node          ;
	mia_program_module_s*   module        ;
	mia_checker_context_s*  parent        ;   // the enclosing context of a local function
	mia_program_instances_s instances     ;
	bool                    plugin_checked;   // plugin functions: whether their plugin provides them was checked
};

typedef enum {
	mia_program_instance_pending ,
	mia_program_instance_checking,
	mia_program_instance_checked ,
} mia_program_instance_state_e;

struct mia_program_instance_s {
	mia_program_function_s*                function      ;
	const mia_type_s**                     arguments     ;
	size_t                                 argument_count;
	mia_type_bindings_s                    bindings      ;
	mia_ast_node_s*                        node          ;   // a copy for generic functions
	mia_type_list_s                        parameters    ;
	const mia_type_s*                      result        ;   // null: nothing
	int                                    depth         ;
	mia_program_instance_state_e           state         ;
	mia_vector_of(mia_program_variable_s*) variables     ;   // of the parameters
};

typedef mia_vector_of(mia_program_layout_s*) mia_program_layouts_s;

struct mia_program_struct_s {
	mia_program_symbol_s  base     ;
	mia_ast_node_s*       node     ;
	mia_program_module_s* module   ;
	mia_program_layouts_s instances;
};

struct mia_program_field_s {
	mia_view_s        name         ;
	const mia_type_s* type         ;
	mia_ast_node_s*   default_value;   // a checked copy, or null
	mia_ast_node_s*   node         ;
	size_t            index        ;
	uint64_t          offset       ;   // in bytes, c layout (set with the struct's size)
};

struct mia_program_layout_s {
	const mia_type_s*                   type     ;
	mia_vector_of(mia_program_field_s*) fields   ;
	bool                                sized    ;   // size and alignment are known
	uint64_t                            size     ;
	uint64_t                            alignment;
};

typedef enum {
	mia_program_constant_pending ,
	mia_program_constant_checking,
	mia_program_constant_done    ,
} mia_program_constant_state_e;

struct mia_program_constant_s {
	mia_program_symbol_s         base   ;
	mia_ast_node_s*              node   ;
	mia_program_module_s*        module ;
	mia_checker_context_s*       context;   // local constants: the enclosing context
	mia_program_constant_state_e state  ;
	const mia_type_s*            type   ;
	mia_evaluator_value_s*       value  ;   // null when it has none
};

typedef enum {
	mia_program_variable_let  ,
	mia_program_variable_param,
	mia_program_variable_for  ,
} mia_program_variable_kind_e;

struct mia_program_variable_s {
	mia_program_symbol_s        base       ;
	mia_view_s                  name       ;
	const mia_type_s*           type       ;   // reference parameters: the referenced type
	mia_program_variable_kind_e kind       ;
	mia_span_s                  span       ;
	bool                        reference  ;
	bool                        changed    ;   // assigned or lent to a reference parameter somewhere
	int                         level      ;
	const mia_ast_node_s*       initializer;   // lets: the value it starts with, or null
};

typedef struct {
	mia_arena_s                            arena    ;   // symbols, copies of trees, values
	mia_type_table_s                       types    ;
	mia_source_table_s*                    sources  ;
	mia_diag_s*                            diag     ;
	mia_vector_of(mia_program_module_s*)   modules  ;   // dependencies before dependents
	mia_program_module_s*                  entry    ;
	mia_program_instances_s                instances;
	mia_program_layouts_s                  layouts  ;
	mia_vector_of(mia_program_constant_s*) constants;
	struct mia_checker_s*                  checker  ;
} mia_program_s;

typedef struct {
	const char* const*    module_dirs     ;   // more module directories (after the program's and the stdlib's)
	size_t                module_dir_count;
	const char*           stdlib          ;   // the standard library directory, or null
	struct mia_plugins_s* plugins         ;   // the plugins (more are loaded as plugin functions need them); null: none
	uint64_t              eval_steps      ;   // the steps compile-time evaluation may take (0: the default, 1000000)
	bool                  library         ;   // the entry module may lack 'main' (check, mono)
} mia_program_check_options_s;

/// the default number of steps compile-time evaluation may take.
#define mia_program_eval_steps_default 1000000

/// loads the program whose entry module is 'path' (and everything it
/// imports) and checks it. errors go to 'diag'; the program is returned in
/// any case and must be freed.
mia_program_s* mia_program_check(const char* const path, mia_source_table_s* const sources, mia_diag_s* const diag,
								 const mia_program_check_options_s* const options);

/// frees a program of mia_program_check.
void mia_program_free(mia_program_s* const program);

/// 'name<int, f32>' of an instance.
void mia_program_instance_text(mia_string_s* const out, const mia_program_instance_s* const instance);

/// the type of a function value of 'instance'.
const mia_type_s* mia_program_instance_type(mia_program_s* const program, const mia_program_instance_s* const instance);

/// whether the function is '[plugin = "name"]' (provided by a plugin).
bool mia_program_function_is_plugin(const mia_program_function_s* const function);

/// the plugin of a '[plugin = "name"]' function (caller frees), or null when
/// the attribute names none (an error).
char* mia_program_function_plugin(const mia_program_function_s* const function);

/// the qualified name of a function: its module's dotted name and its own
/// ('memory.alloc'); 'main' is the module name of the entry file.
char* mia_program_function_qualified_name(const mia_program_function_s* const function);

/// the layout (fields) of a struct type.
mia_program_layout_s* mia_program_layout(mia_program_s* const program, const mia_type_s* const type);

/// size and alignment of a type in bytes (c layout).
uint64_t mia_program_size_of(mia_program_s* const program, const mia_type_s* const type, uint64_t* const alignment);

#endif
