
/// 
/// cairo is a small, single-header unit test framework for c. drop cairo.h into
/// a tests c file, define tests with 'cairo_test_new(suite, name)', assert with
/// the 'cairo_assert_*' family, and run 'cairo_tests_run' in the main function.
/// there is no tests registration list to maintain and no library to link since
/// every test records itself into a dedicated linker section that the framework
/// walks at start up. the framework uses the section trick to stay declarative,
/// does no heap allocations, and aims to build cleanly, identically across many
/// toolchains such as clang, gcc, and msvc. tests are sorted, and can, also, be
/// filtered, shuffled, and repeated from the command line or the main function,
/// failures report the offending source location, with the compared values, and
/// nearly every libc call it makes can be overridden with 'cairo_*' macros.
/// 
/// usage example:
/// {
/// #include "cairo.h"
/// 
/// cairo_test_new(demo, test000) {
/// }
/// 
/// cairo_test_new(demo, test001) {
///     cairo_test_skip();
/// }
/// 
/// cairo_test_new(demo, test002) {
///     cairo_assert_true(true);
/// }
/// 
/// cairo_test_new(demo, test003) {
///     cairo_assert_false(false);
/// }
/// 
/// cairo_test_new(demo, test004) {
///     cairo_assert_eq(0, 0);
/// }
/// 
/// cairo_test_new(demo, test005) {
///     cairo_assert_neq(0, 1);
/// }
/// 
/// cairo_test_new(demo, test006) {
///     cairo_assert_gt(1, 0);
/// }
/// 
/// cairo_test_new(demo, test007) {
///     cairo_assert_ge(1, 1);
/// }
/// 
/// cairo_test_new(demo, test008) {
///     cairo_assert_lt(0, 1);
/// }
/// 
/// cairo_test_new(demo, test009) {
///     cairo_assert_le(1, 1);
/// }
/// 
/// cairo_test_new(demo, test010) {
///     cairo_assert_nr(0, 1e-7, 1e-6);
/// }
/// 
/// cairo_test_new(demo, test011) {
///     cairo_assert_nnr(0, 1, 1e-6);
/// }
/// 
/// cairo_test_new(demo, test012) {
///     cairo_assert_nreq(0, 1e-7);
/// }
/// 
/// cairo_test_new(demo, test013) {
///     cairo_assert_nnreq(0, 1);
/// }
/// 
/// cairo_test_new(demo, test014) {
///     cairo_assert_streq("foo", "foo");
/// }
/// 
/// cairo_test_new(demo, test015) {
///     cairo_assert_strneq("foo", "bar");
/// }
/// 
/// cairo_test_new(demo, test016) {
///     cairo_expect_true(true);
///     cairo_test_defered {}
/// }
/// 
/// cairo_test_new(demo, test017) {
///     cairo_expect_false(false);
///     cairo_test_defered {}
/// }
/// 
/// cairo_test_new(demo, test018) {
///     cairo_expect_eq(0, 0);
///     cairo_test_defered {}
/// }
/// 
/// cairo_test_new(demo, test019) {
///     cairo_expect_neq(0, 1);
///     cairo_test_defered {}
/// }
/// 
/// cairo_test_new(demo, test020) {
///     cairo_expect_gt(1, 0);
///     cairo_test_defered {}
/// }
/// 
/// cairo_test_new(demo, test021) {
///     cairo_expect_ge(1, 1);
///     cairo_test_defered {}
/// }
/// 
/// cairo_test_new(demo, test022) {
///     cairo_expect_lt(0, 1);
///     cairo_test_defered {}
/// }
/// 
/// cairo_test_new(demo, test023) {
///     cairo_expect_le(1, 1);
///     cairo_test_defered {}
/// }
/// 
/// cairo_test_new(demo, test024) {
///     cairo_expect_nr(0, 1e-7, 1e-6);
///     cairo_test_defered {}
/// }
/// 
/// cairo_test_new(demo, test025) {
///     cairo_expect_nnr(0, 1, 1e-6);
///     cairo_test_defered {}
/// }
/// 
/// cairo_test_new(demo, test026) {
///     cairo_expect_nreq(0, 1e-7);
///     cairo_test_defered {}
/// }
/// 
/// cairo_test_new(demo, test027) {
///     cairo_expect_nnreq(0, 1);
///     cairo_test_defered {}
/// }
/// 
/// cairo_test_new(demo, test028) {
///     cairo_expect_streq("foo", "foo");
///     cairo_test_defered {}
/// }
/// 
/// cairo_test_new(demo, test029) {
///     cairo_expect_strneq("foo", "bar");
///     cairo_test_defered {}
/// }
/// 
/// cairo_test_new(demo, test030) {
///     cairo_should_fail(cairo_assert_true(false));
/// }
/// 
/// cairo_test_new(demo, test031) {
///     cairo_should_fail(cairo_expect_true(false));
///     cairo_test_defered {}
/// }
/// 
/// int main(const int argc, const char* argv[]) {
///     {  // simple run
///         return cairo_tests_run(cairo_args_new(argc, argv));
///     }
/// 
///     {  // complex run
///         cairo_args_s args = cairo_args_new(argc, argv);
///         args.include = "demo.test01:demo.test1*";  // or pass these via cli.
///         return cairo_tests_run(args);
///     }
/// }
/// 

#ifndef __cairo_h__
#define __cairo_h__

#include <stdbool.h>
#include <stdint.h>

#ifndef _cairo_version
#	define _cairo_version "1.3.1"
#endif

/// override hook for 'NULL', the null pointer constant.
#ifndef cairo_null
#	include <stddef.h>
#	define cairo_null NULL
#endif

/// override hook for 'SIZE_MAX', the largest value a 'size_t' can hold. used to
/// reject parsed numeric cli args that would not fit in a 'size_t'.
#ifndef cairo_size_max
#	include <stddef.h>
#	define cairo_size_max SIZE_MAX
#endif

/// override hook for 'strcmp', lexicographic c-string comparison. used to order
/// tests by suite/name and to match option and suite spellings.
#ifndef cairo_strcmp
#	include <string.h>
#	define cairo_strcmp strcmp
#endif

/// override hook for 'strstr', finds the first occurrence of a substring in the
/// provided c-string. it is used to find the 'assert_/expect_' call on a source
/// line when  rendering the verbose report's caret and column.
#ifndef cairo_strstr
#	include <string.h>
#	define cairo_strstr strstr
#endif

/// override hook for 'strtoul', parses an unsigned long from text in given base
/// (base 10 is used for now). used to read the numeric '--repeat' and '--ecode'
/// cli values.
#ifndef cairo_strtoul
#	include <stdlib.h>
#	define cairo_strtoul strtoul
#endif

/// override hook for 'qsort', in-place array sort. used to order collected test
/// references by suite and name before running them.
#ifndef cairo_qsort
#	include <stdlib.h>
#	define cairo_qsort qsort
#endif

/// override hook for 'ULONG_MAX', the largest value an unsigned long stores. it
/// is used to guard the 'size_t' range check when parsing numeric cli args.
#ifndef cairo_ulong_max
#	include <limits.h>
#	define cairo_ulong_max ULONG_MAX
#endif

/// override hook for 'putchar', writes a single character to stdout. it is used
/// to emit the caret/tilde underline beneath an offending source in the verbose
/// failure report.
#ifndef cairo_putchar
#	include <stdio.h>
#	define cairo_putchar putchar
#endif

/// override hook for 'snprintf', bounded formatted write into a buffer. used to
/// stringify operand values and format elapsed durations.
#ifndef cairo_snprintf
#	include <stdio.h>
#	define cairo_snprintf snprintf
#endif

/// override hook for 'fprintf', formatted write to a stream. it's used to print
/// usage text and cli argument errors.
#ifndef cairo_fprintf
#	include <stdio.h>
#	define cairo_fprintf fprintf
#endif

/// override hook for 'printf', formatted write to stdout. used for all of tests
/// listing, progress, failure, and summary output.
#ifndef cairo_printf
#	include <stdio.h>
#	define cairo_printf printf
#endif

/// override hook for 'fflush', flushes a stream's buffer. it's used to put each
/// progress character to cairo_stdout as tests run.
#ifndef cairo_fflush
#	include <stdio.h>
#	define cairo_fflush fflush
#endif

/// override hook for 'stdout', the standard output stream. it is used in usage,
/// listing, and report output.
#ifndef cairo_stdout
#	include <stdio.h>
#	define cairo_stdout stdout
#endif

/// override hook for 'stderr', the standard error stream. it's used for command
/// line argument related error messages.
#ifndef cairo_stderr
#	include <stdio.h>
#	define cairo_stderr stderr
#endif

/// override hook for 'FILE', the stdio stream type. used for wokring with files
/// and streams in reporting and more.
#ifndef cairo_file
#	include <stdio.h>
#	define cairo_file FILE
#endif

/// override hook for 'fopen', opens a file by path and mode, returning a stream
/// (or null). used to open a test's source file when rendering verbose reports.
#ifndef cairo_fopen
#	include <stdio.h>
#	define cairo_fopen fopen
#endif

/// override hook for 'fclose', closes a stream opened with 'cairo_fopen'. it is
/// used to release the source file once the verbose report is done reading it.
#ifndef cairo_fclose
#	include <stdio.h>
#	define cairo_fclose fclose
#endif

/// override hook for 'fgets', reads one line into a buffer bounded by its size.
/// used to walk a test file source line by line while and rendering the verbose
/// report failure report.
#ifndef cairo_fgets
#	include <stdio.h>
#	define cairo_fgets fgets
#endif

/// override hook for 'rewind', returns a stream to its start. used to re-read a
/// test's source file: a first pass locates the caret column, a second one then
/// prints the surrounding source lines.
#ifndef cairo_rewind
#	include <stdio.h>
#	define cairo_rewind rewind
#endif

/// override hook for 'errno', the c error indicator. checked for 'ERANGE' after
/// 'cairo_strtoul' to detect numeric overflow.
#ifndef cairo_errno
#	include <errno.h>
#	define cairo_errno errno
#endif

/// override hook for 'ERANGE', the errno value signaling an out-of-range result
/// compared against 'cairo_errno' after 'cairo_strtoul' to detect overflow when
/// parsing numeric cli args.
#ifndef cairo_erange
#	include <errno.h>
#	define cairo_erange ERANGE
#endif

