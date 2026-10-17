
/// module loading. a module is one '.mc' file; 'import a.b' loads 'a/b.mc'
/// from the first search root that has it: the directory of the entry file,
/// then every '-I' directory in order, then the standard library. import
/// cycles are errors.

#include "check.h"
#include "mc/parser.h"
#include "mc/platform.h"

#include <stdio.h>
#include <string.h>

/// 'path' made absolute and normalized (like python's os.path.abspath),
/// with '/' between its parts on every system.
static char* _mc_absolute(const char* const path) {
	mc_string_s joined = {0};
	if (0 == mc_path_root(path)) {
		char* const directory = mc_current_directory();
		if (directory != NULL) mc_string_append_cstr(&joined, directory);
		mc_free(directory);
		mc_string_append_char(&joined, '/');
	}
	mc_string_append_cstr(&joined, path);
	const char* const text = mc_string_cstr(&joined);
	const size_t root = mc_path_root(text);
	// normalize: drop empty parts and '.', resolve '..'.
	mc_vector_of(mc_view_s) parts = {0};
	const char* cursor = text + root;
	while (*cursor != '\0') {
		while (mc_is_path_separator(*cursor)) ++cursor;
		const char* const start = cursor;
		while (*cursor != '\0' && !mc_is_path_separator(*cursor)) ++cursor;
		const mc_view_s part = mc_view_new(start, (size_t)(cursor - start));
		if (0 == part.length || mc_view_eq_cstr(part, ".")) continue;
		if (mc_view_eq_cstr(part, "..")) {
			if (parts.count > 0) parts.count -= 1;
			continue;
		}
		mc_vector_push(&parts, part);
	}
	mc_string_s result = {0};
	for (size_t i = 0; i < root; ++i) mc_string_append_char(&result, mc_is_path_separator(text[i]) ? '/' : text[i]);
	for (size_t i = 0; i < parts.count; ++i) {
		if (i > 0) mc_string_append_char(&result, '/');
		mc_string_append(&result, parts.data[i]);
	}
	mc_vector_free(&parts);
	mc_string_free(&joined);
	return mc_string_take(&result);
}

static char* _mc_directory(const char* const path) {
	const char* const slash = strrchr(path, '/');
	if (NULL == slash) return mc_strdup("");
	if (slash == path) return mc_strdup("/");
	return mc_format("%.*s", (int)(slash - path), path);
}

/// python's os.path.join(root, *parts) + '.mc'.
static char* _mc_module_path(const char* const root, const mc_views_s* const parts) {
	mc_string_s path = {0};
	mc_string_append_cstr(&path, root);
	for (size_t i = 0; i < parts->count; ++i) {
		if (path.length > 0 && '/' != path.data[path.length - 1]) mc_string_append_char(&path, '/');
		mc_string_append(&path, parts->data[i]);
	}
	mc_string_append_cstr(&path, ".mc");
	return mc_string_take(&path);
}

static char* _mc_dotted(const mc_views_s* const parts) {
	mc_string_s dotted = {0};
	for (size_t i = 0; i < parts->count; ++i) {
		if (i > 0) mc_string_append_char(&dotted, '.');
		mc_string_append(&dotted, parts->data[i]);
	}
	return mc_string_take(&dotted);
}

typedef struct {
	mc_checker_s*              checker;
	mc_map_s                   loaded ;   // absolute path -> module
	mc_vector_of(mc_module_s*) stack  ;
} _mc_loader_s;

static const char* _mc_keep(mc_checker_s* const c, char* const text) {
	const char* const kept = mc_arena_strndup(c->arena, text, strlen(text));
	mc_free(text);
	return kept;
}

