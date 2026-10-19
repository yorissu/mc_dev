
/// the checker: a parsed program -> a checked program (see checker.h for
/// how it works). its parts, in this order:
///
///   programs, modules, lookup, declarations, attributes, instances
///   loading modules
///   types, struct layouts, sizes
///   statements, places, flow
///   expressions, calls, operators, literals
///   calls of plugin functions: their checks (the compile-time plugin api)

#include "mia/checker.h"
#include "mia/format.h"
#include "mia/parser.h"
#include "mia/system.h"
#include "mia/printer.h"
#include "mia/plugins.h"

#include <string.h>
#include <stdio.h>

// ========================================================================================
// programs, modules, lookup, declarations, attributes, instances
// ========================================================================================

/// the checker: programs, modules, imports, name lookup, declarations,
/// attributes, constants and generic instances.

#define _mia_checker_instantiation_limit 64

static const char* const _mia_checker_known_attributes = "plugin, uninit, wrap";

static const struct {
	const char* where;
	const char* allowed;
} _mia_checker_allowed_attributes[] = {
	{ "function",  " plugin wrap " },
	{ "struct",    " " },
	{ "constant",  " wrap " },
	{ "field",     " " },
	{ "let",       " uninit wrap " },
	{ "statement", " wrap " },
	{ "block",     " wrap " },
	{ "`if`",      " wrap " },
	{ "`while` loop", " wrap " },
	{ "`for` loop", " wrap " },
	{ "`return`",  " wrap " },
	{ "`break`",   " " },
	{ "`continue`", " " },
};

// --- small helpers -----------------------------------------------------------

const char* mia_checker_view_cstring(mia_checker_s* const c, const mia_view_s view) {
	return mia_arena_strndup(c->arena, view.data, view.length);
}

const char* mia_checker_name_of(mia_checker_s* const c, const mia_type_s* const type) {
	mia_string_s text = {0};
	mia_type_text(&text, type);
	const char* const kept = mia_arena_strndup(c->arena, mia_string_cstr(&text), text.length);
	mia_string_free(&text);
	return kept;
}

void mia_program_instance_text(mia_string_s* const out, const mia_program_instance_s* const instance) {
	mia_string_append(out, instance->function->node->as.function.name);
	if (0 == instance->argument_count) return;
	mia_string_append_char(out, '<');
	for (size_t i = 0; i < instance->argument_count; ++i) {
		if (i > 0) mia_string_append_cstr(out, ", ");
		mia_type_text(out, instance->arguments[i]);
	}
	mia_string_append_char(out, '>');
}

const char* mia_checker_instance_name(mia_checker_s* const c, const mia_program_instance_s* const instance) {
	mia_string_s text = {0};
	mia_program_instance_text(&text, instance);
	const char* const kept = mia_arena_strndup(c->arena, mia_string_cstr(&text), text.length);
	mia_string_free(&text);
	return kept;
}

mia_view_s mia_checker_node_text(mia_checker_s* const c, const mia_ast_node_s* const node) {
	return mia_span_text(c->sources, node->span);
}

bool mia_checker_function_is_generic(const mia_program_function_s* const info) {
	return info->node->as.function.generics.count > 0;
}

bool mia_program_function_is_plugin(const mia_program_function_s* const function) {
	return mia_ast_attribute_find(&function->node->as.function.attributes, "plugin") != NULL;
}

char* mia_program_function_plugin(const mia_program_function_s* const function) {
	const mia_ast_node_s* const attribute = mia_ast_attribute_find(&function->node->as.function.attributes, "plugin");
	if (NULL == attribute) return NULL;
	const mia_ast_attribute_value_e value = attribute->as.attribute.value_kind;
	if (value != mia_ast_attribute_value_string && value != mia_ast_attribute_value_name) return NULL;
	return mia_string_format(mia_view_fmt, mia_view_arg(attribute->as.attribute.text));
}

char* mia_program_function_qualified_name(const mia_program_function_s* const function) {
	return mia_string_format("%s." mia_view_fmt, function->module->name, mia_view_arg(function->node->as.function.name));
}

static bool _mia_checker_symbol_public(const mia_program_symbol_s* const symbol) {
	switch (symbol->kind) {
		case mia_program_symbol_function: return ((const mia_program_function_s*)symbol)->node->as.function.is_public;
		case mia_program_symbol_struct:   return ((const mia_program_struct_s*)symbol)->node->as.structure.is_public;
		case mia_program_symbol_constant: return ((const mia_program_constant_s*)symbol)->node->as.constant.is_public;
		default:                 return false;
	}
}

static const mia_ast_node_s* _mia_checker_symbol_node(const mia_program_symbol_s* const symbol) {
	switch (symbol->kind) {
		case mia_program_symbol_function: return ((const mia_program_function_s*)symbol)->node;
		case mia_program_symbol_struct:   return ((const mia_program_struct_s*)symbol)->node;
		case mia_program_symbol_constant: return ((const mia_program_constant_s*)symbol)->node;
		default:                 return NULL;
	}
}

static mia_program_module_s* _mia_checker_symbol_module(const mia_program_symbol_s* const symbol) {
	switch (symbol->kind) {
		case mia_program_symbol_function: return ((const mia_program_function_s*)symbol)->module;
		case mia_program_symbol_struct:   return ((const mia_program_struct_s*)symbol)->module;
		case mia_program_symbol_constant: return ((const mia_program_constant_s*)symbol)->module;
		default:                 return NULL;
	}
}

// --- variable sets -------------------------------------------------------------

bool mia_checker_set_contains(const mia_checker_variable_set_s* const set, const mia_program_variable_s* const variable) {
	for (size_t i = 0; i < set->count; ++i) if (set->data[i] == variable) return true;
	return false;
}

void mia_checker_set_add(mia_checker_variable_set_s* const set, mia_program_variable_s* const variable) {
	if (!mia_checker_set_contains(set, variable)) mia_vector_push(set, variable);
}

void mia_checker_set_discard(mia_checker_variable_set_s* const set, const mia_program_variable_s* const variable) {
	for (size_t i = 0; i < set->count; ++i) {
		if (set->data[i] == variable) {
			set->data[i] = set->data[set->count - 1];
			set->count -= 1;
			return;
		}
	}
}

void mia_checker_set_assign(mia_checker_variable_set_s* const set, const mia_checker_variable_set_s* const other) {
	set->count = 0;
	for (size_t i = 0; i < other->count; ++i) mia_vector_push(set, other->data[i]);
}

void mia_checker_set_union(mia_checker_variable_set_s* const set, const mia_checker_variable_set_s* const other) {
	for (size_t i = 0; i < other->count; ++i) mia_checker_set_add(set, other->data[i]);
}

// --- contexts --------------------------------------------------------------------

mia_checker_context_s* mia_checker_context_new(mia_checker_s* const c, mia_program_module_s* const module, const mia_type_bindings_s* const bindings,
											   const mia_type_s* const result, mia_checker_context_s* const parent, mia_program_instance_s* const instance,
											   const bool function) {
	mia_checker_context_s* const context = mia_calloc(1, sizeof(*context));
	context->module = module;
	if (bindings != NULL) for (size_t i = 0; i < bindings->count; ++i) mia_vector_push(&context->bindings, bindings->data[i]);
	context->result = result;
	context->parent = parent;
	context->instance = instance;
	context->function = function;
	if (function) mia_checker_context_push_scope(c, context);
	context->wrap = (parent != NULL) ? parent->wrap : false;
	context->level = (parent != NULL) ? parent->level + 1 : 0;
	context->depth = (instance != NULL) ? instance->depth : (parent != NULL) ? parent->depth : 0;
	mia_vector_push(&c->contexts, context);
	return context;
}

static void _mia_checker_context_free(mia_checker_context_s* const context) {
	while (context->scopes.count > 0) mia_checker_context_pop_scope(context);
	mia_vector_free(&context->scopes);
	mia_vector_free(&context->bindings);
	mia_vector_free(&context->unassigned);
	mia_vector_free(&context->variables);
	mia_free(context);
}

void mia_checker_context_push_scope(mia_checker_s* const c, mia_checker_context_s* const context) {
	mia_unused(c);
	mia_map_s* const scope = mia_calloc(1, sizeof(*scope));
	mia_vector_push(&context->scopes, scope);
}

void mia_checker_context_pop_scope(mia_checker_context_s* const context) {
	mia_map_s* const scope = mia_vector_pop(&context->scopes);
	mia_map_free(scope);
	mia_free(scope);
}

mia_program_symbol_s* mia_checker_context_declare(mia_checker_context_s* const context, const mia_view_s name, mia_program_symbol_s* const symbol) {
	mia_map_s* const scope = mia_vector_last(&context->scopes);
	mia_program_symbol_s* const previous = mia_map_get(scope, name);
	mia_map_set(scope, name, symbol);
	return previous;
}

// --- lookup ----------------------------------------------------------------------

static bool _mia_checker_qualifier_prefix(const mia_program_module_s* const module, const char* const prefix) {
	const size_t length = strlen(prefix);
	mia_map_each(&module->qualifiers, entry) {
		if (entry->key.length > length && 0 == memcmp(entry->key.data, prefix, length) && '.' == entry->key.data[length]) return true;
	}
	return false;
}

static mia_program_symbol_s* _mia_checker_prefix_new(mia_checker_s* const c, mia_program_module_s* const module, const char* const prefix) {
	mia_program_prefix_s* const symbol = mia_arena_new(c->arena, mia_program_prefix_s);
	symbol->base.kind = mia_program_symbol_prefix;
	symbol->module = module;
	symbol->prefix = prefix;
	return &symbol->base;
}

mia_program_symbol_s* mia_checker_lookup(mia_checker_s* const c, const mia_view_s name, mia_checker_context_s* const context, const mia_span_s span, const bool report) {
	for (mia_checker_context_s* current = context; current != NULL; current = current->parent) {
		for (size_t i = current->scopes.count; i > 0; --i) {
			mia_program_symbol_s* const symbol = mia_map_get(current->scopes.data[i - 1], name);
			if (NULL == symbol) continue;
			if (mia_program_symbol_variable == symbol->kind && current != context) {
				if (report) {
					mia_diag_help(mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0516, span, "`" mia_view_fmt "` is a variable of the enclosing function", mia_view_arg(name)),
						"lambdas and local functions cannot capture variables"), "pass it as an argument instead");
				}
				return NULL;
			}
			return symbol;
		}
	}
	mia_program_module_s* const module = context->module;
	mia_program_symbol_s* symbol = mia_map_get(&module->symbols, name);
	if (NULL == symbol) symbol = mia_map_get(&module->imported, name);
	if (symbol != NULL) return symbol;
	mia_vector_of(mia_program_symbol_s*) found = {0};
	for (size_t i = 0; i < module->star.count; ++i) {
		mia_program_symbol_s* const candidate = mia_map_get(&module->star.data[i]->symbols, name);
		if (NULL == candidate || !_mia_checker_symbol_public(candidate)) continue;
		bool seen = false;
		for (size_t j = 0; j < found.count; ++j) seen = seen || found.data[j] == candidate;
		if (!seen) mia_vector_push(&found, candidate);
	}
	if (1 == found.count) {
		symbol = found.data[0];
		mia_vector_free(&found);
		return symbol;
	}
	if (found.count > 1) {
		if (report) {
			mia_diag_entry_s* const diagnostic = mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0304, span, "`" mia_view_fmt "` is ambiguous", mia_view_arg(name)),
				"imported by several `*` imports");
			for (size_t i = 0; i < found.count; ++i) {
				const mia_program_module_s* const origin = _mia_checker_symbol_module(found.data[i]);
				mia_diag_note(diagnostic, "`" mia_view_fmt "` from `%s`", mia_view_arg(name), (origin != NULL) ? origin->name : "?");
			}
		}
		mia_vector_free(&found);
		return NULL;
	}
	mia_vector_free(&found);
	mia_program_module_s* const qualified = mia_map_get(&module->qualifiers, name);
	if (qualified != NULL) return &qualified->base;
	const char* const text = mia_checker_view_cstring(c, name);
	if (_mia_checker_qualifier_prefix(module, text)) return _mia_checker_prefix_new(c, module, text);
	// a module the parser cut short (E0207) may lack the declaration: that is reported already
	const bool incomplete = context->module->tree->as.module.incomplete;
	if (report && !incomplete) mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0501, span, "cannot find `" mia_view_fmt "`", mia_view_arg(name)), "not declared");
	return NULL;
}

mia_program_symbol_s* mia_checker_member_of(mia_checker_s* const c, mia_program_symbol_s* const base, const mia_view_s name, const mia_span_s span, mia_checker_context_s* const context) {
	if (mia_program_symbol_prefix == base->kind) {
		const mia_program_prefix_s* const prefix = (const mia_program_prefix_s*)base;
		char* const joined = mia_string_format("%s." mia_view_fmt, prefix->prefix, mia_view_arg(name));
		const char* const dotted = mia_arena_strndup(c->arena, joined, strlen(joined));
		mia_free(joined);
		mia_program_module_s* const module = mia_map_get_cstr(&prefix->module->qualifiers, dotted);
		if (module != NULL) return &module->base;
		if (_mia_checker_qualifier_prefix(prefix->module, dotted)) return _mia_checker_prefix_new(c, prefix->module, dotted);
		mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0301, span, "module `%s` is not imported", dotted), "no such module");
		return NULL;
	}
	mia_program_module_s* const module = (mia_program_module_s*)base;
	mia_program_symbol_s* const symbol = mia_map_get(&module->symbols, name);
	if (NULL == symbol) {
		if (!module->tree->as.module.incomplete) {
			mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0305, span, "module `%s` has no `" mia_view_fmt "`", module->name, mia_view_arg(name)), "not declared there");
		}
		return NULL;
	}
	if (!_mia_checker_symbol_public(symbol) && module != context->module) {
		mia_diag_label(mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0303, span, "`" mia_view_fmt "` is not public in `%s`", mia_view_arg(name), module->name), "not `pub`"),
			mia_ast_declaration_name_span(_mia_checker_symbol_node(symbol)), "declared here without `pub`");
		return NULL;
	}
	return symbol;
}

mia_program_symbol_s* mia_checker_qualified(mia_checker_s* const c, const mia_ast_node_s* const node, mia_checker_context_s* const context, const bool report) {
	if (mia_ast_node_name == node->kind) return mia_checker_lookup(c, node->as.name.name, context, node->span, report);
	if (mia_ast_node_member == node->kind) {
		const mia_ast_node_s* const target = node->as.member.target;
		mia_program_symbol_s* const base = (mia_ast_node_name == target->kind || mia_ast_node_member == target->kind) ? mia_checker_qualified(c, target, context, false) : NULL;
		if (base != NULL && (mia_program_symbol_module == base->kind || mia_program_symbol_prefix == base->kind)) {
			return mia_checker_member_of(c, base, node->as.member.name, node->as.member.name_span, context);
		}
	}
	return NULL;
}

// --- program -----------------------------------------------------------------------

static void _mia_checker_collect(mia_checker_s* const c, mia_program_module_s* const module) {
	const mia_ast_nodes_s* const declarations = &module->tree->as.module.declarations;
	for (size_t i = 0; i < declarations->count; ++i) {
		mia_ast_node_s* const node = declarations->data[i];
		mia_program_symbol_s* symbol = NULL;
		if (mia_ast_node_function == node->kind) {
			mia_program_function_s* const info = mia_arena_new(c->arena, mia_program_function_s);
			info->base.kind = mia_program_symbol_function;
			info->node = node;
			info->module = module;
			symbol = &info->base;
		}
		else if (mia_ast_node_struct == node->kind) {
			mia_program_struct_s* const info = mia_arena_new(c->arena, mia_program_struct_s);
			info->base.kind = mia_program_symbol_struct;
			info->node = node;
			info->module = module;
			symbol = &info->base;
		}
		else {
			mia_program_constant_s* const info = mia_arena_new(c->arena, mia_program_constant_s);
			info->base.kind = mia_program_symbol_constant;
			info->node = node;
			info->module = module;
			symbol = &info->base;
		}
		const mia_view_s name = mia_ast_declaration_name(node);
		mia_program_symbol_s* const previous = mia_map_get(&module->symbols, name);
		if (previous != NULL) {
			mia_diag_label(mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0401, mia_ast_declaration_name_span(node), "`" mia_view_fmt "` is declared twice", mia_view_arg(name)),
				"declared again here"), mia_ast_declaration_name_span(_mia_checker_symbol_node(previous)), "first declared here");
			continue;
		}
		mia_map_set(&module->symbols, name, symbol);
		mia_vector_push(&module->ordered, symbol);
	}
}

static char* _mia_checker_join_path(const mia_view_list_s* const parts) {
	mia_string_s dotted = {0};
	for (size_t i = 0; i < parts->count; ++i) {
		if (i > 0) mia_string_append_char(&dotted, '.');
		mia_string_append(&dotted, parts->data[i]);
	}
	return mia_string_take(&dotted);
}

static void _mia_checker_resolve_imports(mia_checker_s* const c, mia_program_module_s* const module) {
	for (size_t i = 0; i < module->imports.count; ++i) {
		mia_ast_node_s* const node = module->imports.data[i].node;
		mia_program_module_s* const target = module->imports.data[i].target;
		if (mia_ast_node_import == node->kind) {
			char* const dotted = _mia_checker_join_path(&node->as.import.path);
			const char* const alias = node->as.import.has_alias ? mia_checker_view_cstring(c, node->as.import.alias) : mia_arena_strndup(c->arena, dotted, strlen(dotted));
			mia_free(dotted);
			mia_program_module_s* const previous = mia_map_get_cstr(&module->qualifiers, alias);
			if (previous != NULL && previous != target) {
				mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0306, node->span, "`%s` is imported twice", alias), "already imported");
			}
			mia_map_set(&module->qualifiers, mia_view_cstr(alias), target);
		}
		else if (node->as.from_import.star) {
			mia_vector_push(&module->star, target);
		}
		else {
			const mia_ast_nodes_s* const names = &node->as.from_import.names;
			for (size_t n = 0; n < names->count; ++n) {
				const mia_ast_node_s* const item = names->data[n];
				const mia_view_s name = item->as.import_name.name;
				mia_program_symbol_s* const symbol = mia_map_get(&target->symbols, name);
				if (NULL == symbol) {
					mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0305, item->span, "module `%s` has no `" mia_view_fmt "`", target->name, mia_view_arg(name)),
						"not declared there");
					continue;
				}
				if (!_mia_checker_symbol_public(symbol)) {
					mia_diag_label(mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0303, item->span, "`" mia_view_fmt "` is not public in `%s`", mia_view_arg(name), target->name),
						"not `pub`"), mia_ast_declaration_name_span(_mia_checker_symbol_node(symbol)), "declared here without `pub`");
					continue;
				}
				const mia_view_s alias = item->as.import_name.has_alias ? item->as.import_name.alias : name;
				mia_program_symbol_s* const previous = mia_map_get(&module->imported, alias);
				if (previous != NULL && previous != symbol) {
					mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0306, item->span, "`" mia_view_fmt "` is imported twice", mia_view_arg(alias)), "already imported");
					continue;
				}
				mia_map_set(&module->imported, alias, symbol);
			}
		}
	}
}

// --- attributes and declarations -----------------------------------------------------

static bool _mia_checker_word_in(const char* const list, const mia_view_s word) {
	char* const padded = mia_string_format(" " mia_view_fmt " ", mia_view_arg(word));
	const bool found = strstr(list, padded) != NULL;
	mia_free(padded);
	return found;
}

mia_checker_attribute_set_s mia_checker_attributes(mia_checker_s* const c, const mia_ast_nodes_s* const attributes, const char* const where) {
	mia_checker_attribute_set_s found = {0};
	const char* allowed = " ";
	for (size_t i = 0; i < mia_count_of(_mia_checker_allowed_attributes); ++i) {
		if (0 == strcmp(_mia_checker_allowed_attributes[i].where, where)) allowed = _mia_checker_allowed_attributes[i].allowed;
	}
	for (size_t i = 0; i < attributes->count; ++i) {
		const mia_ast_node_s* const attribute = attributes->data[i];
		const mia_view_s name = attribute->as.attribute.name;
		const mia_ast_attribute_value_e value = attribute->as.attribute.value_kind;
		if (!_mia_checker_word_in(" plugin wrap uninit ", name)) {
			mia_diag_note(mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0402, attribute->span, "unknown attribute `" mia_view_fmt "`", mia_view_arg(name)),
				"not an attribute"), "attributes: %s", _mia_checker_known_attributes);
			continue;
		}
		if (!_mia_checker_word_in(allowed, name)) {
			mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0403, attribute->span, "`" mia_view_fmt "` cannot be used on a %s", mia_view_arg(name), where),
				"not applicable here");
			continue;
		}
		const mia_ast_node_s** const slot = mia_view_eq_cstr(name, "plugin") ? &found.plugin : mia_view_eq_cstr(name, "wrap") ? &found.wrap : &found.uninit;
		if (*slot != NULL) {
			mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0403, attribute->span, "attribute `" mia_view_fmt "` is given twice", mia_view_arg(name)), "repeated");
			continue;
		}
		if (mia_view_eq_cstr(name, "plugin") && value != mia_ast_attribute_value_string && value != mia_ast_attribute_value_name) {
			// still a plugin function (no error for its missing body): only the name is wrong
			mia_diag_help(mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0403, attribute->span, "`plugin` needs the name of the plugin that provides the function"),
				"no plugin named"), "write `[plugin = \"name\"]`: the library name.so (.dylib, .dll) in a plugin directory");
		}
		if ((mia_view_eq_cstr(name, "wrap") || mia_view_eq_cstr(name, "uninit")) && value != mia_ast_attribute_value_none) {
			mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0403, attribute->span, "`" mia_view_fmt "` takes no value", mia_view_arg(name)), "remove the value");
			continue;
		}
		*slot = attribute;
	}
	return found;
}

static void _mia_checker_duplicate_generics(mia_checker_s* const c, const mia_ast_nodes_s* const generics) {
	for (size_t i = 0; i < generics->count; ++i) {
		const mia_ast_node_s* const generic = generics->data[i];
		bool seen = false;
		for (size_t j = 0; j < i; ++j) seen = seen || mia_view_eq(generics->data[j]->as.generic.name, generic->as.generic.name);
		const bool scalar = mia_scalar_from_name(generic->as.generic.name) != mia_scalar_none;
		if (seen || scalar) {
			mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0401, generic->span, seen ? "type parameter `" mia_view_fmt "` is declared twice"
				: "type parameter `" mia_view_fmt "` hides a built-in type", mia_view_arg(generic->as.generic.name)), "rename it");
		}
	}
}