/// override hook for 'clock', returns elapsed processor/clock ticks. it is used
/// as the raw time source for measuring test durations.
#ifndef cairo_clock
#	include <time.h>
#	define cairo_clock clock
#endif

/// override hook for 'CLOCKS_PER_SEC', ticks per second for 'cairo_clock'. used
/// to convert raw clock ticks into seconds.
#ifndef cairo_clocks_per_sec
#	include <time.h>
#	define cairo_clocks_per_sec CLOCKS_PER_SEC
#endif

/// override hook for 'fabs', absolute value of a provided double value. used to
/// compute the magnitude of the difference in the near/loose float assertions.
#ifndef cairo_fabs
#	include <math.h>
#	define cairo_fabs fabs
#endif

/// keeping everything 'static inline' lets the whole framework live in a single
/// header file without producing duplicate external symbols when it is included
/// in many translation units.
#define _cairo_func static inline

/// needed to resolve 'unused parameter' warning/error in empty test functions.
#if defined(__STDC_VERSION__) && (__STDC_VERSION__ > 201710L)
#	define _cairo_unused [[maybe_unused]]
#elif defined(__GNUC__) || defined(__clang__)
#	define _cairo_unused __attribute__((unused))
#else
#	define _cairo_unused
#endif

/// emits the 'retain' attribute prefix when the toolchain understands it so the
/// linker keeps test records that are never referenced by name.
#if defined(__has_attribute) && __has_attribute(retain)
#	define _cairo_retain retain,
#else
#	define _cairo_retain
#endif

/// wraps an assert '_body' so that signed/unsigned comparison warnings are kept
/// quiet for just that block, letting type-generic asserts compare values whose
/// literals differ in signedness without noise. the suppression is opt-in, when
/// 'cairo_supress_sign_compare_warnings' is defined the gcc, clang, and/or msvc
/// pragma pair is emitted around '_body'; otherwise, and on unknown toolchains,
/// '_body' is emitted unchanged inside a plain do/while.
#ifdef cairo_supress_sign_compare_warnings
#	if defined(__GNUC__) || defined(__clang__)
#		define _cairo_diag_nosign(_body) do {                                  \
				_Pragma("GCC diagnostic push")                                 \
				_Pragma("GCC diagnostic ignored \"-Wsign-compare\"")           \
				_body                                                          \
				_Pragma("GCC diagnostic pop")                                  \
			} while (0)
#	elif defined(_MSC_VER)
#		define _cairo_diag_nosign(_body) do {                                  \
				__pragma(warning(push))                                        \
				__pragma(warning(disable: 4018 4389))                          \
				_body                                                          \
				__pragma(warning(pop))                                         \
			} while (0)
#	else
#		define _cairo_diag_nosign(_body) do {                                  \
				_body                                                          \
			} while (0)
#	endif
#else
#	define _cairo_diag_nosign(_body) do {                                      \
			_body                                                              \
		} while (0)
#endif

typedef struct _cairo_test_s _cairo_test_s;

/// cross-toolchain handling for special cairo linker section which stores every
/// defined cairo test. each 'cairo_test_new' stores its test's reference in the
/// mentioned cairo section. '_cairo_begin' and '_cairo_end' mark that section's
/// bounds at link time so the cairo runner can walk it without any central test
/// registration list.
#if defined(__ELF__)
#	define _cairo_section __attribute__((used, _cairo_retain section("cairo")))
extern _cairo_test_s* __start_cairo;
extern _cairo_test_s* __stop_cairo;
#	define _cairo_begin (&__start_cairo)
#	define _cairo_end (&__stop_cairo)
#elif defined(_MSC_VER)
#	pragma section("cairo$a", read, write)
#	pragma section("cairo$m", read, write)
#	pragma section("cairo$z", read, write)
__declspec(allocate("cairo$a"))
static _cairo_test_s* _cairo_sec_start = cairo_null;
__declspec(allocate("cairo$z"))
static _cairo_test_s* _cairo_sec_stop  = cairo_null;
#	define _cairo_section __declspec(allocate("cairo$m"))
#	define _cairo_begin (&_cairo_sec_start + 1)
#	define _cairo_end (&_cairo_sec_stop)
#elif defined(__APPLE__)
#	define _cairo_section                                                      \
		__attribute__((used, section("__DATA,__cairo,regular,no_dead_strip")))
extern _cairo_test_s* _cairo_sec_start __asm("section$start$__DATA$__cairo");
extern _cairo_test_s* _cairo_sec_stop  __asm("section$end$__DATA$__cairo");
#	define _cairo_begin (&_cairo_sec_start)
#	define _cairo_end (&_cairo_sec_stop)
#else
#	error "cairo: unsupported toolchain/platform!"
#endif

typedef char               _cairo_c_t  ;
typedef signed char        _cairo_sc_t ;
typedef unsigned char      _cairo_uc_t ;
typedef bool               _cairo_b_t  ;
typedef signed short       _cairo_ss_t ;
typedef unsigned short     _cairo_us_t ;
typedef signed int         _cairo_si_t ;
typedef unsigned int       _cairo_ui_t ;
typedef signed long        _cairo_sl_t ;
typedef unsigned long      _cairo_ul_t ;
typedef signed long long   _cairo_sll_t;
typedef unsigned long long _cairo_ull_t;
typedef float              _cairo_f_t  ;
typedef double             _cairo_d_t  ;
typedef long double        _cairo_ld_t ;

/// formats a char value into a buffer of provided size as text.
_cairo_func _cairo_c_t _cairo_format_c(char* const buffer,
									   const size_t size,
									   const _cairo_c_t value) {
	(void)cairo_snprintf(buffer, size, "%c", value);
	return value;
}

/// formats a signed char value into a buffer of provided size as text.
_cairo_func _cairo_sc_t _cairo_format_sc(char* const buffer,
										 const size_t size,
										 const _cairo_sc_t value) {
	(void)cairo_snprintf(buffer, size, "%hhd", value);
	return value;
}

/// formats a unsigned char value into a buffer of provided size as text.
_cairo_func _cairo_uc_t _cairo_format_uc(char* const buffer,
										 const size_t size,
										 const _cairo_uc_t value) {
	(void)cairo_snprintf(buffer, size, "%hhu", value);
	return value;
}

/// formats a bool value into a buffer of provided size as text.
_cairo_func _cairo_b_t _cairo_format_b(char* const buffer,
									   const size_t size,
									   const _cairo_b_t value) {
	(void)cairo_snprintf(buffer, size, "%s", value ? "true" : "false");
	return value;
}

/// formats a signed short value into a buffer of provided size as text.
_cairo_func _cairo_ss_t _cairo_format_ss(char* const buffer,
										 const size_t size,
										 const _cairo_ss_t value) {
	(void)cairo_snprintf(buffer, size, "%hd", value);
	return value;
}

/// formats a unsigned short value into a buffer of provided size as text.
_cairo_func _cairo_us_t _cairo_format_us(char* const buffer,
										 const size_t size,
										 const _cairo_us_t value) {
	(void)cairo_snprintf(buffer, size, "%hu", value);
	return value;
}

/// formats a signed int value into a buffer of provided size as text.
_cairo_func _cairo_si_t _cairo_format_si(char* const buffer,
										 const size_t size,
										 const _cairo_si_t value) {
	(void)cairo_snprintf(buffer, size, "%d", value);
	return value;
}

/// formats a unsigned int value into a buffer of provided size as text.
_cairo_func _cairo_ui_t _cairo_format_ui(char* const buffer,
										 const size_t size,
										 const _cairo_ui_t value) {
	(void)cairo_snprintf(buffer, size, "%u", value);
	return value;
}

/// formats a signed long value into a buffer of provided size as text.
_cairo_func _cairo_sl_t _cairo_format_sl(char* const buffer,
										 const size_t size,
										 const _cairo_sl_t value) {
	(void)cairo_snprintf(buffer, size, "%ld", value);
	return value;
}

/// formats a unsigned long value into a buffer of provided size as text.
_cairo_func _cairo_ul_t _cairo_format_ul(char* const buffer,
										 const size_t size,
										 const _cairo_ul_t value) {
	(void)cairo_snprintf(buffer, size, "%lu", value);
	return value;
}

/// formats a signed long long value into a buffer of provided size as text.
_cairo_func _cairo_sll_t _cairo_format_sll(char* const buffer,
										   const size_t size,
										   const _cairo_sll_t value) {
	(void)cairo_snprintf(buffer, size, "%lld", value);
	return value;
}

/// formats a unsigned long long value into a buffer of provided size as text.
_cairo_func _cairo_ull_t _cairo_format_ull(char* const buffer,
										   const size_t size,
										   const _cairo_ull_t value) {
	(void)cairo_snprintf(buffer, size, "%llu", value);
	return value;
}

/// formats a float value into a buffer of provided size as text.
_cairo_func _cairo_f_t _cairo_format_f(char* const buffer,
									   const size_t size,
									   const _cairo_f_t value) {
	(void)cairo_snprintf(buffer, size, "%g", (double)value);
	return value;
}

/// formats a double value into a buffer of provided size as text.
_cairo_func _cairo_d_t _cairo_format_d(char* const buffer,
									   const size_t size,
									   const _cairo_d_t value) {
	(void)cairo_snprintf(buffer, size, "%g", value);
	return value;
}

/// formats a long double value into a buffer of provided size as text.
_cairo_func _cairo_ld_t _cairo_format_ld(char* const buffer,
										 const size_t size,
										 const _cairo_ld_t value) {
	(void)cairo_snprintf(buffer, size, "%Lg", value);
	return value;
}

/// formats a char* value into a buffer of provided size as text.
_cairo_func const char* _cairo_format_cstr(char* const buffer,
										   const size_t size,
										   const char* const value) {
	if (cairo_null == value) (void)cairo_snprintf(buffer, size, "null")       ;
	else                     (void)cairo_snprintf(buffer, size, "'%s'", value);
	return value;
}

/// formats a void* value into a buffer of provided size as text.
_cairo_func const void* _cairo_format_ptr(char* const buffer,
										  const size_t size,
										  const void* const value) {
	if (cairo_null == value) (void)cairo_snprintf(buffer, size, "null")     ;
	else                     (void)cairo_snprintf(buffer, size, "%p", value);
	return value;
}

