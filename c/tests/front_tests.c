
/// unit tests of the front end: lexer, types and diagnostics.

#include "cairo.h"

#include "mc/diag.h"
#include "mc/lexer.h"
#include "mc/types.h"

typedef struct {
	mc_sources_s sources;
	mc_diag_s    diag   ;
	mc_arena_s   arena  ;
	mc_tokens_s  tokens ;
} _lexed_s;

static void _lex(_lexed_s* const lexed, const char* const text) {
	*lexed = (_lexed_s) {0};
	lexed->diag.sources = &lexed->sources;
	const mc_source_s* const source = mc_sources_add(&lexed->sources, "test.mc", mc_view_cstr(text));
	mc_lex(source, &lexed->arena, &lexed->diag, &lexed->tokens);
}

static void _lexed_free(_lexed_s* const lexed) {
	mc_vector_free(&lexed->tokens);
	mc_diag_free(&lexed->diag);
	mc_arena_free(&lexed->arena);
	mc_sources_free(&lexed->sources);
}

cairo_test_new(lexer, keywords_names_and_punctuators) {
	_lexed_s lexed;
	_lex(&lexed, "pub main :: () -> int { let mut x = a..=b; }");
	const mc_token_kind_e expected[] = {
		mc_token_pub, mc_token_identifier, mc_token_colon_colon, mc_token_left_paren, mc_token_right_paren, mc_token_arrow,
		mc_token_type, mc_token_left_brace, mc_token_let, mc_token_mut, mc_token_identifier, mc_token_equal, mc_token_identifier,
		mc_token_dot_dot_equal, mc_token_identifier, mc_token_semicolon, mc_token_right_brace, mc_token_eof,
	};
	cairo_assert_eq(lexed.tokens.count, mc_count_of(expected));
	for (size_t i = 0; i < mc_count_of(expected); ++i) cairo_assert_eq((int)lexed.tokens.data[i].kind, (int)expected[i]);
	cairo_assert_eq((int)lexed.tokens.data[6].suffix, (int)mc_scalar_int);   // type tokens: which scalar
	cairo_assert_false(mc_diag_has_errors(&lexed.diag));
	_lexed_free(&lexed);
}

cairo_test_new(lexer, numbers_with_bases_separators_and_suffixes) {
	_lexed_s lexed;
	_lex(&lexed, "0x1F 0b1010 0o17 1_000 255u8 1.5f32 2e3");
	const mc_token_s* const t = lexed.tokens.data;
	cairo_assert_eq((unsigned long long)mc_i128_low(t[0].integer), 31ull);
	cairo_assert_eq((unsigned long long)mc_i128_low(t[1].integer), 10ull);
	cairo_assert_eq((unsigned long long)mc_i128_low(t[2].integer), 15ull);
	cairo_assert_eq((unsigned long long)mc_i128_low(t[3].integer), 1000ull);
	cairo_assert_eq((unsigned long long)mc_i128_low(t[4].integer), 255ull);
	cairo_assert_eq((int)t[4].suffix, (int)mc_scalar_u8);
	cairo_assert_eq((int)t[5].kind, (int)mc_token_float);
	cairo_assert_eq(t[5].real, 1.5);
	cairo_assert_eq((int)t[5].suffix, (int)mc_scalar_f32);
	cairo_assert_eq(t[6].real, 2000.0);
	cairo_assert_false(mc_diag_has_errors(&lexed.diag));
	_lexed_free(&lexed);
}

cairo_test_new(lexer, strings_characters_and_comments) {
	_lexed_s lexed;
	_lex(&lexed, "# comment\n/* a /* nested */ block */ \"a\\n\\x41\" \"hi\"c 'z' // end");
	const mc_token_s* const t = lexed.tokens.data;
	cairo_assert_eq((int)t[0].kind, (int)mc_token_string);
	cairo_assert_eq(t[0].bytes.length, (size_t)3);
	cairo_assert_eq(memcmp(t[0].bytes.data, "a\nA", 3), 0);
	cairo_assert_eq(t[1].bytes.length, (size_t)3);
	cairo_assert_true(t[1].c);
	cairo_assert_eq((int)t[1].bytes.data[2], 0);
	cairo_assert_eq((int)t[2].kind, (int)mc_token_char);
	cairo_assert_eq((unsigned long long)mc_i128_low(t[2].integer), (unsigned long long)'z');
	cairo_assert_eq((int)t[3].kind, (int)mc_token_eof);
	_lexed_free(&lexed);
}

cairo_test_new(lexer, reports_unknown_characters_and_open_strings) {
	_lexed_s lexed;
	_lex(&lexed, "let x = $;\nlet s = \"open\n");
	cairo_assert_eq(lexed.diag.error_count, (size_t)2);
	cairo_assert_eq((int)lexed.diag.items.data[0]->code, (int)mc_E0101);
	cairo_assert_eq((int)lexed.diag.items.data[1]->code, (int)mc_E0102);
	_lexed_free(&lexed);
}

static bool _text_is(const mc_type_s* const type, const char* const expected) {
	char* const text = mc_type_name(type);
	const bool same = 0 == strcmp(text, expected);
	if (!same) (void)fprintf(stderr, "type text `%s`, expected `%s`\n", text, expected);
	mc_free(text);
	return same;
}