void mia_checker_function_declaration(mia_checker_s* const c, mia_program_function_s* const info, mia_checker_context_s* const context) {
	mia_unused(context);
	mia_ast_node_s* const node = info->node;
	_mia_checker_duplicate_generics(c, &node->as.function.generics);
	const mia_checker_attribute_set_s found = mia_checker_attributes(c, &node->as.function.attributes, "function");
	const bool bodyless = found.plugin != NULL;
	if (bodyless && node->as.function.body != NULL) {
		mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0405, node->as.function.body->span, "`" mia_view_fmt "` is provided by a plugin and cannot have a body",
			mia_view_arg(node->as.function.name)), "remove the body");
	}
	if (!bodyless && NULL == node->as.function.body) {
		mia_diag_help(mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0404, node->as.function.name_span, "`" mia_view_fmt "` has no body", mia_view_arg(node->as.function.name)),
			"expected `{ ... }`"), "only `[plugin]` functions, which a plugin provides, are declared without a body");
	}
	const mia_ast_node_s* seen_default = NULL;
	const mia_ast_nodes_s* const parameters = &node->as.function.parameters;
	for (size_t i = 0; i < parameters->count; ++i) {
		const mia_ast_node_s* const parameter = parameters->data[i];
		bool duplicate = false;
		for (size_t j = 0; j < i; ++j) duplicate = duplicate || mia_view_eq(parameters->data[j]->as.param.name, parameter->as.param.name);
		if (duplicate) {
			mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0401, parameter->span, "parameter `" mia_view_fmt "` is declared twice", mia_view_arg(parameter->as.param.name)),
				"declared again");
		}
		if (parameter->as.param.default_value != NULL) {
			seen_default = parameter;
		}
		else if (seen_default != NULL) {
			mia_diag_label(mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0412, parameter->span, "parameters without defaults cannot follow parameters with defaults"),
				"needs a default"), seen_default->span, "has a default");
		}
	}
	for (size_t i = 0; i < parameters->count; ++i) {
		const mia_ast_node_s* const parameter = parameters->data[i];
		if (!parameter->as.param.variadic) continue;
		if (!bodyless) {
			mia_diag_help(mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0415, parameter->span, "only `[plugin]` functions can take `...`"),
				"variadic parameter"), "take a slice instead: `values: []T`");
		}
		else if (i != parameters->count - 1) {
			mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0415, parameter->span, "`...` must be the last parameter"), "not last");
		}
		else if (seen_default != NULL) {
			mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0415, parameter->span, "a variadic function cannot have default parameters"), "variadic");
		}
	}
}

static void _mia_checker_declarations(mia_checker_s* const c, mia_program_module_s* const module) {
	mia_checker_context_s* const context = mia_checker_context_new(c, module, NULL, NULL, NULL, NULL, false);
	for (size_t i = 0; i < module->ordered.count; ++i) {
		mia_program_symbol_s* const symbol = module->ordered.data[i];
		if (mia_program_symbol_struct == symbol->kind) {
			mia_program_struct_s* const info = (mia_program_struct_s*)symbol;
			(void)mia_checker_attributes(c, &info->node->as.structure.attributes, "struct");
			_mia_checker_duplicate_generics(c, &info->node->as.structure.generics);
		}
		else if (mia_program_symbol_function == symbol->kind) {
			mia_checker_function_declaration(c, (mia_program_function_s*)symbol, context);
		}
	}
}

static void _mia_checker_check_main(mia_checker_s* const c, mia_program_module_s* const entry) {
	mia_program_symbol_s* const symbol = mia_map_get_cstr(&entry->symbols, "main");
	if ((NULL == symbol || symbol->kind != mia_program_symbol_function) && c->options->library) return;
	// 'main' may be what the parser left out (nested too deep, reported already)
	if ((NULL == symbol || symbol->kind != mia_program_symbol_function) && entry->tree->as.module.incomplete) return;
	if (NULL == symbol || symbol->kind != mia_program_symbol_function) {
		mia_diag_help(mia_diag_error(c->diag, mia_diag_E0411, mia_span_none, "the entry module `%s` has no `main` function", entry->path),
			"declare `pub main :: () { ... }`");
		return;
	}
	mia_program_function_s* const info = (mia_program_function_s*)symbol;
	const mia_ast_node_s* const node = info->node;
	if (!node->as.function.is_public) {
		mia_diag_note(mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0411, node->as.function.name_span, "`main` must be `pub`"), "add `pub`"),
			"`main` is called from outside the program");
	}
	if (mia_checker_function_is_generic(info)) {
		mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0411, node->as.function.name_span, "`main` takes no type parameters"), "remove them");
	}
	mia_program_instance_s* instance = NULL;
	for (size_t i = 0; i < info->instances.count; ++i) if (0 == info->instances.data[i]->argument_count) instance = info->instances.data[i];
	// no parameters, or the command line as a [][]u8
	const mia_ast_nodes_s* const parameters = &node->as.function.parameters;
	const mia_type_s* const arguments = mia_type_slice_of(c->types, mia_type_slice_of(c->types, mia_checker_scalar_type(c, mia_scalar_u8)));
	const bool valid = 0 == parameters->count || (1 == parameters->count && NULL == parameters->data[0]->as.param.default_value
		&& (NULL == instance || (1 == instance->parameters.count && instance->parameters.data[0] == arguments)));
	if (!valid) {
		mia_diag_help(mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0411, parameters->data[0]->span, "`main` takes nothing, or the command line"),
			"not a valid parameter list for `main`"),
			"write `pub main :: ()`, `pub main :: () -> int`, `pub main :: (arguments: [][]u8)` or `pub main :: (arguments: [][]u8) -> int`");
	}
	if (instance != NULL && instance->result != NULL && instance->result != mia_checker_scalar_type(c, mia_scalar_int)) {
		mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0411, node->as.function.result->span, "`main` returns nothing or `int`"),
			"returns `%s`", mia_checker_name_of(c, instance->result));
	}
}

// --- constants ---------------------------------------------------------------------

const mia_type_s* mia_checker_constant_type(mia_checker_s* const c, mia_program_constant_s* const info) {
	if (mia_program_constant_done == info->state) return info->type;
	if (mia_program_constant_checking == info->state) {
		mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0702, info->node->as.constant.name_span, "constant `" mia_view_fmt "` depends on itself",
			mia_view_arg(info->node->as.constant.name)), "its value needs its own value");
		info->state = mia_program_constant_done;
		info->type = mia_checker_special_type(c, mia_type_special_error);
		return info->type;
	}
	if (c->constant_depth >= mia_ast_depth_limit) {
		// constants that refer to each other recurse here: too long a chain would overflow the stack
		// (reported once: the rest of a long chain fails the same way)
		if (!c->constant_depth_reported) mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0705, info->node->as.constant.name_span, "constants refer to each other too deeply"),
			"more than %d constants deep", mia_ast_depth_limit);
		c->constant_depth_reported = true;
		info->state = mia_program_constant_done;
		info->type = mia_checker_special_type(c, mia_type_special_error);
		return info->type;
	}
	c->constant_depth += 1;
	info->state = mia_program_constant_checking;
	mia_checker_context_s* const context = (info->context != NULL) ? info->context : mia_checker_context_new(c, info->module, NULL, NULL, NULL, NULL, false);
	const mia_checker_attribute_set_s found = mia_checker_attributes(c, &info->node->as.constant.attributes, "constant");
	if (found.wrap != NULL) context->wrapping += 1;
	mia_ast_node_s* const value_node = info->node->as.constant.value;
	const mia_type_s* type = mia_checker_expression(c, value_node, context, NULL);
	const mia_type_s* const error = mia_checker_special_type(c, mia_type_special_error);
	if (mia_type_is_special(type, mia_type_special_nothing)) {
		mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0523, value_node->span, "a constant needs a value"), "this has no value");
		type = error;
	}
	mia_evaluator_value_s value = {0};
	bool has_value = false;
	if (type != error) {
		char* const what = mia_string_format("constant `" mia_view_fmt "`", mia_view_arg(info->node->as.constant.name));
		has_value = mia_checker_require_constant(c, value_node, context, what, &value);
		mia_free(what);
	}
	if (found.wrap != NULL) context->wrapping -= 1;
	c->constant_depth -= 1;
	info->type = has_value ? type : error;
	if (has_value) {
		info->value = mia_arena_new(c->arena, mia_evaluator_value_s);
		*info->value = value;
	}
	info->state = mia_program_constant_done;
	if (NULL == info->context) {
		bool listed = false;
		for (size_t i = 0; i < c->program->constants.count; ++i) listed = listed || c->program->constants.data[i] == info;
		if (!listed) mia_vector_push(&c->program->constants, info);
	}
	return info->type;
}

bool mia_checker_require_constant(mia_checker_s* const c, const mia_ast_node_s* const node, mia_checker_context_s* const context, const char* const what, mia_evaluator_value_s* const value) {
	mia_unused(context);
	const mia_evaluator_result_s result = mia_evaluator_evaluate(&c->evaluator, node, false);
	if (mia_evaluator_ok == result.status) {
		*value = result.value;
		return true;
	}
	if (mia_evaluator_not_constant == result.status) {
		mia_diag_entry_s* const diagnostic = mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0701, node->span, "%s must be known at compile time", what),
			"evaluated at compile time");
		if (result.node != NULL && result.node != node) mia_diag_label(diagnostic, result.node->span, "%s", result.reason);
		else mia_diag_note(diagnostic, "%s", result.reason);
	}
	return false;
}

bool mia_checker_try_value(mia_checker_s* const c, const mia_ast_node_s* const node, mia_evaluator_value_s* const value) {
	const mia_evaluator_result_s result = mia_evaluator_evaluate(&c->evaluator, node, true);
	if (mia_evaluator_ok != result.status) return false;
	*value = result.value;
	return true;
}

// --- functions and instances -----------------------------------------------------------

/// the parameter and result types of a function declaration in 'context'.
static void _mia_checker_resolve_signature(mia_checker_s* const c, const mia_ast_node_s* const node, mia_checker_context_s* const context,
										   mia_type_list_s* const parameters, const mia_type_s** const result) {
	const mia_ast_nodes_s* const nodes = &node->as.function.parameters;
	for (size_t i = 0; i < nodes->count; ++i) {
		const mia_ast_node_s* const parameter = nodes->data[i];
		if (parameter->as.param.variadic) {
			mia_vector_push(parameters, mia_checker_special_type(c, mia_type_special_variadic));
			continue;
		}
		mia_vector_push(parameters, mia_checker_resolve_type(c, parameter->as.param.type, context, true, false));
	}
	*result = (node->as.function.result != NULL) ? mia_checker_resolve_type(c, node->as.function.result, context, false, false) : NULL;
}

mia_program_instance_s* mia_checker_instance(mia_checker_s* const c, mia_program_function_s* const info, const mia_type_s* const* const arguments, const size_t count,
											 mia_checker_context_s* const context, const mia_span_s span) {
	for (size_t i = 0; i < info->instances.count; ++i) {
		mia_program_instance_s* const existing = info->instances.data[i];
		if (existing->argument_count != count) continue;
		bool same = true;
		for (size_t a = 0; a < count; ++a) same = same && existing->arguments[a] == arguments[a];
		if (same) return existing;
	}
	const bool generic = mia_checker_function_is_generic(info);
	const int depth = (context != NULL && generic) ? context->depth + 1 : 0;
	if (depth > _mia_checker_instantiation_limit) {
		mia_diag_note(mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0527, span, "too many nested instantiations of `" mia_view_fmt "`", mia_view_arg(info->node->as.function.name)),
			"more than %d levels deep", _mia_checker_instantiation_limit), "a generic function probably instantiates itself with ever larger types");
		return NULL;
	}
	mia_program_instance_s* const instance = mia_arena_new(c->arena, mia_program_instance_s);
	instance->function = info;
	instance->argument_count = count;
	if (count > 0) {
		instance->arguments = mia_arena_alloc(c->arena, count * sizeof(*instance->arguments));
		memcpy(instance->arguments, arguments, count * sizeof(*instance->arguments));
	}
	const mia_ast_nodes_s* const generics = &info->node->as.function.generics;
	for (size_t i = 0; i < generics->count && i < count; ++i) mia_type_bindings_set(&instance->bindings, generics->data[i]->as.generic.name, arguments[i]);
	instance->node = generic ? mia_ast_node_clone(c->arena, info->node) : info->node;
	instance->depth = depth;
	mia_arena_push(c->arena, &info->instances, instance);
	mia_checker_context_s* const declaration = mia_checker_context_new(c, info->module, &instance->bindings, NULL, info->parent, NULL, false);
	if (info->parent != NULL) {
		declaration->bindings.count = 0;
		for (size_t i = 0; i < info->parent->bindings.count; ++i) mia_type_bindings_set(&declaration->bindings, info->parent->bindings.data[i].name, info->parent->bindings.data[i].type);
		for (size_t i = 0; i < instance->bindings.count; ++i) mia_type_bindings_set(&declaration->bindings, instance->bindings.data[i].name, instance->bindings.data[i].type);
	}
	_mia_checker_resolve_signature(c, instance->node, declaration, &instance->parameters, &instance->result);
	mia_vector_push(&c->program->instances, instance);
	mia_vector_push(&c->queue, instance);
	return instance;
}

void mia_checker_signature(mia_checker_s* const c, mia_program_function_s* const info, mia_type_list_s* const parameters, const mia_type_s** const result) {
	mia_type_bindings_s placeholders = {0};
	const mia_ast_nodes_s* const generics = &info->node->as.function.generics;
	for (size_t i = 0; i < generics->count; ++i) {
		const mia_view_s name = generics->data[i]->as.generic.name;
		mia_type_bindings_set(&placeholders, name, mia_type_parameter_of(c->types, name, info));
	}
	mia_checker_context_s* const context = mia_checker_context_new(c, info->module, &placeholders, NULL, info->parent, NULL, false);
	mia_vector_free(&placeholders);
	_mia_checker_resolve_signature(c, info->node, context, parameters, result);
}

void mia_checker_check_instance(mia_checker_s* const c, mia_program_instance_s* const instance) {
	if (instance->state != mia_program_instance_pending) return;
	instance->state = mia_program_instance_checking;
	mia_program_function_s* const info = instance->function;
	mia_ast_node_s* const node = instance->node;
	mia_type_bindings_s bindings = {0};
	if (info->parent != NULL) {
		for (size_t i = 0; i < info->parent->bindings.count; ++i) mia_type_bindings_set(&bindings, info->parent->bindings.data[i].name, info->parent->bindings.data[i].type);
	}
	for (size_t i = 0; i < instance->bindings.count; ++i) mia_type_bindings_set(&bindings, instance->bindings.data[i].name, instance->bindings.data[i].type);
	mia_checker_context_s* const context = mia_checker_context_new(c, info->module, &bindings, instance->result, info->parent, instance, true);
	mia_vector_free(&bindings);
	context->wrap = context->wrap || mia_ast_attribute_find(&node->as.function.attributes, "wrap") != NULL;
	const mia_ast_nodes_s* const parameters = &node->as.function.parameters;
	for (size_t i = 0; i < parameters->count && i < instance->parameters.count; ++i) {
		mia_ast_node_s* const parameter = parameters->data[i];
		const mia_type_s* const type = instance->parameters.data[i];
		const bool reference = mia_type_reference == type->kind;
		mia_program_variable_s* const variable = mia_arena_new(c->arena, mia_program_variable_s);
		variable->base.kind = mia_program_symbol_variable;
		variable->name = parameter->as.param.name;
		variable->type = reference ? type->as.reference.element : type;
		variable->kind = mia_program_variable_param;
		variable->span = parameter->span;
		variable->reference = reference;
		variable->level = context->level;
		parameter->note.variable = variable;
		mia_vector_push(&instance->variables, variable);
		if (parameter->as.param.default_value != NULL) {
			const mia_type_s* const target = reference ? type->as.reference.element : type;
			const mia_type_s* const actual = mia_checker_expression(c, parameter->as.param.default_value, context, target);
			mia_checker_coerce(c, parameter->as.param.default_value, actual, target);
			mia_evaluator_value_s ignored = {0};
			(void)mia_checker_require_constant(c, parameter->as.param.default_value, context, "a default argument", &ignored);
		}
		(void)mia_checker_context_declare(context, parameter->as.param.name, &variable->base);
		mia_vector_push(&context->variables, variable);
	}
	if (node->as.function.body != NULL) {
		mia_checker_block(c, node->as.function.body, context, false);
		if (instance->result != NULL && !mia_type_is_special(instance->result, mia_type_special_error) && !mia_checker_terminates(node->as.function.body)) {
			const mia_span_s end = node->as.function.body->span;
			const mia_span_s last = { .file = end.file, .begin = end.end - 1, .end = end.end, };
			mia_diag_label(mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0514, node->as.function.name_span, "`" mia_view_fmt "` does not return a value on every path",
				mia_view_arg(info->node->as.function.name)), "must return `%s`", mia_checker_name_of(c, instance->result)), last, "the end of the body can be reached");
		}
	}
	instance->state = mia_program_instance_checked;
}

// --- the program -------------------------------------------------------------------

mia_program_s* mia_program_check(const char* const path, mia_source_table_s* const sources, mia_diag_s* const diag, const mia_program_check_options_s* const options) {
	mia_program_s* const program = mia_calloc(1, sizeof(*program));
	mia_type_table_init(&program->types);
	program->sources = sources;
	program->diag = diag;
	mia_checker_s* const c = mia_calloc(1, sizeof(*c));
	c->program = program;
	c->arena = &program->arena;
	c->types = &program->types;
	c->diag = diag;
	c->sources = sources;
	c->options = options;
	c->evaluator.checker = c;
	c->evaluator.limit = (options->eval_steps > 0) ? options->eval_steps : mia_program_eval_steps_default;
	program->checker = c;
	mia_program_module_s* const entry = mia_checker_load(c, path);
	program->entry = entry;
	for (size_t i = 0; i < program->modules.count; ++i) _mia_checker_collect(c, program->modules.data[i]);
	for (size_t i = 0; i < program->modules.count; ++i) _mia_checker_resolve_imports(c, program->modules.data[i]);
	for (size_t i = 0; i < program->modules.count; ++i) _mia_checker_declarations(c, program->modules.data[i]);
	for (size_t i = 0; i < program->modules.count; ++i) {
		mia_program_module_s* const module = program->modules.data[i];
		for (size_t s = 0; s < module->ordered.count; ++s) {
			mia_program_symbol_s* const symbol = module->ordered.data[s];
			if (mia_program_symbol_constant == symbol->kind) {
				(void)mia_checker_constant_type(c, (mia_program_constant_s*)symbol);
			}
			else if (mia_program_symbol_struct == symbol->kind) {
				mia_program_struct_s* const info = (mia_program_struct_s*)symbol;
				if (0 == info->node->as.structure.generics.count) (void)mia_checker_struct_type(c, info, NULL, 0, info->node->as.structure.name_span);
			}
			else if (mia_program_symbol_function == symbol->kind) {
				mia_program_function_s* const info = (mia_program_function_s*)symbol;
				if (!mia_checker_function_is_generic(info)) (void)mia_checker_instance(c, info, NULL, 0, NULL, info->node->as.function.name_span);
			}
		}
	}
	while (c->queue_head < c->queue.count) mia_checker_check_instance(c, c->queue.data[c->queue_head++]);
	if (entry != NULL) _mia_checker_check_main(c, entry);
	return program;
}

void mia_program_free(mia_program_s* const program) {
	if (NULL == program) return;
	mia_checker_s* const c = program->checker;
	if (c != NULL) {
		for (size_t i = 0; i < c->contexts.count; ++i) _mia_checker_context_free(c->contexts.data[i]);
		mia_vector_free(&c->contexts);
		mia_vector_free(&c->queue);
		mia_vector_free(&c->sizing);
		mia_vector_free(&c->recursive_reported);
		mia_vector_free(&c->large_reported);
		mia_vector_free(&c->roots);
		mia_vector_free(&c->evaluator.frames);
		mia_vector_free(&c->evaluator.running);
		mia_vector_free(&c->plugin_types);
		mia_free(c);
	}
	for (size_t i = 0; i < program->modules.count; ++i) {
		mia_program_module_s* const module = program->modules.data[i];
		mia_vector_free(&module->imports);
		mia_map_free(&module->symbols);
		mia_vector_free(&module->ordered);
		mia_map_free(&module->imported);
		mia_vector_free(&module->star);
		mia_map_free(&module->qualifiers);
	}
	for (size_t i = 0; i < program->instances.count; ++i) {
		mia_program_instance_s* const instance = program->instances.data[i];
		mia_vector_free(&instance->bindings);
		mia_vector_free(&instance->parameters);
		mia_vector_free(&instance->variables);
	}
	mia_vector_free(&program->modules);
	mia_vector_free(&program->instances);
	for (size_t i = 0; i < program->layouts.count; ++i) mia_vector_free(&program->layouts.data[i]->fields);
	mia_vector_free(&program->layouts);
	mia_vector_free(&program->constants);
	mia_type_table_free(&program->types);
	mia_arena_free(&program->arena);
	mia_free(program);
}

// ========================================================================================
// loading modules
// ========================================================================================

/// module loading. a module is one '.mia' file; 'import a.b' loads 'a/b.mia'
/// from the first search root that has it: the directory of the entry file,
/// then the standard library, then every '-M' directory in order. import
/// cycles are errors.

/// 'path' made absolute and normalized (like python's os.path.abspath),
/// with '/' between its parts on every system.
static char* _mia_checker_absolute(const char* const path) {
	mia_string_s joined = {0};
	if (0 == mia_system_path_root(path)) {
		char* const directory = mia_system_current_directory();
		if (directory != NULL) mia_string_append_cstr(&joined, directory);
		mia_free(directory);
		mia_string_append_char(&joined, '/');
	}
	mia_string_append_cstr(&joined, path);
	const char* const text = mia_string_cstr(&joined);
	const size_t root = mia_system_path_root(text);
	// normalize: drop empty parts and '.', resolve '..'.
	mia_vector_of(mia_view_s) parts = {0};
	const char* cursor = text + root;
	while (*cursor != '\0') {
		while (mia_system_path_is_separator(*cursor)) ++cursor;
		const char* const start = cursor;
		while (*cursor != '\0' && !mia_system_path_is_separator(*cursor)) ++cursor;
		const mia_view_s part = mia_view_new(start, (size_t)(cursor - start));
		if (0 == part.length || mia_view_eq_cstr(part, ".")) continue;
		if (mia_view_eq_cstr(part, "..")) {
			if (parts.count > 0) parts.count -= 1;
			continue;
		}
		mia_vector_push(&parts, part);
	}
	mia_string_s result = {0};
	for (size_t i = 0; i < root; ++i) mia_string_append_char(&result, mia_system_path_is_separator(text[i]) ? '/' : text[i]);
	for (size_t i = 0; i < parts.count; ++i) {
		if (i > 0) mia_string_append_char(&result, '/');
		mia_string_append(&result, parts.data[i]);
	}
	mia_vector_free(&parts);
	mia_string_free(&joined);
	return mia_string_take(&result);
}

