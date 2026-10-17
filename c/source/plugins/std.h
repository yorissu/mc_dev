
#ifndef __mc__plugins_std_h__
#define __mc__plugins_std_h__

/// the 'std' plugin: the functions of the standard library's 'memory' and
/// 'io' modules. it is compiled into mc and registered like any plugin,
/// and it uses mc only through mc/plugin.h (and mc's text helpers), so it
/// is also an example of a plugin.
///
///   std.c         registration
///   std_memory.c  memory.alloc, realloc, free, copy
///   std_io.c      io.print, println (with the check of the format), write, write_line

#include "mc/plugin.h"

mc_plugin_value_s mc_std_alloc(mc_plugin_call_s* const call);
mc_plugin_value_s mc_std_realloc(mc_plugin_call_s* const call);
mc_plugin_value_s mc_std_free(mc_plugin_call_s* const call);
mc_plugin_value_s mc_std_copy(mc_plugin_call_s* const call);

mc_plugin_value_s mc_std_print(mc_plugin_call_s* const call);
mc_plugin_value_s mc_std_println(mc_plugin_call_s* const call);
mc_plugin_value_s mc_std_write(mc_plugin_call_s* const call);
mc_plugin_value_s mc_std_write_line(mc_plugin_call_s* const call);
void mc_std_check_format(mc_plugin_check_s* const check);

#endif
