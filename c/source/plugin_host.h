
#ifndef __mc__plugin_host_h__
#define __mc__plugin_host_h__

/// mc's side of the plugin api, shared by its source files:
///
///   plugins.c        the registry, loading libraries, type handles, the function table
///   vm_plugins.c     calls at run time (the machine)
///   check_plugins.c  checks at compile time (the checker)
///   plugins/std*.c   the built-in 'std' plugin (only through mc/plugin.h)

#include "mc/bytecode.h"
#include "mc/plugins.h"

/// a type handle: the same at run time (made from the module's type
/// descriptors) and at compile time (made from the checker's types).
struct mc_plugin_type_s {
	mc_plugin_kind_e               kind         ;
	const char*                    name         ;
	mc_type_code_e                 code         ;   // scalars, functions and slices
	uint64_t                       size         ;
	uint64_t                       alignment    ;
	const mc_plugin_type_s*        element      ;   // arrays and slices
	uint64_t                       length       ;   // arrays
	size_t                         field_count  ;
	const char* const*             field_names  ;
	const mc_plugin_type_s* const* field_types  ;
	const uint64_t*                field_offsets;
	int64_t                        index        ;   // the module's descriptor at run time; -1 at compile time
};

/// the function table every handle carries.
const mc_plugin_interface_s* mc_plugin_table(void);

/// the parts of the table the machine and the checker provide.
void mc_vm_plugin_table(mc_plugin_interface_s* const table);
void mc_check_plugin_table(mc_plugin_interface_s* const table);

/// the type handles of a module's descriptors (one per descriptor, in
/// order); freed with mc_plugin_types_free.
mc_plugin_type_s* mc_plugin_types_new(const mc_module_bc_s* const module);
void mc_plugin_types_free(mc_plugin_type_s* const types, const size_t count);

/// the bits and signedness of a scalar type code.
uint32_t mc_plugin_code_bits(const mc_type_code_e code);
bool mc_plugin_code_signed(const mc_type_code_e code);

/// the built-in plugins: their init functions.
bool mc_plugin_std_init(mc_plugin_api_s* const api);

#endif