static char* _mia_checker_directory(const char* const path) {
	return mia_string_format("%.*s", (int)mia_system_path_directory(path), path);
}

/// python's os.path.join(root, *parts) + '.mia'.
static char* _mia_checker_module_path(const char* const root, const mia_view_list_s* const parts) {
	mia_string_s path = {0};
	mia_string_append_cstr(&path, root);
	for (size_t i = 0; i < parts->count; ++i) {
		if (path.length > 0 && !mia_system_path_is_separator(path.data[path.length - 1])) mia_string_append_char(&path, '/');
		mia_string_append(&path, parts->data[i]);
	}
	mia_string_append_cstr(&path, ".mia");
	return mia_string_take(&path);
}

static char* _mia_checker_dotted(const mia_view_list_s* const parts) {
	mia_string_s dotted = {0};
	for (size_t i = 0; i < parts->count; ++i) {
		if (i > 0) mia_string_append_char(&dotted, '.');
		mia_string_append(&dotted, parts->data[i]);
	}
	return mia_string_take(&dotted);
}

typedef struct {
	mia_checker_s*                       checker;
	mia_map_s                            loaded ;   // absolute path -> module
	mia_vector_of(mia_program_module_s*) stack  ;
} _mia_checker_loader_s;

static const char* _mia_checker_keep(mia_checker_s* const c, char* const text) {
	const char* const kept = mia_arena_strndup(c->arena, text, strlen(text));
	mia_free(text);
	return kept;
}

static mia_program_module_s* _mia_checker_load(_mia_checker_loader_s* const loader, const char* const key, const mia_source_s* const source, const char* const name) {
	mia_checker_s* const c = loader->checker;
	mia_program_module_s* const module = mia_arena_new(c->arena, mia_program_module_s);
	module->base.kind = mia_program_symbol_module;
	module->name = mia_arena_strndup(c->arena, name, strlen(name));
	module->path = source->path;
	module->source = source;
	module->tree = mia_parser_parse(source, c->arena, c->diag);
	mia_map_set(&loader->loaded, mia_view_cstr(key), module);
	mia_vector_push(&loader->stack, module);
	const mia_ast_nodes_s* const imports = &module->tree->as.module.imports;
	for (size_t i = 0; i < imports->count; ++i) {
		mia_ast_node_s* const node = imports->data[i];
		const mia_view_list_s* const parts = (mia_ast_node_import == node->kind) ? &node->as.import.path : &node->as.from_import.path;
		char* const dotted = _mia_checker_dotted(parts);
		const char* found = NULL;
		for (size_t r = 0; r < c->roots.count && NULL == found; ++r) {
			char* const candidate = _mia_checker_module_path(c->roots.data[r], parts);
			if (mia_system_file_exists(candidate)) found = _mia_checker_keep(c, candidate);
			else mia_free(candidate);
		}
		if (NULL == found) {
			mia_string_s searched = {0};
			for (size_t r = 0; r < c->roots.count; ++r) {
				char* const candidate = _mia_checker_module_path(c->roots.data[r], parts);
				mia_string_appendf(&searched, "%s%s", r > 0 ? ", " : "", candidate);
				mia_free(candidate);
			}
			mia_diag_entry_s* const diagnostic = mia_diag_note(mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0301, node->span, "module `%s` not found", dotted),
				"no such module"), "searched: %s", mia_string_cstr(&searched));
			if (NULL == c->options->stdlib) {
				mia_diag_help(diagnostic, "the standard library is not in mia's installation folder (stdlib/ beside mia); pass `--stdlib <dir>`");
			}
			mia_string_free(&searched);
			mia_free(dotted);
			continue;
		}
		char* const found_key = _mia_checker_absolute(found);
		mia_program_module_s* target = mia_map_get_cstr(&loader->loaded, found_key);
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
			mia_string_s chain = {0};
			for (size_t s = at; s < loader->stack.count; ++s) mia_string_appendf(&chain, "%s%s", s > at ? " -> " : "", loader->stack.data[s]->name);
			mia_string_appendf(&chain, " -> %s", target->name);
			mia_diag_entry_s* const diagnostic = mia_diag_error(c->diag, mia_diag_E0302, node->span, "import cycle: `%s` imports `%s`", module->name, dotted);
			mia_diag_label_main(diagnostic, "this import closes a cycle");
			mia_diag_note(diagnostic, "cycle: %s", mia_string_cstr(&chain));
			mia_diag_help(diagnostic, "import cycles are not allowed; move shared declarations into their own module");
			mia_string_free(&chain);
			mia_free(found_key);
			mia_free(dotted);
			continue;
		}
		if (NULL == target) {
			const mia_source_s* const loaded = mia_source_load(c->sources, found);
			if (NULL == loaded) {
				mia_diag_error(c->diag, mia_diag_E0301, node->span, "cannot read module `%s`", dotted);
				mia_free(found_key);
				mia_free(dotted);
				continue;
			}
			const char* const kept_key = _mia_checker_keep(c, found_key);
			if (loader->stack.count >= mia_ast_depth_limit) {
				// loading recurses along the imports: too long a chain would overflow the stack
				mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0307, node->span, "modules import each other too deeply"),
					"more than %d modules in a chain of imports", mia_ast_depth_limit);
				mia_free(dotted);
				continue;
			}
			target = _mia_checker_load(loader, kept_key, loaded, dotted);
		}
		else {
			mia_free(found_key);
		}
		const mia_program_import_s import = { .node = node, .target = target, };
		mia_vector_push(&module->imports, import);
		mia_free(dotted);
	}
	(void)mia_vector_pop(&loader->stack);
	mia_vector_push(&c->program->modules, module);
	return module;
}

mia_program_module_s* mia_checker_load(mia_checker_s* const c, const char* const path) {
	const mia_source_s* const source = mia_source_load(c->sources, path);
	if (NULL == source) {
		mia_diag_error(c->diag, mia_diag_E0601, mia_span_none, "cannot read `%s`", path);
		return NULL;
	}
	char* const absolute = _mia_checker_absolute(path);
	// the entry's directory as it was given, so the paths of its modules are shown that way too
	mia_vector_push(&c->roots, _mia_checker_keep(c, _mia_checker_directory(path)));
	if (c->options->stdlib != NULL) mia_vector_push(&c->roots, c->options->stdlib);
	for (size_t i = 0; i < c->options->module_dir_count; ++i) mia_vector_push(&c->roots, c->options->module_dirs[i]);
	_mia_checker_loader_s loader = { .checker = c, };
	mia_program_module_s* const entry = _mia_checker_load(&loader, _mia_checker_keep(c, absolute), source, "main");
	mia_map_free(&loader.loaded);
	mia_vector_free(&loader.stack);
	return entry;
}

// ========================================================================================
// types, struct layouts, sizes
// ========================================================================================

/// the checker: resolving written types, struct layouts (one per instance),
/// sizes and zero values.

const mia_type_s* mia_checker_scalar_type(mia_checker_s* const c, const mia_scalar_e scalar) {
	return mia_type_of_scalar(c->types, scalar);
}

const mia_type_s* mia_checker_special_type(mia_checker_s* const c, const mia_type_special_e special) {
	return mia_type_of_special(c->types, special);
}

static const char* _mia_checker_path_text(mia_checker_s* const c, const mia_view_list_s* const path) {
	mia_string_s text = {0};
	for (size_t i = 0; i < path->count; ++i) {
		if (i > 0) mia_string_append_char(&text, '.');
		mia_string_append(&text, path->data[i]);
	}
	const char* const kept = mia_arena_strndup(c->arena, mia_string_cstr(&text), text.length);
	mia_string_free(&text);
	return kept;
}

static const char* _mia_checker_generics_text(mia_checker_s* const c, const mia_ast_nodes_s* const generics) {
	mia_string_s text = {0};
	for (size_t i = 0; i < generics->count; ++i) {
		if (i > 0) mia_string_append_cstr(&text, ", ");
		mia_string_append(&text, generics->data[i]->as.generic.name);
	}
	const char* const kept = mia_arena_strndup(c->arena, mia_string_cstr(&text), text.length);
	mia_string_free(&text);
	return kept;
}

/// an array length: a constant non-negative integer; -1 when it is not one.
static int64_t _mia_checker_constant_length(mia_checker_s* const c, mia_ast_node_s* const node, mia_checker_context_s* const context) {
	const mia_type_s* const type = mia_checker_expression(c, node, context, NULL);
	if (mia_type_is_special(type, mia_type_special_error)) return -1;
	if (!mia_type_is_integer(type)) {
		mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0502, node->span, "an array length must be an integer"), "found `%s`", mia_checker_name_of(c, type));
		return -1;
	}
	if (mia_type_is_special(type, mia_type_special_untyped_integer)) {
		mia_evaluator_value_s probe = {0};
		const bool known = mia_checker_try_value(c, node, &probe);
		const bool negative = known && mia_evaluator_value_integer == probe.kind && mia_int128_negative(probe.as.integer);
		mia_checker_finalize(c, node, mia_checker_scalar_type(c, negative ? mia_scalar_int : mia_scalar_uint));
	}
	mia_evaluator_value_s value = {0};
	if (!mia_checker_require_constant(c, node, context, "an array length", &value) || value.kind != mia_evaluator_value_integer) return -2;
	if (mia_int128_negative(value.as.integer)) {
		mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0502, node->span, "an array length cannot be negative"), "length is %lld",
			(long long)mia_int128_to_int(value.as.integer));
		return -1;
	}
	if (mia_int128_compare(value.as.integer, mia_int128_from_uint(mia_bytecode_type_size_limit)) > 0) {
		mia_string_s text = {0};
		mia_format_int128(&text, value.as.integer);
		mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0532, node->span, "an array of %s elements is too large", mia_string_cstr(&text)),
			"at most %llu", (unsigned long long)mia_bytecode_type_size_limit);
		mia_string_free(&text);
		return -1;
	}
	return mia_int128_to_int(value.as.integer);
}

const mia_type_s* mia_checker_resolve_type(mia_checker_s* const c, const mia_ast_node_s* const node, mia_checker_context_s* const context,
										   const bool reference, const bool infer) {
	const mia_type_s* const error = mia_checker_special_type(c, mia_type_special_error);
	switch (node->kind) {
		case mia_ast_node_type_name: {
			const mia_view_list_s* const path = &node->as.type_name.path;
			if (1 == path->count) {
				const mia_scalar_e scalar = mia_scalar_from_name(path->data[0]);
				if (scalar != mia_scalar_none) return mia_checker_scalar_type(c, scalar);
				const mia_type_s* const bound = mia_type_bindings_get(&context->bindings, path->data[0]);
				if (bound != NULL) {
					if (node->as.type_name.arguments.count > 0) {
						mia_diag_error(c->diag, mia_diag_E0521, node->span, "type parameter `" mia_view_fmt "` takes no type arguments", mia_view_arg(path->data[0]));
					}
					return bound;
				}
			}
			mia_program_symbol_s* symbol = NULL;
			if (1 == path->count) {
				symbol = mia_checker_lookup(c, path->data[0], context, node->span, true);
			}
			else {
				mia_program_symbol_s* base = mia_checker_lookup(c, path->data[0], context, node->span, true);
				for (size_t i = 1; i < path->count; ++i) {
					if (NULL == base || (base->kind != mia_program_symbol_module && base->kind != mia_program_symbol_prefix)) break;
					base = mia_checker_member_of(c, base, path->data[i], node->span, context);
				}
				symbol = base;
			}
			if (NULL == symbol) return error;
			if (symbol->kind != mia_program_symbol_struct) {
				mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0503, node->span, "`%s` is not a type", _mia_checker_path_text(c, path)), "expected a type");
				return error;
			}
			mia_program_struct_s* const info = (mia_program_struct_s*)symbol;
			const mia_ast_nodes_s* const generics = &info->node->as.structure.generics;
			mia_vector_of(const mia_type_s*) arguments = {0};
			bool failed = false;
			for (size_t i = 0; i < node->as.type_name.arguments.count; ++i) {
				const mia_type_s* const argument = mia_checker_resolve_type(c, node->as.type_name.arguments.data[i], context, false, false);
				failed = failed || argument == error;
				mia_vector_push(&arguments, argument);
			}
			const mia_type_s* result = error;
			if (arguments.count != generics->count) {
				if (generics->count > 0) {
					mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0520, node->span, "`" mia_view_fmt "` takes %zu type argument(s), found %zu",
						mia_view_arg(info->node->as.structure.name), generics->count, arguments.count),
						"expected `" mia_view_fmt "<%s>`", mia_view_arg(info->node->as.structure.name), _mia_checker_generics_text(c, generics));
				}
				else {
					mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0521, node->span, "`" mia_view_fmt "` is not generic", mia_view_arg(info->node->as.structure.name)),
						"remove the type arguments");
				}
			}
			else if (!failed) {
				result = mia_checker_struct_type(c, info, arguments.data, arguments.count, node->span);
			}
			mia_vector_free(&arguments);
			return result;
		}
		case mia_ast_node_type_slice:
			return mia_type_slice_of(c->types, mia_checker_resolve_type(c, node->as.type_slice.element, context, false, false));
		case mia_ast_node_type_array: {
			const mia_type_s* const element = mia_checker_resolve_type(c, node->as.type_array.element, context, false, false);
			if (NULL == node->as.type_array.length) {
				if (!infer) {
					mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0506, node->span, "`[_]` needs an initializer to take its length from"),
						"only allowed in `let` with a value");
					return error;
				}
				return mia_type_array_of(c->types, element, -1);
			}
			const int64_t length = _mia_checker_constant_length(c, node->as.type_array.length, context);
			if (length < 0) return error;
			const mia_type_s* const array = mia_type_array_of(c->types, element, length);
			if (!mia_type_is_special(element, mia_type_special_error) && !mia_type_contains_parameter(element) && mia_checker_size_of(c, array, NULL) > mia_bytecode_type_size_limit) {
				mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0532, node->span, "the type `%s` is too large", mia_checker_name_of(c, array)),
					"more than %llu bytes", (unsigned long long)mia_bytecode_type_size_limit);
				return error;
			}
			return array;
		}
		case mia_ast_node_type_reference: {
			const mia_type_s* const element = mia_checker_resolve_type(c, node->as.type_reference.element, context, false, false);
			if (!reference) {
				mia_diag_help(mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0413, node->span, "references are only allowed as parameter types"),
					"a reference cannot be stored"), "use a slice to keep a view of an array");
				return element;
			}
			return mia_type_reference_to(c->types, element);
		}
		case mia_ast_node_type_function: {
			mia_vector_of(const mia_type_s*) parameters = {0};
			for (size_t i = 0; i < node->as.type_function.parameters.count; ++i) {
				mia_vector_push(&parameters, mia_checker_resolve_type(c, node->as.type_function.parameters.data[i], context, true, false));
			}
			const mia_type_s* const result = (node->as.type_function.result != NULL)
				? mia_checker_resolve_type(c, node->as.type_function.result, context, false, false) : NULL;
			const mia_type_s* const type = mia_type_function_of(c->types, parameters.data, parameters.count, result);
			mia_vector_free(&parameters);
			return type;
		}
		default:
			mia_unreachable();
	}
}

mia_program_field_s* mia_checker_layout_field(const mia_program_layout_s* const layout, const mia_view_s name) {
	for (size_t i = 0; i < layout->fields.count; ++i) if (mia_view_eq(layout->fields.data[i]->name, name)) return layout->fields.data[i];
	return NULL;
}

static mia_program_layout_s* _mia_checker_find_layout(const mia_program_struct_s* const info, const mia_type_s* const type) {
	for (size_t i = 0; i < info->instances.count; ++i) if (info->instances.data[i]->type == type) return info->instances.data[i];
	return NULL;
}

const mia_type_s* mia_checker_struct_type(mia_checker_s* const c, mia_program_struct_s* const info, const mia_type_s* const* const arguments,
										  const size_t count, const mia_span_s span) {
	mia_unused(span);
	const mia_type_s* const type = mia_type_struct_of(c->types, info, info->node->as.structure.name, arguments, count);
	bool parameter = false;
	for (size_t i = 0; i < count; ++i) parameter = parameter || mia_type_contains_parameter(arguments[i]);
	if (parameter || _mia_checker_find_layout(info, type) != NULL) return type;
	mia_program_layout_s* const layout = mia_arena_new(c->arena, mia_program_layout_s);
	layout->type = type;
	mia_arena_push(c->arena, &info->instances, layout);
	mia_vector_push(&c->program->layouts, layout);
	if (c->layout_depth >= mia_ast_depth_limit) {
		// a field's struct gets its layout here too: too long a chain would overflow the stack
		if (!c->struct_depth_reported) {
			mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0207, info->node->as.structure.name_span, "structs nest too deeply"),
				"more than %d structs inside each other", mia_ast_depth_limit);
		}
		c->struct_depth_reported = true;
		return type;   // its layout stays empty
	}
	c->layout_depth += 1;
	mia_type_bindings_s bindings = {0};
	const mia_ast_nodes_s* const generics = &info->node->as.structure.generics;
	for (size_t i = 0; i < generics->count && i < count; ++i) mia_type_bindings_set(&bindings, generics->data[i]->as.generic.name, arguments[i]);
	mia_checker_context_s* const context = mia_checker_context_new(c, info->module, &bindings, NULL, NULL, NULL, false);
	mia_vector_free(&bindings);
	const mia_ast_nodes_s* const fields = &info->node->as.structure.fields;
	for (size_t i = 0; i < fields->count; ++i) {
		mia_ast_node_s* const node = fields->data[i];
		(void)mia_checker_attributes(c, &node->as.field.attributes, "field");
		const mia_program_field_s* const previous = mia_checker_layout_field(layout, node->as.field.name);
		if (previous != NULL) {
			mia_diag_label(mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0401, node->span, "field `" mia_view_fmt "` is declared twice", mia_view_arg(node->as.field.name)),
				"declared again here"), previous->node->span, "first declared here");
			continue;
		}
		mia_program_field_s* const field = mia_arena_new(c->arena, mia_program_field_s);
		field->name = node->as.field.name;
		field->type = mia_checker_resolve_type(c, node->as.field.type, context, false, false);
		field->default_value = (node->as.field.default_value != NULL) ? mia_ast_node_clone(c->arena, node->as.field.default_value) : NULL;
		field->node = node;
		field->index = layout->fields.count;
		mia_vector_push(&layout->fields, field);
	}
	for (size_t i = 0; i < layout->fields.count; ++i) {
		mia_program_field_s* const field = layout->fields.data[i];
		if (NULL == field->default_value) continue;
		const mia_type_s* const actual = mia_checker_expression(c, field->default_value, context, field->type);
		mia_checker_coerce(c, field->default_value, actual, field->type);
		mia_evaluator_value_s ignored = {0};
		(void)mia_checker_require_constant(c, field->default_value, context, "a field default", &ignored);
	}
	c->layout_depth -= 1;
	(void)mia_checker_size_of(c, type, NULL);
	return type;
}

mia_program_layout_s* mia_checker_layout(mia_checker_s* const c, const mia_type_s* const type) {
	mia_program_struct_s* const info = type->as.structure.info;
	mia_program_layout_s* layout = _mia_checker_find_layout(info, type);
	if (NULL == layout) {
		(void)mia_checker_struct_type(c, info, type->as.structure.arguments, type->as.structure.count, info->node->as.structure.name_span);
		layout = _mia_checker_find_layout(info, type);
	}
	return layout;
}

/// sizes saturate at UINT64_MAX instead of wrapping: anything that large is reported (E0532).
static uint64_t _mia_checker_size_add(const uint64_t a, const uint64_t b) {
	return (a > UINT64_MAX - b) ? UINT64_MAX : a + b;
}

static uint64_t _mia_checker_size_multiply(const uint64_t a, const uint64_t b) {
	return (b != 0 && a > UINT64_MAX / b) ? UINT64_MAX : a * b;
}

/// 'size' rounded up to a multiple of 'alignment'.
static uint64_t _mia_checker_size_round(const uint64_t size, const uint64_t alignment) {
	const uint64_t padded = _mia_checker_size_add(size, alignment - 1);
	return (UINT64_MAX == padded) ? UINT64_MAX : padded / alignment * alignment;
}

