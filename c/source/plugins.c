
/// the plugins: the registry of the functions they provide, loading
/// libraries, type handles and the function table of the plugin api.

#include "plugin_host.h"
#include "mc/platform.h"
#include "mc/string.h"

#include <string.h>

/// the plugins compiled into mc, always loaded.
static const mc_plugin_init_f _mc_builtin_plugins[] = {
	mc_plugin_std_init,
};

// --- registration ------------------------------------------------------------------------

typedef struct {
	mc_plugin_api_s public_;   // first: the handle the plugin gets
	mc_plugins_s*   plugins;
	uint32_t        wanted ;   // the api version the plugin asked for (0: none)
	char*           error  ;
} _mc_registration_s;

static bool _mc_compatible(mc_plugin_api_s* const api, const uint32_t version) {
	_mc_registration_s* const registration = (_mc_registration_s*)api;
	registration->wanted = version;
	return version >= 1 && version <= MC_PLUGIN_API_VERSION;
}

static bool _mc_register(mc_plugin_api_s* const api, const char* const plugin, const char* const name, mc_plugin_run_f run,
						 mc_plugin_check_f check) {
	_mc_registration_s* const registration = (_mc_registration_s*)api;
	mc_plugins_s* const plugins = registration->plugins;
	if (NULL != registration->error) return false;
	if (NULL == plugin || '\0' == plugin[0] || NULL == name || NULL == strchr(name, '.') || NULL == run) {
		registration->error = mc_format("it registers `%s` without a plugin name, a qualified name ('module.function') or a run function",
			(name != NULL) ? name : "?");
		return false;
	}
	const mc_plugin_function_s* const existing = mc_plugins_find(plugins, name);
	if (existing != NULL) {
		registration->error = mc_format("`%s` is provided by the plugins `%s` and `%s`", name, existing->plugin, plugin);
		return false;
	}
	const mc_plugin_function_s function = { .plugin = mc_strdup(plugin), .name = mc_strdup(name), .run = run, .check = check, };
	mc_vector_push(&plugins->functions, function);
	if (!mc_plugins_loaded(plugins, plugin)) mc_vector_push(&plugins->names, mc_strdup(plugin));
	return true;
}

/// runs an init function; false with 'error' set when it refuses or fails.
static bool _mc_initialize(mc_plugins_s* const plugins, const mc_plugin_init_f init, char** const error) {
	const size_t functions = plugins->functions.count, names = plugins->names.count;
	_mc_registration_s registration = { .public_ = { .interface = mc_plugin_table(), }, .plugins = plugins, };
	const bool ready = init(&registration.public_);
	if (ready && NULL == registration.error && registration.wanted != 0) return true;
	// forget what it registered before it failed
	while (plugins->functions.count > functions) {
		mc_plugin_function_s* const function = &plugins->functions.data[--plugins->functions.count];
		mc_free(function->plugin);
		mc_free(function->name);
	}
	while (plugins->names.count > names) mc_free(plugins->names.data[--plugins->names.count]);
	if (registration.error != NULL) {
		*error = registration.error;
	}
	else if (registration.wanted > MC_PLUGIN_API_VERSION) {
		*error = mc_format("it needs plugin api version %u, but this mc has version %u", registration.wanted, MC_PLUGIN_API_VERSION);
	}
	else if (0 == registration.wanted) {
		*error = mc_strdup("its mc_plugin_init did not call mc_plugin_compatible");
	}
	else {
		*error = mc_strdup("its mc_plugin_init failed");
	}
	return false;
}

void mc_plugins_init(mc_plugins_s* const plugins) {
	*plugins = (mc_plugins_s) {0};
	for (size_t i = 0; i < mc_count_of(_mc_builtin_plugins); ++i) {
		char* error = NULL;
		if (!_mc_initialize(plugins, _mc_builtin_plugins[i], &error)) mc_panic("a built-in plugin cannot be registered: %s", error);
	}
}

const char* mc_plugins_extension(void) {
#if defined(_WIN32)
	return ".dll";
#elif defined(__APPLE__)
	return ".dylib";
#else
	return ".so";
#endif
}

