
#ifndef __mia__global_h__
#define __mia__global_h__

/// basic definitions every module uses: assertions, panics for internal
/// errors (bugs in the compiler, never errors in a program) and allocation
/// that never returns null.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/// mia_portable builds with standard c11 only: no __typeof__, no
/// attributes and the portable 128-bit integers (int128.h). without it the
/// build uses the extensions the compiler has; the results are the same.
#if !defined(mia_portable) && (defined(__GNUC__) || defined(__clang__) || (defined(_MSC_VER) && _MSC_VER >= 1939))
#	define mia_has_typeof 1
#else
#	define mia_has_typeof 0
#endif

#define mia_noreturn _Noreturn

#define mia_unused(_value) do {                                                \
		((void)(_value));                                                      \
	} while (0)

#define mia_count_of(_array) (size_t)(                                         \
		sizeof(_array) / sizeof(*(_array))                                     \
	)

/// mingw's printf is c99's (%zu, %lld) with __USE_MINGW_ANSI_STDIO, which
/// the makefile defines on windows: its format checks are gnu_printf's.
#if defined(__MINGW32__)
#	define mia_printf_family gnu_printf
#else
#	define mia_printf_family printf
#endif

#if defined(__GNUC__) && !defined(mia_portable)
#	define mia_printf_like(_format, _arguments)                                \
		__attribute__((format(mia_printf_family, _format, _arguments)))
#else
#	define mia_printf_like(_format, _arguments)
#endif

/// stops with an internal compiler error at 'file':'line' (see mia_panic).
mia_noreturn void mia_panic_at(const char* const file, const int line,
							   const char* const format, ...)
								mia_printf_like(3, 4);

/// stops with an internal compiler error.
#define mia_panic(...) mia_panic_at(__FILE__, __LINE__, __VA_ARGS__)

#ifdef NDEBUG
#	define mia_assert(_expr) do {                                              \
		((void)sizeof(_expr));                                                 \
	} while (0)
#else
#	define mia_assert(_expr) do {                                              \
		((_expr) ? (void)0 : mia_panic("assertion failed: %s", #_expr));       \
	} while (0)
#endif

#define mia_unreachable() mia_panic("unreachable code was reached")

/// the c allocator's functions, stopping when memory runs out.
void* mia_malloc(const size_t size);

void* mia_calloc(const size_t count, const size_t size);

void* mia_realloc(void* const pointer, const size_t size);

void mia_free(void* const pointer);

/// a copy of 'text' (null terminated) owned by the caller.
char* mia_strdup(const char* const text);

#endif
