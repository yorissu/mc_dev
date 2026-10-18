
/// a plugin that never asks whether this mia can run it: refused.

#include "mia/plugin.h"

mia_plugin_export bool mia_plugin_init(mia_plugin_api_s* const api) {
	(void)api;
	return true;
}
