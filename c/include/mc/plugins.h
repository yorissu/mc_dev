
#ifndef __mc__plugins_h__
#define __mc__plugins_h__

/// the plugins mc has loaded: a registry of the functions they provide,
/// by qualified name ('memory.alloc'). the 'std' plugin is compiled in and
/// always there; others are shared libraries loaded with mc_plugins_load
/// (the '--plugin' and '--plugin-dir' options, the MC_PLUGINS variable).

#include "mc/plugin.h"
#include "mc/vector.h"

typedef struct {
	char*             plugin;   // the plugin that provides it
	char*             name  ;   // qualified: 'math.sqrt'
	mc_plugin_run_f   run   ;
	mc_plugin_check_f check ;
} mc_plugin_function_s;

typedef struct mc_plugins_s {
	mc_vector_of(mc_plugin_function_s) functions;
	mc_vector_of(char*)                names    ;   // of the plugins loaded
	mc_vector_of(void*)                libraries;
} mc_plugins_s;

/// a registry with the built-in plugins.
void mc_plugins_init(mc_plugins_s* const plugins);

/// loads a plugin library, or every library in a directory; false with
/// 'error' set (caller frees) when it cannot.
bool mc_plugins_load(mc_plugins_s* const plugins, const char* const path, char** const error);

/// registers the plugin of an init function compiled into the program
/// (embedding mc, tests); false with 'error' set (caller frees).
bool mc_plugins_add(mc_plugins_s* const plugins, const mc_plugin_init_f init, char** const error);

/// the function 'name' provides, or null.
const mc_plugin_function_s* mc_plugins_find(const mc_plugins_s* const plugins, const char* const name);

/// whether the plugin 'plugin' is loaded.
bool mc_plugins_loaded(const mc_plugins_s* const plugins, const char* const plugin);

void mc_plugins_free(mc_plugins_s* const plugins);

/// a registry with only the built-in plugins, made once (for callers that
/// give none).
const mc_plugins_s* mc_plugins_default(void);

/// the file name ending of plugin libraries on this platform ('.so', '.dll', '.dylib').
const char* mc_plugins_extension(void);

#endif