uint64_t mia_checker_size_of(mia_checker_s* const c, const mia_type_s* const type, uint64_t* const alignment) {
	uint64_t ignored = 0;
	uint64_t* const align = (alignment != NULL) ? alignment : &ignored;
	switch (type->kind) {
		case mia_type_scalar: {
			const uint64_t size = (mia_scalar_bool == type->as.scalar) ? 1 : mia_scalar_bits(type->as.scalar) / 8;
			*align = size;
			return size;
		}
		case mia_type_slice:
			*align = mia_scalar_word_size;
			return 2 * mia_scalar_word_size;
		case mia_type_function:
			*align = mia_scalar_word_size;
			return mia_scalar_word_size;
		case mia_type_array: {
			const uint64_t size = mia_checker_size_of(c, type->as.array.element, align);
			const uint64_t length = (uint64_t)(type->as.array.length > 0 ? type->as.array.length : 0);
			return _mia_checker_size_multiply(size, length);
		}
		case mia_type_struct: {
			mia_program_struct_s* const info = type->as.structure.info;
			mia_program_layout_s* const known = mia_checker_layout(c, type);
			if (known->sized) {
				*align = known->alignment;
				return known->size;
			}
			if (c->sizing.count >= mia_ast_depth_limit) {
				// structs inside structs: sizing recurses, so too long a chain would overflow the stack
				if (!c->struct_depth_reported) {
					mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0207, info->node->as.structure.name_span, "structs nest too deeply"),
						"more than %d structs inside each other", mia_ast_depth_limit);
				}
				c->struct_depth_reported = true;
				*align = 1;
				return 0;
			}
			for (size_t i = 0; i < c->sizing.count; ++i) {
				if (c->sizing.data[i] != type) continue;
				bool reported = false;
				for (size_t j = 0; j < c->recursive_reported.count; ++j) reported = reported || c->recursive_reported.data[j] == info;
				if (!reported) {
					mia_vector_push(&c->recursive_reported, info);
					mia_diag_help(mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0414, info->node->as.structure.name_span, "struct `" mia_view_fmt "` contains itself",
						mia_view_arg(info->node->as.structure.name)), "it would have infinite size"), "store it through a slice, for example `next: []Node`");
				}
				*align = 1;
				return 0;
			}
			mia_vector_push(&c->sizing, type);
			uint64_t offset = 0, struct_alignment = 1;
			const mia_program_layout_s* const layout = mia_checker_layout(c, type);
			for (size_t i = 0; i < layout->fields.count; ++i) {
				uint64_t field_alignment = 1;
				const uint64_t size = mia_checker_size_of(c, layout->fields.data[i]->type, &field_alignment);
				if (0 == field_alignment) field_alignment = 1;
				offset = _mia_checker_size_round(offset, field_alignment);
				layout->fields.data[i]->offset = offset;
				offset = _mia_checker_size_add(offset, size);
				if (field_alignment > struct_alignment) struct_alignment = field_alignment;
			}
			for (size_t i = 0; i < c->sizing.count; ++i) {
				if (c->sizing.data[i] == type) {
					c->sizing.data[i] = c->sizing.data[c->sizing.count - 1];
					c->sizing.count -= 1;
					break;
				}
			}
			*align = struct_alignment;
			const uint64_t size = _mia_checker_size_round(offset, struct_alignment);
			bool reported = false;
			for (size_t j = 0; j < c->large_reported.count; ++j) reported = reported || c->large_reported.data[j] == type;
			if (size > mia_bytecode_type_size_limit && !reported) {
				mia_vector_push(&c->large_reported, type);
				mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0532, info->node->as.structure.name_span, "the struct `%s` is too large", mia_checker_name_of(c, type)),
					"its fields take more than %llu bytes", (unsigned long long)mia_bytecode_type_size_limit);
			}
			known->sized = true;
			known->size = size;
			known->alignment = struct_alignment;
			return size;
		}
		default:
			*align = 1;
			return 0;
	}
}

bool mia_checker_zeroable(mia_checker_s* const c, const mia_type_s* const type, mia_type_list_s* const seen) {
	if (mia_type_function == type->kind) return false;
	if (mia_type_array == type->kind) return mia_checker_zeroable(c, type->as.array.element, seen);
	if (mia_type_struct == type->kind) {
		for (size_t i = 0; i < seen->count; ++i) if (seen->data[i] == type) return true;
		if (seen->count >= mia_ast_depth_limit) return true;   // nested too deep: sizing reports it (E0207)
		mia_vector_push(seen, type);
		const mia_program_layout_s* const layout = mia_checker_layout(c, type);
		for (size_t i = 0; i < layout->fields.count; ++i) {
			const mia_program_field_s* const field = layout->fields.data[i];
			if (NULL == field->default_value && !mia_checker_zeroable(c, field->type, seen)) return false;
		}
		return true;
	}
	return true;
}

mia_program_layout_s* mia_program_layout(mia_program_s* const program, const mia_type_s* const type) {
	return mia_checker_layout(program->checker, type);
}

uint64_t mia_program_size_of(mia_program_s* const program, const mia_type_s* const type, uint64_t* const alignment) {
	return mia_checker_size_of(program->checker, type, alignment);
}

// ========================================================================================
// statements, places, flow
// ========================================================================================

/// the checker: statements, assignments and the places they write to,
/// control flow, and the tracking of variables that may not be assigned yet.

static void _mia_checker_statement(mia_checker_s* const c, mia_ast_node_s* const node, mia_checker_context_s* const context);

void mia_checker_block(mia_checker_s* const c, mia_ast_node_s* const node, mia_checker_context_s* const context, const bool new_scope) {
	if (new_scope) mia_checker_context_push_scope(c, context);
	const mia_ast_nodes_s* const statements = &node->as.block.statements;
	// local constants and functions can be used anywhere in their block.
	for (size_t i = 0; i < statements->count; ++i) {
		mia_ast_node_s* const statement = statements->data[i];
		if (statement->kind != mia_ast_node_declaration_statement) continue;
		mia_ast_node_s* const declaration = statement->as.declaration_statement.declaration;
		mia_program_symbol_s* symbol = NULL;
		if (mia_ast_node_function == declaration->kind) {
			mia_program_function_s* const info = mia_arena_new(c->arena, mia_program_function_s);
			info->base.kind = mia_program_symbol_function;
			info->node = declaration;
			info->module = context->module;
			info->parent = context;
			symbol = &info->base;
		}
		else if (mia_ast_node_constant == declaration->kind) {
			mia_program_constant_s* const info = mia_arena_new(c->arena, mia_program_constant_s);
			info->base.kind = mia_program_symbol_constant;
			info->node = declaration;
			info->module = context->module;
			info->context = context;
			symbol = &info->base;
		}
		else {
			continue;
		}
		statement->note.symbol = symbol;
		mia_program_symbol_s* const previous = mia_checker_context_declare(context, mia_ast_declaration_name(declaration), symbol);
		if (previous != NULL) {
			mia_span_s first = mia_span_none;
			if (mia_program_symbol_function == previous->kind) first = ((mia_program_function_s*)previous)->node->as.function.name_span;
			else if (mia_program_symbol_constant == previous->kind) first = ((mia_program_constant_s*)previous)->node->as.constant.name_span;
			else if (mia_program_symbol_variable == previous->kind) first = ((mia_program_variable_s*)previous)->span;
			mia_diag_label(mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0401, mia_ast_declaration_name_span(declaration), "`" mia_view_fmt "` is declared twice in this block",
				mia_view_arg(mia_ast_declaration_name(declaration))), "declared again here"), first, "first declared here");
		}
	}
	for (size_t i = 0; i < statements->count; ++i) _mia_checker_statement(c, statements->data[i], context);
	if (new_scope) mia_checker_context_pop_scope(context);
}

static void _mia_checker_declaration_statement(mia_checker_s* const c, mia_ast_node_s* const node, mia_checker_context_s* const context) {
	mia_program_symbol_s* const symbol = node->note.symbol;
	if (NULL == symbol) return;
	if (mia_program_symbol_constant == symbol->kind) {
		(void)mia_checker_constant_type(c, (mia_program_constant_s*)symbol);
	}
	else if (mia_program_symbol_function == symbol->kind) {
		// checked where it is declared, so that earlier variables are in scope
		// (and reported as captures when the body uses them).
		mia_program_function_s* const info = (mia_program_function_s*)symbol;
		mia_checker_function_declaration(c, info, context);
		if (!mia_checker_function_is_generic(info)) {
			mia_program_instance_s* const instance = mia_checker_instance(c, info, NULL, 0, context, info->node->as.function.name_span);
			if (instance != NULL) mia_checker_check_instance(c, instance);
		}
	}
}

static void _mia_checker_let(mia_checker_s* const c, mia_ast_node_s* const node, mia_checker_context_s* const context) {
	const mia_checker_attribute_set_s found = mia_checker_attributes(c, &node->as.let.attributes, "let");
	if (found.wrap != NULL) context->wrapping += 1;
	const mia_type_s* const error = mia_checker_special_type(c, mia_type_special_error);
	const mia_type_s* const declared = (node->as.let.type != NULL) ? mia_checker_resolve_type(c, node->as.let.type, context, false, true) : NULL;
	const mia_type_s* type = declared;
	mia_ast_node_s* const value = node->as.let.value;
	if (value != NULL) {
		if (declared != NULL) {
			const mia_type_s* const actual = mia_checker_expression(c, value, context, declared);
			if (mia_type_array == declared->kind && declared->as.array.length < 0) {
				if (mia_type_array == actual->kind && actual->as.array.element == declared->as.array.element) {
					type = actual;
				}
				else if (actual != error) {
					mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0502, value->span, "mismatched types"), "expected `%s`, found `%s`",
						mia_checker_name_of(c, declared), mia_checker_name_of(c, actual));
					type = error;
				}
			}
			else {
				mia_checker_coerce(c, value, actual, declared);
			}
		}
		else {
			type = mia_checker_settle(c, value, mia_checker_expression(c, value, context, NULL));
		}
	}
	else if (NULL == declared) {
		mia_diag_help(mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0506, node->as.let.name_span, "variable `" mia_view_fmt "` needs a type or a value", mia_view_arg(node->as.let.name)),
			"cannot infer its type"), "write `let " mia_view_fmt ": T;` or `let " mia_view_fmt " = value;`", mia_view_arg(node->as.let.name), mia_view_arg(node->as.let.name));
		type = error;
	}
	else if (mia_type_array == declared->kind && declared->as.array.length < 0) {
		mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0506, node->as.let.type->span, "`[_]` needs an initializer to take its length from"), "no value given");
		type = error;
	}
	if (found.wrap != NULL) context->wrapping -= 1;
	mia_program_variable_s* const variable = mia_arena_new(c->arena, mia_program_variable_s);
	variable->base.kind = mia_program_symbol_variable;
	variable->name = node->as.let.name;
	variable->type = (type != NULL) ? type : error;
	variable->kind = mia_program_variable_let;
	variable->span = node->as.let.name_span;
	variable->level = context->level;
	variable->initializer = value;
	node->note.variable = variable;
	if (NULL == value && declared != NULL && NULL == found.uninit) mia_checker_set_add(&context->unassigned, variable);
	mia_program_symbol_s* const previous = mia_map_get(mia_vector_last(&context->scopes), node->as.let.name);
	if (previous != NULL && mia_program_symbol_variable == previous->kind) {
		mia_diag_help(mia_diag_label(mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0401, node->as.let.name_span, "`" mia_view_fmt "` is already declared in this scope",
			mia_view_arg(node->as.let.name)), "declared again"), ((mia_program_variable_s*)previous)->span, "first declared here"),
			"use another name, or assign with `=`");
	}
	(void)mia_checker_context_declare(context, node->as.let.name, &variable->base);
	mia_vector_push(&context->variables, variable);
}

// --- assignments -----------------------------------------------------------------

/// whether an assignment can change 'node': a variable, a field or element of
/// one, or any element of a slice (not a constant, a function or a temporary).
static bool _mia_checker_assignable(const mia_ast_node_s* const node) {
	switch (node->kind) {
		case mia_ast_node_paren:  return _mia_checker_assignable(node->as.paren.inner);
		case mia_ast_node_name:   return node->note.variable != NULL;
		case mia_ast_node_member: return node->note.field != NULL && _mia_checker_assignable(node->as.member.target);
		case mia_ast_node_index: {
			const mia_type_s* const target = node->as.index.target->note.type;
			return (target != NULL && mia_type_slice == target->kind) || _mia_checker_assignable(node->as.index.target);
		}
		default:                  return false;
	}
}

/// marks the variable that the place 'node' is part of as changed (assigned,
/// or lent to a reference parameter): the compiler keeps a 'for' variable in
/// the loop's counter only when it never is.
static void _mia_checker_changes(const mia_ast_node_s* const node) {
	switch (node->kind) {
		case mia_ast_node_paren:  _mia_checker_changes(node->as.paren.inner); return;
		case mia_ast_node_member: _mia_checker_changes(node->as.member.target); return;
		case mia_ast_node_index: {
			// an element of a slice is not part of the variable that holds the slice
			const mia_type_s* const target = node->as.index.target->note.type;
			if (target != NULL && mia_type_array == target->kind) _mia_checker_changes(node->as.index.target);
			return;
		}
		case mia_ast_node_name:
			if (node->note.variable != NULL) node->note.variable->changed = true;
			return;
		default:
			return;
	}
}

static void _mia_checker_assign(mia_checker_s* const c, mia_ast_node_s* const node, mia_checker_context_s* const context) {
	mia_ast_node_s* const target = node->as.assign.target;
	const mia_type_s* const error = mia_checker_special_type(c, mia_type_special_error);
	if (mia_ast_node_name == target->kind && mia_view_eq_cstr(target->as.name.name, "_")) {
		if (node->as.assign.operation != mia_ast_operator_assign) {
			mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0508, target->span, "`_` can only be assigned with `=`"), "discards a value");
		}
		(void)mia_checker_settle(c, node->as.assign.value, mia_checker_expression(c, node->as.assign.value, context, NULL));
		return;
	}
	const mia_type_s* target_type = NULL;
	if (mia_ast_operator_assign == node->as.assign.operation && mia_ast_node_name == target->kind) {
		mia_program_symbol_s* const symbol = mia_checker_lookup(c, target->as.name.name, context, target->span, true);
		if (symbol != NULL && mia_program_symbol_variable == symbol->kind) {
			mia_program_variable_s* const variable = (mia_program_variable_s*)symbol;
			target->note.variable = variable;
			target->note.type = target_type = variable->type;
		}
		else {
			target_type = (symbol != NULL) ? mia_checker_symbol_value(c, symbol, target, context, NULL) : error;
			target->note.type = target_type;
		}
	}
	else {
		target_type = mia_checker_expression(c, target, context, NULL);
	}
	_mia_checker_changes(target);
	if (target_type != error && !_mia_checker_assignable(target)) {
		const mia_view_s text = mia_checker_node_text(c, target);
		mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0508, target->span, "cannot assign to `" mia_view_fmt "`", mia_view_arg(text)),
			"not a variable, field or element");
	}
	if (mia_ast_operator_assign == node->as.assign.operation) {
		const mia_type_s* const actual = mia_checker_expression(c, node->as.assign.value, context, target_type);
		mia_checker_coerce(c, node->as.assign.value, actual, target_type);
		if (mia_ast_node_name == target->kind && target->note.variable != NULL) mia_checker_set_discard(&context->unassigned, target->note.variable);
		return;
	}
	const mia_ast_operator_e operation = node->as.assign.operation;
	const mia_type_s* const right = mia_checker_expression(c, node->as.assign.value, context, NULL);
	const mia_type_s* const result = mia_checker_binary_operation(c, node, operation, target, target_type, node->as.assign.value, right, context, node->span);
	if (result != error && target_type != error && result != target_type) {
		mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0502, node->span, "`%s` gives `%s`, which cannot be stored in `%s`", mia_ast_operator_text(operation),
			mia_checker_name_of(c, result), mia_checker_name_of(c, target_type)), "mismatched types");
	}
}

static void _mia_checker_condition(mia_checker_s* const c, mia_ast_node_s* const node, mia_checker_context_s* const context) {
	const mia_type_s* const boolean = mia_checker_scalar_type(c, mia_scalar_bool);
	mia_checker_coerce(c, node, mia_checker_expression(c, node, context, boolean), boolean);
}

static void _mia_checker_if(mia_checker_s* const c, mia_ast_node_s* const node, mia_checker_context_s* const context) {
	_mia_checker_condition(c, node->as.if_statement.condition, context);
	mia_checker_variable_set_s before = {0}, after_then = {0}, after_else = {0};
	mia_checker_set_assign(&before, &context->unassigned);
	mia_checker_block(c, node->as.if_statement.then, context, true);
	mia_checker_set_assign(&after_then, &context->unassigned);
	mia_checker_set_assign(&context->unassigned, &before);
	if (node->as.if_statement.otherwise != NULL) {
		if (mia_ast_node_if == node->as.if_statement.otherwise->kind) _mia_checker_if(c, node->as.if_statement.otherwise, context);
		else mia_checker_block(c, node->as.if_statement.otherwise, context, true);
		mia_checker_set_assign(&after_else, &context->unassigned);
	}
	else {
		mia_checker_set_assign(&after_else, &before);
	}
	const bool then_ends = mia_checker_terminates(node->as.if_statement.then);
	const bool else_ends = node->as.if_statement.otherwise != NULL && mia_checker_terminates(node->as.if_statement.otherwise);
	if (then_ends && !else_ends) {
		mia_checker_set_assign(&context->unassigned, &after_else);
	}
	else if (else_ends && !then_ends) {
		mia_checker_set_assign(&context->unassigned, &after_then);
	}
	else {
		mia_checker_set_assign(&context->unassigned, &after_then);
		mia_checker_set_union(&context->unassigned, &after_else);
	}
	mia_vector_free(&before);
	mia_vector_free(&after_then);
	mia_vector_free(&after_else);
}

static const mia_type_s* _mia_checker_common_integer(mia_checker_s* const c, mia_ast_node_s* const left_node, const mia_type_s* const left_type,
													 mia_ast_node_s* const right_node, const mia_type_s* const right_type) {
	const mia_type_s* left = left_type;
	const mia_type_s* right = right_type;
	const mia_type_s* const error = mia_checker_special_type(c, mia_type_special_error);
	if (left == error || right == error) return error;
	if (mia_type_is_untyped(left) && mia_type_is_untyped(right)) {
		left = mia_checker_settle(c, left_node, left);
		right = mia_checker_settle(c, right_node, right);
	}
	else if (mia_type_is_untyped(left)) {
		mia_checker_coerce(c, left_node, left, right);
		left = right;
	}
	else if (mia_type_is_untyped(right)) {
		mia_checker_coerce(c, right_node, right, left);
		right = left;
	}
	if (!(mia_type_scalar == left->kind && 'i' == mia_scalar_kind(left->as.scalar))) {
		mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0502, left_node->span, "a range needs integers"), "found `%s`", mia_checker_name_of(c, left));
		return error;
	}
	if (left != right) {
		mia_diag_label(mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0502, right_node->span, "both ends of a range need the same type"),
			"expected `%s`, found `%s`", mia_checker_name_of(c, left), mia_checker_name_of(c, right)), left_node->span, "this is `%s`", mia_checker_name_of(c, left));
		return error;
	}
	return left;
}

static void _mia_checker_loop_body(mia_checker_s* const c, mia_ast_node_s* const body, mia_checker_context_s* const context) {
	mia_checker_variable_set_s before = {0};
	mia_checker_set_assign(&before, &context->unassigned);
	context->loops += 1;
	mia_checker_block(c, body, context, true);
	context->loops -= 1;
	mia_checker_set_union(&context->unassigned, &before);
	mia_vector_free(&before);
}

static void _mia_checker_for(mia_checker_s* const c, mia_ast_node_s* const node, mia_checker_context_s* const context) {
	const mia_type_s* const start = mia_checker_expression(c, node->as.for_statement.start, context, NULL);
	const mia_type_s* const end = mia_checker_expression(c, node->as.for_statement.end, context, NULL);
	const mia_type_s* const type = _mia_checker_common_integer(c, node->as.for_statement.start, start, node->as.for_statement.end, end);
	mia_checker_context_push_scope(c, context);
	mia_program_variable_s* const variable = mia_arena_new(c->arena, mia_program_variable_s);
	variable->base.kind = mia_program_symbol_variable;
	variable->name = node->as.for_statement.name;
	variable->type = type;
	variable->kind = mia_program_variable_for;
	variable->span = node->as.for_statement.name_span;
	variable->level = context->level;
	node->note.variable = variable;
	(void)mia_checker_context_declare(context, node->as.for_statement.name, &variable->base);
	_mia_checker_loop_body(c, node->as.for_statement.body, context);
	mia_checker_context_pop_scope(context);
}

static void _mia_checker_return(mia_checker_s* const c, mia_ast_node_s* const node, mia_checker_context_s* const context) {
	mia_ast_node_s* const value = node->as.return_statement.value;
	if (NULL == context->result) {
		if (value != NULL) {
			const mia_type_s* const actual = mia_checker_expression(c, value, context, NULL);
			if (!mia_type_is_special(actual, mia_type_special_nothing) && !mia_type_is_special(actual, mia_type_special_error)) {
				mia_diag_help(mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0524, value->span, "this function returns nothing"), "remove the value"),
					"declare a result type with `-> type`");
			}
		}
		return;
	}
	if (NULL == value) {
		mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0524, node->span, "this function must return `%s`", mia_checker_name_of(c, context->result)), "missing value");
		return;
	}
	mia_checker_coerce(c, value, mia_checker_expression(c, value, context, context->result), context->result);
}

static void _mia_checker_expression_statement(mia_checker_s* const c, mia_ast_node_s* const node, mia_checker_context_s* const context) {
	mia_ast_node_s* const expression = node->as.expression_statement.expression;
	const mia_type_s* type = mia_checker_expression(c, expression, context, NULL);
	if (mia_type_is_untyped(type)) type = mia_checker_settle(c, expression, type);
	if (!mia_type_is_special(type, mia_type_special_nothing) && !mia_type_is_special(type, mia_type_special_error)) {
		const mia_view_s text = mia_checker_node_text(c, expression);
		mia_diag_help(mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0509, expression->span, "the result of this expression is not used"),
			"a `%s` is discarded", mia_checker_name_of(c, type)), "write `_ = " mia_view_fmt ";` to discard it on purpose", mia_view_arg(text));
	}
}

/// what the checker calls the statement in messages about its attributes.
static const char* _mia_checker_statement_kind(const mia_ast_node_s* const node) {
	switch (node->kind) {
		case mia_ast_node_block:    return "block";
		case mia_ast_node_if:       return "`if`";
		case mia_ast_node_while:    return "`while` loop";
		case mia_ast_node_for:      return "`for` loop";
		case mia_ast_node_return:   return "`return`";
		case mia_ast_node_break:    return "`break`";
		case mia_ast_node_continue: return "`continue`";
		default:               return "statement";
	}
}

static void _mia_checker_statement_kinds(mia_checker_s* const c, mia_ast_node_s* const node, mia_checker_context_s* const context);

/// a statement: its attributes (but a let's, which it checks itself), then
/// the statement; '[wrap]' covers everything in it but nested functions.
static void _mia_checker_statement(mia_checker_s* const c, mia_ast_node_s* const node, mia_checker_context_s* const context) {
	if (mia_ast_node_let == node->kind || mia_ast_node_declaration_statement == node->kind) {
		_mia_checker_statement_kinds(c, node, context);
		return;
	}
	const mia_checker_attribute_set_s found = mia_checker_attributes(c, mia_ast_statement_attributes(node), _mia_checker_statement_kind(node));
	if (found.wrap != NULL) context->wrapping += 1;
	_mia_checker_statement_kinds(c, node, context);
	if (found.wrap != NULL) context->wrapping -= 1;
}

