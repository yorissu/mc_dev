
#include "mia/parser.h"

#include <string.h>
#include <setjmp.h>

typedef struct {
	size_t      index;
	mia_token_s token;
} _mia_parser_undo_s;

typedef struct {
	mia_token_list_s                  tokens     ;
	size_t                            position   ;
	mia_arena_s*                      arena      ;
	mia_diag_s*                       diag       ;
	const mia_source_s*               source     ;
	int                               speculating;
	bool                              composites ;   // false in conditions, where '{' starts the body
	int                               lambda_then;   // inside the 'then' statement of a lambda (0 in blocks)
	jmp_buf*                          recover    ;   // where a syntax error continues
	mia_vector_of(_mia_parser_undo_s) undo       ;   // tokens split by '>' (undone when speculation fails)
	size_t                            depth      ;   // of the recursion (see _mia_parser_enter)
	bool                              abandoned  ;   // nested too deep: the rest of the file is skipped silently
} _mia_parser_s;

// --- tokens ------------------------------------------------------------------

static const mia_token_s* _mia_parser_token(const _mia_parser_s* const p) {
	return &p->tokens.data[p->position];
}

static const mia_token_s* _mia_parser_peek(const _mia_parser_s* const p, const size_t offset) {
	const size_t index = p->position + offset;
	return &p->tokens.data[index < p->tokens.count ? index : p->tokens.count - 1];
}

static bool _mia_parser_at(const _mia_parser_s* const p, const mia_token_kind_e kind) {
	return _mia_parser_token(p)->kind == kind;
}

static const mia_token_s* _mia_parser_advance(_mia_parser_s* const p) {
	const mia_token_s* const token = _mia_parser_token(p);
	if (token->kind != mia_token_eof) p->position += 1;
	return token;
}

static const mia_token_s* _mia_parser_accept(_mia_parser_s* const p, const mia_token_kind_e kind) {
	return _mia_parser_at(p, kind) ? _mia_parser_advance(p) : NULL;
}

static bool _mia_parser_is_keyword(const mia_token_kind_e kind) {
	return kind >= mia_token_pub && kind <= mia_token_assert;
}

static mia_span_s _mia_parser_span_from(const _mia_parser_s* const p, const mia_span_s start) {
	const mia_token_s* const previous = (p->position > 0) ? &p->tokens.data[p->position - 1] : _mia_parser_token(p);
	return (const mia_span_s) {
		.file = start.file, .begin = start.begin, .end = previous->span.end > start.end ? previous->span.end : start.end,
	};
}

/// how a token is named in messages: `text`, or 'end of file'.
static char* _mia_parser_describe(const mia_token_s* const token) {
	if (mia_token_eof == token->kind) return mia_strdup("end of file");
	return mia_string_format("`" mia_view_fmt "`", mia_view_arg(token->text));
}

mia_noreturn static void _mia_parser_raise(_mia_parser_s* const p) {
	longjmp(*p->recover, 1);
}

/// reports a syntax error (unless speculating) and continues at the nearest
/// recovery point.
mia_noreturn static void _mia_parser_fail(_mia_parser_s* const p, const mia_diag_code_e code, const mia_span_s span,
										  const char* const label, const char* const format, ...) mia_printf_like(5, 6);

/// whether errors are reported: not while speculating, nor after nesting too deep.
static bool _mia_parser_reporting(const _mia_parser_s* const p) {
	return 0 == p->speculating && !p->abandoned;
}

mia_noreturn static void _mia_parser_fail(_mia_parser_s* const p, const mia_diag_code_e code, const mia_span_s span,
										  const char* const label, const char* const format, ...) {
	if (_mia_parser_reporting(p)) {
		va_list args; va_start(args, format);
		char* const message = mia_string_vformat(format, args);
		va_end(args);
		mia_diag_label_main(mia_diag_error(p->diag, code, span, "%s", message), "%s", label);
		mia_free(message);
	}
	_mia_parser_raise(p);
}

static const mia_token_s* _mia_parser_expect(_mia_parser_s* const p, const mia_token_kind_e kind, const char* const context) {
	if (_mia_parser_at(p, kind)) return _mia_parser_advance(p);
	char* const found = _mia_parser_describe(_mia_parser_token(p));
	char* const label = mia_string_format("expected `%s`", mia_token_kind_text(kind));
	char* const message = mia_string_format("expected `%s`%s%s, found %s", mia_token_kind_text(kind),
		(context[0] != '\0') ? " " : "", context, found);
	if (_mia_parser_reporting(p)) mia_diag_label_main(mia_diag_error(p->diag, mia_diag_E0201, _mia_parser_token(p)->span, "%s", message), "%s", label);
	mia_free(found);
	mia_free(label);
	mia_free(message);
	_mia_parser_raise(p);
}

static const mia_token_s* _mia_parser_expect_identifier(_mia_parser_s* const p, const char* const what) {
	if (_mia_parser_at(p, mia_token_identifier)) return _mia_parser_advance(p);
	char* const found = _mia_parser_describe(_mia_parser_token(p));
	char* const message = mia_string_format("expected %s, found %s", what, found);
	char* const label = mia_string_format("expected %s", what);
	if (_mia_parser_reporting(p)) mia_diag_label_main(mia_diag_error(p->diag, mia_diag_E0201, _mia_parser_token(p)->span, "%s", message), "%s", label);
	mia_free(found);
	mia_free(message);
	mia_free(label);
	_mia_parser_raise(p);
}

/// '>' closing type arguments; splits '>>', '>=' and '>>=' tokens.
static void _mia_parser_expect_close_angle(_mia_parser_s* const p) {
	const mia_token_s token = *_mia_parser_token(p);
	if (mia_token_greater == token.kind) {
		_mia_parser_advance(p);
		return;
	}
	mia_token_kind_e rest = mia_token_eof;
	if (mia_token_shift_right == token.kind) rest = mia_token_greater;
	else if (mia_token_greater_equal == token.kind) rest = mia_token_equal;
	else if (mia_token_shift_right_assign == token.kind) rest = mia_token_greater_equal;
	if (rest != mia_token_eof) {
		const _mia_parser_undo_s undo = { .index = p->position, .token = token, };
		mia_vector_push(&p->undo, undo);
		mia_token_s* const split = &p->tokens.data[p->position];
		split->kind = rest;
		split->span.begin += 1;
		split->text = mia_view_slice(token.text, 1, token.text.length);
		return;
	}
	char* const found = _mia_parser_describe(&token);
	if (_mia_parser_reporting(p)) mia_diag_label_main(mia_diag_error(p->diag, mia_diag_E0201, token.span, "expected `>`, found %s", found), "expected `>`");
	mia_free(found);
	_mia_parser_raise(p);
}

/// skips to just after the next 'stop' token at nesting depth zero.
/// one level deeper into the recursion of expressions, statements or types:
/// past mia_ast_depth_limit, the file is reported (once) and abandoned.
static void _mia_parser_enter(_mia_parser_s* const p) {
	p->depth += 1;
	if (p->depth <= mia_ast_depth_limit) return;
	if (0 == p->speculating) {
		if (!p->abandoned) {
			mia_diag_label_main(mia_diag_error(p->diag, mia_diag_E0207, _mia_parser_token(p)->span, "nested too deep"),
				"more than %d levels", mia_ast_depth_limit);
		}
		p->abandoned = true;
		p->position = p->tokens.count - 1;   // the end of the file: nothing more is parsed
	}
	_mia_parser_raise(p);
}

static void _mia_parser_recover(_mia_parser_s* const p, const mia_token_kind_e stop_a, const mia_token_kind_e stop_b) {
	int depth = 0;
	while (!_mia_parser_at(p, mia_token_eof)) {
		const mia_token_kind_e kind = _mia_parser_token(p)->kind;
		if (0 == depth && (kind == stop_a || kind == stop_b)) {
			_mia_parser_advance(p);
			return;
		}
		if (mia_token_left_brace == kind || mia_token_left_paren == kind || mia_token_left_bracket == kind) {
			depth += 1;
		}
		else if (mia_token_right_brace == kind || mia_token_right_paren == kind || mia_token_right_bracket == kind) {
			if (0 == depth) return;
			depth -= 1;
			if (0 == depth && (mia_token_right_brace == stop_a || mia_token_right_brace == stop_b) && mia_token_right_brace == kind) {
				_mia_parser_advance(p);
				return;
			}
		}
		_mia_parser_advance(p);
	}
}

static mia_ast_node_s* _mia_parser_new(_mia_parser_s* const p, const mia_ast_node_kind_e kind, const mia_span_s span) {
	return mia_ast_node_new(p->arena, kind, span);
}

