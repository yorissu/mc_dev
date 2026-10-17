
/// the checker: programs, modules, imports, name lookup, declarations,
/// attributes, operator functions, constants and generic instances.

#include "check.h"
#include "mc/format.h"
#include "mc/printer.h"

#include <string.h>

#define _mc_instantiation_limit 64

static const char* const _mc_known_attributes = "builtin, extern, op, uninit, wrap";

static const struct {
	const char* where;
	const char* allowed;
} _mc_allowed_attributes[] = {
	{ "function",  " builtin extern op wrap " },
	{ "struct",    " " },
	{ "constant",  " wrap " },
	{ "field",     " " },
	{ "let",       " uninit wrap " },
	{ "statement", " wrap " },
};

static const struct {
	const char* text;
	int         arity;
} _mc_patterns[] = {
#define mc_operator_pattern(_name, _text, _arity) { _text, _arity, },
#include "mc/operators.def"
#undef mc_operator_pattern
};

static const struct {
	const char* pattern;
	const char* note;
} _mc_derived_patterns[] = {
	{ "_!=_", "`!=` is `!(a == b)`" },
	{ "_>_", "`a > b` is `b < a`" },
	{ "_<=_", "`a <= b` is `!(b < a)`" },
	{ "_>=_", "`a >= b` is `!(a < b)`" },
	{ "_+=_", "`a += b` is `a = a + b`" },
	{ "_-=_", "`a -= b` is `a = a - b`" },
	{ "_*=_", "`a *= b` is `a = a * b`" },
	{ "_/=_", "`a /= b` is `a = a / b`" },
	{ "_%=_", "`a %= b` is `a = a % b`" },
	{ "_&=_", "`a &= b` is `a = a & b`" },
	{ "_|=_", "`a |= b` is `a = a | b`" },
	{ "_^=_", "`a ^= b` is `a = a ^ b`" },
	{ "_<<=_", "`a <<= b` is `a = a << b`" },
	{ "_>>=_", "`a >>= b` is `a = a >> b`" },
};

// --- small helpers -----------------------------------------------------------

const char* mc_view_cstring(mc_checker_s* const c, const mc_view_s view) {
	return mc_arena_strndup(c->arena, view.data, view.length);
}

const char* mc_name_of(mc_checker_s* const c, const mc_type_s* const type) {
	mc_string_s text = {0};
	mc_type_text(&text, type);
	const char* const kept = mc_arena_strndup(c->arena, mc_string_cstr(&text), text.length);
	mc_string_free(&text);
	return kept;
}

void mc_instance_text(mc_string_s* const out, const mc_instance_s* const instance) {
	mc_string_append(out, instance->function->node->as.function.name);
	if (0 == instance->argument_count) return;
	mc_string_append_char(out, '<');
	for (size_t i = 0; i < instance->argument_count; ++i) {
		if (i > 0) mc_string_append_cstr(out, ", ");
		mc_type_text(out, instance->arguments[i]);
	}
	mc_string_append_char(out, '>');
}

const char* mc_instance_name(mc_checker_s* const c, const mc_instance_s* const instance) {
	mc_string_s text = {0};
	mc_instance_text(&text, instance);
	const char* const kept = mc_arena_strndup(c->arena, mc_string_cstr(&text), text.length);
	mc_string_free(&text);
	return kept;
}

mc_view_s mc_node_text(mc_checker_s* const c, const mc_node_s* const node) {
	return mc_span_text(c->sources, node->span);
}

bool mc_function_is_generic(const mc_function_s* const info) {
	return info->node->as.function.generics.count > 0;
}

bool mc_function_is_external(const mc_function_s* const function) {
	const mc_nodes_s* const attributes = &function->node->as.function.attributes;
	return mc_attribute_find(attributes, "builtin") != NULL || mc_attribute_find(attributes, "extern") != NULL;
}

char* mc_function_qualified_name(const mc_function_s* const function) {
	return mc_format("%s." mc_view_fmt, function->module->name, mc_view_arg(function->node->as.function.name));
}

static bool _mc_symbol_public(const mc_symbol_s* const symbol) {
	switch (symbol->kind) {
		case mc_symbol_function: return ((const mc_function_s*)symbol)->node->as.function.public_;
		case mc_symbol_struct:   return ((const mc_struct_s*)symbol)->node->as.struct_.public_;
		case mc_symbol_constant: return ((const mc_constant_s*)symbol)->node->as.constant.public_;
		default:                 return false;
	}
}

static const mc_node_s* _mc_symbol_node(const mc_symbol_s* const symbol) {
	switch (symbol->kind) {
		case mc_symbol_function: return ((const mc_function_s*)symbol)->node;
		case mc_symbol_struct:   return ((const mc_struct_s*)symbol)->node;
		case mc_symbol_constant: return ((const mc_constant_s*)symbol)->node;
		default:                 return NULL;
	}
}

static mc_module_s* _mc_symbol_module(const mc_symbol_s* const symbol) {
	switch (symbol->kind) {
		case mc_symbol_function: return ((const mc_function_s*)symbol)->module;
		case mc_symbol_struct:   return ((const mc_struct_s*)symbol)->module;
		case mc_symbol_constant: return ((const mc_constant_s*)symbol)->module;
		default:                 return NULL;
	}
}

// --- variable sets -------------------------------------------------------------

bool mc_set_contains(const mc_variable_set_s* const set, const mc_variable_s* const variable) {
	for (size_t i = 0; i < set->count; ++i) if (set->data[i] == variable) return true;
	return false;
}

void mc_set_add(mc_variable_set_s* const set, mc_variable_s* const variable) {
	if (!mc_set_contains(set, variable)) mc_vector_push(set, variable);
}

void mc_set_discard(mc_variable_set_s* const set, const mc_variable_s* const variable) {
	for (size_t i = 0; i < set->count; ++i) {
		if (set->data[i] == variable) {
			set->data[i] = set->data[set->count - 1];
			set->count -= 1;
			return;
		}
	}
}

void mc_set_assign(mc_variable_set_s* const set, const mc_variable_set_s* const other) {
	set->count = 0;
	for (size_t i = 0; i < other->count; ++i) mc_vector_push(set, other->data[i]);
}