static void _mia_checker_statement_kinds(mia_checker_s* const c, mia_ast_node_s* const node, mia_checker_context_s* const context) {
	switch (node->kind) {
		case mia_ast_node_declaration_statement: _mia_checker_declaration_statement(c, node, context); return;
		case mia_ast_node_let:                   _mia_checker_let(c, node, context); return;
		case mia_ast_node_assign:                _mia_checker_assign(c, node, context); return;
		case mia_ast_node_if: _mia_checker_if(c, node, context); return;
		case mia_ast_node_while:
			_mia_checker_condition(c, node->as.while_statement.condition, context);
			_mia_checker_loop_body(c, node->as.while_statement.body, context);
			return;
		case mia_ast_node_for:    _mia_checker_for(c, node, context); return;
		case mia_ast_node_return: _mia_checker_return(c, node, context); return;
		case mia_ast_node_break:
			if (0 == context->loops) mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0515, node->span, "`break` outside of a loop"), "not inside `while` or `for`");
			return;
		case mia_ast_node_continue:
			if (0 == context->loops) mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0515, node->span, "`continue` outside of a loop"), "not inside `while` or `for`");
			return;
		case mia_ast_node_block: mia_checker_block(c, node, context, true); return;
		case mia_ast_node_expression_statement: _mia_checker_expression_statement(c, node, context); return;
		default: mia_unreachable();
	}
}

static bool _mia_checker_breaks(const mia_ast_node_s* const node) {
	switch (node->kind) {
		case mia_ast_node_break: return true;
		case mia_ast_node_while: case mia_ast_node_for: case mia_ast_node_lambda: return false;
		case mia_ast_node_block:
			for (size_t i = 0; i < node->as.block.statements.count; ++i) if (_mia_checker_breaks(node->as.block.statements.data[i])) return true;
			return false;
		case mia_ast_node_if: return _mia_checker_breaks(node->as.if_statement.then) || (node->as.if_statement.otherwise != NULL && _mia_checker_breaks(node->as.if_statement.otherwise));
		default: return false;
	}
}

bool mia_checker_terminates(const mia_ast_node_s* const node) {
	if (NULL == node) return false;
	switch (node->kind) {
		case mia_ast_node_return: return true;
		case mia_ast_node_block:
			for (size_t i = 0; i < node->as.block.statements.count; ++i) if (mia_checker_terminates(node->as.block.statements.data[i])) return true;
			return false;
		case mia_ast_node_if: return node->as.if_statement.otherwise != NULL && mia_checker_terminates(node->as.if_statement.then) && mia_checker_terminates(node->as.if_statement.otherwise);
		case mia_ast_node_while:
			return mia_ast_node_bool == node->as.while_statement.condition->kind && node->as.while_statement.condition->as.boolean.value && !_mia_checker_breaks(node->as.while_statement.body);
		default: return false;
	}
}

// ========================================================================================
// expressions, calls, operators, literals
// ========================================================================================

/// the checker: expressions. calls (with generic inference and reference
/// arguments), the built-in operators, literals that
/// take their type from where they are used, casts, lambdas and intrinsics.

#define _mia_checker_error(_c) mia_checker_special_type((_c), mia_type_special_error)
#define _mia_checker_bool(_c) mia_checker_scalar_type((_c), mia_scalar_bool)

static bool _mia_checker_is_error(const mia_type_s* const type) {
	return mia_type_is_special(type, mia_type_special_error);
}

/// whether an untyped literal of type 'actual' can become a 'target'.
static bool _mia_checker_accepts_literal(const mia_type_s* const target, const mia_type_s* const actual) {
	if (NULL == target || target->kind != mia_type_scalar) return false;
	if (mia_type_is_special(actual, mia_type_special_untyped_integer)) return mia_type_is_numeric(target);
	if (mia_type_is_special(actual, mia_type_special_untyped_float)) return mia_type_is_float(target);
	return false;
}

static bool _mia_checker_is_arithmetic(const mia_ast_operator_e operation) {
	return operation >= mia_ast_operator_add && operation <= mia_ast_operator_remainder;
}

static bool _mia_checker_is_bitwise(const mia_ast_operator_e operation) {
	return operation >= mia_ast_operator_bit_and && operation <= mia_ast_operator_bit_xor;
}

static bool _mia_checker_is_shift(const mia_ast_operator_e operation) {
	return mia_ast_operator_shift_left == operation || mia_ast_operator_shift_right == operation;
}

static bool _mia_checker_is_equality(const mia_ast_operator_e operation) {
	return mia_ast_operator_equal == operation || mia_ast_operator_not_equal == operation;
}

static bool _mia_checker_is_ordering(const mia_ast_operator_e operation) {
	return operation >= mia_ast_operator_less && operation <= mia_ast_operator_greater_equal;
}

/// whether the type of 'node' comes from where it is used (literals, '{...}').
static bool _mia_checker_context_typed(const mia_ast_node_s* const node) {
	switch (node->kind) {
		case mia_ast_node_integer: return mia_scalar_none == node->as.integer.suffix;
		case mia_ast_node_float:   return mia_scalar_none == node->as.floating.suffix;
		case mia_ast_node_composite: return NULL == node->as.composite.type;
		case mia_ast_node_paren:   return _mia_checker_context_typed(node->as.paren.inner);
		case mia_ast_node_unary:
			return (mia_ast_operator_negate == node->as.unary.operation || mia_ast_operator_bit_not == node->as.unary.operation)
				&& _mia_checker_context_typed(node->as.unary.operand);
		case mia_ast_node_binary: {
			const mia_ast_operator_e operation = node->as.binary.operation;
			return (_mia_checker_is_arithmetic(operation) || _mia_checker_is_bitwise(operation) || _mia_checker_is_shift(operation))
				&& _mia_checker_context_typed(node->as.binary.left) && _mia_checker_context_typed(node->as.binary.right);
		}
		default: return false;
	}
}

/// 'i' for integer literals, 'f' for float literals, 0 otherwise.
static char _mia_checker_untyped_kind(const mia_ast_node_s* const node) {
	switch (node->kind) {
		case mia_ast_node_integer: return mia_scalar_none == node->as.integer.suffix ? 'i' : 0;
		case mia_ast_node_float:   return mia_scalar_none == node->as.floating.suffix ? 'f' : 0;
		case mia_ast_node_paren:   return _mia_checker_untyped_kind(node->as.paren.inner);
		case mia_ast_node_unary:   return _mia_checker_untyped_kind(node->as.unary.operand);
		case mia_ast_node_binary: {
			const char left = _mia_checker_untyped_kind(node->as.binary.left);
			const char right = _mia_checker_untyped_kind(node->as.binary.right);
			if (0 == left || 0 == right) return 0;
			return ('f' == left || 'f' == right) ? 'f' : 'i';
		}
		default: return 0;
	}
}

static const mia_type_s* _mia_checker_checked(mia_checker_s* const c, mia_ast_node_s* const node, mia_checker_context_s* const context, const mia_type_s* const expected) {
	if (node->note.prechecked) return node->note.type;
	return mia_checker_expression(c, node, context, expected);
}

/// checks the arguments of a call that failed, to find their own errors.
static void _mia_checker_loose(mia_checker_s* const c, const mia_ast_nodes_s* const nodes, const size_t from, mia_checker_context_s* const context) {
	for (size_t i = from; i < nodes->count; ++i) {
		mia_ast_node_s* const operand = (mia_ast_node_reference == nodes->data[i]->kind) ? nodes->data[i]->as.reference.operand : nodes->data[i];
		if (operand->note.prechecked) continue;
		const mia_type_s* const type = mia_checker_expression(c, operand, context, NULL);
		if (mia_type_is_untyped(type)) (void)mia_checker_settle(c, operand, type);
	}
}

static void _mia_checker_loose_one(mia_checker_s* const c, mia_ast_node_s* const node, mia_checker_context_s* const context) {
	mia_ast_node_s* holder[1] = { node };
	const mia_ast_nodes_s single = { .data = holder, .count = 1, .capacity = 1, };
	_mia_checker_loose(c, &single, 0, context);
}

// --- literals and names --------------------------------------------------------------

static const mia_type_s* _mia_checker_integer(mia_checker_s* const c, mia_ast_node_s* const node) {
	if (mia_scalar_none == node->as.integer.suffix) return mia_checker_special_type(c, mia_type_special_untyped_integer);
	const mia_type_s* const type = mia_checker_scalar_type(c, node->as.integer.suffix);
	const mia_int128_s value = node->note.negative ? mia_int128_negate(node->as.integer.value) : node->as.integer.value;
	if (!mia_type_holds(type, value)) {
		mia_string_s text = {0}, low = {0}, high = {0};
		mia_format_int128(&text, value);
		mia_format_int128(&low, mia_type_minimum(type));
		mia_format_int128(&high, mia_type_maximum(type));
		mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0511, node->span, "`%s` does not fit in `%s`", mia_string_cstr(&text), mia_checker_name_of(c, type)),
			"`%s` holds %s to %s", mia_checker_name_of(c, type), mia_string_cstr(&low), mia_string_cstr(&high));
		mia_string_free(&text);
		mia_string_free(&low);
		mia_string_free(&high);
	}
	return type;
}

const mia_type_s* mia_checker_symbol_value(mia_checker_s* const c, mia_program_symbol_s* const symbol, mia_ast_node_s* const node, mia_checker_context_s* const context,
										   const mia_type_s* const expected) {
	if (NULL == symbol) return _mia_checker_error(c);
	switch (symbol->kind) {
		case mia_program_symbol_variable: {
			mia_program_variable_s* const variable = (mia_program_variable_s*)symbol;
			node->note.variable = variable;
			if (mia_checker_set_contains(&context->unassigned, variable)) {
				mia_checker_set_discard(&context->unassigned, variable);
				mia_diag_help(mia_diag_label(mia_diag_label_main(mia_diag_warning(c->diag, mia_diag_W0502, node->span, "`" mia_view_fmt "` may be read before it is assigned",
					mia_view_arg(variable->name)), "read here"), variable->span, "declared without a value"),
					"assign it first, or mark the declaration `[uninit]`");
			}
			return variable->type;
		}
		case mia_program_symbol_constant:
			node->note.constant = (mia_program_constant_s*)symbol;
			return mia_checker_constant_type(c, (mia_program_constant_s*)symbol);
		case mia_program_symbol_function: {
			mia_program_function_s* const info = (mia_program_function_s*)symbol;
			const mia_ast_nodes_s* const generics = &info->node->as.function.generics;
			if (generics->count > 0) {
				if (expected != NULL && mia_type_function == expected->kind) {
					mia_type_list_s parameters = {0};
					const mia_type_s* result = NULL;
					mia_checker_signature(c, info, &parameters, &result);
					mia_type_bindings_s bindings = {0};
					const mia_type_s* const pattern = mia_type_function_of(c->types, parameters.data, parameters.count, result);
					bool complete = mia_type_unify(pattern, expected, &bindings);
					mia_vector_of(const mia_type_s*) arguments = {0};
					for (size_t i = 0; i < generics->count && complete; ++i) {
						const mia_type_s* const bound = mia_type_bindings_get(&bindings, generics->data[i]->as.generic.name);
						if (NULL == bound) complete = false;
						else mia_vector_push(&arguments, bound);
					}
					mia_program_instance_s* instance = NULL;
					if (complete) instance = mia_checker_instance(c, info, arguments.data, arguments.count, context, node->span);
					mia_vector_free(&arguments);
					mia_vector_free(&bindings);
					mia_vector_free(&parameters);
					if (complete) {
						if (NULL == instance) return _mia_checker_error(c);
						node->note.instance = instance;
						return mia_program_instance_type(c->program, instance);
					}
				}
				mia_string_s names = {0};
				for (size_t i = 0; i < generics->count; ++i) {
					if (i > 0) mia_string_append_cstr(&names, ", ");
					mia_string_append(&names, generics->data[i]->as.generic.name);
				}
				mia_diag_help(mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0506, node->span, "generic function `" mia_view_fmt "` needs type arguments here",
					mia_view_arg(info->node->as.function.name)), "cannot infer them"), "write `" mia_view_fmt "<%s>`",
					mia_view_arg(info->node->as.function.name), mia_string_cstr(&names));
				mia_string_free(&names);
				return _mia_checker_error(c);
			}
			mia_program_instance_s* const instance = mia_checker_instance(c, info, NULL, 0, context, node->span);
			if (NULL == instance) return _mia_checker_error(c);
			for (size_t i = 0; i < instance->parameters.count; ++i) {
				if (mia_type_is_special(instance->parameters.data[i], mia_type_special_variadic)) {
					mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0529, node->span, "variadic function `" mia_view_fmt "` can only be called",
						mia_view_arg(info->node->as.function.name)), "not a value");
					return _mia_checker_error(c);
				}
			}
			node->note.instance = instance;
			mia_checker_plugin_use(c, info, node->span);
			return mia_program_instance_type(c->program, instance);
		}
		case mia_program_symbol_struct: {
			const mia_program_struct_s* const info = (const mia_program_struct_s*)symbol;
			mia_diag_help(mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0529, node->span, "`" mia_view_fmt "` is a type, not a value", mia_view_arg(info->node->as.structure.name)),
				"expected a value"), "create a value with a literal: `(" mia_view_fmt "){ ... }`", mia_view_arg(info->node->as.structure.name));
			return _mia_checker_error(c);
		}
		default: {
			const mia_view_s text = mia_checker_node_text(c, node);
			mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0529, node->span, "`" mia_view_fmt "` is a module, not a value", mia_view_arg(text)), "expected a value");
			return _mia_checker_error(c);
		}
	}
}

const mia_type_s* mia_program_instance_type(mia_program_s* const program, const mia_program_instance_s* const instance) {
	return mia_type_function_of(&program->types, instance->parameters.data, instance->parameters.count, instance->result);
}

static const mia_type_s* _mia_checker_name(mia_checker_s* const c, mia_ast_node_s* const node, mia_checker_context_s* const context, const mia_type_s* const expected) {
	if (mia_view_eq_cstr(node->as.name.name, "_")) {
		mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0501, node->span, "`_` is not a value"), "only `_ = value;` discards");
		return _mia_checker_error(c);
	}
	return mia_checker_symbol_value(c, mia_checker_lookup(c, node->as.name.name, context, node->span, true), node, context, expected);
}

static bool _mia_checker_is_module_like(const mia_program_symbol_s* const symbol) {
	return symbol != NULL && (mia_program_symbol_module == symbol->kind || mia_program_symbol_prefix == symbol->kind);
}

static const mia_type_s* _mia_checker_member(mia_checker_s* const c, mia_ast_node_s* const node, mia_checker_context_s* const context, const mia_type_s* const expected) {
	mia_ast_node_s* const target_node = node->as.member.target;
	if (mia_ast_node_name == target_node->kind || mia_ast_node_member == target_node->kind) {
		mia_program_symbol_s* const base = mia_checker_qualified(c, target_node, context, false);
		if (_mia_checker_is_module_like(base)) {
			mia_program_symbol_s* const symbol = mia_checker_member_of(c, base, node->as.member.name, node->as.member.name_span, context);
			return mia_checker_symbol_value(c, symbol, node, context, expected);
		}
	}
	const mia_type_s* const target = mia_checker_expression(c, target_node, context, NULL);
	if (_mia_checker_is_error(target)) return target;
	if (mia_type_struct == target->kind) {
		mia_program_layout_s* const layout = mia_checker_layout(c, target);
		mia_program_field_s* const field = mia_checker_layout_field(layout, node->as.member.name);
		if (NULL == field) {
			mia_string_s fields = {0};
			for (size_t i = 0; i < layout->fields.count; ++i) {
				if (i > 0) mia_string_append_cstr(&fields, ", ");
				mia_string_append(&fields, layout->fields.data[i]->name);
			}
			mia_diag_note(mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0512, node->as.member.name_span, "`%s` has no field `" mia_view_fmt "`",
				mia_checker_name_of(c, target), mia_view_arg(node->as.member.name)), "unknown field"), "fields: %s", fields.length > 0 ? mia_string_cstr(&fields) : "none");
			mia_string_free(&fields);
			return _mia_checker_error(c);
		}
		const mia_program_struct_s* const info = target->as.structure.info;
		if (!info->node->as.structure.is_public && info->module != context->module) {
			mia_diag_note(mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0522, node->as.member.name_span, "field `" mia_view_fmt "` of `%s` is private",
				mia_view_arg(node->as.member.name), mia_checker_name_of(c, target)), "not accessible here"),
				"`" mia_view_fmt "` is not `pub` in `%s`, so its fields are private", mia_view_arg(info->node->as.structure.name), info->module->name);
		}
		node->note.field = field;
		return field->type;
	}
	mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0512, node->as.member.name_span, "`%s` has no fields", mia_checker_name_of(c, target)),
		"`." mia_view_fmt "` on a `%s`", mia_view_arg(node->as.member.name), mia_checker_name_of(c, target));
	return _mia_checker_error(c);
}

static const char* _mia_checker_generics_list(mia_checker_s* const c, const mia_program_function_s* const info) {
	mia_string_s names = {0};
	const mia_ast_nodes_s* const generics = &info->node->as.function.generics;
	for (size_t i = 0; i < generics->count; ++i) {
		if (i > 0) mia_string_append_cstr(&names, ", ");
		mia_string_append(&names, generics->data[i]->as.generic.name);
	}
	const char* const kept = mia_arena_strndup(c->arena, mia_string_cstr(&names), names.length);
	mia_string_free(&names);
	return kept;
}

static const mia_type_s* _mia_checker_specialize(mia_checker_s* const c, mia_ast_node_s* const node, mia_checker_context_s* const context) {
	mia_program_symbol_s* const symbol = mia_checker_qualified(c, node->as.specialize.target, context, true);
	if (NULL == symbol) return _mia_checker_error(c);
	mia_program_function_s* const info = (mia_program_symbol_function == symbol->kind) ? (mia_program_function_s*)symbol : NULL;
	if (NULL == info || !mia_checker_function_is_generic(info)) {
		const mia_view_s text = mia_checker_node_text(c, node->as.specialize.target);
		mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0521, node->span, "`" mia_view_fmt "` is not a generic function", mia_view_arg(text)), "remove the type arguments");
		return _mia_checker_error(c);
	}
	mia_vector_of(const mia_type_s*) arguments = {0};
	bool failed = false;
	for (size_t i = 0; i < node->as.specialize.type_arguments.count; ++i) {
		const mia_type_s* const argument = mia_checker_resolve_type(c, node->as.specialize.type_arguments.data[i], context, false, false);
		failed = failed || _mia_checker_is_error(argument);
		mia_vector_push(&arguments, argument);
	}
	const size_t expected_count = info->node->as.function.generics.count;
	const mia_type_s* result = _mia_checker_error(c);
	if (arguments.count != expected_count) {
		mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0520, node->span, "`" mia_view_fmt "` takes %zu type argument(s), found %zu",
			mia_view_arg(info->node->as.function.name), expected_count, arguments.count), "wrong count");
	}
	else if (!failed) {
		mia_program_instance_s* const instance = mia_checker_instance(c, info, arguments.data, arguments.count, context, node->span);
		if (instance != NULL) {
			node->note.instance = instance;
			mia_checker_plugin_use(c, info, node->span);
			result = mia_program_instance_type(c->program, instance);
		}
	}
	mia_vector_free(&arguments);
	return result;
}

// --- calls ---------------------------------------------------------------------------

static mia_program_instance_s* _mia_checker_call_instance(mia_checker_s* const c, mia_program_function_s* const info, mia_ast_node_s* const node, mia_checker_context_s* const context,
														  const mia_type_s* const expected) {
	const mia_ast_nodes_s* const generics = &info->node->as.function.generics;
	const mia_ast_nodes_s* const arguments = &node->as.call.arguments;
	if (node->as.call.has_type_arguments) {
		if (0 == generics->count) {
			mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0521, node->as.call.callee->span, "`" mia_view_fmt "` is not generic", mia_view_arg(info->node->as.function.name)),
				"remove the type arguments");
			return NULL;
		}
		mia_vector_of(const mia_type_s*) types = {0};
		bool failed = false;
		for (size_t i = 0; i < node->as.call.type_arguments.count; ++i) {
			const mia_type_s* const type = mia_checker_resolve_type(c, node->as.call.type_arguments.data[i], context, false, false);
			failed = failed || _mia_checker_is_error(type);
			mia_vector_push(&types, type);
		}
		mia_program_instance_s* instance = NULL;
		if (types.count != generics->count) {
			mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0520, node->as.call.callee->span, "`" mia_view_fmt "` takes %zu type argument(s), found %zu",
				mia_view_arg(info->node->as.function.name), generics->count, types.count),
				"expected `" mia_view_fmt "<%s>`", mia_view_arg(info->node->as.function.name), _mia_checker_generics_list(c, info));
		}
		else if (!failed) {
			instance = mia_checker_instance(c, info, types.data, types.count, context, node->span);
		}
		mia_vector_free(&types);
		return instance;
	}
	if (0 == generics->count) return mia_checker_instance(c, info, NULL, 0, context, node->span);
	mia_type_list_s parameters = {0};
	const mia_type_s* result = NULL;
	mia_checker_signature(c, info, &parameters, &result);
	mia_type_bindings_s bindings = {0};
	mia_program_instance_s* instance = NULL;
	bool failed = false;
	for (size_t i = 0; i < arguments->count && i < parameters.count && !failed; ++i) {
		mia_ast_node_s* const argument = arguments->data[i];
		mia_ast_node_s* const operand = (mia_ast_node_reference == argument->kind) ? argument->as.reference.operand : argument;
		const mia_type_s* const parameter = parameters.data[i];
		const mia_type_s* const pattern = (mia_type_reference == parameter->kind) ? parameter->as.reference.element : parameter;
		if (!mia_type_contains_parameter(pattern) || _mia_checker_context_typed(operand)) continue;
		if (mia_ast_node_reference == argument->kind && mia_ast_node_name == operand->kind) {
			mia_program_symbol_s* const variable = mia_checker_lookup(c, operand->as.name.name, context, operand->span, false);
			if (variable != NULL && mia_program_symbol_variable == variable->kind) mia_checker_set_discard(&context->unassigned, (mia_program_variable_s*)variable);
		}
		const mia_type_s* const actual = mia_checker_expression(c, operand, context, NULL);
		operand->note.prechecked = true;
		if (_mia_checker_is_error(actual) || mia_type_is_untyped(actual)) continue;
		if (!mia_type_unify(pattern, actual, &bindings)) {
			mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0502, operand->span, "mismatched types in the call of `" mia_view_fmt "`", mia_view_arg(info->node->as.function.name)),
				"expected `%s`, found `%s`", mia_checker_name_of(c, mia_type_substitute(c->types, pattern, &bindings)), mia_checker_name_of(c, actual));
			failed = true;
		}
	}
	if (!failed) {
		if (expected != NULL && !_mia_checker_is_error(expected) && result != NULL && mia_type_contains_parameter(result)) {
			mia_type_bindings_s trial = {0};
			for (size_t i = 0; i < bindings.count; ++i) mia_vector_push(&trial, bindings.data[i]);
			if (mia_type_unify(result, expected, &trial)) {
				mia_vector_free(&bindings);
				bindings = trial;
			}
			else {
				mia_vector_free(&trial);
			}
		}
		for (size_t i = 0; i < arguments->count && i < parameters.count; ++i) {
			mia_ast_node_s* const argument = arguments->data[i];
			mia_ast_node_s* const operand = (mia_ast_node_reference == argument->kind) ? argument->as.reference.operand : argument;
			const mia_type_s* const parameter = parameters.data[i];
			const mia_type_s* const pattern = (mia_type_reference == parameter->kind) ? parameter->as.reference.element : parameter;
			if (pattern->kind != mia_type_parameter || mia_type_bindings_get(&bindings, pattern->as.parameter.name) != NULL) continue;
			char kind = 0;
			if (_mia_checker_context_typed(operand)) kind = _mia_checker_untyped_kind(operand);
			else if (mia_type_is_special(operand->note.type, mia_type_special_untyped_integer)) kind = 'i';
			else if (mia_type_is_special(operand->note.type, mia_type_special_untyped_float)) kind = 'f';
			if ('i' == kind) mia_type_bindings_set(&bindings, pattern->as.parameter.name, mia_checker_scalar_type(c, mia_scalar_int));
			else if ('f' == kind) mia_type_bindings_set(&bindings, pattern->as.parameter.name, mia_checker_scalar_type(c, mia_scalar_float));
		}
		mia_vector_of(const mia_type_s*) types = {0};
		const mia_ast_node_s* missing = NULL;
		for (size_t i = 0; i < generics->count; ++i) {
			const mia_type_s* const bound = mia_type_bindings_get(&bindings, generics->data[i]->as.generic.name);
			if (NULL == bound) {
				if (NULL == missing) missing = generics->data[i];
			}
			else {
				mia_vector_push(&types, bound);
			}
		}
		if (missing != NULL) {
			mia_diag_help(mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0506, node->span, "cannot infer the type argument `" mia_view_fmt "` of `" mia_view_fmt "`",
				mia_view_arg(missing->as.generic.name), mia_view_arg(info->node->as.function.name)), "not determined by the arguments"),
				"write it: `" mia_view_fmt "<%s>(...)`", mia_view_arg(info->node->as.function.name), _mia_checker_generics_list(c, info));
		}
		else {
			instance = mia_checker_instance(c, info, types.data, types.count, context, node->span);
		}
		mia_vector_free(&types);
	}
	mia_vector_free(&bindings);
	mia_vector_free(&parameters);
	return instance;
}

