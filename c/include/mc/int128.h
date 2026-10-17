
#ifndef __mc__int128_h__
#define __mc__int128_h__

/// 128-bit integers for the compiler: literals, constants and the checks of
/// overflow need more than 64 bits (an untyped constant is exact, and a
/// u64 times a u64 must be seen to overflow). a value is 128 bits of two's
/// complement; the functions say whether they read them signed ('i128') or
/// unsigned ('u128'). with gcc and clang the type is the compiler's own
/// __int128; elsewhere (msvc), or when MC_INT128_PORTABLE or MC_PORTABLE is
/// defined, it is two 64-bit halves with the same results. the struct around
/// it keeps the operators of c away, so the code is the same for both.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(__SIZEOF_INT128__) && !defined(MC_INT128_PORTABLE) && !defined(MC_PORTABLE)
#	define MC_INT128_NATIVE 1
#else
#	define MC_INT128_NATIVE 0
#endif

#if MC_INT128_NATIVE

#if defined(__GNUC__)
__extension__ typedef unsigned __int128 mc_int128_bits_t_;
__extension__ typedef __int128 mc_int128_signed_t_;
#else
typedef unsigned __int128 mc_int128_bits_t_;
typedef __int128 mc_int128_signed_t_;
#endif

typedef struct {
	mc_int128_bits_t_ bits;
} mc_i128_s;

static inline mc_i128_s mc_i128_from_int(const int64_t value) {
	return (mc_i128_s) { (mc_int128_bits_t_)(mc_int128_signed_t_)value };
}

static inline mc_i128_s mc_i128_from_uint(const uint64_t value) {
	return (mc_i128_s) { (mc_int128_bits_t_)value };
}

static inline mc_i128_s mc_i128_from_halves(const uint64_t high, const uint64_t low) {
	return (mc_i128_s) { ((mc_int128_bits_t_)high << 64) | low };
}

static inline uint64_t mc_i128_low(const mc_i128_s value) {
	return (uint64_t)value.bits;
}

static inline uint64_t mc_i128_high(const mc_i128_s value) {
	return (uint64_t)(value.bits >> 64);
}

static inline mc_i128_s mc_i128_add(const mc_i128_s a, const mc_i128_s b) {
	return (mc_i128_s) { a.bits + b.bits };
}

static inline mc_i128_s mc_i128_sub(const mc_i128_s a, const mc_i128_s b) {
	return (mc_i128_s) { a.bits - b.bits };
}

static inline mc_i128_s mc_i128_mul(const mc_i128_s a, const mc_i128_s b) {
	return (mc_i128_s) { a.bits * b.bits };
}

/// unsigned division; 'b' must not be zero.
static inline void mc_u128_divide(const mc_i128_s a, const mc_i128_s b, mc_i128_s* const quotient, mc_i128_s* const remainder) {
	if (quotient != NULL) quotient->bits = a.bits / b.bits;
	if (remainder != NULL) remainder->bits = a.bits % b.bits;
}

#else

typedef struct {
	uint64_t low ;
	uint64_t high;
} mc_i128_s;

static inline mc_i128_s mc_i128_from_int(const int64_t value) {
	return (mc_i128_s) { (uint64_t)value, (value < 0) ? UINT64_MAX : 0 };
}

static inline mc_i128_s mc_i128_from_uint(const uint64_t value) {
	return (mc_i128_s) { value, 0 };
}

static inline mc_i128_s mc_i128_from_halves(const uint64_t high, const uint64_t low) {
	return (mc_i128_s) { low, high };
}

static inline uint64_t mc_i128_low(const mc_i128_s value) {
	return value.low;
}

static inline uint64_t mc_i128_high(const mc_i128_s value) {
	return value.high;
}

static inline mc_i128_s mc_i128_add(const mc_i128_s a, const mc_i128_s b) {
	const uint64_t low = a.low + b.low;
	return (mc_i128_s) { low, a.high + b.high + (low < a.low ? 1 : 0) };
}

static inline mc_i128_s mc_i128_sub(const mc_i128_s a, const mc_i128_s b) {
	return (mc_i128_s) { a.low - b.low, a.high - b.high - (a.low < b.low ? 1 : 0) };
}

/// the full 128-bit product of two 64-bit halves.
static inline mc_i128_s mc_i128_multiply_64_(const uint64_t a, const uint64_t b) {
	const uint64_t a0 = a & 0xFFFFFFFFu, a1 = a >> 32, b0 = b & 0xFFFFFFFFu, b1 = b >> 32;
	const uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
	const uint64_t middle = (p00 >> 32) + (p01 & 0xFFFFFFFFu) + (p10 & 0xFFFFFFFFu);
	return (mc_i128_s) { (p00 & 0xFFFFFFFFu) | (middle << 32), p11 + (p01 >> 32) + (p10 >> 32) + (middle >> 32) };
}

