
#ifndef __mia__int128_h__
#define __mia__int128_h__

/// 128-bit integers for the compiler: literals, constants and the checks of
/// overflow need more than 64 bits (an untyped constant is exact, and a
/// u64 times a u64 must be seen to overflow). a value is 128 bits of two's
/// complement; the functions say whether they read them signed ('i128') or
/// unsigned ('u128'). with gcc and clang the type is the compiler's own
/// __int128; elsewhere (msvc), or when mia_int128_portable or mia_portable is
/// defined, it is two 64-bit halves with the same results. the struct around
/// it keeps the operators of c away, so the code is the same for both.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(__SIZEOF_INT128__) && !defined(mia_int128_portable) && !defined(mia_portable)
#	define mia_int128_native 1
#else
#	define mia_int128_native 0
#endif

#if mia_int128_native
#	if defined(__GNUC__)
__extension__ typedef unsigned __int128 mia_int128_native_u128;
__extension__ typedef __int128 mia_int128_native_s128;
#	else
typedef unsigned __int128 mia_int128_native_u128;
typedef __int128 mia_int128_native_s128;
#	endif

typedef struct {
	mia_int128_native_u128 bits;
} mia_int128_s;

static inline mia_int128_s mia_int128_from_int(const int64_t value) {
	return (mia_int128_s) { (mia_int128_native_u128)(mia_int128_native_s128)value };
}

static inline mia_int128_s mia_int128_from_uint(const uint64_t value) {
	return (mia_int128_s) { (mia_int128_native_u128)value };
}

static inline mia_int128_s mia_int128_from_halves(const uint64_t high, const uint64_t low) {
	return (mia_int128_s) { ((mia_int128_native_u128)high << 64) | low };
}

static inline uint64_t mia_int128_low(const mia_int128_s value) {
	return (uint64_t)value.bits;
}

static inline uint64_t mia_int128_high(const mia_int128_s value) {
	return (uint64_t)(value.bits >> 64);
}

static inline mia_int128_s mia_int128_add(const mia_int128_s a, const mia_int128_s b) {
	return (mia_int128_s) { a.bits + b.bits };
}

static inline mia_int128_s mia_int128_sub(const mia_int128_s a, const mia_int128_s b) {
	return (mia_int128_s) { a.bits - b.bits };
}

static inline mia_int128_s mia_int128_mul(const mia_int128_s a, const mia_int128_s b) {
	return (mia_int128_s) { a.bits * b.bits };
}

/// unsigned division; 'b' must not be zero.
static inline void mia_int128_divide_unsigned(const mia_int128_s a, const mia_int128_s b, mia_int128_s* const quotient, mia_int128_s* const remainder) {
	if (quotient != NULL) quotient->bits = a.bits / b.bits;
	if (remainder != NULL) remainder->bits = a.bits % b.bits;
}

#else

typedef struct {
	uint64_t low ;
	uint64_t high;
} mia_int128_s;

static inline mia_int128_s mia_int128_from_int(const int64_t value) {
	return (mia_int128_s) { (uint64_t)value, (value < 0) ? UINT64_MAX : 0 };
}

static inline mia_int128_s mia_int128_from_uint(const uint64_t value) {
	return (mia_int128_s) { value, 0 };
}

static inline mia_int128_s mia_int128_from_halves(const uint64_t high, const uint64_t low) {
	return (mia_int128_s) { low, high };
}

static inline uint64_t mia_int128_low(const mia_int128_s value) {
	return value.low;
}

static inline uint64_t mia_int128_high(const mia_int128_s value) {
	return value.high;
}

static inline mia_int128_s mia_int128_add(const mia_int128_s a, const mia_int128_s b) {
	const uint64_t low = a.low + b.low;
	return (mia_int128_s) { low, a.high + b.high + (low < a.low ? 1 : 0) };
}

static inline mia_int128_s mia_int128_sub(const mia_int128_s a, const mia_int128_s b) {
	return (mia_int128_s) { a.low - b.low, a.high - b.high - (a.low < b.low ? 1 : 0) };
}

/// the full 128-bit product of two 64-bit halves.
static inline mia_int128_s mia_int128_multiply_64(const uint64_t a, const uint64_t b) {
	const uint64_t a0 = a & 0xFFFFFFFFu, a1 = a >> 32, b0 = b & 0xFFFFFFFFu, b1 = b >> 32;
	const uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
	const uint64_t middle = (p00 >> 32) + (p01 & 0xFFFFFFFFu) + (p10 & 0xFFFFFFFFu);
	return (mia_int128_s) { (p00 & 0xFFFFFFFFu) | (middle << 32), p11 + (p01 >> 32) + (p10 >> 32) + (middle >> 32) };
}