static bool _mc_load_library(mc_plugins_s* const plugins, const char* const path, char** const error) {
	char* reason = NULL;
	void* const library = mc_library_open(path, &reason);
	if (NULL == library) {
		*error = mc_format("cannot load the plugin `%s`: %s", path, reason);
		mc_free(reason);
		return false;
	}
	void* const symbol = mc_library_symbol(library, MC_PLUGIN_INIT_NAME);
	if (NULL == symbol) {
		mc_library_close(library);
		*error = mc_format("`%s` is not an mc plugin: it has no `%s` function", path, MC_PLUGIN_INIT_NAME);
		return false;
	}
	mc_plugin_init_f init = NULL;
	memcpy(&init, &symbol, sizeof(init));
	if (!_mc_initialize(plugins, init, &reason)) {
		mc_library_close(library);
		*error = mc_format("the plugin `%s` is refused: %s", path, reason);
		mc_free(reason);
		return false;
	}
	mc_vector_push(&plugins->libraries, library);
	return true;
}

typedef struct {
	mc_plugins_s* plugins;
	char*         error  ;
} _mc_directory_load_s;

static void _mc_load_visit(const char* const path, void* const context) {
	_mc_directory_load_s* const load = context;
	if (load->error != NULL || !mc_view_ends_with(mc_view_cstr(path), mc_view_cstr(mc_plugins_extension()))) return;
	(void)_mc_load_library(load->plugins, path, &load->error);
}

bool mc_plugins_load(mc_plugins_s* const plugins, const char* const path, char** const error) {
	if (mc_is_directory(path)) {
		_mc_directory_load_s load = { .plugins = plugins, };
		if (!mc_directory_each(path, _mc_load_visit, &load)) {
			*error = mc_format("cannot read the plugin directory `%s`", path);
			return false;
		}
		*error = load.error;
		return NULL == load.error;
	}
	if (!mc_is_file(path)) {
		*error = mc_format("cannot load the plugin `%s`: there is no such file", path);
		return false;
	}
	return _mc_load_library(plugins, path, error);
}

bool mc_plugins_add(mc_plugins_s* const plugins, const mc_plugin_init_f init, char** const error) {
	return _mc_initialize(plugins, init, error);
}

const mc_plugin_function_s* mc_plugins_find(const mc_plugins_s* const plugins, const char* const name) {
	for (size_t i = 0; i < plugins->functions.count; ++i) if (0 == strcmp(plugins->functions.data[i].name, name)) return &plugins->functions.data[i];
	return NULL;
}

bool mc_plugins_loaded(const mc_plugins_s* const plugins, const char* const plugin) {
	for (size_t i = 0; i < plugins->names.count; ++i) if (0 == strcmp(plugins->names.data[i], plugin)) return true;
	return false;
}

void mc_plugins_free(mc_plugins_s* const plugins) {
	for (size_t i = 0; i < plugins->functions.count; ++i) {
		mc_free(plugins->functions.data[i].plugin);
		mc_free(plugins->functions.data[i].name);
	}
	mc_vector_free(&plugins->functions);
	for (size_t i = 0; i < plugins->names.count; ++i) mc_free(plugins->names.data[i]);
	mc_vector_free(&plugins->names);
	for (size_t i = 0; i < plugins->libraries.count; ++i) mc_library_close(plugins->libraries.data[i]);
	mc_vector_free(&plugins->libraries);
}

const mc_plugins_s* mc_plugins_default(void) {
	static mc_plugins_s plugins;
	static bool ready = false;
	if (!ready) {
		mc_plugins_init(&plugins);
		ready = true;
	}
	return &plugins;
}

// --- types -------------------------------------------------------------------------------

uint32_t mc_plugin_code_bits(const mc_type_code_e code) {
	switch (code) {
		case mc_code_s8: case mc_code_u8: case mc_code_bool: return 8;
		case mc_code_s16: case mc_code_u16:                   return 16;
		case mc_code_s32: case mc_code_u32: case mc_code_f32: return 32;
		case mc_code_slice:                                   return 128;
		default:                                              return 64;
	}
}

bool mc_plugin_code_signed(const mc_type_code_e code) {
	switch (code) {
		case mc_code_s8: case mc_code_s16: case mc_code_s32: case mc_code_s64: case mc_code_int:
		case mc_code_f32: case mc_code_f64: case mc_code_float:
			return true;
		default:
			return false;
	}
}

static mc_plugin_kind_e _mc_type_kind(const mc_plugin_type_s* const type) {
	return type->kind;
}

static const char* _mc_type_name(const mc_plugin_type_s* const type) {
	return type->name;
}

static uint64_t _mc_type_size(const mc_plugin_type_s* const type) {
	return type->size;
}

static uint64_t _mc_type_alignment(const mc_plugin_type_s* const type) {
	return type->alignment;
}

