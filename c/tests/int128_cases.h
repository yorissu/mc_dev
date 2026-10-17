
/// the tests of int128.h, included by int128_tests.c (the compiler's own
/// __int128 where it has one) and int128_portable_tests.c (two 64-bit
/// halves): both must give the same results. where the compiler has
/// __int128, random values are also checked against it.

#include "cairo.h"

#include "mc/int128.h"

#include <math.h>
#include <string.h>

#define _mc_test_(_suite, _name) cairo_test_new(_suite, _name)
#define _mc_test(_name) _mc_test_(MC_INT128_SUITE, _name)

static bool _mc_text_is(const mc_i128_s value, const bool signed_, const char* const expected) {
	char text[48];
	const char* const shown = signed_ ? mc_i128_text(text, value) : mc_u128_text(text, value);
	return 0 == strcmp(shown, expected);
}

static bool _mc_is(const mc_i128_s value, const uint64_t high, const uint64_t low) {
	return mc_i128_high(value) == high && mc_i128_low(value) == low;
}

static mc_i128_s _mc_power_of_two(const unsigned bits) {
	return mc_i128_shift_left(mc_i128_from_uint(1), bits);
}

_mc_test(text_of_the_extremes) {
	cairo_assert_true(_mc_text_is(mc_i128_zero(), true, "0"));
	cairo_assert_true(_mc_text_is(mc_i128_from_int(-1), true, "-1"));
	cairo_assert_true(_mc_text_is(mc_i128_from_int(-1), false, "340282366920938463463374607431768211455"));
	cairo_assert_true(_mc_text_is(_mc_power_of_two(127), true, "-170141183460469231731687303715884105728"));
	cairo_assert_true(_mc_text_is(mc_i128_sub(_mc_power_of_two(127), mc_i128_from_uint(1)), true, "170141183460469231731687303715884105727"));
	cairo_assert_true(_mc_text_is(mc_i128_from_int(INT64_MIN), true, "-9223372036854775808"));
	cairo_assert_true(_mc_text_is(mc_i128_from_uint(UINT64_MAX), true, "18446744073709551615"));
}

_mc_test(carries_between_the_halves) {
	cairo_assert_true(_mc_is(mc_i128_add(mc_i128_from_uint(UINT64_MAX), mc_i128_from_uint(1)), 1, 0));
	cairo_assert_true(_mc_is(mc_i128_sub(mc_i128_from_halves(1, 0), mc_i128_from_uint(1)), 0, UINT64_MAX));
	cairo_assert_true(_mc_is(mc_i128_sub(mc_i128_zero(), mc_i128_from_uint(1)), UINT64_MAX, UINT64_MAX));
	cairo_assert_true(_mc_is(mc_i128_add(mc_i128_from_int(-1), mc_i128_from_uint(1)), 0, 0));
	cairo_assert_true(_mc_is(mc_i128_negate(mc_i128_from_uint(1)), UINT64_MAX, UINT64_MAX));
}

_mc_test(products) {
	const mc_i128_s max = mc_i128_from_uint(UINT64_MAX);
	cairo_assert_true(_mc_is(mc_i128_mul(max, max), UINT64_MAX - 1, 1));
	cairo_assert_true(_mc_is(mc_i128_mul(mc_i128_from_int(-3), mc_i128_from_int(7)), UINT64_MAX, (uint64_t)-21));
	cairo_assert_true(_mc_is(mc_i128_mul(_mc_power_of_two(64), _mc_power_of_two(64)), 0, 0));
	cairo_assert_true(_mc_text_is(mc_i128_mul(mc_i128_from_int(INT64_MIN), mc_i128_from_int(INT64_MIN)), true, "85070591730234615865843651857942052864"));
}