/// formats a '_value' into character buffer '_buffer' by selecting the matching
/// '_cairo_format_*' formatter for the value's type at compile time, so that, a
/// failed assertion can print 'left' and 'right', without the caller naming the
/// types. it is the non-string table, it works the arithmetic and boolean types
/// and sends anything else (any pointer) as a raw address.
#define _cairo_format_non_str_type(_buffer, _value) _Generic((_value),         \
		char:               _cairo_format_c  ,                                 \
		signed char:        _cairo_format_sc ,                                 \
		unsigned char:      _cairo_format_uc ,                                 \
		_Bool:              _cairo_format_b  ,                                 \
		signed short:       _cairo_format_ss ,                                 \
		unsigned short:     _cairo_format_us ,                                 \
		signed int:         _cairo_format_si ,                                 \
		unsigned int:       _cairo_format_ui ,                                 \
		signed long:        _cairo_format_sl ,                                 \
		unsigned long:      _cairo_format_ul ,                                 \
		signed long long:   _cairo_format_sll,                                 \
		unsigned long long: _cairo_format_ull,                                 \
		float:              _cairo_format_f  ,                                 \
		double:             _cairo_format_d  ,                                 \
		long double:        _cairo_format_ld ,                                 \
		default:            _cairo_format_ptr                                  \
	)((_buffer), sizeof(_buffer), (_value))

/// counterpart to '_cairo_format_non_str_type', used for the string asserts. it
/// maps 'char*'/'const char*' to '_cairo_format_cstr' (which quotes the text or
/// emits 'null'), and pushes anything else to '_cairo_format_ptr'.
/// note: 'char[]' array decays to 'char*' here and matches as a c-string.
#define _cairo_format_str_type(_buffer, _value) _Generic((_value),             \
		char*:       _cairo_format_cstr,                                       \
		const char*: _cairo_format_cstr,                                       \
		default:     _cairo_format_ptr                                         \
	)((_buffer), sizeof(_buffer), (_value))

/// content-compares two cstrings for equality via 'cairo_strcmp', returning the
/// pointer identity, when either side is null so two nulls count as equal and a
/// null-vs-string as unequal (never dereferencing a null).
/// note: prefer it over '_eq' for strings, which compares addresses.
_cairo_func bool _cairo_cstr_streq(const char* const lhs,
								   const char* const rhs) {
	if ((cairo_null == lhs) || (cairo_null == rhs)) return lhs == rhs;
	return !cairo_strcmp(lhs, rhs);
}

/// it content-compares two cstrings for inequality by 'cairo_strcmp', returning
/// the pointer inequality when either side is null, so two nulls count as equal
/// and a null-vs-string as unequal (never dereferencing a null).
/// note: prefer it over '_neq' for strings, which compares addresses.
_cairo_func bool _cairo_cstr_strneq(const char* const lhs,
									const char* const rhs) {
	if ((cairo_null == lhs) || (cairo_null == rhs)) return lhs != rhs;
	return cairo_strcmp(lhs, rhs);
}

/// returns a monotonic-ish timestamp in seconds.
/// todo: expose clock functions for user to set in non standard environments.
/// fixme: posix clock()'s cpu time (blocking reads as free); windows clock() is
/// wall time at ~1-15ms resolution; both are poor and should be replaced with a
/// better approach.
_cairo_func double _cairo_time_now(void) {
	return (double)cairo_clock() / (double)cairo_clocks_per_sec;
}

/// formats a duration (in seconds) into a short, human-readable string, picking
/// microseconds, milliseconds, or seconds so the number stays readable.
/// note: returns a pointer to function-local static buffer which is not thread-
/// safe and will be overwritten by the next call.
_cairo_func const char* _cairo_format_time(const double time) {
	static char _[65];
	const size_t size = sizeof(_);
	if (time < 1e-3)     (void)cairo_snprintf(_, size, "%.3fus", time * 1e6);
	else if (time < 1.0) (void)cairo_snprintf(_, size, "%.3fms", time * 1e3);
	else                 (void)cairo_snprintf(_, size, "%.3fs" , time);
	return _;
}

/// matches provided 'text' against a glob 'pattern'. runs in linear time, using
/// a backtracking star/mark pair rather than recursion. it's used to select and
/// exclude tests by their 'suite.name'.
_cairo_func bool _cairo_get_glob(const char* pattern, const char* const end,
													  const char* text) {
	const char* star = cairo_null;
	const char* mark = cairo_null;

	while (*text != '\0') {
		if ((pattern < end) && (('?' == *pattern) || (*pattern == *text))) {
			++pattern;
			++text;
			continue;
		}

		if ((pattern < end) && ('*' == *pattern)) {
			star = pattern++;
			mark = text;
			continue;
		}

		if (star != cairo_null) {
			pattern = star + 1;
			text = ++mark;
			continue;
		}

		return false;
	}

	while ((pattern < end) && ('*' == *pattern)) ++pattern;
	return pattern == end;
}

/// matches 'text' against a ':'-separated list of glob patterns, returning true
/// as soon as any one segment matches (googletest-style). an empty segment (the
/// leading, trailing, or doubled ':') matches nothing. used to select or reject
/// tests by 'suite.name', e.g. "demo.test02:demo.test04:demo.test1*".
_cairo_func bool _cairo_get_globs(const char* pattern, const char* const text) {
	while (1) {
		const char* const segment = pattern;
		while ((*pattern != '\0') && (*pattern != ':')) ++pattern;
		if (_cairo_get_glob(segment, pattern, text)) return true ;
		if ('\0' == *pattern)                        return false;
		++pattern;  // note: skip the ':' and try the next segment.
	}
}

/// advances a 64-bit splitmix64 state and returns the next value. a small, self
/// contained, fully deterministic p-rng so a given seed yields the same shuffle
/// on every platform, without pulling in 'rand'/'srand' or any global state.
_cairo_func uint64_t _cairo_random_next(uint64_t* const state) {
	uint64_t value = (*state += 0x9E3779B97F4A7C15ull);
	value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
	value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
	return value ^ (value >> 31);
}

/// streams a test's source file for the verbose failure report. the file handle
/// is kept open across reports - 'path' records what 'file' was opened from and
/// only a different path reopens it.
typedef struct {
	const char* path      ;
	cairo_file* file      ;
	char        window[64];
	size_t      index     ;
	size_t      column    ;
	size_t      at        ;
	size_t      pending   ;
} _cairo_reader_s;

/// returns an idle reader with no source file open. use it to seed the handle a
/// reporting loop reuses across failures.
_cairo_func _cairo_reader_s _cairo_reader_new(void) {
	return (const _cairo_reader_s) {
		.path    = cairo_null,
		.file    = cairo_null,
		.window  = {0}       ,
		.index   = 0         ,
		.column  = 0         ,
		.at      = 0         ,
		.pending = 0         ,
	};
}

/// closes the reader's open source file, if any, and clears the cached path, so
/// the next '_cairo_reader_print' reopens. call once in the reporting loop that
/// owns the reader and is done with it.
_cairo_func void _cairo_reader_close(_cairo_reader_s* const reader) {
	if (reader->file != cairo_null) {
		(void)cairo_fclose(reader->file);
		reader->file = cairo_null;
		reader->path = cairo_null;
	}
}

/// returns the next raw byte of the source (0-255) with '\r' dropped, so '\r\n'
/// pair reads as a lone '\n', refilling 'window' via 'cairo_fgets' when it runs
/// dry, or -1 once the file is exhausted.
_cairo_func int _cairo_reader_get_byte(_cairo_reader_s* const reader) {
	while (1) {
		while (!reader->window[reader->index]) {
			if (cairo_null == cairo_fgets(
					reader->window, (int)sizeof(reader->window), reader->file
			)) {
				reader->window[0] = 0;
				reader->index     = 0;
				return -1;
			}

			reader->index = 0;
		}

		const int byte = (int)((unsigned char)reader->window[reader->index++]);
		if (byte != '\r') return byte;
	}
}

/// returns the next tab-expanded character of the source. a tab transforms into
/// spaces up to the next four-chars stop (emitted one at a time via 'pending'),
/// a newline returns '\n' and resets the column, and end of file returns -1.
_cairo_func int _cairo_reader_next_byte(_cairo_reader_s* const reader) {
	if (reader->pending > 0) {
		--reader->pending;
		reader->at = reader->column++;
		return ' ';
	}

	const int byte = _cairo_reader_get_byte(reader);
	if (byte < 0) return -1;
	if ('\n' == byte) { reader->column = 0; return '\n'; }

	if ('\t' == byte) {
		const size_t tab_width = 4                                       ;
		const size_t pad       = tab_width - (reader->column % tab_width);
		reader->pending        = pad - 1                                 ;
		reader->at             = reader->column++                        ;
		return ' ';
	}

	reader->at = reader->column++;
	return byte;
}