// declarations of the mutually recursive parts.
static mia_ast_node_s* _mia_parser_declaration(_mia_parser_s* const p, const bool top);
static mia_ast_node_s* _mia_parser_type(_mia_parser_s* const p);
static mia_ast_node_s* _mia_parser_block(_mia_parser_s* const p);
static mia_ast_node_s* _mia_parser_body(_mia_parser_s* const p, const char* const what, const bool then_needed);
static mia_ast_node_s* _mia_parser_statement(_mia_parser_s* const p);
static mia_ast_node_s* _mia_parser_expression(_mia_parser_s* const p);
static mia_ast_node_s* _mia_parser_unary(_mia_parser_s* const p);
static mia_ast_node_s* _mia_parser_composite(_mia_parser_s* const p, const mia_span_s start, mia_ast_node_s* const type);
static mia_ast_nodes_s _mia_parser_attributes(_mia_parser_s* const p);

// --- modules -----------------------------------------------------------------

static mia_view_list_s _mia_parser_path(_mia_parser_s* const p) {
	mia_view_list_s path = {0};
	mia_arena_push(p->arena, &path, _mia_parser_expect_identifier(p, "a module name")->text);
	while (_mia_parser_accept(p, mia_token_dot)) mia_arena_push(p->arena, &path, _mia_parser_expect_identifier(p, "a module name")->text);
	return path;
}

static mia_ast_node_s* _mia_parser_import(_mia_parser_s* const p) {
	const mia_span_s start = _mia_parser_advance(p)->span;
	mia_view_list_s path = _mia_parser_path(p);
	mia_ast_node_s* const node = _mia_parser_new(p, mia_ast_node_import, start);
	node->as.import.path = path;
	if (_mia_parser_accept(p, mia_token_as)) {
		node->as.import.has_alias = true;
		node->as.import.alias = _mia_parser_expect_identifier(p, "an alias")->text;
	}
	node->span = _mia_parser_span_from(p, start);
	return node;
}

static mia_ast_node_s* _mia_parser_from_import(_mia_parser_s* const p) {
	const mia_span_s start = _mia_parser_advance(p)->span;
	mia_ast_node_s* const node = _mia_parser_new(p, mia_ast_node_from_import, start);
	node->as.from_import.path = _mia_parser_path(p);
	_mia_parser_expect(p, mia_token_import, "after the module name");
	if (_mia_parser_accept(p, mia_token_star)) {
		node->as.from_import.star = true;
		node->span = _mia_parser_span_from(p, start);
		return node;
	}
	for (;;) {
		const mia_token_s* const name = _mia_parser_expect_identifier(p, "a name to import");
		mia_ast_node_s* const item = _mia_parser_new(p, mia_ast_node_import_name, name->span);
		item->as.import_name.name = name->text;
		if (_mia_parser_accept(p, mia_token_as)) {
			item->as.import_name.has_alias = true;
			item->as.import_name.alias = _mia_parser_expect_identifier(p, "an alias")->text;
		}
		item->span = _mia_parser_span_from(p, name->span);
		mia_arena_push(p->arena, &node->as.from_import.names, item);
		if (!_mia_parser_accept(p, mia_token_comma)) break;
	}
	node->span = _mia_parser_span_from(p, start);
	return node;
}

static mia_ast_node_s* _mia_parser_module(_mia_parser_s* const p) {
	const mia_span_s start = _mia_parser_token(p)->span;
	mia_ast_node_s* const module = _mia_parser_new(p, mia_ast_node_module, start);
	while (!_mia_parser_at(p, mia_token_eof)) {
		const size_t before = p->position;
		jmp_buf here;
		jmp_buf* const saved = p->recover;
		p->recover = &here;
		p->depth = 0;
		if (0 == setjmp(here)) {
			if (_mia_parser_at(p, mia_token_import)) mia_arena_push(p->arena, &module->as.module.imports, _mia_parser_import(p));
			else if (_mia_parser_at(p, mia_token_from)) mia_arena_push(p->arena, &module->as.module.imports, _mia_parser_from_import(p));
			else mia_arena_push(p->arena, &module->as.module.declarations, _mia_parser_declaration(p, true));
		}
		else {
			p->depth = 0;
			p->lambda_then = 0;
			_mia_parser_recover(p, mia_token_semicolon, mia_token_right_brace);
			if (p->position == before) _mia_parser_advance(p);
		}
		p->recover = saved;
	}
	module->span = _mia_parser_span_from(p, start);
	return module;
}

// --- attributes --------------------------------------------------------------

static mia_ast_node_s* _mia_parser_attribute(_mia_parser_s* const p) {
	const mia_token_s* const token = _mia_parser_token(p);
	const mia_span_s start = token->span;
	if (token->kind != mia_token_identifier && !_mia_parser_is_keyword(token->kind)) {
		char* const found = _mia_parser_describe(token);
		char* const message = mia_string_format("expected an attribute name, found %s", found);
		mia_free(found);
		if (_mia_parser_reporting(p)) mia_diag_label_main(mia_diag_error(p->diag, mia_diag_E0204, token->span, "%s", message), "expected an attribute");
		mia_free(message);
		_mia_parser_raise(p);
	}
	const mia_view_s name = _mia_parser_advance(p)->text;
	mia_ast_node_s* const node = _mia_parser_new(p, mia_ast_node_attribute, start);
	node->as.attribute.name = name;
	if (_mia_parser_accept(p, mia_token_equal)) {
		const mia_token_s* const value = _mia_parser_token(p);
		if (mia_token_string == value->kind) {
			node->as.attribute.value_kind = mia_ast_attribute_value_string;
			node->as.attribute.text = value->bytes;
		}
		else if (mia_token_integer == value->kind) {
			node->as.attribute.value_kind = mia_ast_attribute_value_integer;
			node->as.attribute.integer = mia_int128_to_int(value->integer);
		}
		else if (mia_token_identifier == value->kind || mia_token_type == value->kind || _mia_parser_is_keyword(value->kind)) {
			node->as.attribute.value_kind = mia_ast_attribute_value_name;
			node->as.attribute.text = value->text;
		}
		else {
			char* const found = _mia_parser_describe(value);
			char* const message = mia_string_format("expected an attribute value, found %s", found);
			mia_free(found);
			if (_mia_parser_reporting(p)) mia_diag_label_main(mia_diag_error(p->diag, mia_diag_E0204, value->span, "%s", message), "expected a value");
			mia_free(message);
			_mia_parser_raise(p);
		}
		_mia_parser_advance(p);
	}
	node->span = _mia_parser_span_from(p, start);
	return node;
}

static mia_ast_nodes_s _mia_parser_attributes(_mia_parser_s* const p) {
	mia_ast_nodes_s result = {0};
	while (_mia_parser_at(p, mia_token_left_bracket)) {
		_mia_parser_advance(p);
		for (;;) {
			mia_arena_push(p->arena, &result, _mia_parser_attribute(p));
			if (!_mia_parser_accept(p, mia_token_comma)) break;
		}
		_mia_parser_expect(p, mia_token_right_bracket, "to close the attribute list");
	}
	return result;
}

// --- declarations ------------------------------------------------------------

/// at '(': whether a parameter list (not a parenthesized expression) follows.
static bool _mia_parser_is_function_start(const _mia_parser_s* const p) {
	const mia_token_s* const after = _mia_parser_peek(p, 1);
	return mia_token_right_paren == after->kind || (mia_token_identifier == after->kind && mia_token_colon == _mia_parser_peek(p, 2)->kind);
}

static mia_ast_nodes_s _mia_parser_parameters(_mia_parser_s* const p) {
	_mia_parser_expect(p, mia_token_left_paren, "");
	mia_ast_nodes_s parameters = {0};
	while (!_mia_parser_at(p, mia_token_right_paren)) {
		const mia_span_s start = _mia_parser_token(p)->span;
		const mia_token_s* const name = _mia_parser_expect_identifier(p, "a parameter name");
		_mia_parser_expect(p, mia_token_colon, "after the parameter name");
		mia_ast_node_s* const parameter = _mia_parser_new(p, mia_ast_node_param, start);
		parameter->as.param.name = name->text;
		if (_mia_parser_accept(p, mia_token_ellipsis)) {
			parameter->as.param.variadic = true;
		}
		else {
			parameter->as.param.type = _mia_parser_type(p);
			if (_mia_parser_accept(p, mia_token_equal)) parameter->as.param.default_value = _mia_parser_expression(p);
		}
		parameter->span = _mia_parser_span_from(p, start);
		mia_arena_push(p->arena, &parameters, parameter);
		if (!_mia_parser_accept(p, mia_token_comma)) break;
	}
	_mia_parser_expect(p, mia_token_right_paren, "to close the parameter list");
	return parameters;
}