static mc_module_s* _mc_load(_mc_loader_s* const loader, const char* const key, const mc_source_s* const source, const char* const name) {
	mc_checker_s* const c = loader->checker;
	mc_module_s* const module = mc_arena_new(c->arena, mc_module_s);
	module->base.kind = mc_symbol_module;
	module->name = mc_arena_strndup(c->arena, name, strlen(name));
	module->path = source->path;
	module->source = source;
	module->tree = mc_parse(source, c->arena, c->diag);
	mc_map_set(&loader->loaded, mc_view_cstr(key), module);
	mc_vector_push(&loader->stack, module);
	const mc_nodes_s* const imports = &module->tree->as.module.imports;
	for (size_t i = 0; i < imports->count; ++i) {
		mc_node_s* const node = imports->data[i];
		const mc_views_s* const parts = (mc_node_import == node->kind) ? &node->as.import.path : &node->as.from_import.path;
		char* const dotted = _mc_dotted(parts);
		const char* found = NULL;
		for (size_t r = 0; r < c->roots.count && NULL == found; ++r) {
			char* const candidate = _mc_module_path(c->roots.data[r], parts);
			if (mc_is_file(candidate)) found = _mc_keep(c, candidate);
			else mc_free(candidate);
		}
		if (NULL == found) {
			mc_string_s searched = {0};
			for (size_t r = 0; r < c->roots.count; ++r) {
				char* const candidate = _mc_module_path(c->roots.data[r], parts);
				mc_string_appendf(&searched, "%s%s", r > 0 ? ", " : "", candidate);
				mc_free(candidate);
			}
			mc_note(mc_label_main(mc_error(c->diag, mc_E0301, node->span, "module `%s` not found", dotted), "no such module"),
				"searched: %s", mc_string_cstr(&searched));
			mc_string_free(&searched);
			mc_free(dotted);
			continue;
		}
		char* const found_key = _mc_absolute(found);
		mc_module_s* target = mc_map_get_cstr(&loader->loaded, found_key);
		bool cycle = false;
		size_t at = 0;
		for (size_t s = 0; target != NULL && s < loader->stack.count; ++s) {
			if (loader->stack.data[s] == target) {
				cycle = true;
				at = s;
				break;
			}
		}
		if (cycle) {
			mc_string_s chain = {0};
			for (size_t s = at; s < loader->stack.count; ++s) mc_string_appendf(&chain, "%s%s", s > at ? " -> " : "", loader->stack.data[s]->name);
			mc_string_appendf(&chain, " -> %s", target->name);
			mc_diagnostic_s* const diagnostic = mc_error(c->diag, mc_E0302, node->span, "import cycle: `%s` imports `%s`", module->name, dotted);
			mc_label_main(diagnostic, "this import closes a cycle");
			mc_note(diagnostic, "cycle: %s", mc_string_cstr(&chain));
			mc_help(diagnostic, "import cycles are not allowed; move shared declarations into their own module");
			mc_string_free(&chain);
			mc_free(found_key);
			mc_free(dotted);
			continue;
		}
		if (NULL == target) {
			const mc_source_s* const loaded = mc_sources_load(c->sources, found);
			if (NULL == loaded) {
				mc_error(c->diag, mc_E0301, node->span, "cannot read module `%s`", dotted);
				mc_free(found_key);
				mc_free(dotted);
				continue;
			}
			const char* const kept_key = _mc_keep(c, found_key);
			target = _mc_load(loader, kept_key, loaded, dotted);
		}
		else {
			mc_free(found_key);
		}
		const mc_import_s import = { .node = node, .target = target, };
		mc_vector_push(&module->imports, import);
		mc_free(dotted);
	}
	(void)mc_vector_pop(&loader->stack);
	mc_vector_push(&c->program->modules, module);
	return module;
}

mc_module_s* mc_load(mc_checker_s* const c, const char* const path) {
	const mc_source_s* const source = mc_sources_load(c->sources, path);
	if (NULL == source) {
		mc_error(c->diag, mc_E0601, mc_no_span, "cannot read `%s`", path);
		return NULL;
	}
	char* const absolute = _mc_absolute(path);
	mc_vector_push(&c->roots, _mc_keep(c, _mc_directory(absolute)));
	for (size_t i = 0; i < c->options->include_count; ++i) mc_vector_push(&c->roots, c->options->include[i]);
	if (c->options->stdlib != NULL) mc_vector_push(&c->roots, c->options->stdlib);
	_mc_loader_s loader = { .checker = c, };
	mc_module_s* const entry = _mc_load(&loader, _mc_keep(c, absolute), source, "main");
	mc_map_free(&loader.loaded);
	mc_vector_free(&loader.stack);
	return entry;
}