/// displays the source around 'line' of 'path' for a verbose failure report and
/// underlines the asserted expression with a '^~~~' run, following it across as
/// many lines as it spans.
_cairo_func void _cairo_reader_print(_cairo_reader_s* const reader,
									 const char* const path,
									 const size_t line) {
	if ((cairo_null == reader->file) || (cairo_null == reader->path) ||
		cairo_strcmp(reader->path, path)) {
		if (reader->file != cairo_null) (void)cairo_fclose(reader->file);
		reader->file = cairo_fopen(path, "rt");
		reader->path = path;
	}
	
	if (cairo_null == reader->file) return;
	cairo_rewind(reader->file);
	reader->window[0] = '\0'                                         ;
	reader->index = reader->column = reader->at = reader->pending = 0;

	const size_t target = line                       ;
	const size_t first  = target > 1 ? target - 1 : 1;

	size_t no = 1                              ;
	int    c  = _cairo_reader_next_byte(reader);
	while ((no < first) && (c != -1)) {
		if ('\n' == c) ++no                ;
		c = _cairo_reader_next_byte(reader);
	}

	#define _cairo_call_len 7
	int    depth    = 0                   ;
	char   quote    = '\0'                ;
	bool   escaped  = false               ;
	bool   started  = false               ;
	bool   done     = false               ;
	bool   anchored = false               ;
	char   tail[_cairo_call_len + 1] = {0};
	size_t fill     = 0                   ;
	int    trailing = 0                   ;

	while (c != -1) {
		(void)cairo_printf("  %4zu | ", no);
		const bool scanning = no >= target && !done  ;
		size_t mark_start   = 0                      ;
		size_t trim_end     = 0                      ;
		size_t mark_end     = 0                      ;
		bool   want_start   = scanning && started    ;
		bool   active       = scanning               ;

		while ((c != -1) && ('\n' != c)) {
			(void)cairo_putchar(c);
			const size_t at = reader->at;

			if (active) {
				if ((no == target) && !started && !anchored) {
					if (fill < _cairo_call_len) tail[fill++] = (char)c;
					else {
						for (size_t k = 1; k < _cairo_call_len; ++k) {
							tail[k - 1] = tail[k];
						}
						tail[_cairo_call_len - 1] = (char)c;
					}
					tail[fill] = '\0';
					if ((_cairo_call_len == fill) &&
						(!cairo_strcmp(tail, "assert_") ||
						 !cairo_strcmp(tail, "expect_"))) {
						anchored = true;
					}
				}
				else {
					bool structure = false;
					if (quote != '\0') {
						if (escaped)         escaped = false;
						else if ('\\' == c)  escaped = true ;
						else if (c == quote) quote   = '\0' ;
					}
					else if (('\"' == c) || ('\'' == c)) quote = (char)c;
					else if ('(' == c) {
						if (!started) {
							started    = true;
							want_start = true;
							structure  = true;
						}
						++depth;
					}
					else if (')' == c) {
						if (0 == --depth) {
							mark_end  = trim_end;
							done      = true    ;
							active    = false   ;
							structure = true    ;
						}
					}

					if (started && !structure && (c != ' ')) {
						if (want_start) { mark_start = at; want_start = false; }
						trim_end = at + 1;
					}
				}
			}

			c = _cairo_reader_next_byte(reader);
		}

		(void)cairo_putchar('\n');
		if (scanning && !done) mark_end = trim_end;

		if (mark_end > mark_start) {
			(void)cairo_printf("       | ");
			for (size_t _ = 0; _ < mark_start; ++_) (void)cairo_putchar(' ');
			(void)cairo_putchar('^');
			const size_t offset = mark_start + 1;
			for (size_t _ = offset; _ < mark_end; ++_) (void)cairo_putchar('~');
			(void)cairo_putchar('\n');
		}

		if (done && (trailing++ > 0)) break;
		if (-1 == c)                  break;
		c = _cairo_reader_next_byte(reader);
		++no                               ;
	}
}

/// outcome/status of a single test: failed, passed, or explicitly skipped.
typedef enum _cairo_test_status_e {
	_cairo_test_status_none,
	_cairo_test_status_fail,
	_cairo_test_status_pass,
	_cairo_test_status_skip,
} _cairo_test_status_e;

/// recorded details of failed assertions: an operator and stringified operands,
/// the source location, and the two operand values already formatted to strings
/// for reporting.
typedef struct _cairo_test_fail_s {
	const char* lhs     ;
	const char* rhs     ;
	const char* op      ;
	const char* file    ;
	size_t      line    ;
	char        lhsb[65];
	char        rhsb[65];
} _cairo_test_fail_s;

/// a registered test that holds the test function, its suite/name identity, the
/// status and failure record, the measured elapsed time, and whether the filter
/// selected it to run.
struct _cairo_test_s {
	void(*func)(_cairo_test_s* const)  ;
	const char*          suite         ;
	const char*          name          ;
	const char*          suite_and_name;
	_cairo_test_status_e status        ;
	bool                 result        ;
	_cairo_test_fail_s   fail          ;
	bool                 should_fail   ;
	double               elapsed       ;
	bool                 shall_run     ;
};

/// builds the internal symbol name for a test function from suite and name.
#define _f(_suite, _name)                                                      \
	_suite ## _ ## _name ## _func

/// builds the internal symbol name for a test instance from suite and name.
#define _t(_suite, _name)                                                      \
	_ ## _suite ## _ ## _name ## _test

/// builds the internal symbol name for a test reference from suite and name.
#define _r(_suite, _name)                                                      \
	_ ## _suite ## _ ## _name ## _ref

/// places the cairo test '_name' reference into the cairo section and points it
/// at the '_value' that is a test object which makes the test discoverable by a
/// cairo runner.
#define _cairo_test_ref(_name, _value)                                         \
	_cairo_section static _cairo_test_s* _name = _value

/// it forward-declares the test function, its '_cairo_test_s' test object, that
/// is initialized to pass, registers that object in the section, and then opens
/// the body of the test function for you to fill in. inside the body the hidden
/// '_test' handle carries status and failure info which is needed by the assert
/// macros to reference. should be used like a function header:
/// cairo_test_new(demo1, test01) {
///     cairo_assert_eq(2 + 2, 4);
/// }
#define cairo_test_new(_suite, _name)                                          \
	static void _f(_suite, _name)(_cairo_unused _cairo_test_s* const _test);   \
	                                                                           \
	static _cairo_test_s _t(_suite, _name) = {                                 \
		.func           = _f(_suite, _name)      ,                             \
		.suite          = #_suite                ,                             \
		.name           = #_name                 ,                             \
		.suite_and_name = #_suite "." #_name     ,                             \
		.status         = _cairo_test_status_none,                             \
		.result         = false                  ,                             \
		.fail = {                                                              \
			.lhs  = cairo_null,                                                \
			.rhs  = cairo_null,                                                \
			.op   = cairo_null,                                                \
			.file = cairo_null,                                                \
			.line = (size_t)0 ,                                                \
			.lhsb = {0}       ,                                                \
			.rhsb = {0}       ,                                                \
		},                                                                     \
		.should_fail = false,                                                  \
		.elapsed     = 0.0  ,                                                  \
		.shall_run   = false,                                                  \
	};                                                                         \
	                                                                           \
	_cairo_test_ref(_r(_suite, _name), &_t(_suite, _name));                    \
	                                                                           \
	static void _f(_suite, _name)(_cairo_unused _cairo_test_s* const _test)

/// runs one test: resets its status to pass, times the call to its body, stores
/// the elapsed duration. the test's body sets its status to fail or skip by the
/// the cairo_assert*/cairo_test_skip macros.
_cairo_func void _cairo_test_run(_cairo_test_s* const test) {
	test->status = _cairo_test_status_pass;
	const double begin = _cairo_time_now();
	test->func(test);
	test->elapsed = _cairo_time_now() - begin;
}

/// return whether a finished test counts as a pass, takign into the account its
/// should_fail flag.
_cairo_func bool _cairo_test_has_passed(_cairo_test_s* const test) {
	const bool passed = (( test->result && !test->should_fail) ||
						 (!test->result &&  test->should_fail));
	test->should_fail = false;
	return passed;
}

/// qsort comparator over '_cairo_test_s*' which orders tests by suite and name,
/// so the output is grouped and stable.
/// note: null test references exist because the tests section is populated with
/// such values by certain toolchains to handle padding. cairo does so too, when
/// no tests are defined, certain toolchains fail with empty linker section - we
/// create an empty, not-runnable dummy test refernce to a null test to make the
/// linker happy.
_cairo_func int _cairo_test_compare(const void* const a, const void* const b) {
	const _cairo_test_s* const lhs = *(const _cairo_test_s* const*)a;
	const _cairo_test_s* const rhs = *(const _cairo_test_s* const*)b;
	if (cairo_null == lhs) return cairo_null == rhs ? 0 : 1;
	if (cairo_null == rhs) return -1;
	return cairo_strcmp(lhs->suite_and_name, rhs->suite_and_name);
}

/// returns whether a test's 'suite.name' matches a provided, ':'-separated glob
/// pattern list (via '_cairo_get_globs'); true when any one segment matches.
_cairo_func bool _cairo_test_match(const _cairo_test_s* const test,
								   const char* const pattern) {
	return _cairo_get_globs(pattern, test->suite_and_name);
}

/// reports a single failed 'test'. the header - 'file:line: suite.name failed:'
/// followed by the operands and operator, or the stringified expression for the
/// unary assert - is printed either way. when 'verbose' is set it also displays
/// the offending source through 'reader' (underlining the expression), then the
/// formatted 'left'/'right' values and the elapsed time.
_cairo_func void _cairo_test_report(const _cairo_test_s* const test,
									_cairo_reader_s* const reader,
									const bool verbose) {
	if (test->fail.op != cairo_null) {
		(void)cairo_printf("\n%s:%zu: %s failed: %s %s %s\n",
			test->fail.file, test->fail.line, test->suite_and_name,
			test->fail.lhs, test->fail.op, test->fail.rhs);
	}
	else {
		(void)cairo_printf("\n%s:%zu: %s failed: %s\n",
			test->fail.file, test->fail.line, test->suite_and_name,
			test->fail.lhs != cairo_null ? test->fail.lhs : "assertion failed");
	}

	if (!verbose) return;
	_cairo_reader_print(reader, test->fail.file, test->fail.line);

	(void)cairo_printf("       |\n");
	if (test->fail.op != cairo_null) {
		(void)cairo_printf("       = left:  %s\n", test->fail.lhsb);
		(void)cairo_printf("       = right: %s\n", test->fail.rhsb);
	}
	(void)cairo_printf("       (%s)\n", _cairo_format_time(test->elapsed));
}

/// skips the current test from inside its body. only valid inside a test's body
/// (it uses the hidden '_test' handle and returns from the test function).
#define cairo_test_skip() do {                                                 \
		_test->status = _cairo_test_status_skip;                               \
		return;                                                                \
	} while (0)

/// records a failure into the '_test' handle: sets the stringified operands and
/// operator and source location, and clears value buffers.
#define _cairo_record_fail(_lhs, _rhs, _op, _file, _line) do {                 \
		_test->status    = _cairo_test_status_fail;                            \
		_test->fail.lhs  = _lhs                   ;                            \
		_test->fail.rhs  = _rhs                   ;                            \
		_test->fail.op   = _op                    ;                            \
		_test->fail.file = _file                  ;                            \
		_test->fail.line = (size_t)(_line)        ;                            \
	} while (0)