/// checks the values given for '...' and records their types on the call;
/// when the parameter before it is a '[]u8', it is a format string, which
/// must be known at compile time (the plugin's check reads it).
static void _mia_checker_variadic_arguments(mia_checker_s* const c, mia_ast_node_s* const node, const mia_type_s* const* const parameters, const size_t count,
											mia_checker_context_s* const context) {
	const mia_ast_nodes_s* const arguments = &node->as.call.arguments;
	const mia_type_s* const u8_slice = mia_type_slice_of(c->types, mia_checker_scalar_type(c, mia_scalar_u8));
	mia_ast_node_s* format = NULL;
	if (count > 0 && parameters[count - 1] == u8_slice && arguments->count >= count) format = arguments->data[count - 1];
	mia_type_list_s* const types = mia_arena_new(c->arena, mia_type_list_s);
	for (size_t i = count; i < arguments->count; ++i) {
		mia_ast_node_s* const argument = arguments->data[i];
		if (mia_ast_node_reference == argument->kind) {
			mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0519, argument->span, "values for `...` are passed by value"), "remove the `&`");
			_mia_checker_loose_one(c, argument, context);
			mia_arena_push(c->arena, types, _mia_checker_error(c));
			continue;
		}
		mia_arena_push(c->arena, types, mia_checker_settle(c, argument, mia_checker_expression(c, argument, context, NULL)));
	}
	node->note.variadic = types;
	if (NULL == format || mia_ast_node_reference == format->kind) return;
	const mia_type_s* const actual = mia_checker_expression(c, format, context, u8_slice);
	if (mia_type_array == actual->kind && actual->as.array.element == mia_checker_scalar_type(c, mia_scalar_u8)) {
		format->note.format = true;
	}
	else if (!mia_checker_coerce(c, format, actual, u8_slice)) {
		format->note.format = true;
		return;
	}
	format->note.format = true;
	// what the format says is the plugin's to check; it must be known to be checked
	mia_evaluator_value_s value = {0};
	(void)mia_checker_require_constant(c, format, context, "the format string", &value);
}

/// whether 'node' is the bare name of a reference parameter (in parentheses
/// too): passed to a reference parameter, it is passed on as it is ('&' on it
/// is an error).
static bool _mia_checker_names_reference(mia_checker_s* const c, const mia_ast_node_s* const node, mia_checker_context_s* const context) {
	if (mia_ast_node_paren == node->kind) return _mia_checker_names_reference(c, node->as.paren.inner, context);
	if (node->kind != mia_ast_node_name) return false;
	const mia_program_symbol_s* const symbol = mia_checker_lookup(c, node->as.name.name, context, node->span, false);
	return symbol != NULL && mia_program_symbol_variable == symbol->kind && ((const mia_program_variable_s*)symbol)->reference;
}

/// lends argument 'index' of a call: wraps it in '&', as if the call had it.
static mia_ast_node_s* _mia_checker_lend(mia_checker_s* const c, const mia_ast_nodes_s* const arguments, const size_t index) {
	mia_ast_node_s* const reference = mia_ast_node_new(c->arena, mia_ast_node_reference, arguments->data[index]->span);
	reference->as.reference.operand = arguments->data[index];
	arguments->data[index] = reference;
	return reference;
}

/// a note on an argument count error of a piped call ('x ~ f(a)'): the first argument is the piped value.
static void _mia_checker_piped_note(const mia_ast_node_s* const node, mia_diag_entry_s* const diagnostic) {
	if (node->as.call.piped) mia_diag_note(diagnostic, "the first argument is the value piped in with `~`");
}

static void _mia_checker_arguments(mia_checker_s* const c, mia_ast_node_s* const node, const mia_type_s* const* const parameters, const size_t parameter_count,
								   const mia_ast_nodes_s* const declared, mia_checker_context_s* const context, const char* const name) {
	const mia_ast_nodes_s* parameter_nodes = declared;
	size_t count = parameter_count;
	const mia_ast_nodes_s* const all = &node->as.call.arguments;
	size_t argument_count = all->count;
	const bool variadic = count > 0 && mia_type_is_special(parameters[count - 1], mia_type_special_variadic);
	if (variadic) {
		count -= 1;
		if (all->count < count) {
			_mia_checker_piped_note(node, mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0505, node->span, "`%s` takes at least %zu argument(s), found %zu",
				name, count, all->count), "too few arguments"));
			_mia_checker_loose(c, all, 0, context);
			return;
		}
		_mia_checker_variadic_arguments(c, node, parameters, count, context);
		argument_count = count;
		parameter_nodes = NULL;
	}
	size_t required = count;
	if (parameter_nodes != NULL) {
		required = 0;
		for (size_t i = 0; i < parameter_nodes->count; ++i) if (NULL == parameter_nodes->data[i]->as.param.default_value) required += 1;
	}
	if (argument_count > count || argument_count < required) {
		if (required == count) {
			_mia_checker_piped_note(node, mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0505, node->span, "`%s` takes %zu argument(s), found %zu",
				name, count, argument_count), "wrong number of arguments"));
		}
		else {
			_mia_checker_piped_note(node, mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0505, node->span, "`%s` takes %zu to %zu argument(s), found %zu",
				name, required, count, argument_count), "wrong number of arguments"));
		}
		_mia_checker_loose(c, all, 0, context);
		return;
	}
	for (size_t i = 0; i < argument_count; ++i) {
		mia_ast_node_s* argument = all->data[i];
		const mia_type_s* const parameter = parameters[i];
		if (mia_type_reference == parameter->kind) {
			const mia_type_s* const element = parameter->as.reference.element;
			if (mia_ast_node_reference == argument->kind) {
				// '&x': not on a reference parameter, which is passed on as it is
				if (_mia_checker_names_reference(c, argument->as.reference.operand, context)) {
					const mia_view_s text = mia_checker_node_text(c, argument->as.reference.operand);
					mia_diag_help(mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0519, argument->span, "`" mia_view_fmt "` is already a reference",
						mia_view_arg(text)), "remove the `&`"), "a reference parameter is passed on as it is: write `" mia_view_fmt "`", mia_view_arg(text));
				}
			}
			else if (_mia_checker_names_reference(c, argument, context)) {
				// 'f(vector)' where 'vector' is a reference parameter: the same as 'f(&vector)'
				argument = _mia_checker_lend(c, all, i);
			}
			else if (0 == i && node->as.call.piped) {
				// a piped value: a temporary is lent; a variable needs '&' at the head of the pipe
				(void)_mia_checker_checked(c, argument, context, element);
				argument->note.prechecked = true;
				if (_mia_checker_assignable(argument)) {
					const mia_view_s text = mia_checker_node_text(c, argument);
					mia_diag_help(mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0517, argument->span, "this parameter is a reference"),
						"piped into `&...`"), "start the pipe with `&" mia_view_fmt " ~ ...`", mia_view_arg(text));
					continue;
				}
				argument = _mia_checker_lend(c, all, i);
			}
			else {
				const mia_type_s* const actual = _mia_checker_checked(c, argument, context, element);
				mia_checker_coerce(c, argument, actual, element);
				const mia_view_s text = mia_checker_node_text(c, argument);
				mia_diag_help(mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0517, argument->span, "this parameter is a reference"), "expected `&...`"),
					"write `&" mia_view_fmt "`", mia_view_arg(text));
				continue;
			}
			mia_ast_node_s* const operand = argument->as.reference.operand;
			// the function may write to it, so it counts as assigned
			if (mia_ast_node_name == operand->kind) {
				mia_program_symbol_s* const variable = mia_checker_lookup(c, operand->as.name.name, context, operand->span, false);
				if (variable != NULL && mia_program_symbol_variable == variable->kind) mia_checker_set_discard(&context->unassigned, (mia_program_variable_s*)variable);
			}
			const mia_type_s* const actual = _mia_checker_checked(c, operand, context, element);
			_mia_checker_changes(operand);
			argument->note.type = parameter;
			mia_checker_coerce(c, operand, actual, element);
			continue;
		}
		if (mia_ast_node_reference == argument->kind) {
			mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0519, argument->span, "this parameter is not a reference"), "remove the `&`");
			const mia_type_s* const actual = _mia_checker_checked(c, argument->as.reference.operand, context, parameter);
			mia_checker_coerce(c, argument->as.reference.operand, actual, parameter);
			continue;
		}
		if (argument->note.format) continue;
		mia_checker_coerce(c, argument, _mia_checker_checked(c, argument, context, parameter), parameter);
	}
}

static const mia_type_s* _mia_checker_call(mia_checker_s* const c, mia_ast_node_s* const node, mia_checker_context_s* const context, const mia_type_s* const expected) {
	mia_ast_node_s* const callee = node->as.call.callee;
	const mia_ast_nodes_s* const arguments = &node->as.call.arguments;
	mia_program_symbol_s* symbol = NULL;
	if (mia_ast_node_name == callee->kind) {
		symbol = mia_checker_lookup(c, callee->as.name.name, context, callee->span, true);
		if (NULL == symbol) {
			_mia_checker_loose(c, arguments, 0, context);
			return _mia_checker_error(c);
		}
	}
	else if (mia_ast_node_member == callee->kind && (mia_ast_node_name == callee->as.member.target->kind || mia_ast_node_member == callee->as.member.target->kind)) {
		mia_program_symbol_s* const base = mia_checker_qualified(c, callee->as.member.target, context, false);
		if (_mia_checker_is_module_like(base)) {
			symbol = mia_checker_member_of(c, base, callee->as.member.name, callee->as.member.name_span, context);
			if (NULL == symbol) {
				_mia_checker_loose(c, arguments, 0, context);
				return _mia_checker_error(c);
			}
		}
	}
	if (symbol != NULL && mia_program_symbol_function == symbol->kind) {
		mia_program_instance_s* const instance = _mia_checker_call_instance(c, (mia_program_function_s*)symbol, node, context, expected);
		if (NULL == instance) {
			_mia_checker_loose(c, arguments, 0, context);
			return _mia_checker_error(c);
		}
		callee->note.type = mia_program_instance_type(c->program, instance);
		callee->note.instance = instance;
		node->note.instance = instance;
		_mia_checker_arguments(c, node, instance->parameters.data, instance->parameters.count, &instance->node->as.function.parameters, context,
			mia_checker_instance_name(c, instance));
		if (mia_program_function_is_plugin(instance->function)) mia_checker_plugin_check_call(c, node, instance);
		return (instance->result != NULL) ? instance->result : mia_checker_special_type(c, mia_type_special_nothing);
	}
	if (symbol != NULL && mia_program_symbol_struct == symbol->kind) {
		const mia_program_struct_s* const info = (const mia_program_struct_s*)symbol;
		mia_diag_help(mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0504, callee->span, "`" mia_view_fmt "` is a struct, not a function", mia_view_arg(info->node->as.structure.name)),
			"cannot be called"), "create a value with a literal: `(" mia_view_fmt "){ ... }`", mia_view_arg(info->node->as.structure.name));
		_mia_checker_loose(c, arguments, 0, context);
		return _mia_checker_error(c);
	}
	if (node->as.call.has_type_arguments) {
		const mia_view_s text = mia_checker_node_text(c, callee);
		mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0521, callee->span, "`" mia_view_fmt "` is not a generic function", mia_view_arg(text)),
			"only generic functions take type arguments");
	}
	const mia_type_s* const callee_type = (symbol != NULL) ? mia_checker_symbol_value(c, symbol, callee, context, NULL) : mia_checker_expression(c, callee, context, NULL);
	callee->note.type = callee_type;
	if (_mia_checker_is_error(callee_type)) {
		_mia_checker_loose(c, arguments, 0, context);
		return _mia_checker_error(c);
	}
	if (callee_type->kind != mia_type_function) {
		const mia_view_s text = mia_checker_node_text(c, callee);
		mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0504, callee->span, "`" mia_view_fmt "` is not a function", mia_view_arg(text)), "it is a `%s`", mia_checker_name_of(c, callee_type));
		_mia_checker_loose(c, arguments, 0, context);
		return _mia_checker_error(c);
	}
	const mia_view_s text = mia_checker_node_text(c, callee);
	_mia_checker_arguments(c, node, callee_type->as.function.parameters, callee_type->as.function.count, NULL, context, mia_checker_view_cstring(c, text));
	return (callee_type->as.function.result != NULL) ? callee_type->as.function.result : mia_checker_special_type(c, mia_type_special_nothing);
}

// --- operators -------------------------------------------------------------------------

static void _mia_checker_check_overflow(mia_checker_s* const c, mia_ast_node_s* const node, const mia_ast_operator_e operation, mia_ast_node_s* const left_node,
										mia_ast_node_s* const right_node, const mia_type_s* const type, mia_checker_context_s* const context) {
	if (!mia_type_is_integer(type) || mia_type_scalar != type->kind) return;
	mia_evaluator_value_s left = {0}, right = {0};
	// both are probed, even when the left one is unknown (probes count as evaluation steps)
	const bool left_known = mia_checker_try_value(c, left_node, &left);
	const bool right_known = mia_checker_try_value(c, right_node, &right);
	if (!left_known || !right_known) return;
	if (left.kind != mia_evaluator_value_integer || right.kind != mia_evaluator_value_integer) return;
	if ((mia_ast_operator_divide == operation || mia_ast_operator_remainder == operation) && mia_int128_is_zero(right.as.integer)) {
		mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0704, node->span, "division by zero"), "the divisor is zero at compile time");
		return;
	}
	if (context->wrap || context->wrapping > 0 || (_mia_checker_is_shift(operation) && mia_int128_negative(right.as.integer))) return;
	mia_int128_s exact = mia_int128_zero();
	bool overflow = false;
	if (!mia_evaluator_integer_arithmetic(operation, left.as.integer, right.as.integer, &exact, &overflow)) return;
	if (overflow || !mia_type_holds(type, exact)) {
		const mia_view_s text = mia_checker_node_text(c, node);
		mia_string_s wrapped = {0};
		mia_format_int128(&wrapped, mia_evaluator_integer_wrap(exact, type));
		mia_diag_help(mia_diag_label_main(mia_diag_warning(c->diag, mia_diag_W0701, node->span, "`" mia_view_fmt "` overflows `%s`", mia_view_arg(text), mia_checker_name_of(c, type)),
			"wraps around to %s", mia_string_cstr(&wrapped)), "mark the statement or declaration `[wrap]` if the wrap-around is intended");
		mia_string_free(&wrapped);
	}
}

const mia_type_s* mia_checker_binary_operation(mia_checker_s* const c, mia_ast_node_s* const node, const mia_ast_operator_e operation, mia_ast_node_s* const left_node,
											   const mia_type_s* const left_type, mia_ast_node_s* const right_node, const mia_type_s* const right_type, mia_checker_context_s* const context,
											   const mia_span_s span) {
	const mia_type_s* left = left_type;
	const mia_type_s* right = right_type;
	if (_mia_checker_is_error(left) || _mia_checker_is_error(right)) return _mia_checker_error(c);
	const mia_type_s* const untyped_float = mia_checker_special_type(c, mia_type_special_untyped_float);
	if (mia_type_is_untyped(left) && mia_type_is_untyped(right)) {
		const bool any_float = left == untyped_float || right == untyped_float;
		if (_mia_checker_is_equality(operation) || _mia_checker_is_ordering(operation)) {
			// two known integer constants are compared exactly (they may not fit in `int`): the comparison becomes its result
			mia_evaluator_value_s a = {0}, b = {0};
			if (!any_float && mia_ast_node_binary == node->kind && mia_checker_try_value(c, left_node, &a) && mia_checker_try_value(c, right_node, &b)
					&& mia_evaluator_value_integer == a.kind && mia_evaluator_value_integer == b.kind) {
				const int order = mia_int128_compare(a.as.integer, b.as.integer);
				bool result = false;
				switch (operation) {
					case mia_ast_operator_equal:         result = 0 == order; break;
					case mia_ast_operator_not_equal:     result = 0 != order; break;
					case mia_ast_operator_less:          result = order < 0; break;
					case mia_ast_operator_greater:       result = order > 0; break;
					case mia_ast_operator_less_equal:    result = order <= 0; break;
					default:                             result = order >= 0; break;
				}
				node->kind = mia_ast_node_bool;
				node->as.boolean.value = result;
				return _mia_checker_bool(c);
			}
			mia_checker_finalize(c, left_node, mia_checker_scalar_type(c, any_float ? mia_scalar_float : mia_scalar_int));
			mia_checker_finalize(c, right_node, mia_checker_scalar_type(c, any_float ? mia_scalar_float : mia_scalar_int));
			return _mia_checker_bool(c);
		}
		if ((_mia_checker_is_bitwise(operation) || _mia_checker_is_shift(operation) || mia_ast_operator_remainder == operation) && any_float) {
			mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0507, span, "`%s` needs integers", mia_ast_operator_text(operation)), "a float operand");
			return _mia_checker_error(c);
		}
		if ((mia_ast_operator_divide == operation || mia_ast_operator_remainder == operation) && !any_float) {
			mia_evaluator_value_s divisor = {0};
			if (mia_checker_try_value(c, right_node, &divisor) && mia_evaluator_value_integer == divisor.kind && mia_int128_is_zero(divisor.as.integer)) {
				mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0704, node->span, "division by zero"), "the divisor is zero at compile time");
				return _mia_checker_error(c);
			}
		}
		return any_float ? untyped_float : mia_checker_special_type(c, mia_type_special_untyped_integer);
	}
	const mia_type_s* const untyped_integer = mia_checker_special_type(c, mia_type_special_untyped_integer);
	if (mia_type_is_untyped(left) && _mia_checker_accepts_literal(right, left)) {
		mia_checker_coerce(c, left_node, left, right);
		left = right;
	}
	else if (mia_type_is_untyped(right) && _mia_checker_is_shift(operation) && right == untyped_integer && mia_type_scalar == left->kind
			&& 'i' == mia_scalar_kind(left->as.scalar)) {
		mia_checker_coerce(c, right_node, right, mia_checker_scalar_type(c, mia_scalar_uint));
		right = mia_checker_scalar_type(c, mia_scalar_uint);
	}
	else if (mia_type_is_untyped(right) && _mia_checker_accepts_literal(left, right)) {
		mia_checker_coerce(c, right_node, right, left);
		right = left;
	}
	if (mia_type_scalar == left->kind && mia_type_scalar == right->kind) {
		if (_mia_checker_is_arithmetic(operation) && left == right && mia_type_is_numeric(left) && (operation != mia_ast_operator_remainder || mia_type_is_integer(left))) {
			_mia_checker_check_overflow(c, node, operation, left_node, right_node, left, context);
			return left;
		}
		if (_mia_checker_is_bitwise(operation) && left == right && mia_type_is_integer(left)) return left;
		if (_mia_checker_is_shift(operation) && mia_type_is_integer(left) && mia_type_is_integer(right)) {
			_mia_checker_check_overflow(c, node, operation, left_node, right_node, left, context);
			return left;
		}
		if (_mia_checker_is_equality(operation) && left == right) return _mia_checker_bool(c);
		if (_mia_checker_is_ordering(operation) && left == right && mia_type_is_numeric(left)) return _mia_checker_bool(c);
	}
	if (mia_type_function == left->kind && left == right && _mia_checker_is_equality(operation)) return _mia_checker_bool(c);
	mia_diag_entry_s* const diagnostic = mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0507, span, "cannot apply `%s` to `%s` and `%s`", mia_ast_operator_text(operation),
		mia_checker_name_of(c, left), mia_checker_name_of(c, right)), "no such operation");
	if (mia_type_is_numeric(left) && mia_type_is_numeric(right)) {
		const mia_view_s text = mia_checker_node_text(c, right_node);
		mia_diag_help(diagnostic, "mixed types need an explicit cast, for example `(%s)" mia_view_fmt "`", mia_checker_name_of(c, left), mia_view_arg(text));
	}
	else if (mia_type_struct == left->kind || mia_type_struct == right->kind) {
		mia_diag_help(diagnostic, "operators apply to numbers and `bool`; call a function for structs");
	}
	else if (mia_type_slice == left->kind || mia_type_slice == right->kind) {
		mia_diag_help(diagnostic, "operators do not apply to slices; `lenof(s) == 0` tests for an empty one");
	}
	else if (mia_type_is_untyped(left) || mia_type_is_untyped(right)) {
		mia_diag_help(diagnostic, "a float literal cannot become an integer");
	}
	return _mia_checker_error(c);
}