void mc_set_union(mc_variable_set_s* const set, const mc_variable_set_s* const other) {
	for (size_t i = 0; i < other->count; ++i) mc_set_add(set, other->data[i]);
}

// --- contexts --------------------------------------------------------------------

mc_context_s* mc_context_new(mc_checker_s* const c, mc_module_s* const module, const mc_bindings_s* const bindings,
							 const mc_type_s* const result, mc_context_s* const parent, mc_instance_s* const instance,
							 const bool function) {
	mc_context_s* const context = mc_calloc(1, sizeof(*context));
	context->module = module;
	if (bindings != NULL) for (size_t i = 0; i < bindings->count; ++i) mc_vector_push(&context->bindings, bindings->data[i]);
	context->result = result;
	context->parent = parent;
	context->instance = instance;
	context->function = function;
	if (function) mc_context_push_scope(c, context);
	context->wrap = (parent != NULL) ? parent->wrap : false;
	context->level = (parent != NULL) ? parent->level + 1 : 0;
	context->depth = (instance != NULL) ? instance->depth : (parent != NULL) ? parent->depth : 0;
	mc_vector_push(&c->contexts, context);
	return context;
}

static void _mc_context_free(mc_context_s* const context) {
	while (context->scopes.count > 0) mc_context_pop_scope(context);
	mc_vector_free(&context->scopes);
	mc_vector_free(&context->bindings);
	mc_vector_free(&context->unassigned);
	mc_vector_free(&context->variables);
	mc_free(context);
}

void mc_context_push_scope(mc_checker_s* const c, mc_context_s* const context) {
	mc_unused(c);
	mc_map_s* const scope = mc_calloc(1, sizeof(*scope));
	mc_vector_push(&context->scopes, scope);
}

void mc_context_pop_scope(mc_context_s* const context) {
	mc_map_s* const scope = mc_vector_pop(&context->scopes);
	mc_map_free(scope);
	mc_free(scope);
}

mc_symbol_s* mc_context_declare(mc_context_s* const context, const mc_view_s name, mc_symbol_s* const symbol) {
	mc_map_s* const scope = mc_vector_last(&context->scopes);
	mc_symbol_s* const previous = mc_map_get(scope, name);
	mc_map_set(scope, name, symbol);
	return previous;
}

// --- lookup ----------------------------------------------------------------------

static bool _mc_qualifier_prefix(const mc_module_s* const module, const char* const prefix) {
	const size_t length = strlen(prefix);
	mc_map_each(&module->qualifiers, entry) {
		if (entry->key.length > length && 0 == memcmp(entry->key.data, prefix, length) && '.' == entry->key.data[length]) return true;
	}
	return false;
}

static mc_symbol_s* _mc_prefix_new(mc_checker_s* const c, mc_module_s* const module, const char* const prefix) {
	mc_prefix_s* const symbol = mc_arena_new(c->arena, mc_prefix_s);
	symbol->base.kind = mc_symbol_prefix;
	symbol->module = module;
	symbol->prefix = prefix;
	return &symbol->base;
}

mc_symbol_s* mc_lookup(mc_checker_s* const c, const mc_view_s name, mc_context_s* const context, const mc_span_s span, const bool report) {
	for (mc_context_s* current = context; current != NULL; current = current->parent) {
		for (size_t i = current->scopes.count; i > 0; --i) {
			mc_symbol_s* const symbol = mc_map_get(current->scopes.data[i - 1], name);
			if (NULL == symbol) continue;
			if (mc_symbol_variable == symbol->kind && current != context) {
				if (report) {
					mc_help(mc_label_main(mc_error(c->diag, mc_E0516, span, "`" mc_view_fmt "` is a variable of the enclosing function", mc_view_arg(name)),
						"lambdas and local functions cannot capture variables"), "pass it as an argument instead");
				}
				return NULL;
			}
			return symbol;
		}
	}
	mc_module_s* const module = context->module;
	mc_symbol_s* symbol = mc_map_get(&module->symbols, name);
	if (NULL == symbol) symbol = mc_map_get(&module->imported, name);
	if (symbol != NULL) return symbol;
	mc_vector_of(mc_symbol_s*) found = {0};
	for (size_t i = 0; i < module->star.count; ++i) {
		mc_symbol_s* const candidate = mc_map_get(&module->star.data[i]->symbols, name);
		if (NULL == candidate || !_mc_symbol_public(candidate)) continue;
		bool seen = false;
		for (size_t j = 0; j < found.count; ++j) seen = seen || found.data[j] == candidate;
		if (!seen) mc_vector_push(&found, candidate);
	}
	if (1 == found.count) {
		symbol = found.data[0];
		mc_vector_free(&found);
		return symbol;
	}
	if (found.count > 1) {
		if (report) {
			mc_diagnostic_s* const diagnostic = mc_label_main(mc_error(c->diag, mc_E0304, span, "`" mc_view_fmt "` is ambiguous", mc_view_arg(name)),
				"imported by several `*` imports");
			for (size_t i = 0; i < found.count; ++i) {
				const mc_module_s* const origin = _mc_symbol_module(found.data[i]);
				mc_note(diagnostic, "`" mc_view_fmt "` from `%s`", mc_view_arg(name), (origin != NULL) ? origin->name : "?");
			}
		}
		mc_vector_free(&found);
		return NULL;
	}
	mc_vector_free(&found);
	mc_module_s* const qualified = mc_map_get(&module->qualifiers, name);
	if (qualified != NULL) return &qualified->base;
	const char* const text = mc_view_cstring(c, name);
	if (_mc_qualifier_prefix(module, text)) return _mc_prefix_new(c, module, text);
	if (report) mc_label_main(mc_error(c->diag, mc_E0501, span, "cannot find `" mc_view_fmt "`", mc_view_arg(name)), "not declared");
	return NULL;
}