_mc_test(division_truncates_toward_zero) {
	mc_i128_s q, r;
	cairo_assert_true(mc_i128_divide(mc_i128_from_int(-7), mc_i128_from_int(2), &q, &r));
	cairo_assert_eq(mc_i128_to_int(q), -3);
	cairo_assert_eq(mc_i128_to_int(r), -1);
	cairo_assert_true(mc_i128_divide(mc_i128_from_int(7), mc_i128_from_int(-2), &q, &r));
	cairo_assert_eq(mc_i128_to_int(q), -3);
	cairo_assert_eq(mc_i128_to_int(r), 1);
	cairo_assert_true(mc_i128_divide(mc_i128_from_int(-7), mc_i128_from_int(-2), &q, &r));
	cairo_assert_eq(mc_i128_to_int(q), 3);
	cairo_assert_eq(mc_i128_to_int(r), -1);
	cairo_assert_false(mc_i128_divide(mc_i128_from_int(1), mc_i128_zero(), &q, &r));
	mc_u128_divide(mc_i128_from_int(-1), mc_i128_from_uint(10), &q, &r);
	cairo_assert_true(_mc_text_is(q, false, "34028236692093846346337460743176821145"));
	cairo_assert_eq(mc_i128_low(r), 5u);
	// the most negative value divided by -1 wraps to itself, as two's complement does
	cairo_assert_true(mc_i128_divide(_mc_power_of_two(127), mc_i128_from_int(-1), &q, NULL));
	cairo_assert_true(mc_i128_equal(q, _mc_power_of_two(127)));
}

_mc_test(shifts) {
	const mc_i128_s one = mc_i128_from_uint(1);
	cairo_assert_true(_mc_is(mc_i128_shift_left(one, 0), 0, 1));
	cairo_assert_true(_mc_is(mc_i128_shift_left(one, 63), 0, (uint64_t)1 << 63));
	cairo_assert_true(_mc_is(mc_i128_shift_left(one, 64), 1, 0));
	cairo_assert_true(_mc_is(mc_i128_shift_left(one, 127), (uint64_t)1 << 63, 0));
	cairo_assert_true(_mc_is(mc_i128_shift_left(one, 128), 0, 0));
	cairo_assert_true(_mc_is(mc_i128_shift_left(mc_i128_from_uint(3), 63), 1, (uint64_t)1 << 63));
	cairo_assert_true(_mc_is(mc_u128_shift_right(mc_i128_from_int(-1), 64), 0, UINT64_MAX));
	cairo_assert_true(_mc_is(mc_u128_shift_right(mc_i128_from_int(-1), 127), 0, 1));
	cairo_assert_true(_mc_is(mc_i128_shift_right(mc_i128_from_int(-8), 1), UINT64_MAX, (uint64_t)-4));
	cairo_assert_true(_mc_is(mc_i128_shift_right(mc_i128_from_int(-8), 100), UINT64_MAX, UINT64_MAX));
	cairo_assert_true(_mc_is(mc_i128_shift_right(mc_i128_from_int(-8), 200), UINT64_MAX, UINT64_MAX));
	cairo_assert_true(_mc_is(mc_i128_shift_right(mc_i128_from_halves(4, 0), 66), 0, 1));
}

_mc_test(comparisons) {
	const mc_i128_s minus = mc_i128_from_int(-1), one = mc_i128_from_uint(1);
	cairo_assert_eq(mc_i128_compare(minus, one), -1);
	cairo_assert_eq(mc_u128_compare(minus, one), 1);
	cairo_assert_eq(mc_i128_compare(one, one), 0);
	cairo_assert_true(mc_i128_less(_mc_power_of_two(127), minus));
	cairo_assert_true(mc_i128_less(mc_i128_from_uint(UINT64_MAX), _mc_power_of_two(64)));
	cairo_assert_true(mc_i128_negative(minus));
	cairo_assert_false(mc_i128_negative(mc_i128_from_uint(UINT64_MAX)));
	cairo_assert_true(mc_i128_fits_int(mc_i128_from_int(INT64_MIN)));
	cairo_assert_false(mc_i128_fits_int(mc_i128_from_uint(UINT64_MAX)));
	cairo_assert_false(mc_i128_fits_int(mc_i128_sub(mc_i128_from_int(INT64_MIN), one)));
}

_mc_test(doubles) {
	cairo_assert_true(mc_i128_to_double(_mc_power_of_two(100)) == ldexp(1.0, 100));
	cairo_assert_true(mc_i128_to_double(mc_i128_from_int(-5)) == -5.0);
	cairo_assert_true(mc_u128_to_double(mc_i128_from_int(-1)) == ldexp(1.0, 128));
	cairo_assert_eq(mc_i128_to_int(mc_i128_from_double(-1.9)), -1);
	cairo_assert_true(mc_i128_equal(mc_i128_from_double(ldexp(1.0, 100)), _mc_power_of_two(100)));
	cairo_assert_true(mc_i128_equal(mc_i128_from_double(ldexp(-1.0, 90)), mc_i128_negate(_mc_power_of_two(90))));
	cairo_assert_true(mc_i128_equal(mc_i128_from_double(NAN), _mc_power_of_two(127)));
	cairo_assert_true(mc_i128_equal(mc_i128_from_double(1e300), _mc_power_of_two(127)));
}