static inline mia_int128_s mia_int128_mul(const mia_int128_s a, const mia_int128_s b) {
	mia_int128_s product = mia_int128_multiply_64(a.low, b.low);
	product.high += a.low * b.high + a.high * b.low;
	return product;
}

/// unsigned division (shift and subtract); 'b' must not be zero.
static inline void mia_int128_divide_unsigned(const mia_int128_s a, const mia_int128_s b, mia_int128_s* const quotient, mia_int128_s* const remainder) {
	mia_int128_s q = { 0, 0 }, r = { 0, 0 };
	for (int bit = 127; bit >= 0; --bit) {
		r = (mia_int128_s) { (r.low << 1) | (bit >= 64 ? (a.high >> (bit - 64)) & 1 : (a.low >> bit) & 1), (r.high << 1) | (r.low >> 63) };
		const bool fits = r.high > b.high || (r.high == b.high && r.low >= b.low);
		if (fits) {
			r = mia_int128_sub(r, b);
			if (bit >= 64) q.high |= (uint64_t)1 << (bit - 64);
			else q.low |= (uint64_t)1 << bit;
		}
	}
	if (quotient != NULL) *quotient = q;
	if (remainder != NULL) *remainder = r;
}

#endif

// --- the same on both ---------------------------------------------------------------------

static inline mia_int128_s mia_int128_zero(void) {
	return mia_int128_from_uint(0);
}

static inline bool mia_int128_negative(const mia_int128_s value) {
	return (mia_int128_high(value) >> 63) != 0;
}

static inline bool mia_int128_is_zero(const mia_int128_s value) {
	return 0 == mia_int128_low(value) && 0 == mia_int128_high(value);
}

static inline bool mia_int128_equal(const mia_int128_s a, const mia_int128_s b) {
	return mia_int128_low(a) == mia_int128_low(b) && mia_int128_high(a) == mia_int128_high(b);
}

/// -1, 0 or 1, reading both as unsigned.
static inline int mia_int128_compare_unsigned(const mia_int128_s a, const mia_int128_s b) {
	if (mia_int128_high(a) != mia_int128_high(b)) return (mia_int128_high(a) < mia_int128_high(b)) ? -1 : 1;
	if (mia_int128_low(a) != mia_int128_low(b)) return (mia_int128_low(a) < mia_int128_low(b)) ? -1 : 1;
	return 0;
}

/// -1, 0 or 1, reading both as signed.
static inline int mia_int128_compare(const mia_int128_s a, const mia_int128_s b) {
	const bool a_negative = mia_int128_negative(a), b_negative = mia_int128_negative(b);
	if (a_negative != b_negative) return a_negative ? -1 : 1;
	return mia_int128_compare_unsigned(a, b);
}

static inline bool mia_int128_less(const mia_int128_s a, const mia_int128_s b) {
	return mia_int128_compare(a, b) < 0;
}

static inline mia_int128_s mia_int128_not(const mia_int128_s value) {
	return mia_int128_from_halves(~mia_int128_high(value), ~mia_int128_low(value));
}

static inline mia_int128_s mia_int128_negate(const mia_int128_s value) {
	return mia_int128_add(mia_int128_not(value), mia_int128_from_uint(1));
}

static inline mia_int128_s mia_int128_and(const mia_int128_s a, const mia_int128_s b) {
	return mia_int128_from_halves(mia_int128_high(a) & mia_int128_high(b), mia_int128_low(a) & mia_int128_low(b));
}

static inline mia_int128_s mia_int128_or(const mia_int128_s a, const mia_int128_s b) {
	return mia_int128_from_halves(mia_int128_high(a) | mia_int128_high(b), mia_int128_low(a) | mia_int128_low(b));
}

static inline mia_int128_s mia_int128_xor(const mia_int128_s a, const mia_int128_s b) {
	return mia_int128_from_halves(mia_int128_high(a) ^ mia_int128_high(b), mia_int128_low(a) ^ mia_int128_low(b));
}

/// shifts by 'count' bits; 128 or more shifts everything out.
static inline mia_int128_s mia_int128_shift_left(const mia_int128_s value, const unsigned count) {
	const uint64_t high = mia_int128_high(value), low = mia_int128_low(value);
	if (count >= 128) return mia_int128_zero();
	if (count >= 64) return mia_int128_from_halves(low << (count - 64), 0);
	if (0 == count) return value;
	return mia_int128_from_halves((high << count) | (low >> (64 - count)), low << count);
}

