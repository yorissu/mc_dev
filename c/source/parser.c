
#include "mc/parser.h"

#include <setjmp.h>
#include <string.h>

typedef struct {
	size_t     index;
	mc_token_s token;
} _mc_undo_s;

typedef struct {
	mc_tokens_s                tokens     ;
	size_t                     position   ;
	mc_arena_s*                arena      ;
	mc_diag_s*                 diag       ;
	const mc_source_s*         source     ;
	int                        speculating;
	bool                       composites ;   // false in conditions, where '{' starts the body
	jmp_buf*                   recover    ;   // where a syntax error continues
	mc_vector_of(_mc_undo_s)   undo       ;   // tokens split by '>' (undone when speculation fails)
} _mc_parser_s;

// --- tokens ------------------------------------------------------------------

static const mc_token_s* _mc_token(const _mc_parser_s* const p) {
	return &p->tokens.data[p->position];
}

static const mc_token_s* _mc_peek(const _mc_parser_s* const p, const size_t offset) {
	const size_t index = p->position + offset;
	return &p->tokens.data[index < p->tokens.count ? index : p->tokens.count - 1];
}

static bool _mc_at(const _mc_parser_s* const p, const mc_token_kind_e kind) {
	return _mc_token(p)->kind == kind;
}

static const mc_token_s* _mc_advance(_mc_parser_s* const p) {
	const mc_token_s* const token = _mc_token(p);
	if (token->kind != mc_token_eof) p->position += 1;
	return token;
}

static const mc_token_s* _mc_accept(_mc_parser_s* const p, const mc_token_kind_e kind) {
	return _mc_at(p, kind) ? _mc_advance(p) : NULL;
}

static bool _mc_is_keyword(const mc_token_kind_e kind) {
	return kind >= mc_token_pub && kind <= mc_token_assert;
}

static mc_span_s _mc_span_from(const _mc_parser_s* const p, const mc_span_s start) {
	const mc_token_s* const previous = (p->position > 0) ? &p->tokens.data[p->position - 1] : _mc_token(p);
	return (const mc_span_s) {
		.file = start.file, .begin = start.begin, .end = previous->span.end > start.end ? previous->span.end : start.end,
	};
}

/// how a token is named in messages: `text`, or 'end of file'.
static char* _mc_describe(const mc_token_s* const token) {
	if (mc_token_eof == token->kind) return mc_strdup("end of file");
	return mc_format("`" mc_view_fmt "`", mc_view_arg(token->text));
}

mc_noreturn static void _mc_raise(_mc_parser_s* const p) {
	longjmp(*p->recover, 1);
}

/// reports a syntax error (unless speculating) and continues at the nearest
/// recovery point.
mc_noreturn static void _mc_fail(_mc_parser_s* const p, const mc_code_e code, const mc_span_s span,
								 const char* const label, const char* const format, ...) mc_printf_like(5, 6);

mc_noreturn static void _mc_fail(_mc_parser_s* const p, const mc_code_e code, const mc_span_s span,
								 const char* const label, const char* const format, ...) {
	if (0 == p->speculating) {
		va_list args; va_start(args, format);
		char* const message = mc_vformat(format, args);
		va_end(args);
		mc_label_main(mc_error(p->diag, code, span, "%s", message), "%s", label);
		mc_free(message);
	}
	_mc_raise(p);
}

static const mc_token_s* _mc_expect(_mc_parser_s* const p, const mc_token_kind_e kind, const char* const context) {
	if (_mc_at(p, kind)) return _mc_advance(p);
	char* const found = _mc_describe(_mc_token(p));
	char* const label = mc_format("expected `%s`", mc_token_kind_text(kind));
	char* const message = mc_format("expected `%s`%s%s, found %s", mc_token_kind_text(kind),
		(context[0] != '\0') ? " " : "", context, found);
	if (0 == p->speculating) mc_label_main(mc_error(p->diag, mc_E0201, _mc_token(p)->span, "%s", message), "%s", label);
	mc_free(found);
	mc_free(label);
	mc_free(message);
	_mc_raise(p);
}

static const mc_token_s* _mc_expect_identifier(_mc_parser_s* const p, const char* const what) {
	if (_mc_at(p, mc_token_identifier)) return _mc_advance(p);
	char* const found = _mc_describe(_mc_token(p));
	char* const message = mc_format("expected %s, found %s", what, found);
	char* const label = mc_format("expected %s", what);
	if (0 == p->speculating) mc_label_main(mc_error(p->diag, mc_E0201, _mc_token(p)->span, "%s", message), "%s", label);
	mc_free(found);
	mc_free(message);
	mc_free(label);
	_mc_raise(p);
}

/// '>' closing type arguments; splits '>>', '>=' and '>>=' tokens.
static void _mc_expect_close_angle(_mc_parser_s* const p) {
	const mc_token_s token = *_mc_token(p);
	if (mc_token_greater == token.kind) {
		_mc_advance(p);
		return;
	}
	mc_token_kind_e rest = mc_token_eof;
	if (mc_token_shift_right == token.kind) rest = mc_token_greater;
	else if (mc_token_greater_equal == token.kind) rest = mc_token_equal;
	else if (mc_token_shift_right_assign == token.kind) rest = mc_token_greater_equal;
	if (rest != mc_token_eof) {
		const _mc_undo_s undo = { .index = p->position, .token = token, };
		mc_vector_push(&p->undo, undo);
		mc_token_s* const split = &p->tokens.data[p->position];
		split->kind = rest;
		split->span.begin += 1;
		split->text = mc_view_slice(token.text, 1, token.text.length);
		return;
	}
	char* const found = _mc_describe(&token);
	if (0 == p->speculating) mc_label_main(mc_error(p->diag, mc_E0201, token.span, "expected `>`, found %s", found), "expected `>`");
	mc_free(found);
	_mc_raise(p);
}

/// skips to just after the next 'stop' token at nesting depth zero.
static void _mc_recover(_mc_parser_s* const p, const mc_token_kind_e stop_a, const mc_token_kind_e stop_b) {
	int depth = 0;
	while (!_mc_at(p, mc_token_eof)) {
		const mc_token_kind_e kind = _mc_token(p)->kind;
		if (0 == depth && (kind == stop_a || kind == stop_b)) {
			_mc_advance(p);
			return;
		}
		if (mc_token_left_brace == kind || mc_token_left_paren == kind || mc_token_left_bracket == kind) {
			depth += 1;
		}
		else if (mc_token_right_brace == kind || mc_token_right_paren == kind || mc_token_right_bracket == kind) {
			if (0 == depth) return;
			depth -= 1;
			if (0 == depth && (mc_token_right_brace == stop_a || mc_token_right_brace == stop_b) && mc_token_right_brace == kind) {
				_mc_advance(p);
				return;
			}
		}
		_mc_advance(p);
	}
}

static mc_node_s* _mc_new(_mc_parser_s* const p, const mc_node_kind_e kind, const mc_span_s span) {
	return mc_node_new(p->arena, kind, span);
}

// declarations of the mutually recursive parts.
static mc_node_s* _mc_declaration(_mc_parser_s* const p, const bool top);
static mc_node_s* _mc_type(_mc_parser_s* const p);
static mc_node_s* _mc_block(_mc_parser_s* const p);
static mc_node_s* _mc_statement(_mc_parser_s* const p);
static mc_node_s* _mc_expression(_mc_parser_s* const p);
static mc_node_s* _mc_unary(_mc_parser_s* const p);
static mc_node_s* _mc_composite(_mc_parser_s* const p, const mc_span_s start, mc_node_s* const type);
static mc_nodes_s _mc_attributes(_mc_parser_s* const p);

