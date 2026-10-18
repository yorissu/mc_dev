
/// the plugins: the registry of the functions they provide, loading
/// libraries, type handles and the function table of the plugin api.

#include "mia/checker.h"
#include "mia/plugins.h"
#include "mia/vm.h"
#include "mia/system.h"
#include "mia/string.h"

#include <string.h>

// --- registration ------------------------------------------------------------------------

typedef struct {
	mia_plugin_api_s is_public;   // first: the handle the plugin gets
	mia_plugins_s*   plugins  ;
	uint32_t         wanted   ;   // the api version the plugin asked for (0: none)
	char*            error    ;
} _mia_plugins_registration_s;

static bool _mia_plugins_compatible(mia_plugin_api_s* const api, const uint32_t version) {
	_mia_plugins_registration_s* const registration = (_mia_plugins_registration_s*)api;
	registration->wanted = version;
	return version >= 1 && version <= mia_plugin_api_version;
}

static bool _mia_plugins_register(mia_plugin_api_s* const api, const char* const plugin, const char* const name, const mia_plugin_run_f run,
								  const mia_plugin_check_f check) {
	_mia_plugins_registration_s* const registration = (_mia_plugins_registration_s*)api;
	mia_plugins_s* const plugins = registration->plugins;
	if (NULL != registration->error) return false;
	if (NULL == plugin || '\0' == plugin[0] || NULL == name || NULL == strchr(name, '.') || NULL == run) {
		registration->error = mia_string_format("it registers `%s` without a plugin name, a qualified name ('module.function') or a run function",
			(name != NULL) ? name : "?");
		return false;
	}
	const mia_plugins_function_s* const existing = mia_plugins_find(plugins, name);
	if (existing != NULL) {
		registration->error = mia_string_format("`%s` is provided by the plugins `%s` and `%s`", name, existing->plugin, plugin);
		return false;
	}
	const mia_plugins_function_s function = { .plugin = mia_strdup(plugin), .name = mia_strdup(name), .run = run, .check = check, };
	mia_vector_push(&plugins->functions, function);
	if (!mia_plugins_loaded(plugins, plugin)) mia_vector_push(&plugins->names, mia_strdup(plugin));
	return true;
}

/// runs an init function; false with 'error' set when it refuses or fails.
static bool _mia_plugins_initialize(mia_plugins_s* const plugins, const mia_plugin_init_f init, char** const error) {
	const size_t functions = plugins->functions.count, names = plugins->names.count;
	_mia_plugins_registration_s registration = { .is_public = { .interface = mia_plugins_table(), }, .plugins = plugins, };
	const bool ready = init(&registration.is_public);
	if (ready && NULL == registration.error && registration.wanted != 0) return true;
	// forget what it registered before it failed
	while (plugins->functions.count > functions) {
		mia_plugins_function_s* const function = &plugins->functions.data[--plugins->functions.count];
		mia_free(function->plugin);
		mia_free(function->name);
	}
	while (plugins->names.count > names) mia_free(plugins->names.data[--plugins->names.count]);
	if (registration.error != NULL) {
		*error = registration.error;
	}
	else if (registration.wanted > mia_plugin_api_version) {
		*error = mia_string_format("it needs plugin api version %u, but this mia has version %u", registration.wanted, mia_plugin_api_version);
	}
	else if (0 == registration.wanted) {
		*error = mia_strdup("its mia_plugin_init did not call mia_plugin_compatible");
	}
	else {
		*error = mia_strdup("its mia_plugin_init failed");
	}
	return false;
}

void mia_plugins_init(mia_plugins_s* const plugins) {
	*plugins = (mia_plugins_s) {0};
}

static bool _mia_plugins_load_library(mia_plugins_s* const plugins, const char* const path, char** const error) {
	char* reason = NULL;
	mia_system_library_s* const library = mia_system_library_open(path, &reason);
	if (NULL == library) {
		*error = mia_string_format("cannot load the plugin `%s`: %s", path, reason);
		mia_free(reason);
		return false;
	}
	void* const symbol = mia_system_library_symbol(library, mia_plugin_init_name);
	if (NULL == symbol) {
		mia_system_library_close(library);
		*error = mia_string_format("`%s` is not a mia plugin: it has no `%s` function", path, mia_plugin_init_name);
		return false;
	}
	mia_plugin_init_f init = NULL;
	memcpy(&init, &symbol, sizeof(init));
	if (!_mia_plugins_initialize(plugins, init, &reason)) {
		mia_system_library_close(library);
		*error = mia_string_format("the plugin `%s` is refused: %s", path, reason);
		mia_free(reason);
		return false;
	}
	mia_vector_push(&plugins->libraries, library);
	return true;
}