mc_symbol_s* mc_member_of(mc_checker_s* const c, mc_symbol_s* const base, const mc_view_s name, const mc_span_s span, mc_context_s* const context) {
	if (mc_symbol_prefix == base->kind) {
		const mc_prefix_s* const prefix = (const mc_prefix_s*)base;
		char* const joined = mc_format("%s." mc_view_fmt, prefix->prefix, mc_view_arg(name));
		const char* const dotted = mc_arena_strndup(c->arena, joined, strlen(joined));
		mc_free(joined);
		mc_module_s* const module = mc_map_get_cstr(&prefix->module->qualifiers, dotted);
		if (module != NULL) return &module->base;
		if (_mc_qualifier_prefix(prefix->module, dotted)) return _mc_prefix_new(c, prefix->module, dotted);
		mc_label_main(mc_error(c->diag, mc_E0301, span, "module `%s` is not imported", dotted), "no such module");
		return NULL;
	}
	mc_module_s* const module = (mc_module_s*)base;
	mc_symbol_s* const symbol = mc_map_get(&module->symbols, name);
	if (NULL == symbol) {
		mc_label_main(mc_error(c->diag, mc_E0305, span, "module `%s` has no `" mc_view_fmt "`", module->name, mc_view_arg(name)), "not declared there");
		return NULL;
	}
	if (!_mc_symbol_public(symbol) && module != context->module) {
		mc_label(mc_label_main(mc_error(c->diag, mc_E0303, span, "`" mc_view_fmt "` is not public in `%s`", mc_view_arg(name), module->name), "not `pub`"),
			mc_declaration_name_span(_mc_symbol_node(symbol)), "declared here without `pub`");
		return NULL;
	}
	return symbol;
}

mc_symbol_s* mc_qualified(mc_checker_s* const c, const mc_node_s* const node, mc_context_s* const context, const bool report) {
	if (mc_node_name == node->kind) return mc_lookup(c, node->as.name.name, context, node->span, report);
	if (mc_node_member == node->kind) {
		const mc_node_s* const target = node->as.member.target;
		mc_symbol_s* const base = (mc_node_name == target->kind || mc_node_member == target->kind) ? mc_qualified(c, target, context, false) : NULL;
		if (base != NULL && (mc_symbol_module == base->kind || mc_symbol_prefix == base->kind)) {
			return mc_member_of(c, base, node->as.member.name, node->as.member.name_span, context);
		}
	}
	return NULL;
}

// --- program -----------------------------------------------------------------------

static void _mc_collect(mc_checker_s* const c, mc_module_s* const module) {
	const mc_nodes_s* const declarations = &module->tree->as.module.declarations;
	for (size_t i = 0; i < declarations->count; ++i) {
		mc_node_s* const node = declarations->data[i];
		mc_symbol_s* symbol = NULL;
		if (mc_node_function == node->kind) {
			mc_function_s* const info = mc_arena_new(c->arena, mc_function_s);
			info->base.kind = mc_symbol_function;
			info->node = node;
			info->module = module;
			symbol = &info->base;
		}
		else if (mc_node_struct == node->kind) {
			mc_struct_s* const info = mc_arena_new(c->arena, mc_struct_s);
			info->base.kind = mc_symbol_struct;
			info->node = node;
			info->module = module;
			symbol = &info->base;
		}
		else {
			mc_constant_s* const info = mc_arena_new(c->arena, mc_constant_s);
			info->base.kind = mc_symbol_constant;
			info->node = node;
			info->module = module;
			symbol = &info->base;
		}
		const mc_view_s name = mc_declaration_name(node);
		mc_symbol_s* const previous = mc_map_get(&module->symbols, name);
		if (previous != NULL) {
			mc_label(mc_label_main(mc_error(c->diag, mc_E0401, mc_declaration_name_span(node), "`" mc_view_fmt "` is declared twice", mc_view_arg(name)),
				"declared again here"), mc_declaration_name_span(_mc_symbol_node(previous)), "first declared here");
			continue;
		}
		mc_map_set(&module->symbols, name, symbol);
		mc_vector_push(&module->ordered, symbol);
	}
}

static char* _mc_join_path(const mc_views_s* const parts) {
	mc_string_s dotted = {0};
	for (size_t i = 0; i < parts->count; ++i) {
		if (i > 0) mc_string_append_char(&dotted, '.');
		mc_string_append(&dotted, parts->data[i]);
	}
	return mc_string_take(&dotted);
}

static void _mc_resolve_imports(mc_checker_s* const c, mc_module_s* const module) {
	for (size_t i = 0; i < module->imports.count; ++i) {
		mc_node_s* const node = module->imports.data[i].node;
		mc_module_s* const target = module->imports.data[i].target;
		if (mc_node_import == node->kind) {
			char* const dotted = _mc_join_path(&node->as.import.path);
			const char* const alias = node->as.import.has_alias ? mc_view_cstring(c, node->as.import.alias) : mc_arena_strndup(c->arena, dotted, strlen(dotted));
			mc_free(dotted);
			mc_module_s* const previous = mc_map_get_cstr(&module->qualifiers, alias);
			if (previous != NULL && previous != target) {
				mc_label_main(mc_error(c->diag, mc_E0306, node->span, "`%s` is imported twice", alias), "already imported");
			}
			mc_map_set(&module->qualifiers, mc_view_cstr(alias), target);
		}
		else if (node->as.from_import.star) {
			mc_vector_push(&module->star, target);
		}
		else {
			const mc_nodes_s* const names = &node->as.from_import.names;
			for (size_t n = 0; n < names->count; ++n) {
				const mc_node_s* const item = names->data[n];
				const mc_view_s name = item->as.import_name.name;
				mc_symbol_s* const symbol = mc_map_get(&target->symbols, name);
				if (NULL == symbol) {
					mc_label_main(mc_error(c->diag, mc_E0305, item->span, "module `%s` has no `" mc_view_fmt "`", target->name, mc_view_arg(name)),
						"not declared there");
					continue;
				}
				if (!_mc_symbol_public(symbol)) {
					mc_label(mc_label_main(mc_error(c->diag, mc_E0303, item->span, "`" mc_view_fmt "` is not public in `%s`", mc_view_arg(name), target->name),
						"not `pub`"), mc_declaration_name_span(_mc_symbol_node(symbol)), "declared here without `pub`");
					continue;
				}
				const mc_view_s alias = item->as.import_name.has_alias ? item->as.import_name.alias : name;
				mc_symbol_s* const previous = mc_map_get(&module->imported, alias);
				if (previous != NULL && previous != symbol) {
					mc_label_main(mc_error(c->diag, mc_E0306, item->span, "`" mc_view_fmt "` is imported twice", mc_view_arg(alias)), "already imported");
					continue;
				}
				mc_map_set(&module->imported, alias, symbol);
			}
		}
	}
}

