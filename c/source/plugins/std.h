
#ifndef __mc__plugins_std_h__
#define __mc__plugins_std_h__

/// the 'std' plugin: the functions of the standard library's 'memory' and
/// 'io' modules. it is compiled into mc and registered like any plugin,
/// and it uses mc only through mc/plugin.h (and mc's text helpers), so it
/// is also an example of a plugin.
///
///   std.c         registration
///   std_memory.c  memory.alloc, realloc, free, copy
///   std_io.c      io.print, println (with the check of the format), write, write_line
///   std_format.c  format specifiers
///
/// a format is text with placeholders: '{}' or '{:spec}' takes the next
/// value, '{{' and '}}' write braces. a spec is
///
///     [[fill]align][sign][#][0][width][.precision][type]
///
/// align '<' left, '>' right, '^' center (fill: any printable ascii but '{'
/// and '}'); sign '+' (always) or '-' (the default); '#' writes '0x', '0o' or
/// '0b' before integers in those bases; '0' pads numbers with zeros after
/// the sign; type 'd' (the default), 'x', 'X', 'o' or 'b' for integers, 'f',
/// 'e', 'E' or 'g' (the default: the shortest text that reads back) for
/// floats. a precision is the digits after the point ('f', 'e') or the
/// significant digits ('g'). width and fill apply to every value: numbers
/// are right-aligned, everything else left-aligned unless 'align' says.

#include "mc/plugin.h"
#include "mc/format.h"
#include "mc/string.h"

typedef struct {
	char   fill         ;
	char   align        ;   // '<', '>', '^', or 0
	char   sign         ;   // '+', '-', or 0
	bool   alternate    ;   // '#'
	bool   zero         ;   // '0'
	size_t width        ;
	bool   has_precision;
	size_t precision    ;
	char   type         ;   // one of "dxXobfeEg", or 0
} mc_std_spec_s;

/// a piece of a format: text, then a placeholder (or a problem).
typedef struct {
	size_t        text_start ;
	size_t        text_end   ;
	bool          placeholder;
	size_t        spec_start ;   // the text between the braces
	size_t        spec_end   ;
	mc_std_spec_s spec       ;
	const char*   problem    ;   // the format is invalid here
} mc_std_piece_s;

/// what a spec is applied to.
typedef enum {
	mc_std_value_int  ,
	mc_std_value_float,
	mc_std_value_other,   // bools, text, arrays, structs...
} mc_std_value_e;

/// the next piece of a format from '*at' (which it moves); false at the end.
bool mc_std_next_placeholder(const uint8_t* const data, const size_t length, size_t* const at, mc_std_piece_s* const piece);

/// parses the text after ':'; the problem, or null when it is valid.
const char* mc_std_parse_spec(const uint8_t* const text, const size_t length, mc_std_spec_s* const spec);

/// whether a spec fits a value: the problem, or null.
const char* mc_std_spec_fits(const mc_std_spec_s* const spec, const mc_std_value_e kind);

/// a value formatted by a spec, appended to 'out'.
void mc_std_format_integer(mc_string_s* const out, const mc_std_spec_s* const spec, const bool negative, const uint64_t magnitude);
void mc_std_format_float(mc_string_s* const out, const mc_std_spec_s* const spec, const double value, const int bits);
void mc_std_format_text(mc_string_s* const out, const mc_std_spec_s* const spec, const mc_string_s* const text);

/// io.print and println mark the values that are c strings ("..."c): only
/// they lose their trailing NUL when printed.
#define mc_std_flag_c_string 1u

mc_plugin_value_s mc_std_alloc(mc_plugin_call_s* const call);
mc_plugin_value_s mc_std_realloc(mc_plugin_call_s* const call);
mc_plugin_value_s mc_std_free(mc_plugin_call_s* const call);
mc_plugin_value_s mc_std_copy(mc_plugin_call_s* const call);

mc_plugin_value_s mc_std_print(mc_plugin_call_s* const call);
mc_plugin_value_s mc_std_println(mc_plugin_call_s* const call);
mc_plugin_value_s mc_std_write(mc_plugin_call_s* const call);
mc_plugin_value_s mc_std_write_line(mc_plugin_call_s* const call);
void mc_std_check_format(mc_plugin_check_s* const check);

#endif