static mia_ast_node_s* _mia_parser_function(_mia_parser_s* const p, const mia_span_s start, const mia_token_s* const name,
											const mia_ast_nodes_s generics, const mia_ast_nodes_s attributes, const bool is_public) {
	mia_ast_node_s* const node = _mia_parser_new(p, mia_ast_node_function, start);
	node->as.function.name = name->text;
	node->as.function.name_span = name->span;
	node->as.function.generics = generics;
	node->as.function.attributes = attributes;
	node->as.function.is_public = is_public;
	node->as.function.parameters = _mia_parser_parameters(p);
	if (_mia_parser_accept(p, mia_token_arrow)) node->as.function.result = _mia_parser_type(p);
	if (!_mia_parser_accept(p, mia_token_semicolon)) {
		if (!_mia_parser_at(p, mia_token_left_brace) && !_mia_parser_at(p, mia_token_then)) {
			char* const found = _mia_parser_describe(_mia_parser_token(p));
			char* const message = mia_string_format("expected a function body or `;`, found %s", found);
			mia_free(found);
			if (_mia_parser_reporting(p)) {
				mia_diag_help(mia_diag_label_main(mia_diag_error(p->diag, mia_diag_E0201, _mia_parser_token(p)->span, "%s", message), "expected `{`, `then` or `;`"),
					"a body is a block `{ ... }`, or `then` and one statement: `main :: () then run();`");
			}
			mia_free(message);
			_mia_parser_raise(p);
		}
		// '{ ... }', or 'then' and one statement
		node->as.function.body = _mia_parser_body(p, "a function", true);
	}
	node->span = _mia_parser_span_from(p, start);
	return node;
}

static mia_ast_node_s* _mia_parser_struct(_mia_parser_s* const p, const mia_span_s start, const mia_token_s* const name,
										  const mia_ast_nodes_s generics, const mia_ast_nodes_s attributes, const bool is_public) {
	mia_ast_node_s* const node = _mia_parser_new(p, mia_ast_node_struct, start);
	node->as.structure.name = name->text;
	node->as.structure.name_span = name->span;
	node->as.structure.generics = generics;
	node->as.structure.attributes = attributes;
	node->as.structure.is_public = is_public;
	_mia_parser_expect(p, mia_token_left_brace, "");
	while (!_mia_parser_at(p, mia_token_right_brace) && !_mia_parser_at(p, mia_token_eof)) {
		const mia_ast_nodes_s field_attributes = _mia_parser_attributes(p);
		const mia_span_s field_start = field_attributes.count > 0 ? field_attributes.data[0]->span : _mia_parser_token(p)->span;
		_mia_parser_expect(p, mia_token_let, "to start a field");
		const mia_token_s* const field_name = _mia_parser_expect_identifier(p, "a field name");
		_mia_parser_expect(p, mia_token_colon, "after the field name");
		mia_ast_node_s* const field = _mia_parser_new(p, mia_ast_node_field, field_start);
		field->as.field.name = field_name->text;
		field->as.field.attributes = field_attributes;
		field->as.field.type = _mia_parser_type(p);
		if (_mia_parser_accept(p, mia_token_equal)) field->as.field.default_value = _mia_parser_expression(p);
		_mia_parser_expect(p, mia_token_semicolon, "after the field");
		field->span = _mia_parser_span_from(p, field_start);
		mia_arena_push(p->arena, &node->as.structure.fields, field);
	}
	_mia_parser_expect(p, mia_token_right_brace, "to close the struct");
	node->span = _mia_parser_span_from(p, start);
	return node;
}

static mia_ast_node_s* _mia_parser_declaration_rest(_mia_parser_s* const p, const bool top, const mia_ast_nodes_s attributes) {
	const mia_span_s start = attributes.count > 0 ? attributes.data[0]->span : _mia_parser_token(p)->span;
	bool is_public = false;
	if (_mia_parser_at(p, mia_token_pub)) {
		const mia_token_s* const pub = _mia_parser_advance(p);
		is_public = true;
		if (!top) mia_diag_label_main(mia_diag_error(p->diag, mia_diag_E0205, pub->span, "local declarations cannot be `pub`"),
			"only module-level declarations are exported");
	}
	const mia_token_s* const name = _mia_parser_expect_identifier(p, "a declaration name");
	mia_ast_nodes_s generics = {0};
	if (_mia_parser_at(p, mia_token_less)) {
		_mia_parser_advance(p);
		for (;;) {
			const mia_token_s* const generic = _mia_parser_expect_identifier(p, "a type parameter name");
			mia_ast_node_s* const node = _mia_parser_new(p, mia_ast_node_generic, generic->span);
			node->as.generic.name = generic->text;
			mia_arena_push(p->arena, &generics, node);
			if (!_mia_parser_accept(p, mia_token_comma)) break;
		}
		_mia_parser_expect_close_angle(p);
		if (!top) {
			const mia_span_s span = { .file = name->span.file, .begin = name->span.begin, .end = p->tokens.data[p->position - 1].span.end, };
			mia_diag_label_main(mia_diag_error(p->diag, mia_diag_E0205, span, "generic declarations are only allowed at module level"),
				"declared inside a function");
		}
	}
	char* const context = mia_string_format("after `" mia_view_fmt "`", mia_view_arg(name->text));
	if (!_mia_parser_at(p, mia_token_colon_colon)) {
		char* const found = _mia_parser_describe(_mia_parser_token(p));
		if (_mia_parser_reporting(p)) mia_diag_label_main(mia_diag_error(p->diag, mia_diag_E0201, _mia_parser_token(p)->span,
			"expected `::` %s, found %s", context, found), "expected `::`");
		mia_free(found);
		mia_free(context);
		_mia_parser_raise(p);
	}
	mia_free(context);
	_mia_parser_advance(p);
	if (_mia_parser_at(p, mia_token_left_brace) && (mia_token_let == _mia_parser_peek(p, 1)->kind || mia_token_left_bracket == _mia_parser_peek(p, 1)->kind)) {
		mia_ast_node_s* const node = _mia_parser_struct(p, start, name, generics, attributes, is_public);
		if (!top) mia_diag_label_main(mia_diag_error(p->diag, mia_diag_E0205, name->span, "struct declarations are only allowed at module level"),
			"declared inside a function");
		return node;
	}
	if (_mia_parser_at(p, mia_token_left_brace) && mia_token_right_brace == _mia_parser_peek(p, 1)->kind) {
		const mia_span_s brace = _mia_parser_token(p)->span;
		_mia_parser_advance(p);
		_mia_parser_advance(p);
		_mia_parser_fail(p, mia_diag_E0203, mia_span_join(brace, p->tokens.data[p->position - 1].span),
			"a struct needs at least one `let` field", "empty structs are not allowed");
	}
	if (_mia_parser_at(p, mia_token_left_paren) && _mia_parser_is_function_start(p)) {
		return _mia_parser_function(p, start, name, generics, attributes, is_public);
	}
	if (generics.count > 0) mia_diag_label_main(mia_diag_error(p->diag, mia_diag_E0205, name->span, "only functions and structs can be generic"),
		"a constant cannot have type parameters");
	mia_ast_node_s* const node = _mia_parser_new(p, mia_ast_node_constant, start);
	node->as.constant.name = name->text;
	node->as.constant.name_span = name->span;
	node->as.constant.attributes = attributes;
	node->as.constant.is_public = is_public;
	node->as.constant.value = _mia_parser_expression(p);
	_mia_parser_expect(p, mia_token_semicolon, "after the constant");
	node->span = _mia_parser_span_from(p, start);
	return node;
}

static mia_ast_node_s* _mia_parser_declaration(_mia_parser_s* const p, const bool top) {
	const mia_ast_nodes_s attributes = _mia_parser_attributes(p);
	return _mia_parser_declaration_rest(p, top, attributes);
}

// --- types -------------------------------------------------------------------

static mia_ast_node_s* _mia_parser_type_inner(_mia_parser_s* const p);

static mia_ast_node_s* _mia_parser_type(_mia_parser_s* const p) {
	_mia_parser_enter(p);
	mia_ast_node_s* const node = _mia_parser_type_inner(p);
	p->depth -= 1;
	return node;
}

