
/// a plugin built for an api newer than this mia: refused.

#include "mia/plugin.h"

mia_plugin_export bool mia_plugin_init(mia_plugin_api_s* const api) {
	return mia_plugin_compatible(api, mia_plugin_api_version + 1);
}