// --- indexing --------------------------------------------------------------------------

static void _mia_checker_index_value(mia_checker_s* const c, mia_ast_node_s* const node, mia_checker_context_s* const context, const mia_type_s* const target, const bool end) {
	const mia_type_s* const type = mia_checker_expression(c, node, context, NULL);
	if (_mia_checker_is_error(type)) return;
	if (mia_type_is_special(type, mia_type_special_untyped_integer)) {
		(void)mia_checker_settle(c, node, type);
	}
	else if (!(mia_type_scalar == type->kind && 'i' == mia_scalar_kind(type->as.scalar))) {
		mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0502, node->span, "an index must be an integer"), "found `%s`", mia_checker_name_of(c, type));
		return;
	}
	mia_evaluator_value_s value = {0};
	if (mia_checker_try_value(c, node, &value) && mia_evaluator_value_integer == value.kind) {
		const mia_int128_s index = value.as.integer;
		const bool array = mia_type_array == target->kind;
		const int64_t limit = array ? target->as.array.length : 0;
		const int order = mia_int128_compare(index, mia_int128_from_int(limit));
		if (mia_int128_negative(index) || (array && (end ? order > 0 : order >= 0))) {
			if (array) {
				mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0706, node->span, "index %lld is out of bounds", (long long)mia_int128_to_int(index)), "the array has %lld elements", (long long)limit);
			}
			else {
				mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0706, node->span, "index %lld is out of bounds", (long long)mia_int128_to_int(index)), "indexes cannot be negative");
			}
		}
	}
}

static const mia_type_s* _mia_checker_index(mia_checker_s* const c, mia_ast_node_s* const node, mia_checker_context_s* const context) {
	const mia_type_s* const target = mia_checker_expression(c, node->as.index.target, context, NULL);
	if (_mia_checker_is_error(target)) {
		(void)mia_checker_expression(c, node->as.index.index, context, NULL);
		return target;
	}
	if (target->kind != mia_type_array && target->kind != mia_type_slice) {
		mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0513, node->as.index.target->span, "`%s` cannot be indexed", mia_checker_name_of(c, target)), "not an array or a slice");
		(void)mia_checker_expression(c, node->as.index.index, context, NULL);
		return _mia_checker_error(c);
	}
	_mia_checker_index_value(c, node->as.index.index, context, target, false);
	return (mia_type_array == target->kind) ? target->as.array.element : target->as.slice.element;
}

static const mia_type_s* _mia_checker_slice_of(mia_checker_s* const c, mia_ast_node_s* const node, mia_checker_context_s* const context) {
	const mia_type_s* const target = mia_checker_expression(c, node->as.slice_of.target, context, NULL);
	if (_mia_checker_is_error(target)) return target;
	if (target->kind != mia_type_array && target->kind != mia_type_slice) {
		mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0513, node->as.slice_of.target->span, "`%s` cannot be sliced", mia_checker_name_of(c, target)), "not an array or a slice");
		return _mia_checker_error(c);
	}
	if (node->as.slice_of.low != NULL) _mia_checker_index_value(c, node->as.slice_of.low, context, target, true);
	if (node->as.slice_of.high != NULL) _mia_checker_index_value(c, node->as.slice_of.high, context, target, !node->as.slice_of.inclusive);
	const mia_type_s* const element = (mia_type_array == target->kind) ? target->as.array.element : target->as.slice.element;
	return mia_type_slice_of(c->types, element);
}

// --- operators on nodes ----------------------------------------------------------------

static const mia_type_s* _mia_checker_unary(mia_checker_s* const c, mia_ast_node_s* const node, mia_checker_context_s* const context, const mia_type_s* const expected) {
	const mia_ast_operator_e operation = node->as.unary.operation;
	mia_ast_node_s* const operand_node = node->as.unary.operand;
	if (mia_ast_operator_negate == operation && mia_ast_node_integer == operand_node->kind) operand_node->note.negative = true;
	const mia_type_s* const operand = mia_checker_expression(c, operand_node, context, mia_ast_operator_not == operation ? _mia_checker_bool(c) : expected);
	if (_mia_checker_is_error(operand)) return operand;
	if (mia_ast_operator_not == operation && operand == _mia_checker_bool(c)) return operand;
	if (mia_ast_operator_negate == operation && (mia_type_is_untyped(operand)
			|| (mia_type_scalar == operand->kind && (mia_type_is_float(operand) || (mia_type_is_integer(operand) && mia_type_is_signed(operand)))))) {
		return operand;
	}
	if (mia_ast_operator_bit_not == operation && (mia_type_is_special(operand, mia_type_special_untyped_integer)
			|| (mia_type_scalar == operand->kind && 'i' == mia_scalar_kind(operand->as.scalar)))) {
		return operand;
	}
	mia_diag_entry_s* const diagnostic = mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0507, node->span, "cannot apply unary `%s` to `%s`", mia_ast_operator_text(operation),
		mia_checker_name_of(c, operand)), "no such operation");
	if (mia_ast_operator_negate == operation && mia_type_scalar == operand->kind && 'i' == mia_scalar_kind(operand->as.scalar)) {
		mia_diag_help(diagnostic, "unsigned values cannot be negated; cast to a signed type first");
	}
	return _mia_checker_error(c);
}

static const mia_type_s* _mia_checker_binary(mia_checker_s* const c, mia_ast_node_s* const node, mia_checker_context_s* const context) {
	const mia_ast_operator_e operation = node->as.binary.operation;
	if (mia_ast_operator_and == operation || mia_ast_operator_or == operation) {
		mia_ast_node_s* const sides[2] = { node->as.binary.left, node->as.binary.right };
		for (size_t i = 0; i < 2; ++i) mia_checker_coerce(c, sides[i], mia_checker_expression(c, sides[i], context, _mia_checker_bool(c)), _mia_checker_bool(c));
		return _mia_checker_bool(c);
	}
	const mia_type_s* const left = mia_checker_expression(c, node->as.binary.left, context, NULL);
	const mia_type_s* const right = mia_checker_expression(c, node->as.binary.right, context, NULL);
	return mia_checker_binary_operation(c, node, operation, node->as.binary.left, left, node->as.binary.right, right, context, node->as.binary.operation_span);
}

static const mia_type_s* _mia_checker_cast(mia_checker_s* const c, mia_ast_node_s* const node, mia_checker_context_s* const context) {
	const mia_type_s* const target = mia_checker_resolve_type(c, node->as.cast.type, context, false, false);
	mia_ast_node_s* const operand_node = node->as.cast.operand;
	const mia_type_s* operand = mia_checker_expression(c, operand_node, context, NULL);
	if (_mia_checker_is_error(operand) || _mia_checker_is_error(target)) return target;
	if (mia_type_is_untyped(operand)) {
		mia_evaluator_value_s value = {0};
		const bool known = mia_checker_try_value(c, operand_node, &value);
		const bool integer = known && mia_evaluator_value_integer == value.kind;
		if (mia_type_scalar == target->kind && _mia_checker_accepts_literal(target, operand)
				&& (!integer || mia_type_is_float(target) || mia_type_holds(target, value.as.integer))) {
			mia_checker_finalize(c, operand_node, target);
			operand = target;
		}
		else {
			operand = mia_checker_settle(c, operand_node, operand);
		}
	}
	if (mia_type_scalar == target->kind && mia_type_scalar == operand->kind && (target == operand || (mia_type_is_numeric(target) && mia_type_is_numeric(operand)))) {
		return target;
	}
	mia_diag_help(mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0510, node->span, "cannot cast `%s` to `%s`", mia_checker_name_of(c, operand), mia_checker_name_of(c, target)), "invalid cast"),
		"casts convert between number types");
	return target;
}

// --- literals of structs and arrays ------------------------------------------------------

static const mia_type_s* _mia_checker_composite_of(mia_checker_s* const c, mia_ast_node_s* const node, const mia_type_s* const target, mia_checker_context_s* const context) {
	const mia_ast_nodes_s* const entries = &node->as.composite.entries;
	const bool named = entries->count > 0 && entries->data[0]->as.entry.named;
	if (0 == entries->count) {
		mia_type_list_s seen = {0};
		const bool zeroable = mia_checker_zeroable(c, target, &seen);
		mia_vector_free(&seen);
		if (!zeroable) {
			mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0526, node->span, "`%s` has no zero value", mia_checker_name_of(c, target)), "a function value cannot be empty");
			return _mia_checker_error(c);
		}
		if (mia_type_array == target->kind && target->as.array.length < 0) {
			mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0506, node->span, "`[_]` cannot take its length from `{}`"), "no elements");
			return _mia_checker_error(c);
		}
		return target;
	}
	if (mia_type_struct == target->kind) {
		if (!named) {
			mia_diag_help(mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0525, node->span, "struct literals need field names"), "positional values"),
				"write `{ field: value, ... }`");
			for (size_t i = 0; i < entries->count; ++i) _mia_checker_loose_one(c, entries->data[i]->as.entry.value, context);
			return target;
		}
		mia_program_layout_s* const layout = mia_checker_layout(c, target);
		const mia_program_struct_s* const info = target->as.structure.info;
		if (!info->node->as.structure.is_public && info->module != context->module) {
			mia_diag_note(mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0522, node->span, "the fields of `%s` are private", mia_checker_name_of(c, target)), "cannot be initialized here"),
				"`" mia_view_fmt "` is not `pub` in `%s`", mia_view_arg(info->node->as.structure.name), info->module->name);
		}
		mia_vector_of(mia_view_s) given = {0};
		for (size_t i = 0; i < entries->count; ++i) {
			mia_ast_node_s* const entry = entries->data[i];
			if (!entry->as.entry.named) {
				// a positional entry among named ones (reported by the parser).
				_mia_checker_loose_one(c, entry->as.entry.value, context);
				continue;
			}
			mia_program_field_s* const field = mia_checker_layout_field(layout, entry->as.entry.name);
			if (NULL == field) {
				mia_string_s fields = {0};
				for (size_t f = 0; f < layout->fields.count; ++f) {
					if (f > 0) mia_string_append_cstr(&fields, ", ");
					mia_string_append(&fields, layout->fields.data[f]->name);
				}
				mia_diag_note(mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0512, entry->span, "`%s` has no field `" mia_view_fmt "`", mia_checker_name_of(c, target), mia_view_arg(entry->as.entry.name)),
					"unknown field"), "fields: %s", mia_string_cstr(&fields));
				mia_string_free(&fields);
				_mia_checker_loose_one(c, entry->as.entry.value, context);
				continue;
			}
			bool repeated = false;
			for (size_t g = 0; g < given.count; ++g) repeated = repeated || mia_view_eq(given.data[g], entry->as.entry.name);
			if (repeated) {
				mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0525, entry->span, "field `" mia_view_fmt "` is given twice", mia_view_arg(entry->as.entry.name)), "repeated");
				continue;
			}
			mia_vector_push(&given, entry->as.entry.name);
			mia_checker_coerce(c, entry->as.entry.value, mia_checker_expression(c, entry->as.entry.value, context, field->type), field->type);
		}
		for (size_t f = 0; f < layout->fields.count; ++f) {
			const mia_program_field_s* const field = layout->fields.data[f];
			bool present = field->default_value != NULL;
			for (size_t g = 0; g < given.count; ++g) present = present || mia_view_eq(given.data[g], field->name);
			if (present) continue;
			mia_type_list_s seen = {0};
			const bool zeroable = mia_checker_zeroable(c, field->type, &seen);
			mia_vector_free(&seen);
			if (!zeroable) {
				mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0526, node->span, "field `" mia_view_fmt "` must be given", mia_view_arg(field->name)),
					"a function value has no zero value");
			}
			else {
				mia_diag_help(mia_diag_label_main(mia_diag_warning(c->diag, mia_diag_W0504, node->span, "field `" mia_view_fmt "` is not initialized", mia_view_arg(field->name)),
					"it will be zero"), "give it a value, or a default in `" mia_view_fmt "`", mia_view_arg(info->node->as.structure.name));
			}
		}
		mia_vector_free(&given);
		return target;
	}
	if (mia_type_array == target->kind || mia_type_slice == target->kind) {
		if (named) {
			mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0525, node->span, "array literals take values without names"), "named entries");
			for (size_t i = 0; i < entries->count; ++i) _mia_checker_loose_one(c, entries->data[i]->as.entry.value, context);
			return target;
		}
		const mia_type_s* const element = (mia_type_array == target->kind) ? target->as.array.element : target->as.slice.element;
		for (size_t i = 0; i < entries->count; ++i) {
			mia_ast_node_s* const value = entries->data[i]->as.entry.value;
			mia_checker_coerce(c, value, mia_checker_expression(c, value, context, element), element);
		}
		if (mia_type_array == target->kind) {
			if (target->as.array.length < 0) return mia_type_array_of(c->types, element, (int64_t)entries->count);
			if ((int64_t)entries->count != target->as.array.length) {
				mia_diag_help(mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0525, node->span, "expected %lld elements, found %zu", (long long)target->as.array.length, entries->count),
					"for `%s`", mia_checker_name_of(c, target)), "write `[_]` in the type to take the length from the literal");
			}
		}
		return target;
	}
	mia_diag_help(mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0525, node->span, "a `%s` cannot be written as `{ ... }`", mia_checker_name_of(c, target)), "only `{}` (zero) is allowed"),
		"`{ ... }` with values is for structs, arrays and slices");
	return _mia_checker_error(c);
}

static const mia_type_s* _mia_checker_composite(mia_checker_s* const c, mia_ast_node_s* const node, mia_checker_context_s* const context, const mia_type_s* const expected) {
	const mia_type_s* target = expected;
	if (node->as.composite.type != NULL) target = mia_checker_resolve_type(c, node->as.composite.type, context, false, true);
	if (NULL == target || _mia_checker_is_error(target)) {
		if (NULL == target) {
			mia_diag_help(mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0506, node->span, "cannot infer the type of this literal"), "no type known here"),
				"write the type, `(T){ ... }`, or give the variable a type");
		}
		for (size_t i = 0; i < node->as.composite.entries.count; ++i) _mia_checker_loose_one(c, node->as.composite.entries.data[i]->as.entry.value, context);
		return _mia_checker_error(c);
	}
	return _mia_checker_composite_of(c, node, target, context);
}

// --- lambdas and intrinsics ---------------------------------------------------------------

static const mia_type_s* _mia_checker_lambda(mia_checker_s* const c, mia_ast_node_s* const node, mia_checker_context_s* const context) {
	mia_checker_context_s* const inner = mia_checker_context_new(c, context->module, &context->bindings, NULL, context, NULL, true);
	mia_type_list_s parameters = {0};
	const mia_ast_nodes_s* const nodes = &node->as.lambda.parameters;
	for (size_t i = 0; i < nodes->count; ++i) {
		const mia_ast_node_s* const parameter = nodes->data[i];
		if (parameter->as.param.variadic) {
			mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0415, parameter->span, "lambdas cannot take `...`"), "variadic parameter");
			mia_vector_push(&parameters, _mia_checker_error(c));
			continue;
		}
		const mia_type_s* const type = mia_checker_resolve_type(c, parameter->as.param.type, inner, true, false);
		if (parameter->as.param.default_value != NULL) {
			mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0412, parameter->as.param.default_value->span, "lambda parameters cannot have defaults"), "remove the default");
		}
		mia_vector_push(&parameters, type);
	}
	const mia_type_s* const result = (node->as.lambda.result != NULL) ? mia_checker_resolve_type(c, node->as.lambda.result, inner, false, false) : NULL;
	inner->result = result;
	for (size_t i = 0; i < nodes->count; ++i) {
		mia_ast_node_s* const parameter = nodes->data[i];
		const mia_type_s* const type = parameters.data[i];
		const bool reference = mia_type_reference == type->kind;
		mia_program_variable_s* const variable = mia_arena_new(c->arena, mia_program_variable_s);
		variable->base.kind = mia_program_symbol_variable;
		variable->name = parameter->as.param.name;
		variable->type = reference ? type->as.reference.element : type;
		variable->kind = mia_program_variable_param;
		variable->span = parameter->span;
		variable->reference = reference;
		variable->level = inner->level;
		parameter->note.variable = variable;
		(void)mia_checker_context_declare(inner, parameter->as.param.name, &variable->base);
		mia_vector_push(&inner->variables, variable);
	}
	mia_checker_block(c, node->as.lambda.body, inner, false);
	if (result != NULL && !mia_checker_terminates(node->as.lambda.body)) {
		mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0514, node->span, "this lambda does not return a value on every path"), "must return `%s`", mia_checker_name_of(c, result));
	}
	const mia_type_s* const type = mia_type_function_of(c->types, parameters.data, parameters.count, result);
	mia_vector_free(&parameters);
	return type;
}

static const mia_type_s* _mia_checker_assert(mia_checker_s* const c, mia_ast_node_s* const node, mia_checker_context_s* const context) {
	mia_ast_node_s* const condition = node->as.assert_statement.condition;
	const mia_type_s* const actual = mia_checker_expression(c, condition, context, _mia_checker_bool(c));
	mia_checker_coerce(c, condition, actual, _mia_checker_bool(c));
	mia_ast_node_s* const message = node->as.assert_statement.message;
	if (message != NULL) {
		if (message->kind != mia_ast_node_string) {
			mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0502, message->span, "the message of `assert` must be a string literal"), "not a string literal");
		}
		else {
			message->note.type = mia_type_array_of(c->types, mia_checker_scalar_type(c, mia_scalar_u8), (int64_t)message->as.string.data.length);
		}
	}
	mia_evaluator_value_s value = {0};
	if (actual == _mia_checker_bool(c) && mia_checker_try_value(c, condition, &value) && mia_evaluator_value_boolean == value.kind && !value.as.boolean) {
		mia_diag_entry_s* const diagnostic = mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0703, condition->span, "assertion is always false"), "checked at compile time");
		if (message != NULL && mia_ast_node_string == message->kind) mia_diag_note(diagnostic, mia_view_fmt, mia_view_arg(message->as.string.data));
	}
	return mia_checker_special_type(c, mia_type_special_nothing);
}

// --- the dispatcher ------------------------------------------------------------------------

const mia_type_s* mia_checker_expression(mia_checker_s* const c, mia_ast_node_s* const node, mia_checker_context_s* const context, const mia_type_s* const expected) {
	const mia_type_s* type = NULL;
	switch (node->kind) {
		case mia_ast_node_integer: type = _mia_checker_integer(c, node); break;
		case mia_ast_node_float:
			type = (node->as.floating.suffix != mia_scalar_none) ? mia_checker_scalar_type(c, node->as.floating.suffix) : mia_checker_special_type(c, mia_type_special_untyped_float);
			break;
		case mia_ast_node_char:   type = mia_checker_scalar_type(c, mia_scalar_u8); break;
		case mia_ast_node_string: type = mia_type_array_of(c->types, mia_checker_scalar_type(c, mia_scalar_u8), (int64_t)node->as.string.data.length); break;
		case mia_ast_node_bool:   type = _mia_checker_bool(c); break;
		case mia_ast_node_paren:  type = mia_checker_expression(c, node->as.paren.inner, context, expected); break;
		case mia_ast_node_name:   type = _mia_checker_name(c, node, context, expected); break;
		case mia_ast_node_member: type = _mia_checker_member(c, node, context, expected); break;
		case mia_ast_node_specialize: type = _mia_checker_specialize(c, node, context); break;
		case mia_ast_node_call:   type = _mia_checker_call(c, node, context, expected); break;
		case mia_ast_node_index:  type = _mia_checker_index(c, node, context); break;
		case mia_ast_node_slice_of: type = _mia_checker_slice_of(c, node, context); break;
		case mia_ast_node_unary:  type = _mia_checker_unary(c, node, context, expected); break;
		case mia_ast_node_binary: type = _mia_checker_binary(c, node, context); break;
		case mia_ast_node_cast:   type = _mia_checker_cast(c, node, context); break;
		case mia_ast_node_reference:
			mia_diag_help(mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0519, node->span, "`&` is only allowed on a call argument"), "not an argument of a reference parameter"),
				"references are passed to functions; use a slice to keep a view of an array");
			_mia_checker_loose_one(c, node->as.reference.operand, context);
			type = _mia_checker_error(c);
			break;
		case mia_ast_node_composite: type = _mia_checker_composite(c, node, context, expected); break;
		case mia_ast_node_lambda: type = _mia_checker_lambda(c, node, context); break;
		case mia_ast_node_sizeof: {
			const mia_type_s* const resolved = mia_checker_resolve_type(c, node->as.size_of.type, context, false, false);
			node->note.size = _mia_checker_is_error(resolved) ? 0 : mia_checker_size_of(c, resolved, NULL);
			type = mia_checker_scalar_type(c, mia_scalar_uint);
			break;
		}
		case mia_ast_node_lenof: {
			const mia_type_s* const operand = mia_checker_expression(c, node->as.lenof.operand, context, NULL);
			if (!_mia_checker_is_error(operand) && operand->kind != mia_type_array && operand->kind != mia_type_slice) {
				mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0507, node->as.lenof.operand->span, "`lenof` needs an array or a slice, found `%s`", mia_checker_name_of(c, operand)),
					"no length");
			}
			type = mia_checker_scalar_type(c, mia_scalar_uint);
			break;
		}
		case mia_ast_node_assert: type = _mia_checker_assert(c, node, context); break;
		default: mia_unreachable();
	}
	node->note.type = type;
	return type;
}

// --- conversions ------------------------------------------------------------------------------