static mia_ast_node_s* _mia_parser_type_inner(_mia_parser_s* const p) {
	const mia_token_s* const token = _mia_parser_token(p);
	const mia_span_s start = token->span;
	if (mia_token_left_bracket == token->kind) {
		_mia_parser_advance(p);
		if (_mia_parser_accept(p, mia_token_right_bracket)) {
			mia_ast_node_s* const node = _mia_parser_new(p, mia_ast_node_type_slice, start);
			node->as.type_slice.element = _mia_parser_type(p);
			node->span = _mia_parser_span_from(p, start);
			return node;
		}
		if (_mia_parser_at(p, mia_token_identifier) && mia_view_eq_cstr(_mia_parser_token(p)->text, "_") && mia_token_right_bracket == _mia_parser_peek(p, 1)->kind) {
			_mia_parser_advance(p);
			_mia_parser_advance(p);
			mia_ast_node_s* const node = _mia_parser_new(p, mia_ast_node_type_array, start);
			node->as.type_array.element = _mia_parser_type(p);
			node->span = _mia_parser_span_from(p, start);
			return node;
		}
		const bool saved = p->composites;
		p->composites = true;
		mia_ast_node_s* const length = _mia_parser_expression(p);
		p->composites = saved;
		_mia_parser_expect(p, mia_token_right_bracket, "after the array length");
		mia_ast_node_s* const node = _mia_parser_new(p, mia_ast_node_type_array, start);
		node->as.type_array.length = length;
		node->as.type_array.element = _mia_parser_type(p);
		node->span = _mia_parser_span_from(p, start);
		return node;
	}
	if (mia_token_ampersand == token->kind) {
		_mia_parser_advance(p);
		mia_ast_node_s* const node = _mia_parser_new(p, mia_ast_node_type_reference, start);
		node->as.type_reference.element = _mia_parser_type(p);
		node->span = _mia_parser_span_from(p, start);
		return node;
	}
	if (mia_token_left_paren == token->kind) {
		_mia_parser_advance(p);
		mia_ast_node_s* const node = _mia_parser_new(p, mia_ast_node_type_function, start);
		while (!_mia_parser_at(p, mia_token_right_paren)) {
			mia_arena_push(p->arena, &node->as.type_function.parameters, _mia_parser_type(p));
			if (!_mia_parser_accept(p, mia_token_comma)) break;
		}
		_mia_parser_expect(p, mia_token_right_paren, "to close the function type");
		if (_mia_parser_accept(p, mia_token_arrow)) node->as.type_function.result = _mia_parser_type(p);
		node->span = _mia_parser_span_from(p, start);
		return node;
	}
	if (mia_token_type == token->kind) {
		_mia_parser_advance(p);
		mia_ast_node_s* const node = _mia_parser_new(p, mia_ast_node_type_name, token->span);
		mia_arena_push(p->arena, &node->as.type_name.path, token->text);
		return node;
	}
	if (mia_token_identifier == token->kind) {
		mia_ast_node_s* const node = _mia_parser_new(p, mia_ast_node_type_name, start);
		mia_arena_push(p->arena, &node->as.type_name.path, _mia_parser_advance(p)->text);
		while (_mia_parser_at(p, mia_token_dot) && mia_token_identifier == _mia_parser_peek(p, 1)->kind) {
			_mia_parser_advance(p);
			mia_arena_push(p->arena, &node->as.type_name.path, _mia_parser_advance(p)->text);
		}
		if (_mia_parser_at(p, mia_token_less)) {
			_mia_parser_advance(p);
			for (;;) {
				mia_arena_push(p->arena, &node->as.type_name.arguments, _mia_parser_type(p));
				if (!_mia_parser_accept(p, mia_token_comma)) break;
			}
			_mia_parser_expect_close_angle(p);
		}
		node->span = _mia_parser_span_from(p, start);
		return node;
	}
	char* const found = _mia_parser_describe(token);
	if (_mia_parser_reporting(p)) mia_diag_label_main(mia_diag_error(p->diag, mia_diag_E0201, token->span, "expected a type, found %s", found), "expected a type");
	mia_free(found);
	_mia_parser_raise(p);
}

// --- statements --------------------------------------------------------------

static mia_ast_node_s* _mia_parser_block(_mia_parser_s* const p) {
	const mia_span_s start = _mia_parser_expect(p, mia_token_left_brace, "")->span;
	mia_ast_node_s* const block = _mia_parser_new(p, mia_ast_node_block, start);
	const bool saved = p->composites;
	const int lambda_then = p->lambda_then;
	p->composites = true;
	p->lambda_then = 0;
	while (!_mia_parser_at(p, mia_token_right_brace) && !_mia_parser_at(p, mia_token_eof)) {
		const size_t before = p->position;
		const size_t depth = p->depth;
		jmp_buf here;
		jmp_buf* const outer = p->recover;
		p->recover = &here;
		if (0 == setjmp(here)) {
			mia_arena_push(p->arena, &block->as.block.statements, _mia_parser_statement(p));
		}
		else {
			p->depth = depth;
			p->lambda_then = 0;
			_mia_parser_recover(p, mia_token_semicolon, mia_token_right_brace);
			if (p->position == before) _mia_parser_advance(p);
		}
		p->recover = outer;
	}
	p->composites = saved;
	p->lambda_then = lambda_then;
	_mia_parser_expect(p, mia_token_right_brace, "to close the block");
	block->span = _mia_parser_span_from(p, start);
	return block;
}

/// a block, or a single statement without braces: after 'then' for 'if',
/// 'while', 'for' and functions ('then_needed'), right away after 'else'.
static mia_ast_node_s* _mia_parser_body(_mia_parser_s* const p, const char* const what, const bool then_needed) {
	if (_mia_parser_at(p, mia_token_left_brace)) return _mia_parser_block(p);
	if (then_needed && !_mia_parser_at(p, mia_token_then)) {
		char* const found = _mia_parser_describe(_mia_parser_token(p));
		char* const message = mia_string_format("expected `{` or `then` after the condition of %s, found %s", what, found);
		mia_free(found);
		if (_mia_parser_reporting(p)) {
			mia_diag_help(mia_diag_label_main(mia_diag_error(p->diag, mia_diag_E0201, _mia_parser_token(p)->span, "%s", message), "the body starts here"),
				"a body is a block `{ ... }`, or `then` and one statement: `if done then return;`");
		}
		mia_free(message);
		_mia_parser_raise(p);
	}
	if (then_needed) {
		const mia_token_s* const then = _mia_parser_advance(p);
		if (_mia_parser_at(p, mia_token_left_brace)) {
			_mia_parser_fail(p, mia_diag_E0206, then->span, "remove `then`", "`then` takes a single statement, not a block");
		}
		if (_mia_parser_at(p, mia_token_right_brace) || _mia_parser_at(p, mia_token_eof) || _mia_parser_at(p, mia_token_right_paren)
				|| _mia_parser_at(p, mia_token_comma) || _mia_parser_at(p, mia_token_semicolon)) {
			_mia_parser_fail(p, mia_diag_E0201, then->span, "the body is missing", "expected a statement after `then`");
		}
	}
	const mia_span_s start = _mia_parser_token(p)->span;
	if (_mia_parser_at(p, mia_token_let) || (_mia_parser_at(p, mia_token_identifier) && mia_token_colon_colon == _mia_parser_peek(p, 1)->kind)) {
		_mia_parser_fail(p, mia_diag_E0206, start, "wrap it in `{ }`", "the body of %s without braces cannot be a declaration", what);
	}
	mia_ast_node_s* const statement = _mia_parser_statement(p);
	mia_ast_node_s* const block = _mia_parser_new(p, mia_ast_node_block, start);
	mia_arena_push(p->arena, &block->as.block.statements, statement);
	block->as.block.braceless = true;
	block->span = _mia_parser_span_from(p, start);
	return block;
}

static mia_ast_node_s* _mia_parser_condition(_mia_parser_s* const p) {
	const bool saved = p->composites;
	const size_t depth = p->depth;
	p->composites = false;
	jmp_buf here;
	jmp_buf* const outer = p->recover;
	p->recover = &here;
	if (0 != setjmp(here)) {
		p->recover = outer;
		p->composites = saved;
		p->depth = depth;
		_mia_parser_raise(p);
	}
	mia_ast_node_s* const condition = _mia_parser_expression(p);
	p->recover = outer;
	p->composites = saved;
	return condition;
}

/// at 'name <': whether this is 'name<T, U> ::' (a misplaced generic declaration).
static bool _mia_parser_generic_declaration_ahead(const _mia_parser_s* const p) {
	size_t index = p->position + 2;
	while (index < p->tokens.count) {
		const mia_token_s* const token = &p->tokens.data[index];
		if (token->kind != mia_token_identifier) return false;
		const mia_token_s* const following = (index + 1 < p->tokens.count) ? &p->tokens.data[index + 1] : token;
		if (mia_token_comma == following->kind) {
			index += 2;
			continue;
		}
		return mia_token_greater == following->kind && index + 2 < p->tokens.count && mia_token_colon_colon == p->tokens.data[index + 2].kind;
	}
	return false;
}

static mia_ast_node_s* _mia_parser_if(_mia_parser_s* const p) {
	const mia_span_s start = _mia_parser_advance(p)->span;
	mia_ast_node_s* const node = _mia_parser_new(p, mia_ast_node_if, start);
	node->as.if_statement.condition = _mia_parser_condition(p);
	node->as.if_statement.then = _mia_parser_body(p, "`if`", true);
	if (_mia_parser_accept(p, mia_token_else)) {
		node->as.if_statement.otherwise = _mia_parser_at(p, mia_token_if) ? _mia_parser_if(p) : _mia_parser_body(p, "`else`", false);
	}
	node->span = _mia_parser_span_from(p, start);
	return node;
}

static mia_ast_operator_e _mia_parser_assignment_operator(const mia_token_kind_e kind) {
	switch (kind) {
		case mia_token_equal:              return mia_ast_operator_assign;
		case mia_token_plus_assign:        return mia_ast_operator_add;
		case mia_token_minus_assign:       return mia_ast_operator_subtract;
		case mia_token_star_assign:        return mia_ast_operator_multiply;
		case mia_token_slash_assign:       return mia_ast_operator_divide;
		case mia_token_percent_assign:     return mia_ast_operator_remainder;
		case mia_token_and_assign:         return mia_ast_operator_bit_and;
		case mia_token_or_assign:          return mia_ast_operator_bit_or;
		case mia_token_xor_assign:         return mia_ast_operator_bit_xor;
		case mia_token_shift_left_assign:  return mia_ast_operator_shift_left;
		case mia_token_shift_right_assign: return mia_ast_operator_shift_right;
		default:                          return mia_ast_operator_none;
	}
}