// --- attributes and declarations -----------------------------------------------------

static bool _mc_word_in(const char* const list, const mc_view_s word) {
	char* const padded = mc_format(" " mc_view_fmt " ", mc_view_arg(word));
	const bool found = strstr(list, padded) != NULL;
	mc_free(padded);
	return found;
}

mc_attribute_set_s mc_attributes(mc_checker_s* const c, const mc_nodes_s* const attributes, const char* const where) {
	mc_attribute_set_s found = {0};
	const char* allowed = " ";
	for (size_t i = 0; i < mc_count_of(_mc_allowed_attributes); ++i) {
		if (0 == strcmp(_mc_allowed_attributes[i].where, where)) allowed = _mc_allowed_attributes[i].allowed;
	}
	for (size_t i = 0; i < attributes->count; ++i) {
		const mc_node_s* const attribute = attributes->data[i];
		const mc_view_s name = attribute->as.attribute.name;
		const mc_attribute_value_e value = attribute->as.attribute.value_kind;
		if (!_mc_word_in(" builtin extern op wrap uninit ", name)) {
			mc_note(mc_label_main(mc_error(c->diag, mc_E0402, attribute->span, "unknown attribute `" mc_view_fmt "`", mc_view_arg(name)),
				"not an attribute"), "attributes: %s", _mc_known_attributes);
			continue;
		}
		if (!_mc_word_in(allowed, name)) {
			mc_label_main(mc_error(c->diag, mc_E0403, attribute->span, "`" mc_view_fmt "` cannot be used on a %s", mc_view_arg(name), where),
				"not applicable here");
			continue;
		}
		const mc_node_s** const slot = mc_view_eq_cstr(name, "builtin") ? &found.builtin : mc_view_eq_cstr(name, "extern") ? &found.extern_
			: mc_view_eq_cstr(name, "op") ? &found.op : mc_view_eq_cstr(name, "wrap") ? &found.wrap : &found.uninit;
		if (*slot != NULL) {
			mc_label_main(mc_error(c->diag, mc_E0403, attribute->span, "attribute `" mc_view_fmt "` is given twice", mc_view_arg(name)), "repeated");
			continue;
		}
		if (mc_view_eq_cstr(name, "extern") && value != mc_attribute_value_string && value != mc_attribute_value_name) {
			mc_label_main(mc_error(c->diag, mc_E0403, attribute->span, "`extern` needs the name of the C function"), "write `extern = \"name\"`");
			continue;
		}
		if ((mc_view_eq_cstr(name, "builtin") || mc_view_eq_cstr(name, "wrap") || mc_view_eq_cstr(name, "uninit")) && value != mc_attribute_value_none) {
			mc_label_main(mc_error(c->diag, mc_E0403, attribute->span, "`" mc_view_fmt "` takes no value", mc_view_arg(name)), "remove the value");
			continue;
		}
		*slot = attribute;
	}
	if (found.builtin != NULL && found.extern_ != NULL) {
		mc_label_main(mc_error(c->diag, mc_E0403, found.extern_->span, "a function cannot be both `builtin` and `extern`"), "choose one");
	}
	return found;
}

static void _mc_duplicate_generics(mc_checker_s* const c, const mc_nodes_s* const generics) {
	for (size_t i = 0; i < generics->count; ++i) {
		const mc_node_s* const generic = generics->data[i];
		bool seen = false;
		for (size_t j = 0; j < i; ++j) seen = seen || mc_view_eq(generics->data[j]->as.generic.name, generic->as.generic.name);
		const bool scalar = mc_scalar_from_name(generic->as.generic.name) != mc_scalar_none_;
		if (seen || scalar) {
			mc_label_main(mc_error(c->diag, mc_E0401, generic->span, seen ? "type parameter `" mc_view_fmt "` is declared twice"
				: "type parameter `" mc_view_fmt "` hides a built-in type", mc_view_arg(generic->as.generic.name)), "rename it");
		}
	}
}

static int _mc_pattern_of(const mc_view_s text) {
	for (size_t i = 0; i < mc_count_of(_mc_patterns); ++i) if (mc_view_eq_cstr(text, _mc_patterns[i].text)) return (int)i;
	return -1;
}