// --- modules -----------------------------------------------------------------

static mc_views_s _mc_path(_mc_parser_s* const p) {
	mc_views_s path = {0};
	mc_arena_push(p->arena, &path, _mc_expect_identifier(p, "a module name")->text);
	while (_mc_accept(p, mc_token_dot)) mc_arena_push(p->arena, &path, _mc_expect_identifier(p, "a module name")->text);
	return path;
}

static mc_node_s* _mc_import(_mc_parser_s* const p) {
	const mc_span_s start = _mc_advance(p)->span;
	mc_views_s path = _mc_path(p);
	mc_node_s* const node = _mc_new(p, mc_node_import, start);
	node->as.import.path = path;
	if (_mc_accept(p, mc_token_as)) {
		node->as.import.has_alias = true;
		node->as.import.alias = _mc_expect_identifier(p, "an alias")->text;
	}
	node->span = _mc_span_from(p, start);
	return node;
}

static mc_node_s* _mc_from_import(_mc_parser_s* const p) {
	const mc_span_s start = _mc_advance(p)->span;
	mc_node_s* const node = _mc_new(p, mc_node_from_import, start);
	node->as.from_import.path = _mc_path(p);
	_mc_expect(p, mc_token_import, "after the module name");
	if (_mc_accept(p, mc_token_star)) {
		node->as.from_import.star = true;
		node->span = _mc_span_from(p, start);
		return node;
	}
	for (;;) {
		const mc_token_s* const name = _mc_expect_identifier(p, "a name to import");
		mc_node_s* const item = _mc_new(p, mc_node_import_name, name->span);
		item->as.import_name.name = name->text;
		if (_mc_accept(p, mc_token_as)) {
			item->as.import_name.has_alias = true;
			item->as.import_name.alias = _mc_expect_identifier(p, "an alias")->text;
		}
		item->span = _mc_span_from(p, name->span);
		mc_arena_push(p->arena, &node->as.from_import.names, item);
		if (!_mc_accept(p, mc_token_comma)) break;
	}
	node->span = _mc_span_from(p, start);
	return node;
}

static mc_node_s* _mc_module(_mc_parser_s* const p) {
	const mc_span_s start = _mc_token(p)->span;
	mc_node_s* const module = _mc_new(p, mc_node_module, start);
	while (!_mc_at(p, mc_token_eof)) {
		const size_t before = p->position;
		jmp_buf here;
		jmp_buf* const saved = p->recover;
		p->recover = &here;
		if (0 == setjmp(here)) {
			if (_mc_at(p, mc_token_import)) mc_arena_push(p->arena, &module->as.module.imports, _mc_import(p));
			else if (_mc_at(p, mc_token_from)) mc_arena_push(p->arena, &module->as.module.imports, _mc_from_import(p));
			else mc_arena_push(p->arena, &module->as.module.declarations, _mc_declaration(p, true));
		}
		else {
			_mc_recover(p, mc_token_semicolon, mc_token_right_brace);
			if (p->position == before) _mc_advance(p);
		}
		p->recover = saved;
	}
	module->span = _mc_span_from(p, start);
	return module;
}

// --- attributes --------------------------------------------------------------

static mc_node_s* _mc_attribute(_mc_parser_s* const p) {
	const mc_token_s* const token = _mc_token(p);
	const mc_span_s start = token->span;
	if (token->kind != mc_token_identifier && !_mc_is_keyword(token->kind)) {
		char* const found = _mc_describe(token);
		char* const message = mc_format("expected an attribute name, found %s", found);
		mc_free(found);
		if (0 == p->speculating) mc_label_main(mc_error(p->diag, mc_E0204, token->span, "%s", message), "expected an attribute");
		mc_free(message);
		_mc_raise(p);
	}
	const mc_view_s name = _mc_advance(p)->text;
	mc_node_s* const node = _mc_new(p, mc_node_attribute, start);
	node->as.attribute.name = name;
	if (mc_view_eq_cstr(name, "op") && _mc_at(p, mc_token_dot)) {
		_mc_advance(p);
		mc_string_s pattern = {0};
		int depth = 0;
		while (!_mc_at(p, mc_token_eof)) {
			if (0 == depth && (_mc_at(p, mc_token_comma) || _mc_at(p, mc_token_right_bracket))) break;
			if (_mc_at(p, mc_token_left_bracket)) depth += 1;
			else if (_mc_at(p, mc_token_right_bracket)) depth -= 1;
			mc_string_append(&pattern, _mc_advance(p)->text);
		}
		if (0 == pattern.length) {
			mc_string_free(&pattern);
			_mc_fail(p, mc_E0204, _mc_token(p)->span, "for example `_+_`", "expected an operator pattern after `op.`");
		}
		node->as.attribute.value_kind = mc_attribute_value_name;
		node->as.attribute.text = mc_view_new(mc_arena_strndup(p->arena, pattern.data, pattern.length), pattern.length);
		mc_string_free(&pattern);
		node->span = _mc_span_from(p, start);
		return node;
	}
	if (_mc_accept(p, mc_token_equal)) {
		const mc_token_s* const value = _mc_token(p);
		if (mc_token_string == value->kind) {
			node->as.attribute.value_kind = mc_attribute_value_string;
			node->as.attribute.text = value->bytes;
		}
		else if (mc_token_integer == value->kind) {
			node->as.attribute.value_kind = mc_attribute_value_integer;
			node->as.attribute.integer = (int64_t)value->integer;
		}
		else if (mc_token_identifier == value->kind || mc_token_type == value->kind || _mc_is_keyword(value->kind)) {
			node->as.attribute.value_kind = mc_attribute_value_name;
			node->as.attribute.text = value->text;
		}
		else {
			char* const found = _mc_describe(value);
			char* const message = mc_format("expected an attribute value, found %s", found);
			mc_free(found);
			if (0 == p->speculating) mc_label_main(mc_error(p->diag, mc_E0204, value->span, "%s", message), "expected a value");
			mc_free(message);
			_mc_raise(p);
		}
		_mc_advance(p);
	}
	node->span = _mc_span_from(p, start);
	return node;
}

static mc_nodes_s _mc_attributes(_mc_parser_s* const p) {
	mc_nodes_s result = {0};
	while (_mc_at(p, mc_token_left_bracket)) {
		_mc_advance(p);
		for (;;) {
			mc_arena_push(p->arena, &result, _mc_attribute(p));
			if (!_mc_accept(p, mc_token_comma)) break;
		}
		_mc_expect(p, mc_token_right_bracket, "to close the attribute list");
	}
	return result;
}

// --- declarations ------------------------------------------------------------

/// at '(': whether a parameter list (not a parenthesized expression) follows.
static bool _mc_is_function_start(const _mc_parser_s* const p) {
	const mc_token_s* const after = _mc_peek(p, 1);
	return mc_token_right_paren == after->kind || mc_token_mut == after->kind
		|| (mc_token_identifier == after->kind && mc_token_colon == _mc_peek(p, 2)->kind);
}