#if defined(__SIZEOF_INT128__)

__extension__ typedef unsigned __int128 _mc_u128_t;
__extension__ typedef __int128 _mc_s128_t;

static uint64_t _mc_random(uint64_t* const state) {
	*state ^= *state << 13;
	*state ^= *state >> 7;
	*state ^= *state << 17;
	return *state;
}

/// random halves, biased toward the edges where carries and signs change.
static mc_i128_s _mc_random_value(uint64_t* const state) {
	static const uint64_t edges[] = { 0, 1, UINT64_MAX, (uint64_t)1 << 63, ((uint64_t)1 << 63) - 1, 10, };
	const uint64_t pick = _mc_random(state);
	const uint64_t high = (pick & 3) ? _mc_random(state) : edges[(pick >> 2) % 6];
	const uint64_t low = (pick & 12) ? _mc_random(state) : edges[(pick >> 4) % 6];
	const unsigned narrow = (unsigned)((pick >> 8) & 7);
	if (0 == narrow) return mc_i128_from_int((int64_t)low);
	if (1 == narrow) return mc_i128_from_uint(low >> (pick >> 11 & 63));
	return mc_i128_from_halves(high, low);
}

static _mc_u128_t _mc_bits(const mc_i128_s value) {
	return ((_mc_u128_t)mc_i128_high(value) << 64) | mc_i128_low(value);
}

static bool _mc_same(const mc_i128_s value, const _mc_u128_t expected) {
	return _mc_bits(value) == expected;
}

_mc_test(random_values_against_the_compiler) {
	uint64_t state = 0x9E3779B97F4A7C15u;
	for (int i = 0; i < 20000; ++i) {
		const mc_i128_s a = _mc_random_value(&state), b = _mc_random_value(&state);
		const _mc_u128_t ua = _mc_bits(a), ub = _mc_bits(b);
		const _mc_s128_t sa = (_mc_s128_t)ua, sb = (_mc_s128_t)ub;
		cairo_assert_true(_mc_same(mc_i128_add(a, b), ua + ub));
		cairo_assert_true(_mc_same(mc_i128_sub(a, b), ua - ub));
		cairo_assert_true(_mc_same(mc_i128_mul(a, b), ua * ub));
		cairo_assert_true(_mc_same(mc_i128_xor(a, b), ua ^ ub));
		cairo_assert_eq(mc_u128_compare(a, b), (ua < ub) ? -1 : (ua > ub) ? 1 : 0);
		cairo_assert_eq(mc_i128_compare(a, b), (sa < sb) ? -1 : (sa > sb) ? 1 : 0);
		const unsigned count = (unsigned)(mc_i128_low(b) & 127);
		cairo_assert_true(_mc_same(mc_i128_shift_left(a, count), ua << count));
		cairo_assert_true(_mc_same(mc_u128_shift_right(a, count), ua >> count));
		cairo_assert_true(_mc_same(mc_i128_shift_right(a, count), (_mc_u128_t)(sa >> count)));
		if (ub != 0) {
			mc_i128_s q, r;
			mc_u128_divide(a, b, &q, &r);
			cairo_assert_true(_mc_same(q, ua / ub) && _mc_same(r, ua % ub));
			const bool overflows = sb == -1 && ua == ((_mc_u128_t)1 << 127);
			if (!overflows) {
				cairo_assert_true(mc_i128_divide(a, b, &q, &r));
				cairo_assert_true(_mc_same(q, (_mc_u128_t)(sa / sb)) && _mc_same(r, (_mc_u128_t)(sa % sb)));
			}
		}
		char text[48], expected[48];
		size_t length = 0;
		_mc_u128_t rest = ua;
		do {
			expected[length++] = (char)('0' + (int)(rest % 10));
			rest /= 10;
		} while (rest > 0);
		for (size_t j = 0; j < length / 2; ++j) {
			const char swap = expected[j];
			expected[j] = expected[length - 1 - j];
			expected[length - 1 - j] = swap;
		}
		expected[length] = '\0';
		cairo_assert_streq(mc_u128_text(text, a), expected);
	}
}

#endif