static void _mc_operator_declaration(mc_checker_s* const c, mc_function_s* const info, const mc_node_s* const attribute) {
	const mc_node_s* const node = info->node;
	const bool has_pattern = attribute->as.attribute.value_kind != mc_attribute_value_none;
	const mc_view_s pattern = has_pattern ? attribute->as.attribute.text : mc_view_literal("None");
	for (size_t i = 0; i < mc_count_of(_mc_derived_patterns); ++i) {
		if (has_pattern && mc_view_eq_cstr(pattern, _mc_derived_patterns[i].pattern)) {
			mc_note(mc_label_main(mc_error(c->diag, mc_E0408, attribute->span, "operator `" mc_view_fmt "` cannot be declared", mc_view_arg(pattern)),
				"it is derived"), "%s", _mc_derived_patterns[i].note);
			return;
		}
	}
	const int index = has_pattern ? _mc_pattern_of(pattern) : -1;
	if (index < 0) {
		mc_string_s patterns = {0};
		for (size_t i = 0; i < mc_count_of(_mc_patterns); ++i) mc_string_appendf(&patterns, "%s%s", i > 0 ? " " : "", _mc_patterns[i].text);
		mc_note(mc_label_main(mc_error(c->diag, mc_E0406, attribute->span, "`" mc_view_fmt "` is not an operator pattern", mc_view_arg(pattern)),
			"unknown pattern"), "patterns: %s", mc_string_cstr(&patterns));
		mc_string_free(&patterns);
		return;
	}
	const size_t arity = (size_t)_mc_patterns[index].arity;
	const mc_nodes_s* const parameters = &node->as.function.parameters;
	bool variadic = false;
	for (size_t i = 0; i < parameters->count; ++i) variadic = variadic || parameters->data[i]->as.param.variadic;
	if (variadic) {
		mc_label_main(mc_error(c->diag, mc_E0409, node->as.function.name_span, "operator functions cannot be variadic"), "remove `...`");
		return;
	}
	if (parameters->count != arity) {
		mc_help(mc_label_main(mc_error(c->diag, mc_E0407, node->as.function.name_span,
			"`" mc_view_fmt "` takes %zu operand(s), but `" mc_view_fmt "` has %zu parameter(s)",
			mc_view_arg(pattern), arity, mc_view_arg(node->as.function.name), parameters->count), "parameter count does not match the pattern"),
			"every `_` in the pattern is one parameter, in order");
		return;
	}
	if (mc_function_is_generic(info)) {
		mc_label_main(mc_error(c->diag, mc_E0409, node->as.function.name_span, "operator functions cannot be generic"), "remove the type parameters");
		return;
	}
	mc_instance_s* const instance = mc_instance(c, info, NULL, 0, NULL, node->as.function.name_span);
	if (NULL == instance) return;
	for (size_t i = 0; i < instance->parameters.count; ++i) {
		if (mc_type_reference == instance->parameters.data[i]->kind) {
			mc_label_main(mc_error(c->diag, mc_E0409, node->as.function.name_span, "operator functions take their operands by value"),
				"remove `&` from the parameters");
			return;
		}
	}
	if (NULL == instance->result) {
		mc_label_main(mc_error(c->diag, mc_E0409, node->as.function.name_span, "operator function `" mc_view_fmt "` must return a value",
			mc_view_arg(node->as.function.name)), "add `-> type`");
		return;
	}
	if ((mc_pattern_equal == index || mc_pattern_less == index) && instance->result != mc_scalar_type(c, mc_scalar_bool)) {
		mc_label_main(mc_error(c->diag, mc_E0409, node->as.function.result->span, "`" mc_view_fmt "` must return `bool`", mc_view_arg(pattern)),
			"returns `%s`", mc_name_of(c, instance->result));
		return;
	}
	mc_functions_s* const registered = &info->module->operators[index];
	for (size_t i = 0; i < registered->count; ++i) {
		const mc_function_s* const other = registered->data[i];
		const mc_instance_s* const other_instance = other->instances.count > 0 ? other->instances.data[0] : NULL;
		if (NULL == other_instance || other_instance->parameters.count != instance->parameters.count) continue;
		bool same = true;
		for (size_t p = 0; p < instance->parameters.count; ++p) same = same && other_instance->parameters.data[p] == instance->parameters.data[p];
		if (!same) continue;
		mc_string_s types = {0};
		for (size_t p = 0; p < instance->parameters.count; ++p) {
			if (p > 0) mc_string_append_cstr(&types, ", ");
			mc_type_text(&types, instance->parameters.data[p]);
		}
		mc_label(mc_label_main(mc_error(c->diag, mc_E0410, node->as.function.name_span, "operator `" mc_view_fmt "` for (%s) is declared twice",
			mc_view_arg(pattern), mc_string_cstr(&types)), "declared again"), other->node->as.function.name_span, "first declared here");
		mc_string_free(&types);
		return;
	}
	info->operator_ = true;
	mc_vector_push(registered, info);
}

void mc_function_declaration(mc_checker_s* const c, mc_function_s* const info, mc_context_s* const context) {
	mc_unused(context);
	mc_node_s* const node = info->node;
	_mc_duplicate_generics(c, &node->as.function.generics);
	const mc_attribute_set_s found = mc_attributes(c, &node->as.function.attributes, "function");
	const bool bodyless = found.builtin != NULL || found.extern_ != NULL;
	if (bodyless && node->as.function.body != NULL) {
		mc_label_main(mc_error(c->diag, mc_E0405, node->as.function.body->span, "`" mc_view_fmt "` is `%s` and cannot have a body",
			mc_view_arg(node->as.function.name), found.builtin != NULL ? "builtin" : "extern"), "remove the body");
	}
	if (!bodyless && NULL == node->as.function.body) {
		mc_help(mc_label_main(mc_error(c->diag, mc_E0404, node->as.function.name_span, "`" mc_view_fmt "` has no body", mc_view_arg(node->as.function.name)),
			"expected `{ ... }`"), "only `[builtin]` and `[extern = \"name\"]` functions are declared without a body");
	}
	const mc_node_s* seen_default = NULL;
	const mc_nodes_s* const parameters = &node->as.function.parameters;
	for (size_t i = 0; i < parameters->count; ++i) {
		const mc_node_s* const parameter = parameters->data[i];
		bool duplicate = false;
		for (size_t j = 0; j < i; ++j) duplicate = duplicate || mc_view_eq(parameters->data[j]->as.param.name, parameter->as.param.name);
		if (duplicate) {
			mc_label_main(mc_error(c->diag, mc_E0401, parameter->span, "parameter `" mc_view_fmt "` is declared twice", mc_view_arg(parameter->as.param.name)),
				"declared again");
		}
		if (parameter->as.param.default_ != NULL) {
			seen_default = parameter;
		}
		else if (seen_default != NULL) {
			mc_label(mc_label_main(mc_error(c->diag, mc_E0412, parameter->span, "parameters without defaults cannot follow parameters with defaults"),
				"needs a default"), seen_default->span, "has a default");
		}
	}
	for (size_t i = 0; i < parameters->count; ++i) {
		const mc_node_s* const parameter = parameters->data[i];
		if (!parameter->as.param.variadic) continue;
		if (!bodyless) {
			mc_help(mc_label_main(mc_error(c->diag, mc_E0415, parameter->span, "only `[builtin]` and `[extern]` functions can take `...`"),
				"variadic parameter"), "take a slice instead: `values: []T`");
		}
		else if (i != parameters->count - 1) {
			mc_label_main(mc_error(c->diag, mc_E0415, parameter->span, "`...` must be the last parameter"), "not last");
		}
		else if (parameter->as.param.mutable_ || seen_default != NULL) {
			mc_label_main(mc_error(c->diag, mc_E0415, parameter->span, "a variadic function cannot have `mut` or default parameters"), "variadic");
		}
	}
	if (found.op != NULL) _mc_operator_declaration(c, info, found.op);
}

