
/// the 'std' plugin: registration (see std.h).

#include "std.h"
#include "../plugin_host.h"

bool mc_plugin_std_init(mc_plugin_api_s* const api) {
	if (!mc_plugin_compatible(api, MC_PLUGIN_API_VERSION)) return false;
	static const struct {
		const char*       name ;
		mc_plugin_run_f   run  ;
		mc_plugin_check_f check;
	} functions[] = {
		{ "memory.alloc"  , mc_std_alloc     , NULL                },
		{ "memory.realloc", mc_std_realloc   , NULL                },
		{ "memory.free"   , mc_std_free      , NULL                },
		{ "memory.copy"   , mc_std_copy      , NULL                },
		{ "io.print"      , mc_std_print     , mc_std_check_format },
		{ "io.println"    , mc_std_println   , mc_std_check_format },
		{ "io.write"      , mc_std_write     , NULL                },
		{ "io.write_line" , mc_std_write_line, NULL                },
	};
	for (size_t i = 0; i < sizeof(functions) / sizeof(*functions); ++i) {
		if (!mc_plugin_register(api, "std", functions[i].name, functions[i].run, functions[i].check)) return false;
	}
	return true;
}
