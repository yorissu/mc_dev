
#ifndef __mc__global_h__
#define __mc__global_h__

/// basic definitions every module uses: assertions, panics for internal
/// errors (bugs in the compiler, never errors in a program) and allocation
/// that never returns null.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/// MC_PORTABLE builds with standard c11 only: no __typeof__, no
/// attributes and the portable 128-bit integers (int128.h). without it the
/// build uses the extensions the compiler has; the results are the same.
#if !defined(MC_PORTABLE) && (defined(__GNUC__) || defined(__clang__) || (defined(_MSC_VER) && _MSC_VER >= 1939))
#	define MC_HAS_TYPEOF 1
#else
#	define MC_HAS_TYPEOF 0
#endif

#define mc_noreturn _Noreturn

#define mc_unused(_value) do {                                                 \
		((void)(_value));                                                      \
	} while (0)

#define mc_count_of(_array) (size_t)(                                          \
		sizeof(_array) / sizeof(*(_array))                                     \
	)

#if defined(__GNUC__) && !defined(MC_PORTABLE)
#	define mc_printf_like(_format, _arguments)                                 \
		__attribute__((format(printf, _format, _arguments)))
#else
#	define mc_printf_like(_format, _arguments)
#endif

mc_noreturn void mc_panic_impl_(const char* const file, const int line,
								const char* const format, ...)
								mc_printf_like(3, 4);

/// stops with an internal compiler error.
#define mc_panic(...) mc_panic_impl_(__FILE__, __LINE__, __VA_ARGS__)

#ifdef NDEBUG
#	define mc_assert(_expr) do {                                               \
		((void)sizeof(_expr));                                                 \
	} while (0)
#else
#	define mc_assert(_expr) do {                                               \
		((_expr) ? (void)0 : mc_panic("assertion failed: %s", #_expr));        \
	} while (0)
#endif

#define mc_unreachable() mc_panic("unreachable code was reached")

void* mc_malloc(const size_t size);

void* mc_calloc(const size_t count, const size_t size);

void* mc_realloc(void* const pointer, const size_t size);

void mc_free(void* const pointer);

/// a copy of 'text' (null terminated) owned by the caller.
char* mc_strdup(const char* const text);

#endif