static mc_nodes_s _mc_parameters(_mc_parser_s* const p) {
	_mc_expect(p, mc_token_left_paren, "");
	mc_nodes_s parameters = {0};
	while (!_mc_at(p, mc_token_right_paren)) {
		const mc_span_s start = _mc_token(p)->span;
		const bool mutable_ = NULL != _mc_accept(p, mc_token_mut);
		const mc_token_s* const name = _mc_expect_identifier(p, "a parameter name");
		_mc_expect(p, mc_token_colon, "after the parameter name");
		mc_node_s* const parameter = _mc_new(p, mc_node_param, start);
		parameter->as.param.name = name->text;
		parameter->as.param.mutable_ = mutable_;
		if (_mc_accept(p, mc_token_ellipsis)) {
			parameter->as.param.variadic = true;
		}
		else {
			parameter->as.param.type = _mc_type(p);
			if (_mc_accept(p, mc_token_equal)) parameter->as.param.default_ = _mc_expression(p);
		}
		parameter->span = _mc_span_from(p, start);
		mc_arena_push(p->arena, &parameters, parameter);
		if (!_mc_accept(p, mc_token_comma)) break;
	}
	_mc_expect(p, mc_token_right_paren, "to close the parameter list");
	return parameters;
}

static mc_node_s* _mc_function(_mc_parser_s* const p, const mc_span_s start, const mc_token_s* const name,
							   const mc_nodes_s generics, const mc_nodes_s attributes, const bool public_) {
	mc_node_s* const node = _mc_new(p, mc_node_function, start);
	node->as.function.name = name->text;
	node->as.function.name_span = name->span;
	node->as.function.generics = generics;
	node->as.function.attributes = attributes;
	node->as.function.public_ = public_;
	node->as.function.parameters = _mc_parameters(p);
	if (_mc_accept(p, mc_token_arrow)) node->as.function.result = _mc_type(p);
	if (!_mc_accept(p, mc_token_semicolon)) {
		if (!_mc_at(p, mc_token_left_brace)) {
			char* const found = _mc_describe(_mc_token(p));
			char* const message = mc_format("expected a function body or `;`, found %s", found);
			mc_free(found);
			if (0 == p->speculating) mc_label_main(mc_error(p->diag, mc_E0201, _mc_token(p)->span, "%s", message), "expected `{`");
			mc_free(message);
			_mc_raise(p);
		}
		node->as.function.body = _mc_block(p);
	}
	node->span = _mc_span_from(p, start);
	return node;
}

static mc_node_s* _mc_struct(_mc_parser_s* const p, const mc_span_s start, const mc_token_s* const name,
							 const mc_nodes_s generics, const mc_nodes_s attributes, const bool public_) {
	mc_node_s* const node = _mc_new(p, mc_node_struct, start);
	node->as.struct_.name = name->text;
	node->as.struct_.name_span = name->span;
	node->as.struct_.generics = generics;
	node->as.struct_.attributes = attributes;
	node->as.struct_.public_ = public_;
	_mc_expect(p, mc_token_left_brace, "");
	while (!_mc_at(p, mc_token_right_brace) && !_mc_at(p, mc_token_eof)) {
		const mc_nodes_s field_attributes = _mc_attributes(p);
		const mc_span_s field_start = field_attributes.count > 0 ? field_attributes.data[0]->span : _mc_token(p)->span;
		_mc_expect(p, mc_token_let, "to start a field");
		const bool mutable_ = NULL != _mc_accept(p, mc_token_mut);
		const mc_token_s* const field_name = _mc_expect_identifier(p, "a field name");
		_mc_expect(p, mc_token_colon, "after the field name");
		mc_node_s* const field = _mc_new(p, mc_node_field, field_start);
		field->as.field.name = field_name->text;
		field->as.field.mutable_ = mutable_;
		field->as.field.attributes = field_attributes;
		field->as.field.type = _mc_type(p);
		if (_mc_accept(p, mc_token_equal)) field->as.field.default_ = _mc_expression(p);
		_mc_expect(p, mc_token_semicolon, "after the field");
		field->span = _mc_span_from(p, field_start);
		mc_arena_push(p->arena, &node->as.struct_.fields, field);
	}
	_mc_expect(p, mc_token_right_brace, "to close the struct");
	node->span = _mc_span_from(p, start);
	return node;
}

static mc_node_s* _mc_declaration_rest(_mc_parser_s* const p, const bool top, const mc_nodes_s attributes) {
	const mc_span_s start = attributes.count > 0 ? attributes.data[0]->span : _mc_token(p)->span;
	bool public_ = false;
	if (_mc_at(p, mc_token_pub)) {
		const mc_token_s* const pub = _mc_advance(p);
		public_ = true;
		if (!top) mc_label_main(mc_error(p->diag, mc_E0205, pub->span, "local declarations cannot be `pub`"),
			"only module-level declarations are exported");
	}
	const mc_token_s* const name = _mc_expect_identifier(p, "a declaration name");
	mc_nodes_s generics = {0};
	if (_mc_at(p, mc_token_less)) {
		_mc_advance(p);
		for (;;) {
			const mc_token_s* const generic = _mc_expect_identifier(p, "a type parameter name");
			mc_node_s* const node = _mc_new(p, mc_node_generic, generic->span);
			node->as.generic.name = generic->text;
			mc_arena_push(p->arena, &generics, node);
			if (!_mc_accept(p, mc_token_comma)) break;
		}
		_mc_expect_close_angle(p);
		if (!top) {
			const mc_span_s span = { .file = name->span.file, .begin = name->span.begin, .end = p->tokens.data[p->position - 1].span.end, };
			mc_label_main(mc_error(p->diag, mc_E0205, span, "generic declarations are only allowed at module level"),
				"declared inside a function");
		}
	}
	char* const context = mc_format("after `" mc_view_fmt "`", mc_view_arg(name->text));
	if (!_mc_at(p, mc_token_colon_colon)) {
		char* const found = _mc_describe(_mc_token(p));
		if (0 == p->speculating) mc_label_main(mc_error(p->diag, mc_E0201, _mc_token(p)->span,
			"expected `::` %s, found %s", context, found), "expected `::`");
		mc_free(found);
		mc_free(context);
		_mc_raise(p);
	}
	mc_free(context);
	_mc_advance(p);
	if (_mc_at(p, mc_token_left_brace) && (mc_token_let == _mc_peek(p, 1)->kind || mc_token_left_bracket == _mc_peek(p, 1)->kind)) {
		mc_node_s* const node = _mc_struct(p, start, name, generics, attributes, public_);
		if (!top) mc_label_main(mc_error(p->diag, mc_E0205, name->span, "struct declarations are only allowed at module level"),
			"declared inside a function");
		return node;
	}
	if (_mc_at(p, mc_token_left_brace) && mc_token_right_brace == _mc_peek(p, 1)->kind) {
		const mc_span_s brace = _mc_token(p)->span;
		_mc_advance(p);
		_mc_advance(p);
		_mc_fail(p, mc_E0203, mc_span_join(brace, p->tokens.data[p->position - 1].span),
			"a struct needs at least one `let` field", "empty structs are not allowed");
	}
	if (_mc_at(p, mc_token_left_paren) && _mc_is_function_start(p)) {
		return _mc_function(p, start, name, generics, attributes, public_);
	}
	if (generics.count > 0) mc_label_main(mc_error(p->diag, mc_E0205, name->span, "only functions and structs can be generic"),
		"a constant cannot have type parameters");
	mc_node_s* const node = _mc_new(p, mc_node_constant, start);
	node->as.constant.name = name->text;
	node->as.constant.name_span = name->span;
	node->as.constant.attributes = attributes;
	node->as.constant.public_ = public_;
	node->as.constant.value = _mc_expression(p);
	_mc_expect(p, mc_token_semicolon, "after the constant");
	node->span = _mc_span_from(p, start);
	return node;
}

