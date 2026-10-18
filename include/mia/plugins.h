
#ifndef __mia__plugins_h__
#define __mia__plugins_h__

/// the plugins mia has loaded: a registry of the functions they provide,
/// by qualified name ('memory.alloc'). a plugin is a shared library, looked
/// up by its name in the plugin directories when a '[plugin = "name"]'
/// function needs it (or compiled in, mia_plugins_add). the plugin 'io' is
/// io.so (.dylib, .dll), 'a.b' is a/b.so: a plugin's library sits beside the
/// module of the same name (io.mia), as the standard library's do.

#include "mia/bytecode.h"
#include "mia/plugin.h"
#include "mia/vector.h"

typedef struct {
	char*              plugin;   // the plugin that provides it
	char*              name  ;   // qualified: 'math.sqrt'
	mia_plugin_run_f   run   ;
	mia_plugin_check_f check ;
} mia_plugins_function_s;

typedef struct mia_plugins_s {
	mia_vector_of(mia_plugins_function_s)       functions  ;
	mia_vector_of(char*)                        names      ;   // of the plugins loaded
	mia_vector_of(struct mia_system_library_s*) libraries  ;
	mia_vector_of(char*)                        directories;   // where plugins are looked up, in order
} mia_plugins_s;

/// an empty registry.
void mia_plugins_init(mia_plugins_s* const plugins);

/// adds a directory where mia_plugins_require looks (after the others).
void mia_plugins_add_directory(mia_plugins_s* const plugins, const char* const directory);

/// makes sure the plugin 'plugin' is loaded: when it is not, loads its
/// library from the first directory that has it. false when no directory
/// has it ('error' null) or it cannot be loaded ('error' set, caller frees).
bool mia_plugins_require(mia_plugins_s* const plugins, const char* const plugin, char** const error);

/// registers the plugin of an init function compiled into the program
/// (embedding mia, tests); false with 'error' set (caller frees).
bool mia_plugins_add(mia_plugins_s* const plugins, const mia_plugin_init_f init, char** const error);

/// the function 'name' provides, or null.
const mia_plugins_function_s* mia_plugins_find(const mia_plugins_s* const plugins, const char* const name);

/// whether the plugin 'plugin' is loaded.
bool mia_plugins_loaded(const mia_plugins_s* const plugins, const char* const plugin);

void mia_plugins_free(mia_plugins_s* const plugins);

/// an empty registry, made once (for callers that give none).
const mia_plugins_s* mia_plugins_default(void);

/// a type handle: the same at run time (made from the module's type
/// descriptors) and at compile time (made from the checker's types).
struct mia_plugin_type_s {
	mia_plugin_kind_e               kind         ;
	const char*                     name         ;
	mia_code_e                      code         ;   // scalars, functions and slices
	uint64_t                        size         ;
	uint64_t                        alignment    ;
	const mia_plugin_type_s*        element      ;   // arrays and slices
	uint64_t                        length       ;   // arrays
	size_t                          field_count  ;
	const char* const*              field_names  ;
	const mia_plugin_type_s* const* field_types  ;
	const uint64_t*                 field_offsets;
	int64_t                         index        ;   // the module's descriptor at run time; -1 at compile time
};

/// the function table every handle carries.
const mia_plugin_interface_s* mia_plugins_table(void);

/// the type handles of a module's descriptors (one per descriptor, in
/// order); freed with mia_plugins_types_free.
mia_plugin_type_s* mia_plugins_types_new(const mia_bytecode_module_s* const module);
void mia_plugins_types_free(mia_plugin_type_s* const types, const size_t count);

#endif