static mia_ast_node_s* _mia_parser_statement_inner(_mia_parser_s* const p);

/// whether 'kind' ends the 'then' statement of a lambda: the expression around the lambda goes on.
static bool _mia_parser_lambda_end(const _mia_parser_s* const p, const mia_token_kind_e kind) {
	return p->lambda_then > 0 && (mia_token_semicolon == kind || mia_token_right_paren == kind || mia_token_comma == kind
		|| mia_token_right_bracket == kind || mia_token_right_brace == kind || mia_token_eof == kind);
}

/// the ';' that ends a simple statement. the 'then' statement of a lambda has
/// none of its own ('map(s, (x: int) -> int then return x * 2)'); there a ';'
/// is taken only before 'else' ('then if c then return 1; else return 2').
static void _mia_parser_statement_end(_mia_parser_s* const p, const char* const context) {
	if (p->lambda_then > 0) {
		if (_mia_parser_at(p, mia_token_semicolon) && mia_token_else == _mia_parser_peek(p, 1)->kind) _mia_parser_advance(p);
		return;
	}
	_mia_parser_expect(p, mia_token_semicolon, context);
}

static mia_ast_node_s* _mia_parser_statement(_mia_parser_s* const p) {
	_mia_parser_enter(p);
	mia_ast_node_s* const node = _mia_parser_statement_inner(p);
	p->depth -= 1;
	return node;
}

static mia_ast_node_s* _mia_parser_statement_inner(_mia_parser_s* const p) {
	const mia_ast_nodes_s attributes = _mia_parser_at(p, mia_token_left_bracket) ? _mia_parser_attributes(p) : (mia_ast_nodes_s) {0};
	const mia_token_s* const token = _mia_parser_token(p);
	const mia_span_s start = attributes.count > 0 ? attributes.data[0]->span : token->span;
	if (mia_token_let == token->kind) {
		_mia_parser_advance(p);
		mia_ast_node_s* const node = _mia_parser_new(p, mia_ast_node_let, start);
		const mia_token_s* const name = _mia_parser_expect_identifier(p, "a variable name");
		node->as.let.name = name->text;
		node->as.let.name_span = name->span;
		node->as.let.attributes = attributes;
		if (_mia_parser_accept(p, mia_token_colon)) node->as.let.type = _mia_parser_type(p);
		if (_mia_parser_accept(p, mia_token_equal)) node->as.let.value = _mia_parser_expression(p);
		_mia_parser_expect(p, mia_token_semicolon, "after the variable");
		node->span = _mia_parser_span_from(p, start);
		return node;
	}
	if ((mia_token_identifier == token->kind && (mia_token_colon_colon == _mia_parser_peek(p, 1)->kind
			|| (mia_token_less == _mia_parser_peek(p, 1)->kind && _mia_parser_generic_declaration_ahead(p)))) || mia_token_pub == token->kind) {
		mia_ast_node_s* const declaration = _mia_parser_declaration_rest(p, false, attributes);
		mia_ast_node_s* const node = _mia_parser_new(p, mia_ast_node_declaration_statement, declaration->span);
		node->as.declaration_statement.declaration = declaration;
		return node;
	}
	switch (token->kind) {
		case mia_token_else:
			_mia_parser_fail(p, mia_diag_E0201, token->span, "no `if` before it", "`else` must follow the body of an `if`");
		case mia_token_if: {
			mia_ast_node_s* const node = _mia_parser_if(p);
			node->as.if_statement.attributes = attributes;
			node->span = _mia_parser_span_from(p, start);
			return node;
		}
		case mia_token_while: {
			_mia_parser_advance(p);
			mia_ast_node_s* const node = _mia_parser_new(p, mia_ast_node_while, start);
			node->as.while_statement.condition = _mia_parser_condition(p);
			node->as.while_statement.body = _mia_parser_body(p, "`while`", true);
			node->as.while_statement.attributes = attributes;
			node->span = _mia_parser_span_from(p, start);
			return node;
		}
		case mia_token_for: {
			_mia_parser_advance(p);
			mia_ast_node_s* const node = _mia_parser_new(p, mia_ast_node_for, start);
			const mia_token_s* const name = _mia_parser_expect_identifier(p, "a loop variable");
			node->as.for_statement.name = name->text;
			node->as.for_statement.name_span = name->span;
			_mia_parser_expect(p, mia_token_in, "after the loop variable");
			node->as.for_statement.start = _mia_parser_condition(p);
			if (!_mia_parser_at(p, mia_token_dot_dot) && !_mia_parser_at(p, mia_token_dot_dot_equal)) {
				char* const found = _mia_parser_describe(_mia_parser_token(p));
				char* const message = mia_string_format("expected `..` or `..=`, found %s", found);
				mia_free(found);
				if (_mia_parser_reporting(p)) mia_diag_label_main(mia_diag_error(p->diag, mia_diag_E0201, _mia_parser_token(p)->span, "%s", message),
					"a `for` loop counts over a range");
				mia_free(message);
				_mia_parser_raise(p);
			}
			node->as.for_statement.inclusive = mia_token_dot_dot_equal == _mia_parser_advance(p)->kind;
			node->as.for_statement.end = _mia_parser_condition(p);
			node->as.for_statement.body = _mia_parser_body(p, "`for`", true);
			node->as.for_statement.attributes = attributes;
			node->span = _mia_parser_span_from(p, start);
			return node;
		}
		case mia_token_return: {
			_mia_parser_advance(p);
			mia_ast_node_s* const node = _mia_parser_new(p, mia_ast_node_return, start);
			if (!_mia_parser_at(p, mia_token_semicolon) && !_mia_parser_lambda_end(p, _mia_parser_token(p)->kind)) {
				node->as.return_statement.value = _mia_parser_expression(p);
			}
			_mia_parser_statement_end(p, "after `return`");
			node->as.return_statement.attributes = attributes;
			node->span = _mia_parser_span_from(p, start);
			return node;
		}
		case mia_token_break: {
			_mia_parser_advance(p);
			_mia_parser_statement_end(p, "after `break`");
			mia_ast_node_s* const node = _mia_parser_new(p, mia_ast_node_break, _mia_parser_span_from(p, start));
			node->as.jump.attributes = attributes;
			return node;
		}
		case mia_token_continue: {
			_mia_parser_advance(p);
			_mia_parser_statement_end(p, "after `continue`");
			mia_ast_node_s* const node = _mia_parser_new(p, mia_ast_node_continue, _mia_parser_span_from(p, start));
			node->as.jump.attributes = attributes;
			return node;
		}
		case mia_token_left_brace: {
			mia_ast_node_s* const node = _mia_parser_block(p);
			node->as.block.attributes = attributes;
			node->span = _mia_parser_span_from(p, start);
			return node;
		}
		default:
			break;
	}
	mia_ast_node_s* const expression = _mia_parser_expression(p);
	const mia_ast_operator_e operation = _mia_parser_assignment_operator(_mia_parser_token(p)->kind);
	if (operation != mia_ast_operator_none) {
		_mia_parser_advance(p);
		mia_ast_node_s* const node = _mia_parser_new(p, mia_ast_node_assign, start);
		node->as.assign.operation = operation;
		node->as.assign.target = expression;
		node->as.assign.value = _mia_parser_expression(p);
		node->as.assign.attributes = attributes;
		_mia_parser_statement_end(p, "after the assignment");
		node->span = _mia_parser_span_from(p, start);
		return node;
	}
	_mia_parser_statement_end(p, "after the expression");
	mia_ast_node_s* const node = _mia_parser_new(p, mia_ast_node_expression_statement, start);
	node->as.expression_statement.expression = expression;
	node->as.expression_statement.attributes = attributes;
	node->span = _mia_parser_span_from(p, start);
	return node;
}

// --- expressions -------------------------------------------------------------

typedef enum {
	_mia_parser_group_or, _mia_parser_group_and, _mia_parser_group_compare, _mia_parser_group_bit, _mia_parser_group_shift, _mia_parser_group_add, _mia_parser_group_multiply,
} _mia_parser_group_e;

typedef struct {
	mia_token_kind_e    token     ;
	mia_ast_operator_e  operation ;
	int                 precedence;
	_mia_parser_group_e group     ;
} _mia_parser_binary_s;