static mc_node_s* _mc_declaration(_mc_parser_s* const p, const bool top) {
	const mc_nodes_s attributes = _mc_attributes(p);
	return _mc_declaration_rest(p, top, attributes);
}

// --- types -------------------------------------------------------------------

static mc_node_s* _mc_type(_mc_parser_s* const p) {
	const mc_token_s* const token = _mc_token(p);
	const mc_span_s start = token->span;
	if (mc_token_left_bracket == token->kind) {
		_mc_advance(p);
		if (_mc_accept(p, mc_token_right_bracket)) {
			mc_node_s* const node = _mc_new(p, mc_node_type_slice, start);
			node->as.type_slice.element = _mc_type(p);
			node->span = _mc_span_from(p, start);
			return node;
		}
		if (_mc_at(p, mc_token_identifier) && mc_view_eq_cstr(_mc_token(p)->text, "_") && mc_token_right_bracket == _mc_peek(p, 1)->kind) {
			_mc_advance(p);
			_mc_advance(p);
			mc_node_s* const node = _mc_new(p, mc_node_type_array, start);
			node->as.type_array.element = _mc_type(p);
			node->span = _mc_span_from(p, start);
			return node;
		}
		const bool saved = p->composites;
		p->composites = true;
		mc_node_s* const length = _mc_expression(p);
		p->composites = saved;
		_mc_expect(p, mc_token_right_bracket, "after the array length");
		mc_node_s* const node = _mc_new(p, mc_node_type_array, start);
		node->as.type_array.length = length;
		node->as.type_array.element = _mc_type(p);
		node->span = _mc_span_from(p, start);
		return node;
	}
	if (mc_token_ampersand == token->kind) {
		_mc_advance(p);
		mc_node_s* const node = _mc_new(p, mc_node_type_reference, start);
		node->as.type_reference.element = _mc_type(p);
		node->span = _mc_span_from(p, start);
		return node;
	}
	if (mc_token_left_paren == token->kind) {
		_mc_advance(p);
		mc_node_s* const node = _mc_new(p, mc_node_type_function, start);
		while (!_mc_at(p, mc_token_right_paren)) {
			if (_mc_at(p, mc_token_mut) && mc_token_ampersand == _mc_peek(p, 1)->kind) {
				const mc_span_s mut_start = _mc_advance(p)->span;
				_mc_advance(p);
				mc_node_s* const element = _mc_type(p);
				mc_node_s* const reference = _mc_new(p, mc_node_type_reference, mut_start);
				reference->as.type_reference.element = element;
				reference->as.type_reference.mutable_ = true;
				reference->span = _mc_span_from(p, mut_start);
				mc_arena_push(p->arena, &node->as.type_function.parameters, reference);
			}
			else {
				mc_arena_push(p->arena, &node->as.type_function.parameters, _mc_type(p));
			}
			if (!_mc_accept(p, mc_token_comma)) break;
		}
		_mc_expect(p, mc_token_right_paren, "to close the function type");
		if (_mc_accept(p, mc_token_arrow)) node->as.type_function.result = _mc_type(p);
		node->span = _mc_span_from(p, start);
		return node;
	}
	if (mc_token_type == token->kind) {
		_mc_advance(p);
		mc_node_s* const node = _mc_new(p, mc_node_type_name, token->span);
		mc_arena_push(p->arena, &node->as.type_name.path, token->text);
		return node;
	}
	if (mc_token_identifier == token->kind) {
		mc_node_s* const node = _mc_new(p, mc_node_type_name, start);
		mc_arena_push(p->arena, &node->as.type_name.path, _mc_advance(p)->text);
		while (_mc_at(p, mc_token_dot) && mc_token_identifier == _mc_peek(p, 1)->kind) {
			_mc_advance(p);
			mc_arena_push(p->arena, &node->as.type_name.path, _mc_advance(p)->text);
		}
		if (_mc_at(p, mc_token_less)) {
			_mc_advance(p);
			for (;;) {
				mc_arena_push(p->arena, &node->as.type_name.arguments, _mc_type(p));
				if (!_mc_accept(p, mc_token_comma)) break;
			}
			_mc_expect_close_angle(p);
		}
		node->span = _mc_span_from(p, start);
		return node;
	}
	char* const found = _mc_describe(token);
	if (0 == p->speculating) mc_label_main(mc_error(p->diag, mc_E0201, token->span, "expected a type, found %s", found), "expected a type");
	mc_free(found);
	_mc_raise(p);
}

// --- statements --------------------------------------------------------------

static mc_node_s* _mc_block(_mc_parser_s* const p) {
	const mc_span_s start = _mc_expect(p, mc_token_left_brace, "")->span;
	mc_node_s* const block = _mc_new(p, mc_node_block, start);
	const bool saved = p->composites;
	p->composites = true;
	while (!_mc_at(p, mc_token_right_brace) && !_mc_at(p, mc_token_eof)) {
		const size_t before = p->position;
		jmp_buf here;
		jmp_buf* const outer = p->recover;
		p->recover = &here;
		if (0 == setjmp(here)) {
			mc_arena_push(p->arena, &block->as.block.statements, _mc_statement(p));
		}
		else {
			_mc_recover(p, mc_token_semicolon, mc_token_right_brace);
			if (p->position == before) _mc_advance(p);
		}
		p->recover = outer;
	}
	p->composites = saved;
	_mc_expect(p, mc_token_right_brace, "to close the block");
	block->span = _mc_span_from(p, start);
	return block;
}

/// a block, or a single statement without braces.
static mc_node_s* _mc_body(_mc_parser_s* const p, const char* const what) {
	if (_mc_at(p, mc_token_left_brace)) return _mc_block(p);
	const mc_span_s start = _mc_token(p)->span;
	if (_mc_at(p, mc_token_let) || (_mc_at(p, mc_token_identifier) && mc_token_colon_colon == _mc_peek(p, 1)->kind)) {
		_mc_fail(p, mc_E0206, start, "wrap it in `{ }`", "the body of %s without braces cannot be a declaration", what);
	}
	mc_node_s* const statement = _mc_statement(p);
	mc_node_s* const block = _mc_new(p, mc_node_block, start);
	mc_arena_push(p->arena, &block->as.block.statements, statement);
	block->as.block.braceless = true;
	block->span = _mc_span_from(p, start);
	return block;
}

static mc_node_s* _mc_condition(_mc_parser_s* const p) {
	const bool saved = p->composites;
	p->composites = false;
	jmp_buf here;
	jmp_buf* const outer = p->recover;
	p->recover = &here;
	if (0 != setjmp(here)) {
		p->recover = outer;
		p->composites = saved;
		_mc_raise(p);
	}
	mc_node_s* const condition = _mc_expression(p);
	p->recover = outer;
	p->composites = saved;
	return condition;
}

/// at 'name <': whether this is 'name<T, U> ::' (a misplaced generic declaration).
static bool _mc_generic_declaration_ahead(const _mc_parser_s* const p) {
	size_t index = p->position + 2;
	while (index < p->tokens.count) {
		const mc_token_s* const token = &p->tokens.data[index];
		if (token->kind != mc_token_identifier) return false;
		const mc_token_s* const following = (index + 1 < p->tokens.count) ? &p->tokens.data[index + 1] : token;
		if (mc_token_comma == following->kind) {
			index += 2;
			continue;
		}
		return mc_token_greater == following->kind && index + 2 < p->tokens.count && mc_token_colon_colon == p->tokens.data[index + 2].kind;
	}
	return false;
}