static uint32_t _mc_type_bits(const mc_plugin_type_s* const type) {
	return (type->code != mc_type_code_count_) ? mc_plugin_code_bits(type->code) : (uint32_t)(type->size * 8);
}

static bool _mc_type_signed(const mc_plugin_type_s* const type) {
	return type->code != mc_type_code_count_ && mc_plugin_code_signed(type->code);
}

static const mc_plugin_type_s* _mc_type_element(const mc_plugin_type_s* const type) {
	return type->element;
}

static uint64_t _mc_type_length(const mc_plugin_type_s* const type) {
	return type->length;
}

static size_t _mc_type_field_count(const mc_plugin_type_s* const type) {
	return type->field_count;
}

static const char* _mc_type_field_name(const mc_plugin_type_s* const type, const size_t index) {
	return (index < type->field_count) ? type->field_names[index] : NULL;
}

static const mc_plugin_type_s* _mc_type_field_type(const mc_plugin_type_s* const type, const size_t index) {
	return (index < type->field_count) ? type->field_types[index] : NULL;
}

static uint64_t _mc_type_field_offset(const mc_plugin_type_s* const type, const size_t index) {
	return (index < type->field_count) ? type->field_offsets[index] : 0;
}

mc_plugin_type_s* mc_plugin_types_new(const mc_module_bc_s* const module) {
	const size_t count = module->types.count;
	mc_plugin_type_s* const types = mc_calloc(count > 0 ? count : 1, sizeof(mc_plugin_type_s));
	for (size_t i = 0; i < count; ++i) {
		const mc_bc_type_s* const descriptor = &module->types.data[i];
		mc_plugin_type_s* const type = &types[i];
		static const mc_plugin_kind_e kinds[] = {
			[mc_descriptor_int] = mc_plugin_kind_int, [mc_descriptor_float] = mc_plugin_kind_float, [mc_descriptor_bool] = mc_plugin_kind_bool,
			[mc_descriptor_array] = mc_plugin_kind_array, [mc_descriptor_slice] = mc_plugin_kind_slice, [mc_descriptor_struct] = mc_plugin_kind_struct,
			[mc_descriptor_function] = mc_plugin_kind_function,
		};
		type->kind = kinds[descriptor->kind];
		type->name = descriptor->name;
		type->code = descriptor->code;
		type->size = descriptor->size;
		type->alignment = descriptor->alignment;
		type->element = (descriptor->element >= 0 && (size_t)descriptor->element < count) ? &types[descriptor->element] : NULL;
		type->length = (descriptor->length > 0) ? (uint64_t)descriptor->length : 0;
		type->index = (int64_t)i;
		type->field_count = descriptor->field_names.count;
		if (type->field_count > 0) {
			const mc_plugin_type_s** const fields = mc_calloc(type->field_count, sizeof(*fields));
			for (size_t f = 0; f < type->field_count; ++f) fields[f] = &types[descriptor->field_types.data[f]];
			type->field_types = fields;
			type->field_names = (const char* const*)descriptor->field_names.data;
			type->field_offsets = descriptor->field_offsets.data;
		}
	}
	return types;
}

void mc_plugin_types_free(mc_plugin_type_s* const types, const size_t count) {
	if (NULL == types) return;
	for (size_t i = 0; i < count; ++i) mc_free((void*)(uintptr_t)types[i].field_types);
	mc_free(types);
}

// --- the table ---------------------------------------------------------------------------

const mc_plugin_interface_s* mc_plugin_table(void) {
	static mc_plugin_interface_s table;
	static bool ready = false;
	if (!ready) {
		table.version = MC_PLUGIN_API_VERSION;
		table.size = (uint32_t)sizeof(table);
		table.compatible = _mc_compatible;
		table.register_function = _mc_register;
		table.type_kind = _mc_type_kind;
		table.type_name = _mc_type_name;
		table.type_size = _mc_type_size;
		table.type_alignment = _mc_type_alignment;
		table.type_bits = _mc_type_bits;
		table.type_signed = _mc_type_signed;
		table.type_element = _mc_type_element;
		table.type_length = _mc_type_length;
		table.type_field_count = _mc_type_field_count;
		table.type_field_name = _mc_type_field_name;
		table.type_field_type = _mc_type_field_type;
		table.type_field_offset = _mc_type_field_offset;
		mc_vm_plugin_table(&table);
		mc_check_plugin_table(&table);
		ready = true;
	}
	return &table;
}