/// evaluates '_expr', then on a falsy result it records the stringified '_expr'
/// as the failure and runs the _on_fail action.
#define _cairo_check_unary(_expr, _on_fail) do {                               \
		_test->result = (bool)(_expr);                                         \
		                                                                       \
		if (!_cairo_test_has_passed(_test)) {                                  \
			_cairo_record_fail(                                                \
				#_expr, cairo_null, cairo_null, __FILE__, __LINE__);           \
			_on_fail;                                                          \
		}                                                                      \
	} while (0)

/// evaluates '_lhs' '_op' '_rhs', when that is false it records the stringified
/// operands, the operator, and the source location, formats both operand values
/// into the failure buffers, and runs the _on_fail action.
#define _cairo_check_binary(_lhs, _rhs, _op, _on_fail) do {                    \
		_cairo_diag_nosign({                                                   \
			_test->result = (bool)(                                            \
				_cairo_format_non_str_type(_test->fail.lhsb, (_lhs)) _op       \
				_cairo_format_non_str_type(_test->fail.rhsb, (_rhs))           \
			);                                                                 \
		});                                                                    \
		                                                                       \
		if (!_cairo_test_has_passed(_test)) {                                  \
			_cairo_record_fail(#_lhs, #_rhs, #_op, __FILE__, __LINE__);        \
			_on_fail;                                                          \
		}                                                                      \
	} while (0)

/// evaluates as pass when '_lhs' and '_rhs' are within '_epsilon' of each other
/// (|lhs - rhs| <= epsilon, computed in double). on failure it records operands
/// with the '~=' operator, formats '_lhs' and '_rhs' values into the respective
/// failure buffers, and runs the _on_fail action.
/// note: prefer these over '_eq'/'_neq' for real numbers.
#define _cairo_check_loosely(_lhs, _rhs, _epsilon, _op, _op_str, _on_fail) do {\
		_test->result = (bool)(cairo_fabs(                                     \
			(double)_cairo_format_non_str_type(_test->fail.lhsb, (_lhs)) -     \
			(double)_cairo_format_non_str_type(_test->fail.rhsb, (_rhs))       \
		) _op (double)(_epsilon));                                             \
		                                                                       \
		if (!_cairo_test_has_passed(_test)) {                                  \
			_cairo_record_fail(#_lhs, #_rhs, _op_str, __FILE__, __LINE__);     \
			_on_fail;                                                          \
		}                                                                      \
	} while (0)

/// evaluates a cstring comparison by pasting '_op' onto '_cairo_cstr_', to pick
/// the '_streq'/'_strneq' helper, running it on both operands, after formatting
/// each into the failure buffers. on a false result, it records the stringified
/// operands with '_op' as the reported operator, and fail source location, then
/// runs the _on_fail action.
#define _cairo_check_strings(_lhs, _rhs, _op, _on_fail) do {                   \
		_test->result = (bool)(_cairo_cstr_ ## _op(                            \
			_cairo_format_str_type(_test->fail.lhsb, (_lhs)),                  \
			_cairo_format_str_type(_test->fail.rhsb, (_rhs))                   \
		));                                                                    \
		                                                                       \
		if (!_cairo_test_has_passed(_test)) {                                  \
			_cairo_record_fail(#_lhs, #_rhs, #_op, __FILE__, __LINE__);        \
			_on_fail;                                                          \
		}                                                                      \
	} while (0)

/// public assertion, for use inside a test body. on failure, assert records the
/// location and operands and aborts the rest of its test.
/// note: cairo_assert_true(x) - x is truthy.
#define cairo_assert_true(_expr) do {                                          \
		_cairo_check_unary(_expr, return);                                     \
	} while (0)

/// public assertion, for use inside a test body. on failure, assert records the
/// location and operands and aborts the rest of its test.
/// note: cairo_assert_false(x) - x is falsy.
#define cairo_assert_false(_expr) do {                                         \
		_cairo_check_unary(!(_expr), return);                                  \
	} while (0)

/// public assertion, for use inside a test body. on failure, assert records the
/// location and operands and aborts the rest of its test.
/// note: cairo_assert_eq(lhs, rhs) - lhs == rhs.
#define cairo_assert_eq(_lhs, _rhs) do {                                       \
		_cairo_check_binary(_lhs, _rhs, ==, return);                           \
	} while (0)

/// public assertion, for use inside a test body. on failure, assert records the
/// location and operands and aborts the rest of its test.
/// note: cairo_assert_neq(lhs, rhs) - lhs != rhs.
#define cairo_assert_neq(_lhs, _rhs) do {                                      \
		_cairo_check_binary(_lhs, _rhs, !=, return);                           \
	} while (0)

/// public assertion, for use inside a test body. on failure, assert records the
/// location and operands and aborts the rest of its test.
/// note: cairo_assert_gt(lhs, rhs) - lhs > rhs.
#define cairo_assert_gt(_lhs, _rhs) do {                                       \
		_cairo_check_binary(_lhs, _rhs, >, return);                            \
	} while (0)

/// public assertion, for use inside a test body. on failure, assert records the
/// location and operands and aborts the rest of its test.
/// note: cairo_assert_ge(lhs, rhs) - lhs >= rhs.
#define cairo_assert_ge(_lhs, _rhs) do {                                       \
		_cairo_check_binary(_lhs, _rhs, >=, return);                           \
	} while (0)

/// public assertion, for use inside a test body. on failure, assert records the
/// location and operands and aborts the rest of its test.
/// note: cairo_assert_lt(lhs, rhs) - lhs < rhs.
#define cairo_assert_lt(_lhs, _rhs) do {                                       \
		_cairo_check_binary(_lhs, _rhs, <, return);                            \
	} while (0)

/// public assertion, for use inside a test body. on failure, assert records the
/// location and operands and aborts the rest of its test.
/// note: cairo_assert_le(lhs, rhs) - lhs <= rhs.
#define cairo_assert_le(_lhs, _rhs) do {                                       \
		_cairo_check_binary(_lhs, _rhs, <=, return);                           \
	} while (0)

/// public assertion, for use inside a test body. on failure, assert records the
/// location and operands and aborts the rest of its test.
/// note: cairo_assert_nr(lhs, rhs epsilon) - lhs and rhs are within epsilon.
/// note: use the '_nr'/'_nreq' forms for floating-point comparisons; '_eq' form
/// on floats compares bit-exactly and will surprise you.
#define cairo_assert_nr(_lhs, _rhs, _epsilon) do {                             \
		_cairo_check_loosely(_lhs, _rhs, _epsilon, <=, "~=", return);          \
	} while (0)

/// public assertion, for use inside a test body. on failure, assert records the
/// location and operands and aborts the rest of its test.
/// note: cairo_assert_nnr(lhs, rhs, epsilon) - lhs and rhs diverge by more than
/// epsilon (|lhs - rhs| > epsilon).
/// note: the '_nnr'/'_nnreq' forms are the inverse of the '_nr'/'_nreq'; prefer
/// them over '_neq' for floating-point inequality checks.
#define cairo_assert_nnr(_lhs, _rhs, _epsilon) do {                            \
		_cairo_check_loosely(_lhs, _rhs, _epsilon, >, "~/=", return);          \
	} while (0)

/// public assertion, for use inside a test body. on failure, assert records the
/// location and operands and aborts the rest of its test.
/// note: cairo_assert_nreq(lhs, rhs) - lhs and rhs are within 1e-6 epsilon.
/// note: use the '_nr'/'_nreq' forms for floating-point comparisons; '_eq' form
/// on floats compares bit-exactly and will surprise you.
#define cairo_assert_nreq(_lhs, _rhs) do {                                     \
		_cairo_check_loosely(_lhs, _rhs, 1e-6, <=, "~=", return);              \
	} while (0)

/// public assertion, for use inside a test body. on failure, assert records the
/// location and operands and aborts the rest of its test.
/// note: cairo_assert_nnreq(lhs, rhs) - lhs and rhs diverge by more than a 1e-6
/// epsilon (|lhs - rhs| > 1e-6).
/// note: the '_nnr'/'_nnreq' forms are the inverse of the '_nr'/'_nreq'; prefer
/// them over '_neq' for floating-point inequality.
#define cairo_assert_nnreq(_lhs, _rhs) do {                                    \
		_cairo_check_loosely(_lhs, _rhs, 1e-6, >, "~/=", return);              \
	} while (0)

/// public assertion, for use inside a test body. on failure, assert records the
/// location and operands and aborts the rest of its test.
/// note: cairo_assert_streq(lhs, rhs) - lhs and rhs have equal string content.
/// note: use the '_streq'/'_strneq' forms for c-strings; the '_eq'/'_neq' forms
/// compare pointers, not content, and will surprise you.
#define cairo_assert_streq(_lhs, _rhs) do {                                    \
		_cairo_check_strings(_lhs, _rhs, streq, return);                       \
	} while (0)

/// public assertion, for use inside a test body. on failure, assert records the
/// location and operands and aborts the rest of its test.
/// note: cairo_assert_strneq(lhs, rhs) - lhs and rhs differ in string content.
/// note: use the '_streq'/'_strneq' forms for c-strings; the '_eq'/'_neq' forms
/// compare pointers, not content, and will surprise you.
#define cairo_assert_strneq(_lhs, _rhs) do {                                   \
		_cairo_check_strings(_lhs, _rhs, strneq, return);                      \
	} while (0)

/// internal spelling of the cleanup-epilogue label that 'cairo_expect_*' family
/// jumps to on failure. aliased to the user-facing 'cairo_test_end' so both the
/// jump and the 'cairo_test_defered' label resolve to the same name.
#define _cairo_test_end cairo_test_end

/// public assertion, for use inside a test body. on failure, jumps (via a goto)
/// to '_cairo_test_end' instead of returning.
/// note: cairo_expect_true(x) - x is truthy.
/// note: a test that uses any cairo_expect_* must provide 'cairo_test_defered'.
#define cairo_expect_true(_expr) do {                                          \
		_cairo_check_unary(_expr, goto _cairo_test_end);                       \
	} while (0)

/// public assertion, for use inside a test body. on failure, jumps (via a goto)
/// to '_cairo_test_end' instead of returning.
/// note: cairo_expect_false(x) - x is falsy.
/// note: a test that uses any cairo_expect_* must provide 'cairo_test_defered'.
#define cairo_expect_false(_expr) do {                                         \
		_cairo_check_unary(!(_expr), goto _cairo_test_end);                    \
	} while (0)