static mc_node_s* _mc_if(_mc_parser_s* const p) {
	const mc_span_s start = _mc_advance(p)->span;
	mc_node_s* const node = _mc_new(p, mc_node_if, start);
	node->as.if_.condition = _mc_condition(p);
	node->as.if_.then = _mc_body(p, "`if`");
	if (_mc_accept(p, mc_token_else)) {
		node->as.if_.otherwise = _mc_at(p, mc_token_if) ? _mc_if(p) : _mc_body(p, "`else`");
	}
	node->span = _mc_span_from(p, start);
	return node;
}

static mc_operator_e _mc_assignment_operator(const mc_token_kind_e kind) {
	switch (kind) {
		case mc_token_equal:              return mc_operator_assign;
		case mc_token_plus_assign:        return mc_operator_add;
		case mc_token_minus_assign:       return mc_operator_subtract;
		case mc_token_star_assign:        return mc_operator_multiply;
		case mc_token_slash_assign:       return mc_operator_divide;
		case mc_token_percent_assign:     return mc_operator_remainder;
		case mc_token_and_assign:         return mc_operator_bit_and;
		case mc_token_or_assign:          return mc_operator_bit_or;
		case mc_token_xor_assign:         return mc_operator_bit_xor;
		case mc_token_shift_left_assign:  return mc_operator_shift_left;
		case mc_token_shift_right_assign: return mc_operator_shift_right;
		default:                          return mc_operator_none_;
	}
}

static mc_node_s* _mc_statement(_mc_parser_s* const p) {
	const mc_nodes_s attributes = _mc_at(p, mc_token_left_bracket) ? _mc_attributes(p) : (mc_nodes_s) {0};
	const mc_token_s* const token = _mc_token(p);
	const mc_span_s start = attributes.count > 0 ? attributes.data[0]->span : token->span;
	if (mc_token_let == token->kind) {
		_mc_advance(p);
		mc_node_s* const node = _mc_new(p, mc_node_let, start);
		node->as.let.mutable_ = NULL != _mc_accept(p, mc_token_mut);
		const mc_token_s* const name = _mc_expect_identifier(p, "a variable name");
		node->as.let.name = name->text;
		node->as.let.name_span = name->span;
		node->as.let.attributes = attributes;
		if (_mc_accept(p, mc_token_colon)) node->as.let.type = _mc_type(p);
		if (_mc_accept(p, mc_token_equal)) node->as.let.value = _mc_expression(p);
		_mc_expect(p, mc_token_semicolon, "after the variable");
		node->span = _mc_span_from(p, start);
		return node;
	}
	if ((mc_token_identifier == token->kind && (mc_token_colon_colon == _mc_peek(p, 1)->kind
			|| (mc_token_less == _mc_peek(p, 1)->kind && _mc_generic_declaration_ahead(p)))) || mc_token_pub == token->kind) {
		mc_node_s* const declaration = _mc_declaration_rest(p, false, attributes);
		mc_node_s* const node = _mc_new(p, mc_node_declaration_statement, declaration->span);
		node->as.declaration_statement.declaration = declaration;
		return node;
	}
	switch (token->kind) {
		case mc_token_if: {
			mc_node_s* const node = _mc_if(p);
			node->as.if_.attributes = attributes;
			node->span = _mc_span_from(p, start);
			return node;
		}
		case mc_token_while: {
			_mc_advance(p);
			mc_node_s* const node = _mc_new(p, mc_node_while, start);
			node->as.while_.condition = _mc_condition(p);
			node->as.while_.body = _mc_body(p, "`while`");
			node->as.while_.attributes = attributes;
			node->span = _mc_span_from(p, start);
			return node;
		}
		case mc_token_for: {
			_mc_advance(p);
			mc_node_s* const node = _mc_new(p, mc_node_for, start);
			const mc_token_s* const name = _mc_expect_identifier(p, "a loop variable");
			node->as.for_.name = name->text;
			node->as.for_.name_span = name->span;
			_mc_expect(p, mc_token_in, "after the loop variable");
			node->as.for_.start = _mc_condition(p);
			if (!_mc_at(p, mc_token_dot_dot) && !_mc_at(p, mc_token_dot_dot_equal)) {
				char* const found = _mc_describe(_mc_token(p));
				char* const message = mc_format("expected `..` or `..=`, found %s", found);
				mc_free(found);
				if (0 == p->speculating) mc_label_main(mc_error(p->diag, mc_E0201, _mc_token(p)->span, "%s", message),
					"a `for` loop counts over a range");
				mc_free(message);
				_mc_raise(p);
			}
			node->as.for_.inclusive = mc_token_dot_dot_equal == _mc_advance(p)->kind;
			node->as.for_.end = _mc_condition(p);
			node->as.for_.body = _mc_body(p, "`for`");
			node->as.for_.attributes = attributes;
			node->span = _mc_span_from(p, start);
			return node;
		}
		case mc_token_return: {
			_mc_advance(p);
			mc_node_s* const node = _mc_new(p, mc_node_return, start);
			if (!_mc_at(p, mc_token_semicolon)) node->as.return_.value = _mc_expression(p);
			_mc_expect(p, mc_token_semicolon, "after `return`");
			node->as.return_.attributes = attributes;
			node->span = _mc_span_from(p, start);
			return node;
		}
		case mc_token_break: {
			_mc_advance(p);
			_mc_expect(p, mc_token_semicolon, "after `break`");
			mc_node_s* const node = _mc_new(p, mc_node_break, _mc_span_from(p, start));
			node->as.jump.attributes = attributes;
			return node;
		}
		case mc_token_continue: {
			_mc_advance(p);
			_mc_expect(p, mc_token_semicolon, "after `continue`");
			mc_node_s* const node = _mc_new(p, mc_node_continue, _mc_span_from(p, start));
			node->as.jump.attributes = attributes;
			return node;
		}
		case mc_token_left_brace: {
			mc_node_s* const node = _mc_block(p);
			node->as.block.attributes = attributes;
			node->span = _mc_span_from(p, start);
			return node;
		}
		default:
			break;
	}
	mc_node_s* const expression = _mc_expression(p);
	const mc_operator_e operator_ = _mc_assignment_operator(_mc_token(p)->kind);
	if (operator_ != mc_operator_none_) {
		_mc_advance(p);
		mc_node_s* const node = _mc_new(p, mc_node_assign, start);
		node->as.assign.operator_ = operator_;
		node->as.assign.target = expression;
		node->as.assign.value = _mc_expression(p);
		node->as.assign.attributes = attributes;
		_mc_expect(p, mc_token_semicolon, "after the assignment");
		node->span = _mc_span_from(p, start);
		return node;
	}
	_mc_expect(p, mc_token_semicolon, "after the expression");
	mc_node_s* const node = _mc_new(p, mc_node_expression_statement, start);
	node->as.expression_statement.expression = expression;
	node->as.expression_statement.attributes = attributes;
	node->span = _mc_span_from(p, start);
	return node;
}

// --- expressions -------------------------------------------------------------

typedef enum {
	_mc_group_or, _mc_group_and, _mc_group_compare, _mc_group_bit, _mc_group_shift, _mc_group_add, _mc_group_multiply,
} _mc_group_e;