void mia_checker_finalize(mia_checker_s* const c, mia_ast_node_s* const node, const mia_type_s* const target) {
	const mia_type_s* const current = node->note.type;
	if (current != NULL && !mia_type_is_untyped(current)) return;
	// an untyped operation is a constant, exact in 128 bits (or a double): it becomes a literal
	// of its value, so its operands are not computed in 'target', where they may not fit
	const bool computed = mia_ast_node_paren == node->kind || mia_ast_node_unary == node->kind || mia_ast_node_binary == node->kind;
	mia_evaluator_value_s value = {0};
	if (current != NULL && computed && mia_checker_try_value(c, node, &value)) {
		if (mia_evaluator_value_integer == value.kind) {
			node->kind = mia_ast_node_integer;
			node->as.integer.value = value.as.integer;
			node->as.integer.suffix = mia_scalar_none;
			node->note.type = target;
			return;
		}
		if (mia_evaluator_value_real == value.kind) {
			node->kind = mia_ast_node_float;
			node->as.floating.value = value.as.real;
			node->as.floating.suffix = mia_scalar_none;
			node->note.type = target;
			return;
		}
	}
	node->note.type = target;
	if (mia_ast_node_paren == node->kind) {
		mia_checker_finalize(c, node->as.paren.inner, target);
	}
	else if (mia_ast_node_unary == node->kind) {
		mia_checker_finalize(c, node->as.unary.operand, target);
	}
	else if (mia_ast_node_binary == node->kind) {
		const mia_ast_operator_e operation = node->as.binary.operation;
		if (!_mia_checker_is_equality(operation) && !_mia_checker_is_ordering(operation) && operation != mia_ast_operator_and && operation != mia_ast_operator_or) {
			mia_checker_finalize(c, node->as.binary.left, target);
			mia_checker_finalize(c, node->as.binary.right, target);
		}
	}
}

const mia_type_s* mia_checker_settle(mia_checker_s* const c, mia_ast_node_s* const node, const mia_type_s* const actual) {
	if (mia_type_is_special(actual, mia_type_special_untyped_integer)) {
		const mia_type_s* const integer = mia_checker_scalar_type(c, mia_scalar_int);
		mia_evaluator_value_s value = {0};
		if (mia_checker_try_value(c, node, &value) && mia_evaluator_value_integer == value.kind
				&& !mia_type_holds(integer, value.as.integer)) {
			mia_string_s text = {0};
			mia_format_int128(&text, value.as.integer);
			mia_diag_help(mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0511, node->span, "`%s` does not fit in `int`", mia_string_cstr(&text)), "too large for the default type"),
				"give it a type, for example with the suffix `u64`");
			mia_string_free(&text);
		}
		mia_checker_finalize(c, node, integer);
		return integer;
	}
	if (mia_type_is_special(actual, mia_type_special_untyped_float)) {
		mia_checker_finalize(c, node, mia_checker_scalar_type(c, mia_scalar_float));
		return mia_checker_scalar_type(c, mia_scalar_float);
	}
	if (mia_type_is_special(actual, mia_type_special_nothing)) {
		mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0523, node->span, "this has no value"), "the function returns nothing");
		return _mia_checker_error(c);
	}
	return actual;
}

bool mia_checker_coerce(mia_checker_s* const c, mia_ast_node_s* const node, const mia_type_s* const actual, const mia_type_s* const target) {
	if (NULL == target || _mia_checker_is_error(actual) || _mia_checker_is_error(target) || actual == target) return true;
	if (_mia_checker_accepts_literal(target, actual)) {
		mia_evaluator_value_s value = {0};
		if (mia_checker_try_value(c, node, &value) && mia_evaluator_value_integer == value.kind && mia_type_is_integer(target)
				&& !mia_type_holds(target, value.as.integer)) {
			mia_string_s text = {0}, low = {0}, high = {0};
			mia_format_int128(&text, value.as.integer);
			mia_format_int128(&low, mia_type_minimum(target));
			mia_format_int128(&high, mia_type_maximum(target));
			mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0511, node->span, "`%s` does not fit in `%s`", mia_string_cstr(&text), mia_checker_name_of(c, target)),
				"`%s` holds %s to %s", mia_checker_name_of(c, target), mia_string_cstr(&low), mia_string_cstr(&high));
			mia_string_free(&text);
			mia_string_free(&low);
			mia_string_free(&high);
			mia_checker_finalize(c, node, target);
			return false;
		}
		mia_checker_finalize(c, node, target);
		return true;
	}
	if (mia_type_is_special(actual, mia_type_special_nothing)) {
		mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0523, node->span, "this has no value"), "the function returns nothing");
		return false;
	}
	mia_diag_entry_s* const diagnostic = mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0502, node->span, "mismatched types"), "expected `%s`, found `%s`",
		mia_checker_name_of(c, target), mia_checker_name_of(c, actual));
	const mia_view_s text = mia_checker_node_text(c, node);
	if (mia_type_array == actual->kind && mia_type_slice == target->kind && actual->as.array.element == target->as.slice.element) {
		mia_diag_help(diagnostic, "take a slice of the array: `" mia_view_fmt "[..]`", mia_view_arg(text));
	}
	else if (mia_type_is_numeric(actual) && mia_type_is_numeric(target)) {
		mia_diag_help(diagnostic, "convert explicitly: `(%s)" mia_view_fmt "`", mia_checker_name_of(c, target), mia_view_arg(text));
	}
	else if (mia_type_is_special(actual, mia_type_special_untyped_float) && mia_type_scalar == target->kind && mia_type_is_integer(target)) {
		mia_diag_help(diagnostic, "a float literal cannot become an integer");
	}
	return false;
}

// ========================================================================================
// calls of plugin functions: their checks (the compile-time plugin api)
// ========================================================================================

/// plugin functions in the checker: whether a loaded plugin provides each
/// '[plugin]' function that is used, and the compile-time half of the
/// plugin api (see mia/plugin.h): the check of a plugin function runs on
/// every call of it, after the arguments are checked.

typedef struct {
	mia_plugin_check_s            is_public;   // first: the handle the plugin gets
	mia_checker_s*                checker  ;
	mia_ast_node_s*               node     ;   // the call
	const char*                   name     ;
	size_t                        count    ;
	size_t                        fixed    ;
	mia_ast_node_s**              arguments;
	const mia_type_s**            types    ;
	const mia_program_instance_s* instance ;
} _mia_checker_check_s;

/// a diagnostic being reported, with what its labels need.
struct mia_plugin_diagnostic_s {
	mia_diag_entry_s*     diagnostic;
	_mia_checker_check_s* check     ;
	size_t                index     ;   // where it was reported
	bool                  main      ;   // the main label is given
};

static _mia_checker_check_s* _mia_checker_check_of(mia_plugin_check_s* const check) {
	return (_mia_checker_check_s*)check;
}

static const mia_plugins_s* _mia_checker_plugins(const mia_checker_s* const c) {
	return (c->options != NULL && c->options->plugins != NULL) ? c->options->plugins : mia_plugins_default();
}

// --- whether a plugin provides a function -----------------------------------------------------

void mia_checker_plugin_use(mia_checker_s* const c, mia_program_function_s* const function, const mia_span_s span) {
	if (!mia_program_function_is_plugin(function) || function->plugin_checked) return;
	function->plugin_checked = true;
	char* const plugin = mia_program_function_plugin(function);
	if (NULL == plugin) return;   // '[plugin]' without a name: reported with the attribute
	char* const name = mia_program_function_qualified_name(function);
	const mia_span_s declared = function->node->as.function.name_span;
	const mia_plugins_function_s* provided = mia_plugins_find(_mia_checker_plugins(c), name);
	if (NULL == provided && c->options != NULL && c->options->plugins != NULL) {
		// look the plugin up in the plugin directories
		char* error = NULL;
		if (mia_plugins_require(c->options->plugins, plugin, &error)) provided = mia_plugins_find(c->options->plugins, name);
		if (error != NULL) {
			mia_diag_label(mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0603, span, "%s", error), "needed here"), declared,
				"declared `[plugin = \"%s\"]` here", plugin);
			mia_free(error);
			mia_free(plugin);
			mia_free(name);
			return;
		}
	}
	const mia_plugins_s* const plugins = _mia_checker_plugins(c);
	if (NULL == provided) {
		mia_diag_entry_s* const diagnostic = mia_diag_label(mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0416, span, "no plugin provides `%s`", name),
			"used here"), declared, "declared `[plugin = \"%s\"]` here", plugin);
		if (!mia_plugins_loaded(plugins, plugin)) {
			mia_diag_help(mia_diag_note(diagnostic, "the plugin `%s` is in no plugin directory", plugin),
				"the plugin directories are the program's, the standard library's and those of `-P <dir>`");
		}
		else {
			mia_diag_note(diagnostic, "the plugin `%s` is loaded, but it does not provide `%s`", plugin, name);
		}
	}
	else if (strcmp(plugin, provided->plugin) != 0) {
		mia_diag_note(mia_diag_label(mia_diag_label_main(mia_diag_error(c->diag, mia_diag_E0416, span, "the plugin `%s` does not provide `%s`", plugin, name), "used here"),
			declared, "declared `[plugin = \"%s\"]` here", plugin), "`%s` comes from the plugin `%s`", name, provided->plugin);
	}
	mia_free(plugin);
	mia_free(name);
}

// --- types -------------------------------------------------------------------------------

static mia_code_e _mia_checker_scalar_code(const mia_scalar_e scalar) {
	switch (scalar) {
#define mia_token_keyword(_name)
#define mia_token_punctuator(_name, _text)
#define mia_token_scalar(_name, _kind, _bits, _signed) case mia_scalar_##_name: return mia_code_##_name;
#include "mia/tokens.def"
#undef mia_token_scalar
#undef mia_token_punctuator
#undef mia_token_keyword
		default: return mia_code_int;
	}
}

/// the handle of a checker type (made once per type, in the program's arena).
static const mia_plugin_type_s* _mia_checker_type_handle(mia_checker_s* const c, const mia_type_s* const given) {
	const mia_type_s* type = given;
	if (mia_type_reference == type->kind) type = type->as.reference.element;
	if (mia_type_is_special(type, mia_type_special_untyped_integer)) type = mia_checker_scalar_type(c, mia_scalar_int);
	if (mia_type_is_special(type, mia_type_special_untyped_float)) type = mia_checker_scalar_type(c, mia_scalar_float);
	for (size_t i = 0; i < c->plugin_types.count; ++i) if (c->plugin_types.data[i].type == type) return c->plugin_types.data[i].handle;
	mia_plugin_type_s* const handle = mia_arena_new(c->arena, mia_plugin_type_s);
	const mia_checker_plugin_type_s entry = { .type = type, .handle = handle, };
	mia_vector_push(&c->plugin_types, entry);
	handle->name = mia_checker_name_of(c, type);
	handle->index = -1;
	handle->code = mia_code_bytes;
	uint64_t alignment = 1;
	const bool sized = !mia_type_is_special(type, mia_type_special_error) && type->kind != mia_type_special && type->kind != mia_type_parameter;
	handle->size = sized ? mia_checker_size_of(c, type, &alignment) : 0;
	handle->alignment = (alignment > 0) ? alignment : 1;
	switch (type->kind) {
		case mia_type_scalar: {
			const char kind = mia_scalar_kind(type->as.scalar);
			handle->kind = ('b' == kind) ? mia_plugin_kind_bool : ('f' == kind) ? mia_plugin_kind_float : mia_plugin_kind_int;
			handle->code = _mia_checker_scalar_code(type->as.scalar);
			break;
		}
		case mia_type_array:
			handle->kind = mia_plugin_kind_array;
			handle->element = _mia_checker_type_handle(c, type->as.array.element);
			handle->length = (type->as.array.length > 0) ? (uint64_t)type->as.array.length : 0;
			break;
		case mia_type_slice:
			handle->kind = mia_plugin_kind_slice;
			handle->code = mia_code_slice;
			handle->element = _mia_checker_type_handle(c, type->as.slice.element);
			break;
		case mia_type_function:
			handle->kind = mia_plugin_kind_function;
			handle->code = mia_code_fn;
			break;
		case mia_type_struct: {
			handle->kind = mia_plugin_kind_struct;
			const mia_program_layout_s* const layout = mia_checker_layout(c, type);
			const size_t count = layout->fields.count;
			const char** const names = mia_arena_alloc(c->arena, (count + 1) * sizeof(*names));
			const mia_plugin_type_s** const types = mia_arena_alloc(c->arena, (count + 1) * sizeof(*types));
			uint64_t* const offsets = mia_arena_alloc(c->arena, (count + 1) * sizeof(*offsets));
			handle->field_count = count;
			handle->field_names = names;
			handle->field_types = types;
			handle->field_offsets = offsets;
			for (size_t i = 0; i < count; ++i) {
				names[i] = mia_checker_view_cstring(c, layout->fields.data[i]->name);
				offsets[i] = layout->fields.data[i]->offset;
				types[i] = _mia_checker_type_handle(c, layout->fields.data[i]->type);
			}
			break;
		}
		default:
			// a type without a value (an error, '...'): a placeholder
			handle->kind = mia_plugin_kind_int;
			handle->code = mia_code_int;
			break;
	}
	return handle;
}

// --- the check of a call -------------------------------------------------------------------

static size_t _mia_checker_argument_index(_mia_checker_check_s* const check, const size_t index) {
	if (index >= check->count) mia_panic("the check of `%s` asks for argument %zu, but the call has %zu", check->name, index, check->count);
	return index;
}

static const char* _mia_checker_check_name(mia_plugin_check_s* const check) {
	return _mia_checker_check_of(check)->name;
}

static size_t _mia_checker_check_count(mia_plugin_check_s* const check) {
	return _mia_checker_check_of(check)->count;
}

static size_t _mia_checker_check_fixed_count(mia_plugin_check_s* const check) {
	return _mia_checker_check_of(check)->fixed;
}

static const mia_plugin_type_s* _mia_checker_check_type(mia_plugin_check_s* const check, const size_t index) {
	_mia_checker_check_s* const self = _mia_checker_check_of(check);
	return _mia_checker_type_handle(self->checker, self->types[_mia_checker_argument_index(self, index)]);
}

static size_t _mia_checker_check_type_argument_count(mia_plugin_check_s* const check) {
	return _mia_checker_check_of(check)->instance->argument_count;
}

static const mia_plugin_type_s* _mia_checker_check_type_argument(mia_plugin_check_s* const check, const size_t index) {
	_mia_checker_check_s* const self = _mia_checker_check_of(check);
	if (index >= self->instance->argument_count) mia_panic("the check of `%s` asks for type argument %zu", self->name, index);
	return _mia_checker_type_handle(self->checker, self->instance->arguments[index]);
}

/// whether 'node' is a "..."c literal or a constant made from one, or a slice
/// of either (a variable can change, so it never counts).
static bool _mia_checker_c_string(const mia_ast_node_s* const node, const int depth) {
	if (NULL == node || depth > 32) return false;
	switch (node->kind) {
		case mia_ast_node_paren:    return _mia_checker_c_string(node->as.paren.inner, depth + 1);
		case mia_ast_node_string:   return node->as.string.c;
		case mia_ast_node_slice_of: return _mia_checker_c_string(node->as.slice_of.target, depth + 1);
		case mia_ast_node_name:
		case mia_ast_node_member: {
			if (node->note.field != NULL) return false;
			const mia_program_variable_s* const variable = node->note.variable;
			if (variable != NULL) return false;
			if (NULL == node->note.constant) return false;
			return _mia_checker_c_string(node->note.constant->node->as.constant.value, depth + 1);
		}
		default:
			return false;
	}
}

static mia_plugin_constant_s _mia_checker_check_constant(mia_plugin_check_s* const check, const size_t index) {
	_mia_checker_check_s* const self = _mia_checker_check_of(check);
	mia_checker_s* const c = self->checker;
	mia_ast_node_s* const node = self->arguments[_mia_checker_argument_index(self, index)];
	mia_plugin_constant_s constant = { .kind = mia_plugin_constant_none, .c_string = _mia_checker_c_string(node, 0), };
	mia_evaluator_value_s value = {0};
	if (mia_ast_node_reference == node->kind || !mia_checker_try_value(c, node, &value)) return constant;
	switch (value.kind) {
		case mia_evaluator_value_integer:
			constant.kind = mia_plugin_constant_integer;
			constant.integer = mia_int128_to_int(value.as.integer);
			return constant;
		case mia_evaluator_value_real:
			constant.kind = mia_plugin_constant_float;
			constant.real = value.as.real;
			return constant;
		case mia_evaluator_value_boolean:
			constant.kind = mia_plugin_constant_bool;
			constant.boolean = value.as.boolean;
			return constant;
		case mia_evaluator_value_list:
		case mia_evaluator_value_slice: {
			const mia_type_s* const type = self->types[index];
			const mia_type_s* const element = (mia_type_array == type->kind) ? type->as.array.element
				: (mia_type_slice == type->kind) ? type->as.slice.element : NULL;
			if (element != mia_checker_scalar_type(c, mia_scalar_u8)) break;
			const mia_evaluator_list_s* const list = (mia_evaluator_value_list == value.kind) ? value.as.list : value.as.slice.list;
			const int64_t start = (mia_evaluator_value_list == value.kind) ? 0 : value.as.slice.start;
			const int64_t length = (NULL == list) ? 0 : (mia_evaluator_value_list == value.kind) ? (int64_t)list->count : value.as.slice.length;
			uint8_t* const bytes = mia_arena_alloc(c->arena, (size_t)length + 1);
			for (int64_t i = 0; i < length; ++i) bytes[i] = (uint8_t)mia_int128_low(list->items[start + i].as.integer);
			constant.kind = mia_plugin_constant_bytes;
			constant.bytes = bytes;
			constant.length = (size_t)length;
			return constant;
		}
		default:
			break;
	}
	constant.kind = mia_plugin_constant_other;
	return constant;
}

static void _mia_checker_check_mark(mia_plugin_check_s* const check, const size_t index, const uint32_t flags) {
	_mia_checker_check_s* const self = _mia_checker_check_of(check);
	mia_ast_node_s* const node = self->node;
	if (NULL == node->note.flags) {
		node->note.flags = mia_arena_alloc(self->checker->arena, (self->count + 1) * sizeof(uint32_t));
		node->note.flag_count = (uint32_t)self->count;
	}
	node->note.flags[_mia_checker_argument_index(self, index)] = flags;
}

static mia_span_s _mia_checker_span(_mia_checker_check_s* const check, const size_t index) {
	if (mia_plugin_call == index) return check->node->span;
	return check->arguments[_mia_checker_argument_index(check, index)]->span;
}

static mia_plugin_diagnostic_s* _mia_checker_report(mia_plugin_check_s* const check, const bool warning, const char* const code, const size_t index,
													const char* const message) {
	_mia_checker_check_s* const self = _mia_checker_check_of(check);
	mia_diag_code_e parsed = (code != NULL) ? mia_diag_code_parse(code) : mia_diag_code_count;
	if (mia_diag_code_count == parsed || warning != ('W' == mia_diag_code_text(parsed)[0])) parsed = warning ? mia_diag_W0531 : mia_diag_E0531;
	const mia_span_s span = _mia_checker_span(self, index);
	mia_diag_entry_s* const diagnostic = warning ? mia_diag_warning(self->checker->diag, parsed, span, "%s", message)
		: mia_diag_error(self->checker->diag, parsed, span, "%s", message);
	mia_plugin_diagnostic_s* const handle = mia_arena_new(self->checker->arena, mia_plugin_diagnostic_s);
	handle->diagnostic = diagnostic;
	handle->check = self;
	handle->index = index;
	return handle;
}

static void _mia_checker_label(mia_plugin_diagnostic_s* const diagnostic, const size_t index, const char* const text) {
	if (index == diagnostic->index && !diagnostic->main) {
		diagnostic->main = true;
		mia_diag_label_main(diagnostic->diagnostic, "%s", text);
		return;
	}
	mia_diag_label(diagnostic->diagnostic, _mia_checker_span(diagnostic->check, index), "%s", text);
}

static void _mia_checker_note(mia_plugin_diagnostic_s* const diagnostic, const char* const text) {
	mia_diag_note(diagnostic->diagnostic, "%s", text);
}

static void _mia_checker_help(mia_plugin_diagnostic_s* const diagnostic, const char* const text) {
	mia_diag_help(diagnostic->diagnostic, "%s", text);
}

void mia_checker_plugin_check_call(mia_checker_s* const c, mia_ast_node_s* const node, mia_program_instance_s* const instance) {
	mia_program_function_s* const function = instance->function;
	mia_checker_plugin_use(c, function, node->as.call.callee->span);
	char* const name = mia_program_function_qualified_name(function);
	const mia_plugins_function_s* const provided = mia_plugins_find(_mia_checker_plugins(c), name);
	const mia_type_list_s* const parameters = &instance->parameters;
	const bool variadic = parameters->count > 0 && mia_type_is_special(mia_vector_last(parameters), mia_type_special_variadic);
	const size_t fixed = variadic ? parameters->count - 1 : parameters->count;
	const mia_ast_nodes_s* const arguments = &node->as.call.arguments;
	const size_t extra = (variadic && node->note.variadic != NULL) ? node->note.variadic->count : 0;
	// a call with errors in its arguments is not checked further
	bool clean = NULL != provided && NULL != provided->check && arguments->count == fixed + extra;
	for (size_t i = 0; clean && i < arguments->count; ++i) {
		const mia_type_s* const type = arguments->data[i]->note.type;
		clean = type != NULL && !mia_type_is_special(type, mia_type_special_error);
	}
	if (!clean) {
		mia_free(name);
		return;
	}
	_mia_checker_check_s check = {
		.is_public = { .interface = mia_plugins_table(), }, .checker = c, .node = node, .name = mia_arena_strndup(c->arena, name, strlen(name)),
		.count = arguments->count, .fixed = fixed, .instance = instance,
	};
	mia_free(name);
	check.arguments = mia_arena_alloc(c->arena, (check.count + 1) * sizeof(*check.arguments));
	check.types = mia_arena_alloc(c->arena, (check.count + 1) * sizeof(*check.types));
	for (size_t i = 0; i < check.count; ++i) {
		mia_ast_node_s* const argument = arguments->data[i];
		check.arguments[i] = (mia_ast_node_reference == argument->kind) ? argument->as.reference.operand : argument;
		check.types[i] = (i < fixed) ? parameters->data[i] : node->note.variadic->data[i - fixed];
		if (i < fixed && mia_type_reference == check.types[i]->kind) check.types[i] = check.types[i]->as.reference.element;
		// a string literal given for a []u8 parameter is seen as its array
		if (i < fixed && argument->note.format) check.types[i] = argument->note.type;
	}
	provided->check(&check.is_public);
}

void mia_checker_plugin_table(mia_plugin_interface_s* const table) {
	table->check_name = _mia_checker_check_name;
	table->check_count = _mia_checker_check_count;
	table->check_fixed_count = _mia_checker_check_fixed_count;
	table->check_type = _mia_checker_check_type;
	table->check_type_argument_count = _mia_checker_check_type_argument_count;
	table->check_type_argument = _mia_checker_check_type_argument;
	table->check_constant = _mia_checker_check_constant;
	table->check_mark = _mia_checker_check_mark;
	table->report = _mia_checker_report;
	table->label = _mia_checker_label;
	table->note = _mia_checker_note;
	table->help = _mia_checker_help;
}