/// a logical shift right (unsigned).
static inline mia_int128_s mia_int128_shift_right_unsigned(const mia_int128_s value, const unsigned count) {
	const uint64_t high = mia_int128_high(value), low = mia_int128_low(value);
	if (count >= 128) return mia_int128_zero();
	if (count >= 64) return mia_int128_from_halves(0, high >> (count - 64));
	if (0 == count) return value;
	return mia_int128_from_halves(high >> count, (low >> count) | (high << (64 - count)));
}

/// an arithmetic shift right (signed): the sign fills the top.
static inline mia_int128_s mia_int128_shift_right(const mia_int128_s value, const unsigned count) {
	if (!mia_int128_negative(value)) return mia_int128_shift_right_unsigned(value, count);
	return mia_int128_not(mia_int128_shift_right_unsigned(mia_int128_not(value), count));
}

/// the magnitude of a signed value (the most negative one stays itself, as unsigned).
static inline mia_int128_s mia_int128_magnitude(const mia_int128_s value) {
	return mia_int128_negative(value) ? mia_int128_negate(value) : value;
}

/// signed division truncating toward zero, like c; false when 'b' is zero.
static inline bool mia_int128_divide(const mia_int128_s a, const mia_int128_s b, mia_int128_s* const quotient, mia_int128_s* const remainder) {
	if (mia_int128_is_zero(b)) return false;
	mia_int128_s q, r;
	mia_int128_divide_unsigned(mia_int128_magnitude(a), mia_int128_magnitude(b), &q, &r);
	if (mia_int128_negative(a) != mia_int128_negative(b)) q = mia_int128_negate(q);
	if (mia_int128_negative(a)) r = mia_int128_negate(r);
	if (quotient != NULL) *quotient = q;
	if (remainder != NULL) *remainder = r;
	return true;
}

/// the low 64 bits, as a signed integer.
static inline int64_t mia_int128_to_int(const mia_int128_s value) {
	return (int64_t)mia_int128_low(value);
}

/// whether a signed value fits an int64_t.
static inline bool mia_int128_fits_int(const mia_int128_s value) {
	return mia_int128_high(value) == (mia_int128_negative(mia_int128_from_int(mia_int128_to_int(value))) ? UINT64_MAX : 0);
}

/// an unsigned value as a double (rounded).
static inline double mia_int128_to_double_unsigned(const mia_int128_s value) {
	return (double)mia_int128_high(value) * 18446744073709551616.0 + (double)mia_int128_low(value);
}

/// a signed value as a double (rounded).
static inline double mia_int128_to_double(const mia_int128_s value) {
	const double result = mia_int128_to_double_unsigned(mia_int128_magnitude(value));
	return mia_int128_negative(value) ? -result : result;
}

/// a double truncated toward zero; nan and values out of range give the
/// most negative value, as x86 does.
static inline mia_int128_s mia_int128_from_double(const double value) {
	const double magnitude = (value < 0) ? -value : value;
	if (!(magnitude < 170141183460469231731687303715884105728.0)) return mia_int128_from_halves((uint64_t)1 << 63, 0);
	const double high = (double)(uint64_t)(magnitude / 18446744073709551616.0);
	const uint64_t low = (uint64_t)(magnitude - high * 18446744073709551616.0);
	const mia_int128_s result = mia_int128_from_halves((uint64_t)high, low);
	return (value < 0) ? mia_int128_negate(result) : result;
}

/// the decimal text of a value read as unsigned or signed; 'buffer' needs 41 bytes.
static inline const char* mia_int128_text_unsigned(char* const buffer, const mia_int128_s value) {
	char digits[48];
	size_t count = 0;
	mia_int128_s rest = value;
	const mia_int128_s ten = mia_int128_from_uint(10);
	do {
		mia_int128_s digit;
		mia_int128_divide_unsigned(rest, ten, &rest, &digit);
		digits[count++] = (char)('0' + (int)mia_int128_low(digit));
	} while (!mia_int128_is_zero(rest));
	size_t length = 0;
	while (count > 0) buffer[length++] = digits[--count];
	buffer[length] = '\0';
	return buffer;
}

static inline const char* mia_int128_text(char* const buffer, const mia_int128_s value) {
	if (!mia_int128_negative(value)) return mia_int128_text_unsigned(buffer, value);
	buffer[0] = '-';
	(void)mia_int128_text_unsigned(buffer + 1, mia_int128_magnitude(value));
	return buffer;
}

#endif