typedef struct {
	mc_token_kind_e token     ;
	mc_operator_e   operator_ ;
	int             precedence;
	_mc_group_e     group     ;
} _mc_binary_s;

static const _mc_binary_s _mc_binaries[] = {
	{ mc_token_or_or,         mc_operator_or,            1, _mc_group_or,       },
	{ mc_token_and_and,       mc_operator_and,           2, _mc_group_and,      },
	{ mc_token_equal_equal,   mc_operator_equal,         3, _mc_group_compare,  },
	{ mc_token_not_equal,     mc_operator_not_equal,     3, _mc_group_compare,  },
	{ mc_token_less,          mc_operator_less,          3, _mc_group_compare,  },
	{ mc_token_greater,       mc_operator_greater,       3, _mc_group_compare,  },
	{ mc_token_less_equal,    mc_operator_less_equal,    3, _mc_group_compare,  },
	{ mc_token_greater_equal, mc_operator_greater_equal, 3, _mc_group_compare,  },
	{ mc_token_pipe,          mc_operator_bit_or,        4, _mc_group_bit,      },
	{ mc_token_caret,         mc_operator_bit_xor,       5, _mc_group_bit,      },
	{ mc_token_ampersand,     mc_operator_bit_and,       6, _mc_group_bit,      },
	{ mc_token_shift_left,    mc_operator_shift_left,    7, _mc_group_shift,    },
	{ mc_token_shift_right,   mc_operator_shift_right,   7, _mc_group_shift,    },
	{ mc_token_plus,          mc_operator_add,           8, _mc_group_add,      },
	{ mc_token_minus,         mc_operator_subtract,      8, _mc_group_add,      },
	{ mc_token_star,          mc_operator_multiply,      9, _mc_group_multiply, },
	{ mc_token_slash,         mc_operator_divide,        9, _mc_group_multiply, },
	{ mc_token_percent,       mc_operator_remainder,     9, _mc_group_multiply, },
};

static const _mc_binary_s* _mc_binary_of_token(const mc_token_kind_e kind) {
	for (size_t i = 0; i < mc_count_of(_mc_binaries); ++i) if (_mc_binaries[i].token == kind) return &_mc_binaries[i];
	return NULL;
}

static const _mc_binary_s* _mc_binary_of_operator(const mc_operator_e operator_) {
	for (size_t i = 0; i < mc_count_of(_mc_binaries); ++i) if (_mc_binaries[i].operator_ == operator_) return &_mc_binaries[i];
	return NULL;
}

static _mc_group_e _mc_group_of(const mc_operator_e operator_) {
	const _mc_binary_s* const binary = _mc_binary_of_operator(operator_);
	if (NULL == binary) mc_unreachable();
	return binary->group;
}

/// whether 'child' may appear unparenthesized as an operand of 'parent'.
static bool _mc_combinable(const mc_operator_e parent, const mc_operator_e child) {
	const _mc_group_e parent_group = _mc_group_of(parent);
	const _mc_group_e child_group = _mc_group_of(child);
	if (parent_group == child_group) {
		if (_mc_group_compare == parent_group) return false;
		if (_mc_group_add == parent_group || _mc_group_multiply == parent_group || _mc_group_and == parent_group || _mc_group_or == parent_group) return true;
		return parent == child;
	}
	if (_mc_group_and == parent_group || _mc_group_or == parent_group) return child_group != _mc_group_and && child_group != _mc_group_or;
	if (_mc_group_compare == parent_group) return _mc_group_add == child_group || _mc_group_multiply == child_group;
	if (_mc_group_add == parent_group) return _mc_group_multiply == child_group;
	return false;
}

static void _mc_check_mix(_mc_parser_s* const p, const mc_node_s* const parent, const mc_node_s* const child) {
	if (child->kind != mc_node_binary || _mc_combinable(parent->as.binary.operator_, child->as.binary.operator_)) return;
	if (p->speculating > 0) return;
	const mc_operator_e parent_operator = parent->as.binary.operator_;
	const mc_operator_e child_operator = child->as.binary.operator_;
	mc_diagnostic_s* diagnostic = NULL;
	if (_mc_group_compare == _mc_group_of(parent_operator) && _mc_group_compare == _mc_group_of(child_operator)) {
		diagnostic = mc_error(p->diag, mc_E0202, parent->as.binary.operator_span, "comparisons cannot be chained");
		mc_label(mc_label_main(diagnostic, "ambiguous without parentheses"), child->as.binary.operator_span, "%s", "");
		mc_help(diagnostic, "compare separately and combine with `&&`, for example `a < b && b < c`");
	}
	else {
		diagnostic = mc_error(p->diag, mc_E0202, parent->as.binary.operator_span, "`%s` and `%s` need parentheses to show their order",
			mc_operator_text(child_operator), mc_operator_text(parent_operator));
		mc_label(mc_label_main(diagnostic, "ambiguous without parentheses"), child->as.binary.operator_span, "%s", "");
		const mc_view_s text = mc_view_slice(p->source->text, child->span.begin, child->span.end);
		mc_help(diagnostic, "write `(" mc_view_fmt ") %s ...` or group the other side", mc_view_arg(text), mc_operator_text(parent_operator));
	}
}

static mc_node_s* _mc_binary(_mc_parser_s* const p, const int minimum) {
	mc_node_s* left = _mc_unary(p);
	for (;;) {
		const mc_token_s* const token = _mc_token(p);
		const _mc_binary_s* const binary = _mc_binary_of_token(token->kind);
		if (NULL == binary || binary->precedence < minimum) return left;
		const mc_span_s operator_span = token->span;
		_mc_advance(p);
		mc_node_s* const right = _mc_binary(p, binary->precedence + 1);
		mc_node_s* const node = _mc_new(p, mc_node_binary, mc_span_join(left->span, right->span));
		node->as.binary.operator_ = binary->operator_;
		node->as.binary.left = left;
		node->as.binary.right = right;
		node->as.binary.operator_span = operator_span;
		_mc_check_mix(p, node, left);
		_mc_check_mix(p, node, right);
		left = node;
	}
}

static mc_node_s* _mc_expression(_mc_parser_s* const p) {
	return _mc_binary(p, 1);
}

static mc_node_s* _mc_postfix(_mc_parser_s* const p, mc_node_s* node);
static mc_node_s* _mc_primary(_mc_parser_s* const p);

static mc_node_s* _mc_unary(_mc_parser_s* const p) {
	const mc_token_s* const token = _mc_token(p);
	if (mc_token_minus == token->kind || mc_token_bang == token->kind || mc_token_tilde == token->kind) {
		const mc_span_s span = token->span;
		const mc_operator_e operator_ = (mc_token_minus == token->kind) ? mc_operator_negate
			: (mc_token_bang == token->kind) ? mc_operator_not : mc_operator_bit_not;
		_mc_advance(p);
		mc_node_s* const operand = _mc_unary(p);
		mc_node_s* const node = _mc_new(p, mc_node_unary, mc_span_join(span, operand->span));
		node->as.unary.operator_ = operator_;
		node->as.unary.operand = operand;
		return node;
	}
	if (mc_token_ampersand == token->kind) {
		const mc_span_s span = token->span;
		_mc_advance(p);
		mc_node_s* const operand = _mc_unary(p);
		mc_node_s* const node = _mc_new(p, mc_node_reference, mc_span_join(span, operand->span));
		node->as.reference.operand = operand;
		return node;
	}
	if (mc_token_and_and == token->kind) {
		_mc_fail(p, mc_E0201, token->span, "a reference to a reference is not allowed", "`&&` cannot start an expression");
	}
	if (mc_token_mut == token->kind && mc_token_ampersand == _mc_peek(p, 1)->kind) {
		const mc_span_s span = token->span;
		_mc_advance(p);
		_mc_advance(p);
		mc_node_s* const operand = _mc_unary(p);
		mc_node_s* const node = _mc_new(p, mc_node_reference, mc_span_join(span, operand->span));
		node->as.reference.operand = operand;
		node->as.reference.mutable_ = true;
		return node;
	}
	return _mc_postfix(p, _mc_primary(p));
}