static inline mc_i128_s mc_i128_mul(const mc_i128_s a, const mc_i128_s b) {
	mc_i128_s product = mc_i128_multiply_64_(a.low, b.low);
	product.high += a.low * b.high + a.high * b.low;
	return product;
}

/// unsigned division (shift and subtract); 'b' must not be zero.
static inline void mc_u128_divide(const mc_i128_s a, const mc_i128_s b, mc_i128_s* const quotient, mc_i128_s* const remainder) {
	mc_i128_s q = { 0, 0 }, r = { 0, 0 };
	for (int bit = 127; bit >= 0; --bit) {
		r = (mc_i128_s) { (r.low << 1) | (bit >= 64 ? (a.high >> (bit - 64)) & 1 : (a.low >> bit) & 1), (r.high << 1) | (r.low >> 63) };
		const bool fits = r.high > b.high || (r.high == b.high && r.low >= b.low);
		if (fits) {
			r = mc_i128_sub(r, b);
			if (bit >= 64) q.high |= (uint64_t)1 << (bit - 64);
			else q.low |= (uint64_t)1 << bit;
		}
	}
	if (quotient != NULL) *quotient = q;
	if (remainder != NULL) *remainder = r;
}

#endif

// --- the same on both ---------------------------------------------------------------------

static inline mc_i128_s mc_i128_zero(void) {
	return mc_i128_from_uint(0);
}

static inline bool mc_i128_negative(const mc_i128_s value) {
	return (mc_i128_high(value) >> 63) != 0;
}

static inline bool mc_i128_is_zero(const mc_i128_s value) {
	return 0 == mc_i128_low(value) && 0 == mc_i128_high(value);
}

static inline bool mc_i128_equal(const mc_i128_s a, const mc_i128_s b) {
	return mc_i128_low(a) == mc_i128_low(b) && mc_i128_high(a) == mc_i128_high(b);
}

/// -1, 0 or 1, reading both as unsigned.
static inline int mc_u128_compare(const mc_i128_s a, const mc_i128_s b) {
	if (mc_i128_high(a) != mc_i128_high(b)) return (mc_i128_high(a) < mc_i128_high(b)) ? -1 : 1;
	if (mc_i128_low(a) != mc_i128_low(b)) return (mc_i128_low(a) < mc_i128_low(b)) ? -1 : 1;
	return 0;
}

/// -1, 0 or 1, reading both as signed.
static inline int mc_i128_compare(const mc_i128_s a, const mc_i128_s b) {
	const bool a_negative = mc_i128_negative(a), b_negative = mc_i128_negative(b);
	if (a_negative != b_negative) return a_negative ? -1 : 1;
	return mc_u128_compare(a, b);
}

static inline bool mc_i128_less(const mc_i128_s a, const mc_i128_s b) {
	return mc_i128_compare(a, b) < 0;
}

static inline mc_i128_s mc_i128_not(const mc_i128_s value) {
	return mc_i128_from_halves(~mc_i128_high(value), ~mc_i128_low(value));
}

static inline mc_i128_s mc_i128_negate(const mc_i128_s value) {
	return mc_i128_add(mc_i128_not(value), mc_i128_from_uint(1));
}

static inline mc_i128_s mc_i128_and(const mc_i128_s a, const mc_i128_s b) {
	return mc_i128_from_halves(mc_i128_high(a) & mc_i128_high(b), mc_i128_low(a) & mc_i128_low(b));
}

static inline mc_i128_s mc_i128_or(const mc_i128_s a, const mc_i128_s b) {
	return mc_i128_from_halves(mc_i128_high(a) | mc_i128_high(b), mc_i128_low(a) | mc_i128_low(b));
}

static inline mc_i128_s mc_i128_xor(const mc_i128_s a, const mc_i128_s b) {
	return mc_i128_from_halves(mc_i128_high(a) ^ mc_i128_high(b), mc_i128_low(a) ^ mc_i128_low(b));
}

/// shifts by 'count' bits; 128 or more shifts everything out.
static inline mc_i128_s mc_i128_shift_left(const mc_i128_s value, const unsigned count) {
	const uint64_t high = mc_i128_high(value), low = mc_i128_low(value);
	if (count >= 128) return mc_i128_zero();
	if (count >= 64) return mc_i128_from_halves(low << (count - 64), 0);
	if (0 == count) return value;
	return mc_i128_from_halves((high << count) | (low >> (64 - count)), low << count);
}