static void _mc_declarations(mc_checker_s* const c, mc_module_s* const module) {
	mc_context_s* const context = mc_context_new(c, module, NULL, NULL, NULL, NULL, false);
	for (size_t i = 0; i < module->ordered.count; ++i) {
		mc_symbol_s* const symbol = module->ordered.data[i];
		if (mc_symbol_struct == symbol->kind) {
			mc_struct_s* const info = (mc_struct_s*)symbol;
			(void)mc_attributes(c, &info->node->as.struct_.attributes, "struct");
			_mc_duplicate_generics(c, &info->node->as.struct_.generics);
		}
		else if (mc_symbol_function == symbol->kind) {
			mc_function_declaration(c, (mc_function_s*)symbol, context);
		}
	}
}

static void _mc_check_main(mc_checker_s* const c, mc_module_s* const entry) {
	mc_symbol_s* const symbol = mc_map_get_cstr(&entry->symbols, "main");
	if (NULL == symbol || symbol->kind != mc_symbol_function) {
		mc_help(mc_error(c->diag, mc_E0411, mc_no_span, "the entry module `%s` has no `main` function", entry->path),
			"declare `pub main :: () { ... }`");
		return;
	}
	mc_function_s* const info = (mc_function_s*)symbol;
	const mc_node_s* const node = info->node;
	if (!node->as.function.public_) {
		mc_note(mc_label_main(mc_error(c->diag, mc_E0411, node->as.function.name_span, "`main` must be `pub`"), "add `pub`"),
			"`main` is called from outside the program");
	}
	if (mc_function_is_generic(info) || node->as.function.parameters.count > 0) {
		mc_label_main(mc_error(c->diag, mc_E0411, node->as.function.name_span, "`main` takes no parameters and no type parameters"), "remove them");
	}
	mc_instance_s* instance = NULL;
	for (size_t i = 0; i < info->instances.count; ++i) if (0 == info->instances.data[i]->argument_count) instance = info->instances.data[i];
	if (instance != NULL && instance->result != NULL && instance->result != mc_scalar_type(c, mc_scalar_int)) {
		mc_label_main(mc_error(c->diag, mc_E0411, node->as.function.result->span, "`main` returns nothing or `int`"),
			"returns `%s`", mc_name_of(c, instance->result));
	}
}

// --- constants ---------------------------------------------------------------------

const mc_type_s* mc_constant_type(mc_checker_s* const c, mc_constant_s* const info) {
	if (mc_constant_done == info->state) return info->type;
	if (mc_constant_checking == info->state) {
		mc_label_main(mc_error(c->diag, mc_E0702, info->node->as.constant.name_span, "constant `" mc_view_fmt "` depends on itself",
			mc_view_arg(info->node->as.constant.name)), "its value needs its own value");
		info->state = mc_constant_done;
		info->type = mc_special_type(c, mc_special_error);
		return info->type;
	}
	info->state = mc_constant_checking;
	mc_context_s* const context = (info->context != NULL) ? info->context : mc_context_new(c, info->module, NULL, NULL, NULL, NULL, false);
	const mc_attribute_set_s found = mc_attributes(c, &info->node->as.constant.attributes, "constant");
	const bool saved = context->wrap;
	context->wrap = saved || found.wrap != NULL;
	mc_node_s* const value_node = info->node->as.constant.value;
	const mc_type_s* type = mc_expression(c, value_node, context, NULL);
	const mc_type_s* const error = mc_special_type(c, mc_special_error);
	if (mc_type_is_special(type, mc_special_nothing)) {
		mc_label_main(mc_error(c->diag, mc_E0523, value_node->span, "a constant needs a value"), "this has no value");
		type = error;
	}
	else if (mc_type_is_special(type, mc_special_null)) {
		mc_label_main(mc_error(c->diag, mc_E0506, value_node->span, "cannot infer the type of `null`"), "write `(T){}` or use a typed slice");
		type = error;
	}
	mc_cvalue_s value = {0};
	bool has_value = false;
	if (type != error) {
		char* const what = mc_format("constant `" mc_view_fmt "`", mc_view_arg(info->node->as.constant.name));
		has_value = mc_require_constant(c, value_node, context, what, &value);
		mc_free(what);
	}
	context->wrap = saved;
	info->type = has_value ? type : error;
	if (has_value) {
		info->value = mc_arena_new(c->arena, mc_cvalue_s);
		*info->value = value;
	}
	info->state = mc_constant_done;
	if (NULL == info->context) {
		bool listed = false;
		for (size_t i = 0; i < c->program->constants.count; ++i) listed = listed || c->program->constants.data[i] == info;
		if (!listed) mc_vector_push(&c->program->constants, info);
	}
	return info->type;
}

bool mc_require_constant(mc_checker_s* const c, const mc_node_s* const node, mc_context_s* const context, const char* const what, mc_cvalue_s* const value) {
	mc_unused(context);
	const mc_evaluation_s result = mc_evaluate(&c->evaluator, node, false);
	if (mc_evaluation_ok == result.status) {
		*value = result.value;
		return true;
	}
	if (mc_evaluation_not_constant == result.status) {
		mc_diagnostic_s* const diagnostic = mc_label_main(mc_error(c->diag, mc_E0701, node->span, "%s must be known at compile time", what),
			"evaluated at compile time");
		if (result.node != NULL && result.node != node) mc_label(diagnostic, result.node->span, "%s", result.reason);
		else mc_note(diagnostic, "%s", result.reason);
	}
	return false;
}