static const _mia_parser_binary_s _mia_parser_binaries[] = {
	{ mia_token_or_or,         mia_ast_operator_or,            1, _mia_parser_group_or,       },
	{ mia_token_and_and,       mia_ast_operator_and,           2, _mia_parser_group_and,      },
	{ mia_token_equal_equal,   mia_ast_operator_equal,         3, _mia_parser_group_compare,  },
	{ mia_token_not_equal,     mia_ast_operator_not_equal,     3, _mia_parser_group_compare,  },
	{ mia_token_less,          mia_ast_operator_less,          3, _mia_parser_group_compare,  },
	{ mia_token_greater,       mia_ast_operator_greater,       3, _mia_parser_group_compare,  },
	{ mia_token_less_equal,    mia_ast_operator_less_equal,    3, _mia_parser_group_compare,  },
	{ mia_token_greater_equal, mia_ast_operator_greater_equal, 3, _mia_parser_group_compare,  },
	{ mia_token_pipe,          mia_ast_operator_bit_or,        4, _mia_parser_group_bit,      },
	{ mia_token_caret,         mia_ast_operator_bit_xor,       5, _mia_parser_group_bit,      },
	{ mia_token_ampersand,     mia_ast_operator_bit_and,       6, _mia_parser_group_bit,      },
	{ mia_token_shift_left,    mia_ast_operator_shift_left,    7, _mia_parser_group_shift,    },
	{ mia_token_shift_right,   mia_ast_operator_shift_right,   7, _mia_parser_group_shift,    },
	{ mia_token_plus,          mia_ast_operator_add,           8, _mia_parser_group_add,      },
	{ mia_token_minus,         mia_ast_operator_subtract,      8, _mia_parser_group_add,      },
	{ mia_token_star,          mia_ast_operator_multiply,      9, _mia_parser_group_multiply, },
	{ mia_token_slash,         mia_ast_operator_divide,        9, _mia_parser_group_multiply, },
	{ mia_token_percent,       mia_ast_operator_remainder,     9, _mia_parser_group_multiply, },
};

static const _mia_parser_binary_s* _mia_parser_binary_of_token(const mia_token_kind_e kind) {
	for (size_t i = 0; i < mia_count_of(_mia_parser_binaries); ++i) if (_mia_parser_binaries[i].token == kind) return &_mia_parser_binaries[i];
	return NULL;
}

static const _mia_parser_binary_s* _mia_parser_binary_of_operator(const mia_ast_operator_e operation) {
	for (size_t i = 0; i < mia_count_of(_mia_parser_binaries); ++i) if (_mia_parser_binaries[i].operation == operation) return &_mia_parser_binaries[i];
	return NULL;
}

static _mia_parser_group_e _mia_parser_group_of(const mia_ast_operator_e operation) {
	const _mia_parser_binary_s* const binary = _mia_parser_binary_of_operator(operation);
	if (binary != NULL) return binary->group;
	mia_unreachable();
}

/// whether 'child' may appear unparenthesized as an operand of 'parent'.
static bool _mia_parser_combinable(const mia_ast_operator_e parent, const mia_ast_operator_e child) {
	const _mia_parser_group_e parent_group = _mia_parser_group_of(parent);
	const _mia_parser_group_e child_group = _mia_parser_group_of(child);
	if (parent_group == child_group) {
		if (_mia_parser_group_compare == parent_group) return false;
		if (_mia_parser_group_add == parent_group || _mia_parser_group_multiply == parent_group || _mia_parser_group_and == parent_group || _mia_parser_group_or == parent_group) return true;
		return parent == child;
	}
	if (_mia_parser_group_and == parent_group || _mia_parser_group_or == parent_group) return child_group != _mia_parser_group_and && child_group != _mia_parser_group_or;
	if (_mia_parser_group_compare == parent_group) return _mia_parser_group_add == child_group || _mia_parser_group_multiply == child_group;
	if (_mia_parser_group_add == parent_group) return _mia_parser_group_multiply == child_group;
	return false;
}

static void _mia_parser_check_mix(_mia_parser_s* const p, const mia_ast_node_s* const parent, const mia_ast_node_s* const child) {
	if (child->kind != mia_ast_node_binary || _mia_parser_combinable(parent->as.binary.operation, child->as.binary.operation)) return;
	if (p->speculating > 0) return;
	const mia_ast_operator_e parent_operator = parent->as.binary.operation;
	const mia_ast_operator_e child_operator = child->as.binary.operation;
	mia_diag_entry_s* diagnostic = NULL;
	if (_mia_parser_group_compare == _mia_parser_group_of(parent_operator) && _mia_parser_group_compare == _mia_parser_group_of(child_operator)) {
		diagnostic = mia_diag_error(p->diag, mia_diag_E0202, parent->as.binary.operation_span, "comparisons cannot be chained");
		mia_diag_label(mia_diag_label_main(diagnostic, "ambiguous without parentheses"), child->as.binary.operation_span, "%s", "");
		mia_diag_help(diagnostic, "compare separately and combine with `&&`, for example `a < b && b < c`");
	}
	else {
		diagnostic = mia_diag_error(p->diag, mia_diag_E0202, parent->as.binary.operation_span, "`%s` and `%s` need parentheses to show their order",
			mia_ast_operator_text(child_operator), mia_ast_operator_text(parent_operator));
		mia_diag_label(mia_diag_label_main(diagnostic, "ambiguous without parentheses"), child->as.binary.operation_span, "%s", "");
		const mia_view_s text = mia_view_slice(p->source->text, child->span.begin, child->span.end);
		mia_diag_help(diagnostic, "write `(" mia_view_fmt ") %s ...` or group the other side", mia_view_arg(text), mia_ast_operator_text(parent_operator));
	}
}

static mia_ast_node_s* _mia_parser_binary(_mia_parser_s* const p, const int minimum) {
	mia_ast_node_s* left = _mia_parser_unary(p);
	for (;;) {
		const mia_token_s* const token = _mia_parser_token(p);
		const _mia_parser_binary_s* const binary = _mia_parser_binary_of_token(token->kind);
		if (NULL == binary || binary->precedence < minimum) return left;
		const mia_span_s operation_span = token->span;
		_mia_parser_advance(p);
		mia_ast_node_s* const right = _mia_parser_binary(p, binary->precedence + 1);
		mia_ast_node_s* const node = _mia_parser_new(p, mia_ast_node_binary, mia_span_join(left->span, right->span));
		node->as.binary.operation = binary->operation;
		node->as.binary.left = left;
		node->as.binary.right = right;
		node->as.binary.operation_span = operation_span;
		_mia_parser_check_mix(p, node, left);
		_mia_parser_check_mix(p, node, right);
		left = node;
	}
}

static mia_ast_node_s* _mia_parser_postfix(_mia_parser_s* const p, mia_ast_node_s* node);
static mia_ast_node_s* _mia_parser_primary(_mia_parser_s* const p);

/// an expression, with pipes: 'value ~ f(a, b)' is 'f(value, a, b)' and
/// 'value ~ f' is 'f(value)'. a pipe binds loosest, and a chain runs left to
/// right; each step is a function, optionally with its other arguments. '~'
/// after an operand is a pipe; in front of one it is the bit-not ('~mask').
static mia_ast_node_s* _mia_parser_expression(_mia_parser_s* const p) {
	mia_ast_node_s* value = _mia_parser_binary(p, 1);
	while (_mia_parser_accept(p, mia_token_tilde)) {
		mia_ast_node_s* step = _mia_parser_postfix(p, _mia_parser_primary(p));
		if (step->kind != mia_ast_node_call) {
			mia_ast_node_s* const call = _mia_parser_new(p, mia_ast_node_call, step->span);
			call->as.call.callee = step;
			step = call;
		}
		// the value goes in front of the step's own arguments
		mia_ast_nodes_s* const arguments = &step->as.call.arguments;
		mia_arena_push(p->arena, arguments, value);
		for (size_t i = arguments->count - 1; i > 0; --i) arguments->data[i] = arguments->data[i - 1];
		arguments->data[0] = value;
		step->as.call.piped = true;
		step->span = mia_span_join(value->span, step->span);
		value = step;
		const mia_token_s* const next = _mia_parser_token(p);
		const _mia_parser_binary_s* const binary = _mia_parser_binary_of_token(next->kind);
		if (binary != NULL) {
			// 'a ~ f == b': the pipe binds loosest, so its result needs parentheses
			if (_mia_parser_reporting(p)) {
				mia_diag_help(mia_diag_label_main(mia_diag_error(p->diag, mia_diag_E0202, next->span, "`~` and `%s` need parentheses to show their order",
					mia_ast_operator_text(binary->operation)), "the pipe ends before this"), "write `(... ~ f) %s ...`", mia_ast_operator_text(binary->operation));
			}
			_mia_parser_raise(p);
		}
	}
	return value;
}

static mia_ast_node_s* _mia_parser_unary_inner(_mia_parser_s* const p);

static mia_ast_node_s* _mia_parser_unary(_mia_parser_s* const p) {
	_mia_parser_enter(p);
	mia_ast_node_s* const node = _mia_parser_unary_inner(p);
	p->depth -= 1;
	return node;
}