/// a logical shift right (unsigned).
static inline mc_i128_s mc_u128_shift_right(const mc_i128_s value, const unsigned count) {
	const uint64_t high = mc_i128_high(value), low = mc_i128_low(value);
	if (count >= 128) return mc_i128_zero();
	if (count >= 64) return mc_i128_from_halves(0, high >> (count - 64));
	if (0 == count) return value;
	return mc_i128_from_halves(high >> count, (low >> count) | (high << (64 - count)));
}

/// an arithmetic shift right (signed): the sign fills the top.
static inline mc_i128_s mc_i128_shift_right(const mc_i128_s value, const unsigned count) {
	if (!mc_i128_negative(value)) return mc_u128_shift_right(value, count);
	return mc_i128_not(mc_u128_shift_right(mc_i128_not(value), count));
}

/// the magnitude of a signed value (the most negative one stays itself, as unsigned).
static inline mc_i128_s mc_i128_magnitude(const mc_i128_s value) {
	return mc_i128_negative(value) ? mc_i128_negate(value) : value;
}

/// signed division truncating toward zero, like c; false when 'b' is zero.
static inline bool mc_i128_divide(const mc_i128_s a, const mc_i128_s b, mc_i128_s* const quotient, mc_i128_s* const remainder) {
	if (mc_i128_is_zero(b)) return false;
	mc_i128_s q, r;
	mc_u128_divide(mc_i128_magnitude(a), mc_i128_magnitude(b), &q, &r);
	if (mc_i128_negative(a) != mc_i128_negative(b)) q = mc_i128_negate(q);
	if (mc_i128_negative(a)) r = mc_i128_negate(r);
	if (quotient != NULL) *quotient = q;
	if (remainder != NULL) *remainder = r;
	return true;
}

/// the low 64 bits, as a signed integer.
static inline int64_t mc_i128_to_int(const mc_i128_s value) {
	return (int64_t)mc_i128_low(value);
}

/// whether a signed value fits an int64_t.
static inline bool mc_i128_fits_int(const mc_i128_s value) {
	return mc_i128_high(value) == (mc_i128_negative(mc_i128_from_int(mc_i128_to_int(value))) ? UINT64_MAX : 0);
}

/// an unsigned value as a double (rounded).
static inline double mc_u128_to_double(const mc_i128_s value) {
	return (double)mc_i128_high(value) * 18446744073709551616.0 + (double)mc_i128_low(value);
}

/// a signed value as a double (rounded).
static inline double mc_i128_to_double(const mc_i128_s value) {
	const double result = mc_u128_to_double(mc_i128_magnitude(value));
	return mc_i128_negative(value) ? -result : result;
}

/// a double truncated toward zero; nan and values out of range give the
/// most negative value, as x86 does.
static inline mc_i128_s mc_i128_from_double(const double value) {
	const double magnitude = (value < 0) ? -value : value;
	if (!(magnitude < 170141183460469231731687303715884105728.0)) return mc_i128_from_halves((uint64_t)1 << 63, 0);
	const double high = (double)(uint64_t)(magnitude / 18446744073709551616.0);
	const uint64_t low = (uint64_t)(magnitude - high * 18446744073709551616.0);
	const mc_i128_s result = mc_i128_from_halves((uint64_t)high, low);
	return (value < 0) ? mc_i128_negate(result) : result;
}

/// the decimal text of a value read as unsigned or signed; 'buffer' needs 41 bytes.
static inline const char* mc_u128_text(char* const buffer, const mc_i128_s value) {
	char digits[48];
	size_t count = 0;
	mc_i128_s rest = value;
	const mc_i128_s ten = mc_i128_from_uint(10);
	do {
		mc_i128_s digit;
		mc_u128_divide(rest, ten, &rest, &digit);
		digits[count++] = (char)('0' + (int)mc_i128_low(digit));
	} while (!mc_i128_is_zero(rest));
	size_t length = 0;
	while (count > 0) buffer[length++] = digits[--count];
	buffer[length] = '\0';
	return buffer;
}

static inline const char* mc_i128_text(char* const buffer, const mc_i128_s value) {
	if (!mc_i128_negative(value)) return mc_u128_text(buffer, value);
	buffer[0] = '-';
	(void)mc_u128_text(buffer + 1, mc_i128_magnitude(value));
	return buffer;
}

#endif