cairo_test_new(types, interned_types_are_equal_pointers) {
	mc_types_s types;
	mc_types_init(&types);
	const mc_type_s* const integer = mc_type_of_scalar(&types, mc_scalar_int);
	cairo_assert_eq(integer, mc_type_of_scalar(&types, mc_scalar_int));
	cairo_assert_eq(mc_type_slice_of(&types, integer), mc_type_slice_of(&types, integer));
	cairo_assert_eq(mc_type_array_of(&types, integer, 4), mc_type_array_of(&types, integer, 4));
	cairo_assert_neq(mc_type_array_of(&types, integer, 4), mc_type_array_of(&types, integer, 5));
	const mc_type_s* const parameters[] = { integer, mc_type_slice_of(&types, mc_type_of_scalar(&types, mc_scalar_u8)), };
	const mc_type_s* const function = mc_type_function_of(&types, parameters, 2, integer);
	cairo_assert_eq(function, mc_type_function_of(&types, parameters, 2, integer));
	cairo_assert_true(_text_is(function, "(int, []u8) -> int"));
	cairo_assert_true(_text_is(mc_type_array_of(&types, integer, 4), "[4]int"));
	cairo_assert_true(_text_is(mc_type_of_special(&types, mc_special_untyped_integer), "{integer}"));
	mc_types_free(&types);
}

cairo_test_new(types, unify_binds_parameters) {
	mc_types_s types;
	mc_types_init(&types);
	const mc_type_s* const integer = mc_type_of_scalar(&types, mc_scalar_int);
	const mc_type_s* const t = mc_type_parameter_of(&types, mc_view_cstr("T"), NULL);
	mc_bindings_s bindings = {0};
	cairo_assert_true(mc_type_unify(mc_type_slice_of(&types, t), mc_type_slice_of(&types, integer), &bindings));
	cairo_assert_eq(mc_bindings_get(&bindings, mc_view_cstr("T")), integer);
	cairo_assert_eq(mc_type_substitute(&types, mc_type_array_of(&types, t, 3), &bindings), mc_type_array_of(&types, integer, 3));
	cairo_assert_false(mc_type_unify(mc_type_slice_of(&types, t), integer, &bindings));
	mc_vector_free(&bindings);
	mc_types_free(&types);
}

cairo_test_new(types, integer_ranges) {
	mc_types_s types;
	mc_types_init(&types);
	cairo_assert_eq((long long)mc_i128_to_int(mc_type_minimum(mc_type_of_scalar(&types, mc_scalar_s8))), -128ll);
	cairo_assert_eq((unsigned long long)mc_i128_low(mc_type_maximum(mc_type_of_scalar(&types, mc_scalar_u64))), 18446744073709551615ull);
	cairo_assert_eq(mc_i128_high(mc_type_maximum(mc_type_of_scalar(&types, mc_scalar_u64))), 0ull);
	cairo_assert_true(mc_type_is_signed(mc_type_of_scalar(&types, mc_scalar_int)));
	cairo_assert_false(mc_type_is_signed(mc_type_of_scalar(&types, mc_scalar_uint)));
	mc_types_free(&types);
}

cairo_test_new(diag, short_and_human_rendering) {
	mc_sources_s sources = {0};
	mc_diag_s diag = { .sources = &sources, };
	const mc_source_s* const source = mc_sources_add(&sources, "test.mc", mc_view_cstr("let x: u8 = 300;\nlet y = x;\n"));
	const mc_span_s span = { .file = source->id, .begin = 12, .end = 15, };
	mc_diagnostic_s* const diagnostic = mc_error(&diag, mc_E0502, span, "`%d` does not fit in `u8`", 300);
	mc_label_main(diagnostic, "too large");
	mc_help(diagnostic, "use a wider type");
	mc_string_s out = {0};
	mc_diag_render(&diag, diagnostic, mc_diag_format_short, false, &out);
	cairo_assert_streq(mc_string_cstr(&out), "test.mc:1:13: error[E0502]: `300` does not fit in `u8`\n");
	mc_string_clear(&out);
	mc_diag_render(&diag, diagnostic, mc_diag_format_human, false, &out);
	const char* const human = mc_string_cstr(&out);
	cairo_assert_true(strstr(human, "error[E0502]: `300` does not fit in `u8`\n") == human);
	cairo_assert_true(strstr(human, " --> test.mc:1:13\n") != NULL);
	cairo_assert_true(strstr(human, "1 | let x: u8 = 300;\n") != NULL);
	cairo_assert_true(strstr(human, "^^^ too large") != NULL);
	cairo_assert_true(strstr(human, "= help: use a wider type") != NULL);
	mc_string_free(&out);
	// the same error again is reported once
	(void)mc_error(&diag, mc_E0502, span, "`%d` does not fit in `u8`", 300);
	cairo_assert_eq(diag.error_count, (size_t)1);
	mc_diag_free(&diag);
	mc_sources_free(&sources);
}

cairo_test_new(diag, runtime_errors_have_no_code) {
	mc_sources_s sources = {0};
	mc_diag_s diag = { .sources = &sources, };
	mc_diagnostic_s* const diagnostic = mc_runtime_error(&diag, mc_no_span, "division by zero");
	mc_string_s out = {0};
	mc_diag_render(&diag, diagnostic, mc_diag_format_short, false, &out);
	cairo_assert_streq(mc_string_cstr(&out), "runtime error: division by zero\n");
	mc_string_free(&out);
	mc_diag_free(&diag);
	mc_sources_free(&sources);
}

cairo_test_new(diag, codes_parse_and_explain) {
	cairo_assert_eq((int)mc_code_parse("E0502"), (int)mc_E0502);
	cairo_assert_eq((int)mc_code_parse("w0503"), (int)mc_W0503);
	cairo_assert_eq((int)mc_code_parse("E9999"), (int)mc_code_count_);
	cairo_assert_streq(mc_code_text(mc_E0101), "E0101");
	cairo_assert_true(strlen(mc_code_summary(mc_E0502)) > 10);
}