bool mc_try_value(mc_checker_s* const c, const mc_node_s* const node, mc_cvalue_s* const value) {
	const mc_evaluation_s result = mc_evaluate(&c->evaluator, node, true);
	if (mc_evaluation_ok != result.status) return false;
	*value = result.value;
	return true;
}

// --- functions and instances -----------------------------------------------------------

/// the parameter and result types of a function declaration in 'context'.
static void _mc_resolve_signature(mc_checker_s* const c, const mc_node_s* const node, mc_context_s* const context,
								  mc_type_list_s* const parameters, const mc_type_s** const result) {
	const mc_nodes_s* const nodes = &node->as.function.parameters;
	for (size_t i = 0; i < nodes->count; ++i) {
		const mc_node_s* const parameter = nodes->data[i];
		if (parameter->as.param.variadic) {
			mc_vector_push(parameters, mc_special_type(c, mc_special_variadic));
			continue;
		}
		const mc_type_s* type = mc_resolve_type(c, parameter->as.param.type, context, true, false);
		if (mc_type_reference == type->kind) type = mc_type_reference_to(c->types, type->as.reference.element, parameter->as.param.mutable_);
		mc_vector_push(parameters, type);
	}
	*result = (node->as.function.result != NULL) ? mc_resolve_type(c, node->as.function.result, context, false, false) : NULL;
}

mc_instance_s* mc_instance(mc_checker_s* const c, mc_function_s* const info, const mc_type_s* const* const arguments, const size_t count,
						   mc_context_s* const context, const mc_span_s span) {
	for (size_t i = 0; i < info->instances.count; ++i) {
		mc_instance_s* const existing = info->instances.data[i];
		if (existing->argument_count != count) continue;
		bool same = true;
		for (size_t a = 0; a < count; ++a) same = same && existing->arguments[a] == arguments[a];
		if (same) return existing;
	}
	const bool generic = mc_function_is_generic(info);
	const int depth = (context != NULL && generic) ? context->depth + 1 : 0;
	if (depth > _mc_instantiation_limit) {
		mc_note(mc_label_main(mc_error(c->diag, mc_E0527, span, "too many nested instantiations of `" mc_view_fmt "`", mc_view_arg(info->node->as.function.name)),
			"more than %d levels deep", _mc_instantiation_limit), "a generic function probably instantiates itself with ever larger types");
		return NULL;
	}
	mc_instance_s* const instance = mc_arena_new(c->arena, mc_instance_s);
	instance->function = info;
	instance->argument_count = count;
	if (count > 0) {
		instance->arguments = mc_arena_alloc(c->arena, count * sizeof(*instance->arguments));
		memcpy(instance->arguments, arguments, count * sizeof(*instance->arguments));
	}
	const mc_nodes_s* const generics = &info->node->as.function.generics;
	for (size_t i = 0; i < generics->count && i < count; ++i) mc_bindings_set(&instance->bindings, generics->data[i]->as.generic.name, arguments[i]);
	instance->node = generic ? mc_node_clone(c->arena, info->node) : info->node;
	instance->depth = depth;
	mc_arena_push(c->arena, &info->instances, instance);
	mc_context_s* const declaration = mc_context_new(c, info->module, &instance->bindings, NULL, info->parent, NULL, false);
	if (info->parent != NULL) {
		declaration->bindings.count = 0;
		for (size_t i = 0; i < info->parent->bindings.count; ++i) mc_bindings_set(&declaration->bindings, info->parent->bindings.data[i].name, info->parent->bindings.data[i].type);
		for (size_t i = 0; i < instance->bindings.count; ++i) mc_bindings_set(&declaration->bindings, instance->bindings.data[i].name, instance->bindings.data[i].type);
	}
	_mc_resolve_signature(c, instance->node, declaration, &instance->parameters, &instance->result);
	mc_vector_push(&c->program->instances, instance);
	mc_vector_push(&c->queue, instance);
	return instance;
}

void mc_signature(mc_checker_s* const c, mc_function_s* const info, mc_type_list_s* const parameters, const mc_type_s** const result) {
	mc_bindings_s placeholders = {0};
	const mc_nodes_s* const generics = &info->node->as.function.generics;
	for (size_t i = 0; i < generics->count; ++i) {
		const mc_view_s name = generics->data[i]->as.generic.name;
		mc_bindings_set(&placeholders, name, mc_type_parameter_of(c->types, name, info));
	}
	mc_context_s* const context = mc_context_new(c, info->module, &placeholders, NULL, info->parent, NULL, false);
	mc_vector_free(&placeholders);
	_mc_resolve_signature(c, info->node, context, parameters, result);
}