/// public assertion, for use inside a test body. on failure, jumps (via a goto)
/// to '_cairo_test_end' instead of returning.
/// note: cairo_expect_eq(lhs, rhs) - lhs == rhs.
/// note: a test that uses any cairo_expect_* must provide 'cairo_test_defered'.
#define cairo_expect_eq(_lhs, _rhs) do {                                       \
		_cairo_check_binary(_lhs, _rhs, ==, goto _cairo_test_end);             \
	} while (0)

/// public assertion, for use inside a test body. on failure, jumps (via a goto)
/// to '_cairo_test_end' instead of returning.
/// note: cairo_expect_neq(lhs, rhs) - lhs != rhs.
/// note: a test that uses any cairo_expect_* must provide 'cairo_test_defered'.
#define cairo_expect_neq(_lhs, _rhs) do {                                      \
		_cairo_check_binary(_lhs, _rhs, !=, goto _cairo_test_end);             \
	} while (0)

/// public assertion, for use inside a test body. on failure, jumps (via a goto)
/// to '_cairo_test_end' instead of returning.
/// note: cairo_expect_gt(lhs, rhs) - lhs > rhs.
/// note: a test that uses any cairo_expect_* must provide 'cairo_test_defered'.
#define cairo_expect_gt(_lhs, _rhs) do {                                       \
		_cairo_check_binary(_lhs, _rhs, >, goto _cairo_test_end);              \
	} while (0)

/// public assertion, for use inside a test body. on failure, jumps (via a goto)
/// to '_cairo_test_end' instead of returning.
/// note: cairo_expect_ge(lhs, rhs) - lhs >= rhs.
/// note: a test that uses any cairo_expect_* must provide 'cairo_test_defered'.
#define cairo_expect_ge(_lhs, _rhs) do {                                       \
		_cairo_check_binary(_lhs, _rhs, >=, goto _cairo_test_end);             \
	} while (0)

/// public assertion, for use inside a test body. on failure, jumps (via a goto)
/// to '_cairo_test_end' instead of returning.
/// note: cairo_expect_lt(lhs, rhs) - lhs < rhs.
/// note: a test that uses any cairo_expect_* must provide 'cairo_test_defered'.
#define cairo_expect_lt(_lhs, _rhs) do {                                       \
		_cairo_check_binary(_lhs, _rhs, <, goto _cairo_test_end);              \
	} while (0)

/// public assertion, for use inside a test body. on failure, jumps (via a goto)
/// to '_cairo_test_end' instead of returning.
/// note: cairo_expect_le(lhs, rhs) - lhs <= rhs.
/// note: a test that uses any cairo_expect_* must provide 'cairo_test_defered'.
#define cairo_expect_le(_lhs, _rhs) do {                                       \
		_cairo_check_binary(_lhs, _rhs, <=, goto _cairo_test_end);             \
	} while (0)

/// public assertion, for use inside a test body. on failure, jumps (via a goto)
/// to '_cairo_test_end' instead of returning.
/// note: cairo_expect_nr(lhs, rhs epsilon) - lhs and rhs are within epsilon.
/// note: use the '_nr'/'_nreq' forms for floating-point comparisons; '_eq' form
/// on floats compares bit-exactly and will surprise you.
/// note: a test that uses any cairo_expect_* must provide 'cairo_test_defered'.
#define cairo_expect_nr(_lhs, _rhs, _epsilon) do {                             \
		_cairo_check_loosely(                                                  \
			_lhs, _rhs, _epsilon, <=, "~=", goto _cairo_test_end               \
		);                                                                     \
	} while (0)

/// public assertion, for use inside a test body. on failure, jumps (via a goto)
/// to '_cairo_test_end' instead of returning.
/// note: cairo_expect_nnr(lhs, rhs, epsilon) - lhs and rhs diverge by more than
/// epsilon (|lhs - rhs| > epsilon).
/// note: the '_nnr'/'_nnreq' forms are the inverse of the '_nr'/'_nreq'; prefer
/// them over '_neq' for floating-point inequality.
/// note: a test that uses any cairo_expect_* must provide 'cairo_test_defered'.
#define cairo_expect_nnr(_lhs, _rhs, _epsilon) do {                            \
		_cairo_check_loosely(                                                  \
			_lhs, _rhs, _epsilon, >, "~/=", goto _cairo_test_end               \
		);                                                                     \
	} while (0)

/// public assertion, for use inside a test body. on failure, jumps (via a goto)
/// to '_cairo_test_end' instead of returning.
/// note: cairo_expect_nreq(lhs, rhs) - lhs and rhs are within 1e-6 epsilon.
/// note: use the '_nr'/'_nreq' forms for floating-point comparisons; '_eq' form
/// on floats compares bit-exactly and will surprise you.
/// note: a test that uses any cairo_expect_* must provide 'cairo_test_defered'.
#define cairo_expect_nreq(_lhs, _rhs) do {                                     \
		_cairo_check_loosely(                                                  \
			_lhs, _rhs, 1e-6, <=, "~=", goto _cairo_test_end                   \
		);                                                                     \
	} while (0)

/// public assertion, for use inside a test body. on failure, jumps (via a goto)
/// to '_cairo_test_end' instead of returning.
/// note: cairo_expect_nnreq(lhs, rhs) - lhs and rhs diverge by more than a 1e-6
/// epsilon (|lhs - rhs| > 1e-6).
/// note: the '_nnr'/'_nnreq' forms are the inverse of the '_nr'/'_nreq'; prefer
/// them over '_neq' for floating-point inequality.
/// note: a test that uses any cairo_expect_* must provide 'cairo_test_defered'.
#define cairo_expect_nnreq(_lhs, _rhs) do {                                    \
		_cairo_check_loosely(                                                  \
			_lhs, _rhs, 1e-6, >, "~/=", goto _cairo_test_end                   \
		);                                                                     \
	} while (0)

/// public assertion, for use inside a test body. on failure, jumps (via a goto)
/// to '_cairo_test_end' instead of returning.
/// note: cairo_expect_streq(lhs, rhs) - lhs and rhs have equal string content.
/// note: use the '_streq'/'_strneq' forms for c-strings; the '_eq'/'_neq' forms
/// compare pointers, not content, and will surprise you.
/// note: a test that uses any cairo_expect_* must provide 'cairo_test_defered'.
#define cairo_expect_streq(_lhs, _rhs) do {                                    \
		_cairo_check_strings(_lhs, _rhs, streq, goto _cairo_test_end);         \
	} while (0)

/// public assertion, for use inside a test body. on failure, jumps (via a goto)
/// to '_cairo_test_end' instead of returning.
/// note: cairo_expect_strneq(lhs, rhs) - lhs and rhs differ in string content.
/// note: use the '_streq'/'_strneq' forms for c-strings; the '_eq'/'_neq' forms
/// compare pointers, not content, and will surprise you.
/// note: a test that uses any cairo_expect_* must provide 'cairo_test_defered'.
#define cairo_expect_strneq(_lhs, _rhs) do {                                   \
		_cairo_check_strings(_lhs, _rhs, strneq, goto _cairo_test_end);        \
	} while (0)

/// defines a defer section of a test. any tests that use 'cairo_expect_*', must
/// end with this or the jump has no label to target.
#define cairo_test_defered _cairo_test_end: (void)0;

/// run a block in which failure is the expected outcome. sets should_fail on an
/// enclosing test, executes the wrapped statement(s). usage example:
///     cairo_should_fail(cairo_assert_true(false));
/// note: the should_fail flag is cleared by the _cairo_test_has_passed function
/// to ensure that the flag gets cleared regardless of the test result.
/// note: in case a non-cairo mechanism's used inside the cairo_should_fail call
/// the should_fail flag is cleared by the cairo_should_fail itself to ensure it
/// is never a case of a test having should_fail left set to true. 
#define cairo_should_fail(...) do {                                            \
		_test->should_fail = true;                                             \
		__VA_ARGS__;                                                           \
		_test->should_fail = false;                                            \
	} while (0)

/// parse-status of the command line and ordered so the enumerator value doubles
/// as an exit code: 'exit' for a clean, early stop, 'go_on' to keep the program
/// running, 'error' for a usage error.
/// note: keep the order, 'cairo_tests_run' returns '(int)args._status' directly
/// for the non-'go_on' cases, so reordering silently changes exit code mapping.
typedef enum _cairo_args_status_e {
	_cairo_args_status_exit ,
	_cairo_args_status_go_on,
	_cairo_args_status_error,
} _cairo_args_status_e;

/// parsed command-line options configuring a tests run: which tests to include,
/// which to exclude, how many times to repeat the tests set, and what exit code
/// to use when tests fail. the 'status' reports how parsing ended.
typedef struct cairo_args_s {
	_cairo_args_status_e _status;
	const char*          program;
	const char*          include;
	const char*          exclude;
	size_t               shuffle;
	size_t               repeat ;
	bool                 verbose;
	bool                 rerun  ;
	size_t               ecode  ;
	bool                 list   ;
} cairo_args_s;

/// returns whether the provided arg equals either the brief (e.g. '-e') or full
/// (e.g. '--example') spelling of an option.
_cairo_func bool _cairo_args_is(const char* const arg, const char* const brief,
													   const char* const full) {
	return !cairo_strcmp(arg, brief) ? true : !cairo_strcmp(arg, full);
}

/// parses 'arg' as a base10 number, returning false on empty input, non-digits,
/// overflow, or trailing garbage. used for the numeric options.
_cairo_func bool _cairo_args_number(const char* const arg, size_t* const out) {
	if ((cairo_null == arg) || ('\0' == *arg)) return false;
	for (const char* digit = arg; *digit != '\0'; ++digit) {
		if ((*digit < '0') || (*digit > '9')) return false;
	}

	char* end = cairo_null; cairo_errno = 0;
	const unsigned long value = cairo_strtoul(arg, &end, 10);
	if (cairo_erange == cairo_errno)            return false;
	if (*end != '\0')                           return false;
#if cairo_size_max < cairo_ulong_max
	if (value > (unsigned long)cairo_size_max)  return false;
#endif
	*out = (size_t)value;
	return true;
}

