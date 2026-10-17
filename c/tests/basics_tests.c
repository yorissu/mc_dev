
/// unit tests of the building blocks: arena, map, strings and formatting.

#include "cairo.h"

#include "mc/arena.h"
#include "mc/format.h"
#include "mc/map.h"
#include "mc/string.h"

#include <math.h>

static char* _repr(void (*format)(mc_string_s* const, const double), const double value) {
	mc_string_s out = {0};
	format(&out, value);
	return mc_string_take(&out);
}

static bool _float_is(const double value, const char* const expected) {
	char* const text = _repr(mc_format_float_repr, value);
	const bool same = 0 == strcmp(text, expected);
	if (!same) (void)fprintf(stderr, "repr(%.17g) = %s, expected %s\n", value, text, expected);
	mc_free(text);
	return same;
}

cairo_test_new(format, float_repr_matches_python) {
	cairo_assert_true(_float_is(0.1, "0.1"));
	cairo_assert_true(_float_is(1.5, "1.5"));
	cairo_assert_true(_float_is(100.0, "100.0"));
	cairo_assert_true(_float_is(-0.0, "-0.0"));
	cairo_assert_true(_float_is(1e16, "1e+16"));
	cairo_assert_true(_float_is(1e15, "1000000000000000.0"));
	cairo_assert_true(_float_is(1e-5, "1e-05"));
	cairo_assert_true(_float_is(0.0001, "0.0001"));
	cairo_assert_true(_float_is(1.0 / 3.0, "0.3333333333333333"));
	cairo_assert_true(_float_is(123456789012345680.0, "1.2345678901234568e+17"));
	cairo_assert_true(_float_is(NAN, "nan"));
	cairo_assert_true(_float_is(-INFINITY, "-inf"));
}

cairo_test_new(format, integers_of_128_bits) {
	mc_string_s out = {0};
	mc_format_int128(&out, -((__int128)1 << 100));
	cairo_assert_streq(mc_string_cstr(&out), "-1267650600228229401496703205376");
	mc_string_clear(&out);
	mc_format_int128(&out, 0);
	cairo_assert_streq(mc_string_cstr(&out), "0");
	mc_string_clear(&out);
	mc_format_uint128(&out, ~(unsigned __int128)0);
	cairo_assert_streq(mc_string_cstr(&out), "340282366920938463463374607431768211455");
	mc_string_free(&out);
}

cairo_test_new(format, repr_of_text_and_bytes) {
	mc_string_s out = {0};
	mc_format_repr_text(&out, mc_view_cstr("ab"));
	cairo_assert_streq(mc_string_cstr(&out), "'ab'");
	mc_string_clear(&out);
	mc_format_repr_text(&out, mc_view_cstr("a'b"));
	cairo_assert_streq(mc_string_cstr(&out), "\"a'b\"");
	mc_string_clear(&out);
	mc_format_repr_bytes(&out, mc_view_new("a\n\0", 3));
	cairo_assert_streq(mc_string_cstr(&out), "'a\\n\\x00'");
	mc_string_free(&out);
}

cairo_test_new(format, utf8_replaces_maximal_invalid_parts) {
	mc_string_s out = {0};
	mc_format_utf8(&out, mc_view_cstr("h\xC3\xA9llo"));
	cairo_assert_streq(mc_string_cstr(&out), "h\xC3\xA9llo");
	mc_string_clear(&out);
	mc_format_utf8(&out, mc_view_cstr("a\xFF" "b"));
	cairo_assert_streq(mc_string_cstr(&out), "a\xEF\xBF\xBD" "b");
	mc_string_clear(&out);
	// a cut three byte sequence is one replacement, like python
	mc_format_utf8(&out, mc_view_cstr("\xE2\x82" "x"));
	cairo_assert_streq(mc_string_cstr(&out), "\xEF\xBF\xBD" "x");
	mc_string_clear(&out);
	// an encoded surrogate is three
	mc_format_utf8(&out, mc_view_cstr("\xED\xA0\x80"));
	cairo_assert_streq(mc_string_cstr(&out), "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD");
	mc_string_free(&out);
}

cairo_test_new(strings, format_and_views) {
	char* const text = mc_format("%d-%s", 7, "x");
	cairo_assert_streq(text, "7-x");
	mc_free(text);
	cairo_assert_true(mc_view_eq_cstr(mc_view_slice(mc_view_cstr("hello"), 1, 3), "el"));
	cairo_assert_true(mc_view_starts_with(mc_view_cstr("module.mc"), mc_view_cstr("mod")));
	cairo_assert_true(mc_view_ends_with(mc_view_cstr("module.mc"), mc_view_cstr(".mc")));
	cairo_assert_false(mc_view_eq(mc_view_cstr("a"), mc_view_cstr("ab")));
}

cairo_test_new(arena, allocations_are_zeroed_and_aligned) {
	mc_arena_s arena = {0};
	for (size_t i = 1; i < 2000; i += 7) {
		unsigned char* const memory = mc_arena_alloc(&arena, i);
		cairo_assert_eq((unsigned long)((uintptr_t)memory % 16), 0ul);
		for (size_t k = 0; k < i; ++k) cairo_assert_eq((int)memory[k], 0);
		memset(memory, 0xAB, i);
	}
	char* const copy = mc_arena_strndup(&arena, "abcdef", 3);
	cairo_assert_streq(copy, "abc");
	mc_arena_free(&arena);
}

cairo_test_new(map, set_get_and_overwrite) {
	mc_map_s map = {0};
	int values[300];
	char names[300][8];
	for (int i = 0; i < 300; ++i) {
		(void)snprintf(names[i], sizeof(names[i]), "k%d", i);
		cairo_assert_false(mc_map_set(&map, mc_view_cstr(names[i]), &values[i]));   // new
	}
	for (int i = 0; i < 300; ++i) cairo_assert_eq(mc_map_get_cstr(&map, names[i]), (void*)&values[i]);
	cairo_assert_eq(mc_map_get_cstr(&map, "missing"), NULL);
	cairo_assert_true(mc_map_set(&map, mc_view_cstr("k5"), &values[0]));          // replaced
	cairo_assert_eq(mc_map_get_cstr(&map, "k5"), (void*)&values[0]);
	void* found = NULL;
	cairo_assert_true(mc_map_find(&map, mc_view_cstr("k7"), &found));
	cairo_assert_eq(found, (void*)&values[7]);
	mc_map_free(&map);
}