static mc_nodes_s _mc_arguments(_mc_parser_s* const p) {
	_mc_expect(p, mc_token_left_paren, "");
	const bool saved = p->composites;
	p->composites = true;
	mc_nodes_s arguments = {0};
	while (!_mc_at(p, mc_token_right_paren)) {
		mc_arena_push(p->arena, &arguments, _mc_expression(p));
		if (!_mc_accept(p, mc_token_comma)) break;
	}
	p->composites = saved;
	_mc_expect(p, mc_token_right_paren, "to close the argument list");
	return arguments;
}

static void _mc_undo_to(_mc_parser_s* const p, const size_t count) {
	while (p->undo.count > count) {
		const _mc_undo_s undo = mc_vector_pop(&p->undo);
		p->tokens.data[undo.index] = undo.token;
	}
}

static bool _mc_specialize_follower(const mc_token_kind_e kind) {
	return mc_token_semicolon == kind || mc_token_right_paren == kind || mc_token_comma == kind
		|| mc_token_right_bracket == kind || mc_token_right_brace == kind || mc_token_eof == kind;
}

/// at '<': type arguments when they parse and a call or terminator follows.
static bool _mc_try_type_arguments(_mc_parser_s* const p, mc_nodes_s* const arguments) {
	const size_t saved_position = p->position;
	const size_t saved_undo = p->undo.count;
	jmp_buf here;
	jmp_buf* const outer = p->recover;
	p->speculating += 1;
	p->recover = &here;
	volatile bool success = false;
	if (0 == setjmp(here)) {
		_mc_advance(p);
		mc_nodes_s parsed = {0};
		for (;;) {
			mc_arena_push(p->arena, &parsed, _mc_type(p));
			if (!_mc_accept(p, mc_token_comma)) break;
		}
		_mc_expect_close_angle(p);
		if (_mc_at(p, mc_token_left_paren) || _mc_specialize_follower(_mc_token(p)->kind)) {
			*arguments = parsed;
			success = true;
		}
	}
	p->recover = outer;
	p->speculating -= 1;
	if (!success) {
		p->position = saved_position;
		_mc_undo_to(p, saved_undo);
	}
	return success;
}

static mc_node_s* _mc_postfix(_mc_parser_s* const p, mc_node_s* node) {
	for (;;) {
		if (_mc_at(p, mc_token_left_paren)) {
			mc_nodes_s arguments = _mc_arguments(p);
			mc_node_s* const call = _mc_new(p, mc_node_call, _mc_span_from(p, node->span));
			call->as.call.callee = node;
			call->as.call.arguments = arguments;
			node = call;
		}
		else if (_mc_at(p, mc_token_less) && (mc_node_name == node->kind || mc_node_member == node->kind)) {
			mc_nodes_s type_arguments = {0};
			if (!_mc_try_type_arguments(p, &type_arguments)) return node;
			if (_mc_at(p, mc_token_left_paren)) {
				mc_nodes_s arguments = _mc_arguments(p);
				mc_node_s* const call = _mc_new(p, mc_node_call, _mc_span_from(p, node->span));
				call->as.call.callee = node;
				call->as.call.has_type_arguments = true;
				call->as.call.type_arguments = type_arguments;
				call->as.call.arguments = arguments;
				node = call;
			}
			else {
				mc_node_s* const specialize = _mc_new(p, mc_node_specialize, _mc_span_from(p, node->span));
				specialize->as.specialize.target = node;
				specialize->as.specialize.type_arguments = type_arguments;
				node = specialize;
			}
		}
		else if (_mc_at(p, mc_token_left_bracket)) {
			_mc_advance(p);
			const bool saved = p->composites;
			p->composites = true;
			mc_node_s* result = NULL;
			if (_mc_at(p, mc_token_dot_dot) || _mc_at(p, mc_token_dot_dot_equal)) {
				result = _mc_new(p, mc_node_slice_of, node->span);
				result->as.slice_of.inclusive = mc_token_dot_dot_equal == _mc_advance(p)->kind;
				if (!_mc_at(p, mc_token_right_bracket)) result->as.slice_of.high = _mc_expression(p);
				result->as.slice_of.target = node;
			}
			else {
				mc_node_s* const index = _mc_expression(p);
				if (_mc_at(p, mc_token_dot_dot) || _mc_at(p, mc_token_dot_dot_equal)) {
					result = _mc_new(p, mc_node_slice_of, node->span);
					result->as.slice_of.inclusive = mc_token_dot_dot_equal == _mc_advance(p)->kind;
					if (!_mc_at(p, mc_token_right_bracket)) result->as.slice_of.high = _mc_expression(p);
					result->as.slice_of.target = node;
					result->as.slice_of.low = index;
				}
				else {
					result = _mc_new(p, mc_node_index, node->span);
					result->as.index.target = node;
					result->as.index.index = index;
				}
			}
			p->composites = saved;
			_mc_expect(p, mc_token_right_bracket, "to close the index");
			result->span = _mc_span_from(p, node->span);
			node = result;
		}
		else if (_mc_at(p, mc_token_dot)) {
			_mc_advance(p);
			const mc_token_s* const name = _mc_expect_identifier(p, "a field name");
			mc_node_s* const member = _mc_new(p, mc_node_member, mc_span_join(node->span, name->span));
			member->as.member.target = node;
			member->as.member.name = name->text;
			member->as.member.name_span = name->span;
			node = member;
		}
		else {
			return node;
		}
	}
}

static mc_node_s* _mc_composite(_mc_parser_s* const p, const mc_span_s start, mc_node_s* const type) {
	_mc_expect(p, mc_token_left_brace, "");
	const bool saved = p->composites;
	p->composites = true;
	mc_node_s* const node = _mc_new(p, mc_node_composite, start);
	node->as.composite.type = type;
	int named = -1;
	while (!_mc_at(p, mc_token_right_brace) && !_mc_at(p, mc_token_eof)) {
		const mc_span_s entry_start = _mc_token(p)->span;
		mc_node_s* const entry = _mc_new(p, mc_node_entry, entry_start);
		if (_mc_at(p, mc_token_identifier) && mc_token_colon == _mc_peek(p, 1)->kind) {
			entry->as.entry.named = true;
			entry->as.entry.name = _mc_advance(p)->text;
			_mc_advance(p);
		}
		if (named >= 0 && (named != 0) != entry->as.entry.named) {
			mc_label_main(mc_error(p->diag, mc_E0201, entry_start, "a literal cannot mix named and positional entries"),
				"all entries must be `name: value`, or none");
		}
		named = entry->as.entry.named ? 1 : 0;
		entry->as.entry.value = _mc_expression(p);
		entry->span = _mc_span_from(p, entry_start);
		mc_arena_push(p->arena, &node->as.composite.entries, entry);
		if (!_mc_accept(p, mc_token_comma)) break;
	}
	p->composites = saved;
	_mc_expect(p, mc_token_right_brace, "to close the literal");
	node->span = _mc_span_from(p, start);
	return node;
}