/// prints the usage/help text for 'program' to the provided 'stream'.
_cairo_func void _cairo_args_usage(cairo_file* const stream,
								   const char* const program) {
	(void)cairo_fprintf(stream,
		"usage: %s [options]\n"
		"  -i, --include <glob>  run only tests whose suite.name matches.\n"
		"  -e, --exclude <glob>  skip tests whose suite.name matches.\n"
		"  -s, --shuffle <seed>  deterministic shuffle with a provided seed.\n"
		"  -r, --repeat <n>      run selected tests n times.\n"
		"  -V, --verbose         enable/disable verbose output.\n"
		"  -R, --rerun           print full command to rerun failed tests.\n"
		"  -x, --ecode <n>       exit code to use when tests fail.\n"
		"  -l, --list            print all collected tests and exit.\n"
		"  -v, --version         print the version of the cairo.\n"
		"  -h, --help            print this message and exit.\n"
		"\n"
		"globs accept *, ?, and : and are matched against \'suite.name\'.\n",
		program
	);
}

/// prints cairo's version string for 'program' to the provided 'stream', in the
/// form "<program> <version>" (e.g. after the '-v'/'--version' flag).
_cairo_func void _cairo_args_version(cairo_file* const stream,
									 const char* const program) {
	(void)cairo_fprintf(stream, "%s v%s\n", program, _cairo_version);
}

/// prints a one-liner error ('reason', offending 'option', optional 'value') to
/// cairo_stderr, followed by the usage text.
_cairo_func void _cairo_args_report(const char* const program,
									const char* const reason,
									const char* const option,
									const char* const value) {
	(void)cairo_fprintf(cairo_stderr, "%s: %s '%s'.", program, reason, option);
	if (value != cairo_null) (void)cairo_fprintf(cairo_stderr, ": '%s'", value);
	(void)cairo_fprintf(cairo_stderr, "\n");
	_cairo_args_usage(cairo_stderr, program);
}

/// returns the default options: run everything, exclude nothing, do not repeat.
/// use this as a starting point to define custom options or pass it straight to
/// 'cairo_tests_run' to run with defaults.
_cairo_func cairo_args_s cairo_args_default(void) {
	return (const cairo_args_s) {
		._status = _cairo_args_status_go_on,
		.program = cairo_null              ,
		.include = "*"                     ,
		.exclude = ""                      ,
		.shuffle = 0                       ,
		.repeat  = 1                       ,
		.verbose = false                   ,
		.rerun   = false                   ,
		.ecode   = 1                       ,
		.list    = false                   ,
	};
}

/// parses 'argc'/'argv' into a 'cairo_args_s', starting from the defaults while
/// following the usage definition for available and allowed options. here is an
/// easy way to use it as follows:
/// int main(const int argc, const char* argv[]) {
///     return cairo_tests_run(cairo_args_new(argc, argv));
/// }
_cairo_func cairo_args_s cairo_args_new(const int argc, const char** argv) {
	size_t       index   = 0                   ;
	const size_t count   = (size_t)argc        ;
	cairo_args_s args    = cairo_args_default();
	args.program         = argv[index++]       ;

	for (; index < count; ++index) {
		const char* const arg  = argv[index]                               ;
		const size_t nindex    = index + 1                                 ;
		const char* const next = nindex < count ? argv[nindex] : cairo_null;

		if (_cairo_args_is(arg, "-h", "--help")) {
			_cairo_args_usage(cairo_stdout, args.program);
			args._status = _cairo_args_status_exit;
			break;
		}

		if (_cairo_args_is(arg, "-v", "--version")) {
			_cairo_args_version(cairo_stdout, args.program);
			args._status = _cairo_args_status_exit;
			break;
		}

		if (_cairo_args_is(arg, "-i", "--include")) {
			if (cairo_null == next) {
				_cairo_args_report(
					args.program, "missing value for", arg, cairo_null);
				args._status = _cairo_args_status_error;
				break;
			}

			args.include = next;
			++index;
		}
		else if (_cairo_args_is(arg, "-e", "--exclude")) {
			if (cairo_null == next) {
				_cairo_args_report(
					args.program, "missing value for", arg, cairo_null);
				args._status = _cairo_args_status_error;
				break;
			}

			args.exclude = next;
			++index;
		}
		else if (_cairo_args_is(arg, "-s", "--shuffle")) {
			if (cairo_null == next) {
				_cairo_args_report(
					args.program, "missing value for", arg, cairo_null);
				args._status = _cairo_args_status_error;
				break;
			}

			if (!_cairo_args_number(next, &args.shuffle) || !args.shuffle) {
				_cairo_args_report(
					args.program, "expected a positive number for", arg, next);
				args._status = _cairo_args_status_error;
				break;
			}

			++index;
		}
		else if (_cairo_args_is(arg, "-r", "--repeat")) {
			if (cairo_null == next) {
				_cairo_args_report(
					args.program, "missing value for", arg, cairo_null);
				args._status = _cairo_args_status_error;
				break;
			}

			if (!_cairo_args_number(next, &args.repeat) || !args.repeat) {
				_cairo_args_report(
					args.program, "expected a positive number for", arg, next);
				args._status = _cairo_args_status_error;
				break;
			}

			++index;
		}
		else if (_cairo_args_is(arg, "-V", "--verbose")) {
			args.verbose = true;
		}
		else if (_cairo_args_is(arg, "-R", "--rerun")) {
			args.rerun = true;
		}
		else if (_cairo_args_is(arg, "-x", "--ecode")) {
			if (cairo_null == next) {
				_cairo_args_report(
					args.program, "missing value for", arg, cairo_null);
				args._status = _cairo_args_status_error;
				break;
			}

			if (!_cairo_args_number(next, &args.ecode) || !args.ecode) {
				_cairo_args_report(
					args.program, "expected a positive number for", arg, next);
				args._status = _cairo_args_status_error;
				break;
			}

			++index;
		}
		else if (_cairo_args_is(arg, "-l", "--list")) {
			args.list = true;
		}
		else {
			_cairo_args_report(args.program, "unknown option", arg, cairo_null);
			args._status = _cairo_args_status_error;
			break;
		}
	}

	return args;
}

// todo: document!
_cairo_func void _cairo_args_print_begin(const cairo_args_s* const args) {
	(void)cairo_printf("%s ", args->program);
	(void)cairo_printf("--include \"");
}

// todo: document!
_cairo_func void _cairo_args_print_end(const cairo_args_s* const args) {
	(void)cairo_printf("\"");
	if (args->shuffle != 0) (void)cairo_printf(" --shuffle %zu", args->shuffle);
	if (args->repeat  != 1) (void)cairo_printf(" --repeat %zu" , args->repeat );
	if (args->verbose     ) (void)cairo_printf(" --verbose"                   );
	if (args->rerun       ) (void)cairo_printf(" --rerun"                     );
	if (args->ecode   != 1) (void)cairo_printf(" --ecode %zu"  , args->ecode  );
	(void)cairo_printf("\n");
}

/// the full tests run: the array of test references collected from the section,
/// running tallies (passed/failed/skipped), total elapsed time, and args.
typedef struct _cairo_tests_s {
	_cairo_test_s**     data   ;
	size_t              count  ;
	size_t              passed ;
	size_t              failed ;
	size_t              skipped;
	double              elapsed;
	const cairo_args_s* args   ;
} _cairo_tests_s;

/// builds a '_cairo_tests_s' over the tests in the linker section, with all its
/// tallies zeroed and 'args' attached.
_cairo_func _cairo_tests_s _cairo_tests_new(const cairo_args_s* const args) {
	return (const _cairo_tests_s) {
		.data    = _cairo_begin                       ,
		.count   = (size_t)(_cairo_end - _cairo_begin),
		.passed  = 0                                  ,
		.failed  = 0                                  ,
		.skipped = 0                                  ,
		.elapsed = 0.0                                ,
		.args    = args                               ,
	};
}

/// sorts the collected test references into suite.name order to have the output
/// grouped and stable.
_cairo_func void _cairo_tests_sort(_cairo_tests_s* const tests) {
	cairo_qsort(
		tests->data, tests->count, sizeof(*tests->data), _cairo_test_compare
	);
}

/// fisher-yates shuffle of the collected test references using a 'seed'-derived
/// splitmix64 stream. null padding entries shuffle along harmlessly since every
/// execution and reporting pass skips them.
_cairo_func void _cairo_tests_shuffle(_cairo_tests_s* const tests,
									  const uint64_t seed) {
	uint64_t state = seed;

	for (size_t index = tests->count; index > 1; --index) {
		const size_t nindex = (size_t)(
			_cairo_random_next(&state) % (uint64_t)index
		);

		_cairo_test_s* const test = tests->data[index - 1];
		tests->data[index - 1]    = tests->data[nindex]   ;
		tests->data[nindex]       = test                  ;
	}
}

/// sorts the tests into suite/name order and marks each's 'shall_run' according
/// to the include pattern and (if set) the exclude pattern.
_cairo_func void _cairo_tests_prepare(_cairo_tests_s* const tests) {
	_cairo_tests_sort(tests);
	if (tests->args->shuffle) _cairo_tests_shuffle(tests, tests->args->shuffle);

	for (size_t index = 0; index < tests->count; ++index) {
		_cairo_test_s* const test = tests->data[index];
		if (cairo_null == test) continue;

		test->shall_run = (
			 _cairo_test_match(test, tests->args->include) &&
			!_cairo_test_match(test, tests->args->exclude)
		);
	}
}

/// prints the name of every selected ('shall_run' is true) test, grouped by the
/// suite and name: the suite on its own line with each of its test names listed
/// beneath it.
/// note: call after '_cairo_tests_prepare', which sorts and applies filters for
/// the tests and sets the 'shall_run' field.
_cairo_func void _cairo_tests_list(_cairo_tests_s* const tests) {
	_cairo_tests_sort(tests);
	const char* suite = cairo_null;
	size_t      count = 0         ;

	for (size_t index = 0; index < tests->count; ++index) {
		_cairo_test_s* const test = tests->data[index];
		if (cairo_null == test) continue;
		if (!test->shall_run)   continue;

		if ((cairo_null == suite) || cairo_strcmp(suite, test->suite)) {
			suite = test->suite;
			(void)cairo_printf("%s\n", suite);
		}

		(void)cairo_printf("  %zu. %s\n", ++count, test->name);
	}

	(void)cairo_printf("\n%zu tests\n", count);
}