void mia_plugins_add_directory(mia_plugins_s* const plugins, const char* const directory) {
	mia_vector_push(&plugins->directories, mia_strdup(directory));
}

bool mia_plugins_require(mia_plugins_s* const plugins, const char* const plugin, char** const error) {
	*error = NULL;
	if (mia_plugins_loaded(plugins, plugin)) return true;
	// 'a.b' -> a/b.so, like the module a.b is a/b.mia
	mia_string_s file = {0};
	for (const char* character = plugin; *character != '\0'; ++character) mia_string_append_char(&file, ('.' == *character) ? '/' : *character);
	mia_string_append_cstr(&file, mia_system_library_extension());
	bool found = false;
	for (size_t i = 0; i < plugins->directories.count && !found; ++i) {
		char* const path = mia_string_format("%s/%s", plugins->directories.data[i], mia_string_cstr(&file));
		if (mia_system_file_exists(path)) {
			found = true;
			const size_t names = plugins->names.count;
			if (_mia_plugins_load_library(plugins, path, error) && !mia_plugins_loaded(plugins, plugin)) {
				*error = (plugins->names.count > names)
					? mia_string_format("`%s` is the plugin `%s`, not `%s`", path, plugins->names.data[names], plugin)
					: mia_string_format("`%s` is not the plugin `%s`: it registers no function", path, plugin);
			}
		}
		mia_free(path);
	}
	mia_string_free(&file);
	return found && NULL == *error;
}

bool mia_plugins_add(mia_plugins_s* const plugins, const mia_plugin_init_f init, char** const error) {
	return _mia_plugins_initialize(plugins, init, error);
}

const mia_plugins_function_s* mia_plugins_find(const mia_plugins_s* const plugins, const char* const name) {
	for (size_t i = 0; i < plugins->functions.count; ++i) if (0 == strcmp(plugins->functions.data[i].name, name)) return &plugins->functions.data[i];
	return NULL;
}

bool mia_plugins_loaded(const mia_plugins_s* const plugins, const char* const plugin) {
	for (size_t i = 0; i < plugins->names.count; ++i) if (0 == strcmp(plugins->names.data[i], plugin)) return true;
	return false;
}

void mia_plugins_free(mia_plugins_s* const plugins) {
	for (size_t i = 0; i < plugins->functions.count; ++i) {
		mia_free(plugins->functions.data[i].plugin);
		mia_free(plugins->functions.data[i].name);
	}
	mia_vector_free(&plugins->functions);
	for (size_t i = 0; i < plugins->names.count; ++i) mia_free(plugins->names.data[i]);
	mia_vector_free(&plugins->names);
	for (size_t i = 0; i < plugins->libraries.count; ++i) {
		mia_system_library_s* const library = plugins->libraries.data[i];
		void* const symbol = mia_system_library_symbol(library, mia_plugin_exit_name);
		if (symbol != NULL) {
			mia_plugin_exit_f exit = NULL;
			memcpy(&exit, &symbol, sizeof(exit));
			exit();
		}
		mia_system_library_close(library);
	}
	mia_vector_free(&plugins->libraries);
	for (size_t i = 0; i < plugins->directories.count; ++i) mia_free(plugins->directories.data[i]);
	mia_vector_free(&plugins->directories);
}

const mia_plugins_s* mia_plugins_default(void) {
	static mia_plugins_s plugins;
	static bool ready = false;
	if (!ready) {
		mia_plugins_init(&plugins);
		ready = true;
	}
	return &plugins;
}

// --- types -------------------------------------------------------------------------------

static mia_plugin_kind_e _mia_plugins_type_kind(const mia_plugin_type_s* const type) {
	return type->kind;
}

static const char* _mia_plugins_type_name(const mia_plugin_type_s* const type) {
	return type->name;
}

static uint64_t _mia_plugins_type_size(const mia_plugin_type_s* const type) {
	return type->size;
}