static mia_ast_node_s* _mia_parser_unary_inner(_mia_parser_s* const p) {
	const mia_token_s* const token = _mia_parser_token(p);
	if (mia_token_minus == token->kind || mia_token_bang == token->kind || mia_token_tilde == token->kind) {
		const mia_span_s span = token->span;
		const mia_ast_operator_e operation = (mia_token_minus == token->kind) ? mia_ast_operator_negate
			: (mia_token_bang == token->kind) ? mia_ast_operator_not : mia_ast_operator_bit_not;
		_mia_parser_advance(p);
		mia_ast_node_s* const operand = _mia_parser_unary(p);
		mia_ast_node_s* const node = _mia_parser_new(p, mia_ast_node_unary, mia_span_join(span, operand->span));
		node->as.unary.operation = operation;
		node->as.unary.operand = operand;
		return node;
	}
	if (mia_token_ampersand == token->kind) {
		const mia_span_s span = token->span;
		_mia_parser_advance(p);
		mia_ast_node_s* const operand = _mia_parser_unary(p);
		mia_ast_node_s* const node = _mia_parser_new(p, mia_ast_node_reference, mia_span_join(span, operand->span));
		node->as.reference.operand = operand;
		return node;
	}
	if (mia_token_and_and == token->kind) {
		_mia_parser_fail(p, mia_diag_E0201, token->span, "a reference to a reference is not allowed", "`&&` cannot start an expression");
	}
	return _mia_parser_postfix(p, _mia_parser_primary(p));
}

static mia_ast_nodes_s _mia_parser_arguments(_mia_parser_s* const p) {
	_mia_parser_expect(p, mia_token_left_paren, "");
	const bool saved = p->composites;
	p->composites = true;
	mia_ast_nodes_s arguments = {0};
	while (!_mia_parser_at(p, mia_token_right_paren)) {
		mia_arena_push(p->arena, &arguments, _mia_parser_expression(p));
		if (!_mia_parser_accept(p, mia_token_comma)) break;
	}
	p->composites = saved;
	_mia_parser_expect(p, mia_token_right_paren, "to close the argument list");
	return arguments;
}

static void _mia_parser_undo_to(_mia_parser_s* const p, const size_t count) {
	while (p->undo.count > count) {
		const _mia_parser_undo_s undo = mia_vector_pop(&p->undo);
		p->tokens.data[undo.index] = undo.token;
	}
}

static bool _mia_parser_specialize_follower(const mia_token_kind_e kind) {
	return mia_token_semicolon == kind || mia_token_right_paren == kind || mia_token_comma == kind
		|| mia_token_right_bracket == kind || mia_token_right_brace == kind || mia_token_eof == kind;
}

/// at '<': type arguments when they parse and a call or terminator follows.
static bool _mia_parser_try_type_arguments(_mia_parser_s* const p, mia_ast_nodes_s* const arguments) {
	const size_t saved_position = p->position;
	const size_t saved_undo = p->undo.count;
	const size_t depth = p->depth;
	jmp_buf here;
	jmp_buf* const outer = p->recover;
	p->speculating += 1;
	// cppcheck-suppress autoVariables ; 'outer' is put back before it returns
	p->recover = &here;
	volatile bool success = false;
	if (0 == setjmp(here)) {
		_mia_parser_advance(p);
		mia_ast_nodes_s parsed = {0};
		for (;;) {
			mia_arena_push(p->arena, &parsed, _mia_parser_type(p));
			if (!_mia_parser_accept(p, mia_token_comma)) break;
		}
		_mia_parser_expect_close_angle(p);
		if (_mia_parser_at(p, mia_token_left_paren) || _mia_parser_specialize_follower(_mia_parser_token(p)->kind)) {
			*arguments = parsed;
			success = true;
		}
	}
	p->recover = outer;
	p->depth = depth;
	p->speculating -= 1;
	if (!success) {
		p->position = saved_position;
		_mia_parser_undo_to(p, saved_undo);
	}
	return success;
}

