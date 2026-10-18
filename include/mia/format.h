
#ifndef __mia__format_h__
#define __mia__format_h__

/// text forms of values. they follow python's conventions (repr of floats,
/// strings and bytes, the 'g' format), so trees, values and messages print
/// the same on every system.

#include "mia/int128.h"
#include "mia/string.h"

/// the shortest text that reads back as 'value' (python's repr: '1.5',
/// '1e+20', '0.0001', 'inf', 'nan').
void mia_format_float_repr(mia_string_s* const out, const double value);

/// 'value' as c's printf writes it with 'type' ('e', 'E', 'f' or 'g') and
/// 'precision', at any length; the exponent has two digits or more, as on
/// every system (some c libraries write three: 1e+003).
void mia_format_float_c(mia_string_s* const out, const double value, const int precision, const char type);

/// an f32 value: the shortest 'g' text that reads back as the same f32.
void mia_format_f32(mia_string_s* const out, const double value);

/// a float as 'print' shows it: the shortest text that reads back as the
/// same value (of 'bits' 32 or 64), always with a '.' or an exponent
/// ('1.0', '0.1', '1e+20', 'inf', 'nan').
void mia_format_float_value(mia_string_s* const out, const double value, const int bits);

/// a 128-bit integer in decimal.
void mia_format_int128(mia_string_s* const out, const mia_int128_s value);

void mia_format_uint128(mia_string_s* const out, const mia_int128_s value);

/// python's repr of a text ('name', "it's") and of bytes (the part after b).
void mia_format_repr_text(mia_string_s* const out, const mia_view_s text);

/// 'bytes' as utf-8 text: each maximal invalid subsequence becomes U+FFFD,
/// like python's bytes.decode('utf-8', 'replace').
void mia_format_utf8(mia_string_s* const out, const mia_view_s bytes);

void mia_format_repr_bytes(mia_string_s* const out, const mia_view_s bytes);

#endif