static uint64_t _mia_plugins_type_alignment(const mia_plugin_type_s* const type) {
	return type->alignment;
}

static uint32_t _mia_plugins_type_bits(const mia_plugin_type_s* const type) {
	return (type->code != mia_code_bytes) ? (uint32_t)(mia_code_size(type->code) * 8) : (uint32_t)(type->size * 8);
}

static bool _mia_plugins_type_signed(const mia_plugin_type_s* const type) {
	// floats count as signed for plugins
	return mia_code_is_signed(type->code) || mia_code_is_float(type->code);
}

static const mia_plugin_type_s* _mia_plugins_type_element(const mia_plugin_type_s* const type) {
	return type->element;
}

static uint64_t _mia_plugins_type_length(const mia_plugin_type_s* const type) {
	return type->length;
}

static size_t _mia_plugins_type_field_count(const mia_plugin_type_s* const type) {
	return type->field_count;
}

static const char* _mia_plugins_type_field_name(const mia_plugin_type_s* const type, const size_t index) {
	return (index < type->field_count) ? type->field_names[index] : NULL;
}

static const mia_plugin_type_s* _mia_plugins_type_field_type(const mia_plugin_type_s* const type, const size_t index) {
	return (index < type->field_count) ? type->field_types[index] : NULL;
}

static uint64_t _mia_plugins_type_field_offset(const mia_plugin_type_s* const type, const size_t index) {
	return (index < type->field_count) ? type->field_offsets[index] : 0;
}

mia_plugin_type_s* mia_plugins_types_new(const mia_bytecode_module_s* const module) {
	const size_t count = module->types.count;
	mia_plugin_type_s* const types = mia_calloc(count > 0 ? count : 1, sizeof(mia_plugin_type_s));
	for (size_t i = 0; i < count; ++i) {
		const mia_bytecode_type_s* const descriptor = &module->types.data[i];
		mia_plugin_type_s* const type = &types[i];
		static const mia_plugin_kind_e kinds[] = {
			[mia_bytecode_descriptor_int] = mia_plugin_kind_int, [mia_bytecode_descriptor_float] = mia_plugin_kind_float, [mia_bytecode_descriptor_bool] = mia_plugin_kind_bool,
			[mia_bytecode_descriptor_array] = mia_plugin_kind_array, [mia_bytecode_descriptor_slice] = mia_plugin_kind_slice, [mia_bytecode_descriptor_struct] = mia_plugin_kind_struct,
			[mia_bytecode_descriptor_function] = mia_plugin_kind_function,
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
			const mia_plugin_type_s** const fields = mia_calloc(type->field_count, sizeof(*fields));
			for (size_t f = 0; f < type->field_count; ++f) fields[f] = &types[descriptor->field_types.data[f]];
			type->field_types = fields;
			type->field_names = (const char* const*)descriptor->field_names.data;
			type->field_offsets = descriptor->field_offsets.data;
		}
	}
	return types;
}

void mia_plugins_types_free(mia_plugin_type_s* const types, const size_t count) {
	if (NULL == types) return;
	for (size_t i = 0; i < count; ++i) mia_free((void*)(uintptr_t)types[i].field_types);
	mia_free(types);
}

// --- the table ---------------------------------------------------------------------------

const mia_plugin_interface_s* mia_plugins_table(void) {
	static mia_plugin_interface_s table;
	static bool ready = false;
	if (!ready) {
		table.version = mia_plugin_api_version;
		table.size = (uint32_t)sizeof(table);
		table.compatible = _mia_plugins_compatible;
		table.register_function = _mia_plugins_register;
		table.type_kind = _mia_plugins_type_kind;
		table.type_name = _mia_plugins_type_name;
		table.type_size = _mia_plugins_type_size;
		table.type_alignment = _mia_plugins_type_alignment;
		table.type_bits = _mia_plugins_type_bits;
		table.type_signed = _mia_plugins_type_signed;
		table.type_element = _mia_plugins_type_element;
		table.type_length = _mia_plugins_type_length;
		table.type_field_count = _mia_plugins_type_field_count;
		table.type_field_name = _mia_plugins_type_field_name;
		table.type_field_type = _mia_plugins_type_field_type;
		table.type_field_offset = _mia_plugins_type_field_offset;
		mia_vm_plugin_table(&table);
		mia_checker_plugin_table(&table);
		ready = true;
	}
	return &table;
}