static mia_ast_node_s* _mia_parser_postfix(_mia_parser_s* const p, mia_ast_node_s* const operand) {
	mia_ast_node_s* node = operand;
	for (;;) {
		if (_mia_parser_at(p, mia_token_left_paren)) {
			mia_ast_nodes_s arguments = _mia_parser_arguments(p);
			mia_ast_node_s* const call = _mia_parser_new(p, mia_ast_node_call, _mia_parser_span_from(p, node->span));
			call->as.call.callee = node;
			call->as.call.arguments = arguments;
			node = call;
		}
		else if (_mia_parser_at(p, mia_token_less) && (mia_ast_node_name == node->kind || mia_ast_node_member == node->kind)) {
			mia_ast_nodes_s type_arguments = {0};
			if (!_mia_parser_try_type_arguments(p, &type_arguments)) return node;
			if (_mia_parser_at(p, mia_token_left_paren)) {
				mia_ast_nodes_s arguments = _mia_parser_arguments(p);
				mia_ast_node_s* const call = _mia_parser_new(p, mia_ast_node_call, _mia_parser_span_from(p, node->span));
				call->as.call.callee = node;
				call->as.call.has_type_arguments = true;
				call->as.call.type_arguments = type_arguments;
				call->as.call.arguments = arguments;
				node = call;
			}
			else {
				mia_ast_node_s* const specialize = _mia_parser_new(p, mia_ast_node_specialize, _mia_parser_span_from(p, node->span));
				specialize->as.specialize.target = node;
				specialize->as.specialize.type_arguments = type_arguments;
				node = specialize;
			}
		}
		else if (_mia_parser_at(p, mia_token_left_bracket)) {
			_mia_parser_advance(p);
			const bool saved = p->composites;
			p->composites = true;
			mia_ast_node_s* result = NULL;
			if (_mia_parser_at(p, mia_token_dot_dot) || _mia_parser_at(p, mia_token_dot_dot_equal)) {
				result = _mia_parser_new(p, mia_ast_node_slice_of, node->span);
				result->as.slice_of.inclusive = mia_token_dot_dot_equal == _mia_parser_advance(p)->kind;
				if (!_mia_parser_at(p, mia_token_right_bracket)) result->as.slice_of.high = _mia_parser_expression(p);
				result->as.slice_of.target = node;
			}
			else {
				mia_ast_node_s* const index = _mia_parser_expression(p);
				if (_mia_parser_at(p, mia_token_dot_dot) || _mia_parser_at(p, mia_token_dot_dot_equal)) {
					result = _mia_parser_new(p, mia_ast_node_slice_of, node->span);
					result->as.slice_of.inclusive = mia_token_dot_dot_equal == _mia_parser_advance(p)->kind;
					if (!_mia_parser_at(p, mia_token_right_bracket)) result->as.slice_of.high = _mia_parser_expression(p);
					result->as.slice_of.target = node;
					result->as.slice_of.low = index;
				}
				else {
					result = _mia_parser_new(p, mia_ast_node_index, node->span);
					result->as.index.target = node;
					result->as.index.index = index;
				}
			}
			p->composites = saved;
			_mia_parser_expect(p, mia_token_right_bracket, "to close the index");
			result->span = _mia_parser_span_from(p, node->span);
			node = result;
		}
		else if (_mia_parser_at(p, mia_token_dot)) {
			_mia_parser_advance(p);
			const mia_token_s* const name = _mia_parser_expect_identifier(p, "a field name");
			mia_ast_node_s* const member = _mia_parser_new(p, mia_ast_node_member, mia_span_join(node->span, name->span));
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

static mia_ast_node_s* _mia_parser_composite(_mia_parser_s* const p, const mia_span_s start, mia_ast_node_s* const type) {
	_mia_parser_expect(p, mia_token_left_brace, "");
	const bool saved = p->composites;
	p->composites = true;
	mia_ast_node_s* const node = _mia_parser_new(p, mia_ast_node_composite, start);
	node->as.composite.type = type;
	int named = -1;
	while (!_mia_parser_at(p, mia_token_right_brace) && !_mia_parser_at(p, mia_token_eof)) {
		const mia_span_s entry_start = _mia_parser_token(p)->span;
		mia_ast_node_s* const entry = _mia_parser_new(p, mia_ast_node_entry, entry_start);
		if (_mia_parser_at(p, mia_token_identifier) && mia_token_colon == _mia_parser_peek(p, 1)->kind) {
			entry->as.entry.named = true;
			entry->as.entry.name = _mia_parser_advance(p)->text;
			_mia_parser_advance(p);
		}
		if (named >= 0 && (named != 0) != entry->as.entry.named) {
			mia_diag_label_main(mia_diag_error(p->diag, mia_diag_E0201, entry_start, "a literal cannot mix named and positional entries"),
				"all entries must be `name: value`, or none");
		}
		named = entry->as.entry.named ? 1 : 0;
		entry->as.entry.value = _mia_parser_expression(p);
		entry->span = _mia_parser_span_from(p, entry_start);
		mia_arena_push(p->arena, &node->as.composite.entries, entry);
		if (!_mia_parser_accept(p, mia_token_comma)) break;
	}
	p->composites = saved;
	_mia_parser_expect(p, mia_token_right_brace, "to close the literal");
	node->span = _mia_parser_span_from(p, start);
	return node;
}

static mia_ast_node_s* _mia_parser_parenthesized(_mia_parser_s* const p) {
	const mia_span_s start = _mia_parser_token(p)->span;
	if (_mia_parser_is_function_start(p)) {
		mia_ast_node_s* const node = _mia_parser_new(p, mia_ast_node_lambda, start);
		node->as.lambda.parameters = _mia_parser_parameters(p);
		if (_mia_parser_accept(p, mia_token_arrow)) node->as.lambda.result = _mia_parser_type(p);
		if (!_mia_parser_at(p, mia_token_left_brace) && !_mia_parser_at(p, mia_token_then)) {
			char* const found = _mia_parser_describe(_mia_parser_token(p));
			char* const message = mia_string_format("expected the body of the lambda, found %s", found);
			mia_free(found);
			if (_mia_parser_reporting(p)) mia_diag_label_main(mia_diag_error(p->diag, mia_diag_E0201, _mia_parser_token(p)->span, "%s", message), "expected `{` or `then`");
			mia_free(message);
			_mia_parser_raise(p);
		}
		if (_mia_parser_at(p, mia_token_then)) {
			// 'then' and one statement, without a ';' of its own
			p->lambda_then += 1;
			node->as.lambda.body = _mia_parser_body(p, "a lambda", true);
			p->lambda_then -= 1;
		}
		else {
			node->as.lambda.body = _mia_parser_block(p);
		}
		node->span = _mia_parser_span_from(p, start);
		return node;
	}
	// '(int)x' is a cast; '(int){...}' a typed literal.
	if (mia_token_type == _mia_parser_peek(p, 1)->kind && mia_token_right_paren == _mia_parser_peek(p, 2)->kind
			&& !(mia_token_left_brace == _mia_parser_peek(p, 3)->kind && p->composites)) {
		_mia_parser_advance(p);
		const mia_token_s* const type_token = _mia_parser_advance(p);
		_mia_parser_advance(p);
		mia_ast_node_s* const type = _mia_parser_new(p, mia_ast_node_type_name, type_token->span);
		mia_arena_push(p->arena, &type->as.type_name.path, type_token->text);
		mia_ast_node_s* const operand = _mia_parser_unary(p);
		mia_ast_node_s* const node = _mia_parser_new(p, mia_ast_node_cast, start);
		node->as.cast.type = type;
		node->as.cast.operand = operand;
		node->span = _mia_parser_span_from(p, start);
		return node;
	}
	if (p->composites) {
		const size_t saved_position = p->position;
		const size_t saved_undo = p->undo.count;
		const size_t depth = p->depth;
		jmp_buf here;
		jmp_buf* const outer = p->recover;
		p->recover = &here;
		p->speculating += 1;
		mia_ast_node_s* volatile result = NULL;
		volatile bool literal = false;
		if (0 == setjmp(here)) {
			_mia_parser_advance(p);
			mia_ast_node_s* const type = _mia_parser_type(p);
			_mia_parser_expect(p, mia_token_right_paren, "");
			if (_mia_parser_at(p, mia_token_left_brace)) {
				// the literal itself is not speculative: its errors are reported.
				literal = true;
				p->speculating -= 1;
				result = _mia_parser_composite(p, start, type);
				p->speculating += 1;
				literal = false;
			}
		}
		else if (literal) {
			p->speculating += 1;
		}
		p->speculating -= 1;
		p->recover = outer;
		p->depth = depth;
		if (result != NULL) return result;
		p->position = saved_position;
		_mia_parser_undo_to(p, saved_undo);
	}
	_mia_parser_advance(p);
	const bool saved = p->composites;
	p->composites = true;
	mia_ast_node_s* const inner = _mia_parser_expression(p);
	p->composites = saved;
	_mia_parser_expect(p, mia_token_right_paren, "to close the parentheses");
	mia_ast_node_s* const node = _mia_parser_new(p, mia_ast_node_paren, start);
	node->as.paren.inner = inner;
	node->span = _mia_parser_span_from(p, start);
	return node;
}

static mia_ast_node_s* _mia_parser_primary(_mia_parser_s* const p) {
	const mia_token_s* const token = _mia_parser_token(p);
	const mia_span_s span = token->span;
	switch (token->kind) {
		case mia_token_integer: {
			_mia_parser_advance(p);
			mia_ast_node_s* const node = _mia_parser_new(p, mia_ast_node_integer, span);
			node->as.integer.value = token->integer;
			node->as.integer.suffix = token->suffix;
			return node;
		}
		case mia_token_float: {
			_mia_parser_advance(p);
			mia_ast_node_s* const node = _mia_parser_new(p, mia_ast_node_float, span);
			node->as.floating.value = token->real;
			node->as.floating.suffix = token->suffix;
			return node;
		}
		case mia_token_char: {
			_mia_parser_advance(p);
			mia_ast_node_s* const node = _mia_parser_new(p, mia_ast_node_char, span);
			node->as.character.value = (uint8_t)mia_int128_low(token->integer);
			return node;
		}
		case mia_token_string: {
			_mia_parser_advance(p);
			mia_ast_node_s* const node = _mia_parser_new(p, mia_ast_node_string, span);
			node->as.string.data = token->bytes;
			node->as.string.c = token->c;
			return node;
		}
		case mia_token_identifier: {
			_mia_parser_advance(p);
			mia_ast_node_s* const node = _mia_parser_new(p, mia_ast_node_name, span);
			node->as.name.name = token->text;
			return node;
		}
		case mia_token_true:
		case mia_token_false: {
			_mia_parser_advance(p);
			mia_ast_node_s* const node = _mia_parser_new(p, mia_ast_node_bool, span);
			node->as.boolean.value = mia_token_true == token->kind;
			return node;
		}
		case mia_token_sizeof: {
			_mia_parser_advance(p);
			_mia_parser_expect(p, mia_token_left_paren, "after `sizeof`");
			mia_ast_node_s* const type = _mia_parser_type(p);
			_mia_parser_expect(p, mia_token_right_paren, "after the type");
			mia_ast_node_s* const node = _mia_parser_new(p, mia_ast_node_sizeof, _mia_parser_span_from(p, span));
			node->as.size_of.type = type;
			return node;
		}
		case mia_token_lenof: {
			_mia_parser_advance(p);
			_mia_parser_expect(p, mia_token_left_paren, "after `lenof`");
			mia_ast_node_s* const operand = _mia_parser_expression(p);
			_mia_parser_expect(p, mia_token_right_paren, "after the operand");
			mia_ast_node_s* const node = _mia_parser_new(p, mia_ast_node_lenof, _mia_parser_span_from(p, span));
			node->as.lenof.operand = operand;
			return node;
		}
		case mia_token_assert: {
			_mia_parser_advance(p);
			_mia_parser_expect(p, mia_token_left_paren, "after `assert`");
			mia_ast_node_s* const condition = _mia_parser_expression(p);
			mia_ast_node_s* message = NULL;
			if (_mia_parser_accept(p, mia_token_comma)) message = _mia_parser_expression(p);
			_mia_parser_expect(p, mia_token_right_paren, "to close `assert`");
			mia_ast_node_s* const node = _mia_parser_new(p, mia_ast_node_assert, _mia_parser_span_from(p, span));
			node->as.assert_statement.condition = condition;
			node->as.assert_statement.message = message;
			return node;
		}
		case mia_token_left_brace:
			if (!p->composites) _mia_parser_fail(p, mia_diag_E0201, span, "`{` here would start the body", "a literal is not allowed here without parentheses");
			return _mia_parser_composite(p, span, NULL);
		case mia_token_left_paren:
			return _mia_parser_parenthesized(p);
		case mia_token_type:
			_mia_parser_fail(p, mia_diag_E0201, span, "expected an expression", "type `" mia_view_fmt "` cannot be used as a value", mia_view_arg(token->text));
		default:
			break;
	}
	char* const found = _mia_parser_describe(token);
	if (_mia_parser_reporting(p)) mia_diag_label_main(mia_diag_error(p->diag, mia_diag_E0201, span, "expected an expression, found %s", found), "expected an expression");
	mia_free(found);
	_mia_parser_raise(p);
}

mia_ast_node_s* mia_parser_parse(const mia_source_s* const source, mia_arena_s* const arena, mia_diag_s* const diag) {
	_mia_parser_s parser = { .arena = arena, .diag = diag, .source = source, .composites = true, };
	mia_lexer_lex(source, arena, diag, &parser.tokens);
	mia_ast_node_s* const module = _mia_parser_module(&parser);
	// a chain of operators nests without recursing here, but the passes after recurse:
	// a declaration that nests too deep is reported and left out
	mia_ast_nodes_s* const declarations = &module->as.module.declarations;
	size_t kept = 0;
	for (size_t i = 0; i < declarations->count; ++i) {
		const mia_ast_node_s* const deep = parser.abandoned ? NULL : mia_ast_node_too_deep(declarations->data[i], mia_ast_depth_limit);
		if (deep != NULL) {
			mia_diag_label_main(mia_diag_error(diag, mia_diag_E0207, deep->span, "nested too deep"), "more than %d levels", mia_ast_depth_limit);
			continue;
		}
		declarations->data[kept++] = declarations->data[i];
	}
	module->as.module.incomplete = parser.abandoned || kept < declarations->count;
	declarations->count = kept;
	mia_vector_free(&parser.tokens);
	mia_vector_free(&parser.undo);
	return module;
}
