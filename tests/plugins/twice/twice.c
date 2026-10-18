
/// a plugin that provides a function the plugin 'io' provides too: when
/// 'io' is looked up later, it is refused.

#include "mia/plugin.h"

static mia_plugin_value_s _twice_nothing(mia_plugin_call_s* const call) {
	return mia_plugin_make_none(call);
}

mia_plugin_export bool mia_plugin_init(mia_plugin_api_s* const api) {
	return mia_plugin_compatible(api, mia_plugin_api_version) && mia_plugin_register(api, "twice", "io.println", _twice_nothing, NULL);
}