void mc_check_instance(mc_checker_s* const c, mc_instance_s* const instance) {
	if (instance->state != mc_instance_pending) return;
	instance->state = mc_instance_checking;
	mc_function_s* const info = instance->function;
	mc_node_s* const node = instance->node;
	mc_bindings_s bindings = {0};
	if (info->parent != NULL) {
		for (size_t i = 0; i < info->parent->bindings.count; ++i) mc_bindings_set(&bindings, info->parent->bindings.data[i].name, info->parent->bindings.data[i].type);
	}
	for (size_t i = 0; i < instance->bindings.count; ++i) mc_bindings_set(&bindings, instance->bindings.data[i].name, instance->bindings.data[i].type);
	mc_context_s* const context = mc_context_new(c, info->module, &bindings, instance->result, info->parent, instance, true);
	mc_vector_free(&bindings);
	context->wrap = context->wrap || mc_attribute_find(&node->as.function.attributes, "wrap") != NULL;
	const mc_nodes_s* const parameters = &node->as.function.parameters;
	for (size_t i = 0; i < parameters->count && i < instance->parameters.count; ++i) {
		mc_node_s* const parameter = parameters->data[i];
		const mc_type_s* const type = instance->parameters.data[i];
		const bool reference = mc_type_reference == type->kind;
		mc_variable_s* const variable = mc_arena_new(c->arena, mc_variable_s);
		variable->base.kind = mc_symbol_variable;
		variable->name = parameter->as.param.name;
		variable->type = reference ? type->as.reference.element : type;
		variable->kind = mc_variable_param;
		variable->mutable_ = parameter->as.param.mutable_;
		variable->span = parameter->span;
		variable->reference = reference;
		variable->level = context->level;
		parameter->note.variable = variable;
		mc_vector_push(&instance->variables, variable);
		if (parameter->as.param.default_ != NULL) {
			const mc_type_s* const target = reference ? type->as.reference.element : type;
			const mc_type_s* const actual = mc_expression(c, parameter->as.param.default_, context, target);
			mc_coerce(c, parameter->as.param.default_, actual, target);
			mc_cvalue_s ignored = {0};
			(void)mc_require_constant(c, parameter->as.param.default_, context, "a default argument", &ignored);
		}
		(void)mc_context_declare(context, parameter->as.param.name, &variable->base);
		mc_vector_push(&context->variables, variable);
	}
	if (node->as.function.body != NULL) {
		mc_block(c, node->as.function.body, context, false);
		if (instance->result != NULL && !mc_type_is_special(instance->result, mc_special_error) && !mc_terminates(node->as.function.body)) {
			const mc_span_s end = node->as.function.body->span;
			const mc_span_s last = { .file = end.file, .begin = end.end - 1, .end = end.end, };
			mc_label(mc_label_main(mc_error(c->diag, mc_E0514, node->as.function.name_span, "`" mc_view_fmt "` does not return a value on every path",
				mc_view_arg(info->node->as.function.name)), "must return `%s`", mc_name_of(c, instance->result)), last, "the end of the body can be reached");
		}
		mc_unused_mut(c, context);
	}
	instance->state = mc_instance_checked;
}

void mc_unused_mut(mc_checker_s* const c, mc_context_s* const context) {
	for (size_t i = 0; i < context->variables.count; ++i) {
		const mc_variable_s* const variable = context->variables.data[i];
		if (!variable->mutable_ || variable->mutated || mc_variable_for == variable->kind) continue;
		const char* const what = variable->reference ? "reference parameter" : (mc_variable_let == variable->kind) ? "variable" : "parameter";
		mc_label_main(mc_warning(c->diag, mc_W0503, variable->span, "%s `" mc_view_fmt "` is `mut` but never changed", what, mc_view_arg(variable->name)),
			"remove `mut`");
	}
}

// --- the program -------------------------------------------------------------------

mc_program_s* mc_check(const char* const path, mc_sources_s* const sources, mc_diag_s* const diag, const mc_check_options_s* const options) {
	mc_program_s* const program = mc_calloc(1, sizeof(*program));
	mc_types_init(&program->types);
	program->sources = sources;
	program->diag = diag;
	mc_checker_s* const c = mc_calloc(1, sizeof(*c));
	c->program = program;
	c->arena = &program->arena;
	c->types = &program->types;
	c->diag = diag;
	c->sources = sources;
	c->options = options;
	c->evaluator.checker = c;
	program->checker = c;
	mc_module_s* const entry = mc_load(c, path);
	program->entry = entry;
	for (size_t i = 0; i < program->modules.count; ++i) _mc_collect(c, program->modules.data[i]);
	for (size_t i = 0; i < program->modules.count; ++i) _mc_resolve_imports(c, program->modules.data[i]);
	for (size_t i = 0; i < program->modules.count; ++i) _mc_declarations(c, program->modules.data[i]);
	for (size_t i = 0; i < program->modules.count; ++i) {
		mc_module_s* const module = program->modules.data[i];
		for (size_t s = 0; s < module->ordered.count; ++s) {
			mc_symbol_s* const symbol = module->ordered.data[s];
			if (mc_symbol_constant == symbol->kind) {
				(void)mc_constant_type(c, (mc_constant_s*)symbol);
			}
			else if (mc_symbol_struct == symbol->kind) {
				mc_struct_s* const info = (mc_struct_s*)symbol;
				if (0 == info->node->as.struct_.generics.count) (void)mc_struct_type(c, info, NULL, 0, info->node->as.struct_.name_span);
			}
			else if (mc_symbol_function == symbol->kind) {
				mc_function_s* const info = (mc_function_s*)symbol;
				if (!mc_function_is_generic(info)) (void)mc_instance(c, info, NULL, 0, NULL, info->node->as.function.name_span);
			}
		}
	}
	while (c->queue_head < c->queue.count) mc_check_instance(c, c->queue.data[c->queue_head++]);
	if (entry != NULL) _mc_check_main(c, entry);
	return program;
}

void mc_program_free(mc_program_s* const program) {
	if (NULL == program) return;
	mc_checker_s* const c = program->checker;
	if (c != NULL) {
		for (size_t i = 0; i < c->contexts.count; ++i) _mc_context_free(c->contexts.data[i]);
		mc_vector_free(&c->contexts);
		mc_vector_free(&c->queue);
		mc_vector_free(&c->sizing);
		mc_vector_free(&c->recursive_reported);
		mc_vector_free(&c->roots);
		mc_vector_free(&c->evaluator.frames);
		mc_free(c);
	}
	for (size_t i = 0; i < program->modules.count; ++i) {
		mc_module_s* const module = program->modules.data[i];
		mc_vector_free(&module->imports);
		mc_map_free(&module->symbols);
		mc_vector_free(&module->ordered);
		mc_map_free(&module->imported);
		mc_vector_free(&module->star);
		mc_map_free(&module->qualifiers);
		for (size_t p = 0; p < mc_pattern_count_; ++p) mc_vector_free(&module->operators[p]);
	}
	for (size_t i = 0; i < program->instances.count; ++i) {
		mc_instance_s* const instance = program->instances.data[i];
		mc_vector_free(&instance->bindings);
		mc_vector_free(&instance->parameters);
		mc_vector_free(&instance->variables);
	}
	mc_vector_free(&program->modules);
	mc_vector_free(&program->instances);
	for (size_t i = 0; i < program->layouts.count; ++i) mc_vector_free(&program->layouts.data[i]->fields);
	mc_vector_free(&program->layouts);
	mc_vector_free(&program->constants);
	mc_types_free(&program->types);
	mc_arena_free(&program->arena);
	mc_free(program);
}