/// runs every selected test in order, updating pass/fail/skip counts, and total
/// elapsed time. skips null records and tests not marked to run.
_cairo_func void _cairo_tests_execute(_cairo_tests_s* const tests) {
	tests->passed  = 0                ;
	tests->failed  = 0                ;
	tests->skipped = 0                ;
	tests->elapsed = _cairo_time_now();

	for (size_t index = 0; index < tests->count; ++index) {
		_cairo_test_s* const test = tests->data[index];
		if (cairo_null == test) continue;
		if (!test->shall_run)   continue;

		_cairo_test_run(test);
		// note: unrunned test is an actual state and should be ignored as it is
		// reserved for internal workings of this project.
		tests->passed  += (_cairo_test_status_pass == test->status);
		tests->failed  += (_cairo_test_status_fail == test->status);
		tests->skipped += (_cairo_test_status_skip == test->status);
	}

	tests->elapsed = _cairo_time_now() - tests->elapsed;
}

/// prints the per-suite progress line, grouping tests by suite and emitting one
/// character per test - '.' pass, 'F' fail, 'S' skip - with the suite's elapsed
/// time in parentheses, see example:
/// demo S..FF..... (123.456ms)
_cairo_func void _cairo_tests_report_suite(const _cairo_tests_s* const tests) {
	if ((tests->passed > 0) || (tests->failed > 0) || (tests->skipped > 0)) {
		const char* suite   = cairo_null;
		size_t      passed  = 0         ;
		size_t      failed  = 0         ;
		size_t      skipped = 0         ;
		double      elapsed = 0.0       ;

		#define print_suite_summary() do {                                     \
				(void)cairo_printf(                                            \
					" %zu passed, %zu failed, %zu skipped / %zu (%s)\n",       \
					passed, failed, skipped, passed + failed + skipped,        \
					_cairo_format_time(elapsed));                              \
			} while (0)

		for (size_t index = 0; index < tests->count; ++index) {
			_cairo_test_s* const test = tests->data[index];
			if (cairo_null == test) continue;
			if (!test->shall_run)   continue;

			if ((cairo_null == suite) || cairo_strcmp(suite, test->suite)) {
				if (suite != cairo_null) print_suite_summary();
				suite   = test->suite;
				passed  = 0          ;
				failed  = 0          ;
				skipped = 0          ;
				elapsed = 0.0        ;
				(void)cairo_printf("%s ", suite);
			}

			const bool did_pass = _cairo_test_status_pass == test->status;
			const bool did_fail = _cairo_test_status_fail == test->status;
			const bool did_skip = _cairo_test_status_skip == test->status;
			(void)cairo_printf("%s", did_pass ? "." : (did_fail ? "F" : "S"));
			(void)cairo_fflush(cairo_stdout);

			passed  += (size_t)did_pass;
			failed  += (size_t)did_fail;
			skipped += (size_t)did_skip;
			elapsed += test->elapsed   ;
		}

		print_suite_summary();
	}
}

/// prints a detailed failure report (via '_cairo_test_report') for every failed
/// test, when there are any.
_cairo_func void _cairo_tests_report_tests(const _cairo_tests_s* const tests) {
	if (tests->failed > 0) {
		_cairo_reader_s reader = _cairo_reader_new();

		for (size_t index = 0; index < tests->count; ++index) {
			_cairo_test_s* const test = tests->data[index];
			if (cairo_null == test) continue;
			if (!test->shall_run)   continue;

			if (_cairo_test_status_fail == test->status) {
				_cairo_test_report(test, &reader, tests->args->verbose);
			}
		}

		_cairo_reader_close(&reader);
		(void)cairo_printf("\n");
	}
}

/// prints the whole report: per-suite progress, each failure in detail, and the
/// summary line of passed/failed/skipped counts with total time.
_cairo_func void _cairo_tests_report(_cairo_tests_s* const tests) {
	_cairo_tests_sort(tests);
	_cairo_tests_report_suite(tests);
	_cairo_tests_report_tests(tests);
	(void)cairo_printf("%zu passed, %zu failed, %zu skipped / %zu (%s)\n",
		tests->passed, tests->failed, tests->skipped,
		tests->passed + tests->failed + tests->skipped,
		_cairo_format_time(tests->elapsed));

	if (tests->args->rerun && tests->failed) {
		(void)cairo_printf("\n");
		_cairo_args_print_begin(tests->args);
		for (size_t index = 0, findex = 0; index < tests->count; ++index) {
			_cairo_test_s* const test = tests->data[index];
			if (cairo_null == test) continue;

			if (_cairo_test_status_fail == test->status) {
				if (findex > 0) (void)cairo_printf(":");
				(void)cairo_printf("%s", test->suite_and_name);
				++findex;
			}
		}
		_cairo_args_print_end(tests->args);
	}
}

/// runs the tests according to 'args' and returns a process-ready exit code. if
/// parsing did not end in 'go_on' (e.g. --help or an usage error), returns that
/// status as the code without running. otherwise it prepares, runs, and reports
/// the selected tests 'args.repeat' times, returning 1 if any run had a failure
/// and 0 if all passed. with no tests at all, returns 0.
_cairo_func int cairo_tests_run(const cairo_args_s args) {
	if (args._status != _cairo_args_status_go_on) return (int)args._status;
	_cairo_tests_s tests = _cairo_tests_new(&args);
	if (0 == tests.count) return 0;

	if (args.list) {
		_cairo_tests_prepare(&tests);
		_cairo_tests_list(&tests);
		return 0;
	}

	bool failed = false;
	for (size_t _ = 0; _ < args.repeat; ++_) {
		_cairo_tests_prepare(&tests);
		_cairo_tests_execute(&tests);
		_cairo_tests_report(&tests);
		if (!failed && (tests.failed > 0)) failed = true;
	}
	return failed ? (int)args.ecode : 0;
}

/// helper entry point that runs all tests with default options and no cli args.
/// equivalent to 'cairo_tests_run(cairo_args_default())'.
_cairo_func int cairo_tests_run_default(void) {
	return cairo_tests_run(cairo_args_default());
}

/// note: this ref to a null test is needed to have at least one test in a cairo
/// tests section to not get linker errors.
_cairo_test_ref(_cairo_test_dummy_ref, cairo_null);

#ifdef cairo_enable_prefixless
#	define test_new          cairo_test_new
#	define test_skip         cairo_test_skip
#	define assert_true       cairo_assert_true
#	define assert_false      cairo_assert_false
#	define assert_eq         cairo_assert_eq
#	define assert_neq        cairo_assert_neq
#	define assert_gt         cairo_assert_gt
#	define assert_ge         cairo_assert_ge
#	define assert_lt         cairo_assert_lt
#	define assert_le         cairo_assert_le
#	define assert_nr         cairo_assert_nr
#	define assert_nnr        cairo_assert_nnr
#	define assert_nreq       cairo_assert_nreq
#	define assert_nnreq      cairo_assert_nnreq
#	define assert_streq      cairo_assert_streq
#	define assert_strneq     cairo_assert_strneq
#	define expect_true       cairo_expect_true
#	define expect_false      cairo_expect_false
#	define expect_eq         cairo_expect_eq
#	define expect_neq        cairo_expect_neq
#	define expect_gt         cairo_expect_gt
#	define expect_ge         cairo_expect_ge
#	define expect_lt         cairo_expect_lt
#	define expect_le         cairo_expect_le
#	define expect_nr         cairo_expect_nr
#	define expect_nnr        cairo_expect_nnr
#	define expect_nreq       cairo_expect_nreq
#	define expect_nnreq      cairo_expect_nnreq
#	define expect_streq      cairo_expect_streq
#	define expect_strneq     cairo_expect_strneq
#	define test_defered      cairo_test_defered
#	define should_fail       cairo_should_fail
#	define args_s            cairo_args_s
#	define args_default      cairo_args_default
#	define args_new          cairo_args_new
#	define tests_run         cairo_tests_run
#	define tests_run_default cairo_tests_run_default
#endif

/// todo: implement naming style macros for public api names.

/// todo: introduce deprecation mechanism and macros.

#endif

/// 
/// revision history:
///     v1.3.1 (2026-09-03)
///         add --rerun/-r for printing command to run all failed tests.
///     v1.3.0 (2026-08-29)
///         update reporting to include tests information by suite.
///         change --pattern/-p to --include/-i.
///         add cairo_should_fail mechanism.
///         add cairo_assert_nnr and cairo_assert_nnreq.
///         add cairo_expect_nnr and cairo_expect_nnreq.
///         update reporting to support unlimited multi-line expressions.
///         remove cairo_disable_verbose_output setting.
///     v1.2.0 (2026-08-14)
///         add cairo_expect_* family and cairo_test_defered for test cleanup.
///         change --verbose short flag to -V, add -v/--version flag.
///         add cairo_disable_verbose_output setting.
///         add cairo_assert_streq/strneq cstrings asserts.
///         fix single line suite reporting bug.
///         add --shuffle for seeded test order randomization.
///     v1.1.0 (2026-08-01)
///         add ':'-separated glob lists for include and exclude patterns.
///         add cairo_supress_sign_compare_warnings setting.
///         add overridable c stdlib function wrappers.
///         fix asserts evaluating arguments twice in tests.
///         add verbosity mechanism and cli --verbose flag.
///     v1.0.0 (2026-07-28)
///         first release.
/// 
/// copyright (c) 2026 yorissu
/// permission is hereby granted, free of charge, to any person obtaining a copy
/// of this software and associated documents, and documentation files (referred
/// to as the "software" from here on), to deal in and with the software without
/// restriction, including, without limitation, the rights to use, copy, modify,
/// merge, publish, distribute, sublicense, and/or sell copies of this software,
/// and to permit persons to whom the software is furnished to do so, subject to
/// the following conditions:
/// the above copyright notice and this permission notice shall be included with
/// any and in all copies or substantial portions of the software.
/// the software is provided "as is", without warranty of any kind, expressed or
/// implied, including but not limited to the warranties of the merchantability,
/// fitness for a particular purpose and noninfringement. in no event, shall the
/// authors or copyright holders be liable for any claim, damages, incidents, or
/// other liability, whether in an action of contract, tort or otherwise arising
/// from, out of or in connection with the software or the use or other dealings
/// in the software.
/// 