static mc_node_s* _mc_parenthesized(_mc_parser_s* const p) {
	const mc_span_s start = _mc_token(p)->span;
	if (_mc_is_function_start(p)) {
		mc_node_s* const node = _mc_new(p, mc_node_lambda, start);
		node->as.lambda.parameters = _mc_parameters(p);
		if (_mc_accept(p, mc_token_arrow)) node->as.lambda.result = _mc_type(p);
		if (!_mc_at(p, mc_token_left_brace)) {
			char* const found = _mc_describe(_mc_token(p));
			char* const message = mc_format("expected the body of the lambda, found %s", found);
			mc_free(found);
			if (0 == p->speculating) mc_label_main(mc_error(p->diag, mc_E0201, _mc_token(p)->span, "%s", message), "expected `{`");
			mc_free(message);
			_mc_raise(p);
		}
		node->as.lambda.body = _mc_block(p);
		node->span = _mc_span_from(p, start);
		return node;
	}
	// '(int)x' is a cast; '(int){...}' a typed literal.
	if (mc_token_type == _mc_peek(p, 1)->kind && mc_token_right_paren == _mc_peek(p, 2)->kind
			&& !(mc_token_left_brace == _mc_peek(p, 3)->kind && p->composites)) {
		_mc_advance(p);
		const mc_token_s* const type_token = _mc_advance(p);
		_mc_advance(p);
		mc_node_s* const type = _mc_new(p, mc_node_type_name, type_token->span);
		mc_arena_push(p->arena, &type->as.type_name.path, type_token->text);
		mc_node_s* const operand = _mc_unary(p);
		mc_node_s* const node = _mc_new(p, mc_node_cast, start);
		node->as.cast.type = type;
		node->as.cast.operand = operand;
		node->span = _mc_span_from(p, start);
		return node;
	}
	if (p->composites) {
		const size_t saved_position = p->position;
		const size_t saved_undo = p->undo.count;
		jmp_buf here;
		jmp_buf* const outer = p->recover;
		p->recover = &here;
		p->speculating += 1;
		mc_node_s* volatile result = NULL;
		volatile bool literal = false;
		if (0 == setjmp(here)) {
			_mc_advance(p);
			mc_node_s* const type = _mc_type(p);
			_mc_expect(p, mc_token_right_paren, "");
			if (_mc_at(p, mc_token_left_brace)) {
				// the literal itself is not speculative: its errors are reported.
				literal = true;
				p->speculating -= 1;
				result = _mc_composite(p, start, type);
				p->speculating += 1;
				literal = false;
			}
		}
		else if (literal) {
			p->speculating += 1;
		}
		p->speculating -= 1;
		p->recover = outer;
		if (result != NULL) return result;
		p->position = saved_position;
		_mc_undo_to(p, saved_undo);
	}
	_mc_advance(p);
	const bool saved = p->composites;
	p->composites = true;
	mc_node_s* const inner = _mc_expression(p);
	p->composites = saved;
	_mc_expect(p, mc_token_right_paren, "to close the parentheses");
	mc_node_s* const node = _mc_new(p, mc_node_paren, start);
	node->as.paren.inner = inner;
	node->span = _mc_span_from(p, start);
	return node;
}

static mc_node_s* _mc_primary(_mc_parser_s* const p) {
	const mc_token_s* const token = _mc_token(p);
	const mc_span_s span = token->span;
	switch (token->kind) {
		case mc_token_integer: {
			_mc_advance(p);
			mc_node_s* const node = _mc_new(p, mc_node_integer, span);
			node->as.integer.value = token->integer;
			node->as.integer.suffix = token->suffix;
			return node;
		}
		case mc_token_float: {
			_mc_advance(p);
			mc_node_s* const node = _mc_new(p, mc_node_float, span);
			node->as.float_.value = token->real;
			node->as.float_.suffix = token->suffix;
			return node;
		}
		case mc_token_char: {
			_mc_advance(p);
			mc_node_s* const node = _mc_new(p, mc_node_char, span);
			node->as.char_.value = (uint8_t)token->integer;
			return node;
		}
		case mc_token_string: {
			_mc_advance(p);
			mc_node_s* const node = _mc_new(p, mc_node_string, span);
			node->as.string.data = token->bytes;
			node->as.string.c = token->c;
			return node;
		}
		case mc_token_identifier: {
			_mc_advance(p);
			mc_node_s* const node = _mc_new(p, mc_node_name, span);
			node->as.name.name = token->text;
			return node;
		}
		case mc_token_true:
		case mc_token_false: {
			_mc_advance(p);
			mc_node_s* const node = _mc_new(p, mc_node_bool, span);
			node->as.bool_.value = mc_token_true == token->kind;
			return node;
		}
		case mc_token_null:
			_mc_advance(p);
			return _mc_new(p, mc_node_null, span);
		case mc_token_sizeof: {
			_mc_advance(p);
			_mc_expect(p, mc_token_left_paren, "after `sizeof`");
			mc_node_s* const type = _mc_type(p);
			_mc_expect(p, mc_token_right_paren, "after the type");
			mc_node_s* const node = _mc_new(p, mc_node_sizeof, _mc_span_from(p, span));
			node->as.sizeof_.type = type;
			return node;
		}
		case mc_token_lenof: {
			_mc_advance(p);
			_mc_expect(p, mc_token_left_paren, "after `lenof`");
			mc_node_s* const operand = _mc_expression(p);
			_mc_expect(p, mc_token_right_paren, "after the operand");
			mc_node_s* const node = _mc_new(p, mc_node_lenof, _mc_span_from(p, span));
			node->as.lenof.operand = operand;
			return node;
		}
		case mc_token_assert: {
			_mc_advance(p);
			_mc_expect(p, mc_token_left_paren, "after `assert`");
			mc_node_s* const condition = _mc_expression(p);
			mc_node_s* message = NULL;
			if (_mc_accept(p, mc_token_comma)) message = _mc_expression(p);
			_mc_expect(p, mc_token_right_paren, "to close `assert`");
			mc_node_s* const node = _mc_new(p, mc_node_assert, _mc_span_from(p, span));
			node->as.assert_.condition = condition;
			node->as.assert_.message = message;
			return node;
		}
		case mc_token_left_brace:
			if (!p->composites) _mc_fail(p, mc_E0201, span, "`{` here would start the body", "a literal is not allowed here without parentheses");
			return _mc_composite(p, span, NULL);
		case mc_token_left_paren:
			return _mc_parenthesized(p);
		case mc_token_type:
			_mc_fail(p, mc_E0201, span, "expected an expression", "type `" mc_view_fmt "` cannot be used as a value", mc_view_arg(token->text));
		default:
			break;
	}
	char* const found = _mc_describe(token);
	if (0 == p->speculating) mc_label_main(mc_error(p->diag, mc_E0201, span, "expected an expression, found %s", found), "expected an expression");
	mc_free(found);
	_mc_raise(p);
}

mc_node_s* mc_parse(const mc_source_s* const source, mc_arena_s* const arena, mc_diag_s* const diag) {
	_mc_parser_s parser = { .arena = arena, .diag = diag, .source = source, .composites = true, };
	mc_lex(source, arena, diag, &parser.tokens);
	mc_node_s* const module = _mc_module(&parser);
	mc_vector_free(&parser.tokens);
	mc_vector_free(&parser.undo);
	return module;
}
