
#ifndef __mc__format_h__
#define __mc__format_h__

/// text forms of values. they follow python's conventions exactly (repr of
/// floats, strings and bytes, the 'g' format), so the c and the python
/// implementations print identical trees, values and messages.

#include "mc/int128.h"
#include "mc/string.h"

/// the shortest text that reads back as 'value' (python's repr: '1.5',
/// '1e+20', '0.0001', 'inf', 'nan').
void mc_format_float_repr(mc_string_s* const out, const double value);

/// printf's "%.<precision>g".
void mc_format_float_g(mc_string_s* const out, const double value, const int precision);

/// an f32 value: the shortest 'g' text that reads back as the same f32.
void mc_format_f32(mc_string_s* const out, const double value);

/// a float as 'print' shows it: the shortest text that reads back as the
/// same value (of 'bits' 32 or 64), always with a '.' or an exponent
/// ('1.0', '0.1', '1e+20', 'inf', 'nan').
void mc_format_float_value(mc_string_s* const out, const double value, const int bits);

/// a 128-bit integer in decimal.
void mc_format_int128(mc_string_s* const out, const mc_i128_s value);

void mc_format_uint128(mc_string_s* const out, const mc_i128_s value);

/// python's repr of a text ('name', "it's") and of bytes (the part after b).
void mc_format_repr_text(mc_string_s* const out, const mc_view_s text);

/// 'bytes' as utf-8 text: each maximal invalid subsequence becomes U+FFFD,
/// like python's bytes.decode('utf-8', 'replace').
void mc_format_utf8(mc_string_s* const out, const mc_view_s bytes);

void mc_format_repr_bytes(mc_string_s* const out, const mc_view_s bytes);

#endif
