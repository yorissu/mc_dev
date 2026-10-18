
#include "mia/global.h"

#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>

void mia_panic_at(const char* const file, const int line,
				  const char* const format, ...) {
	(void)fflush(stdout);
	(void)fprintf(stderr, "mia: internal compiler error at %s:%d: ", file, line);
	va_list args; va_start(args, format);
	(void)vfprintf(stderr, format, args);
	va_end(args);
	(void)fprintf(stderr, "\nthis is a bug in mia, please report it.\n");
	abort();
}

void* mia_malloc(const size_t size) {
	void* const pointer = malloc(size > 0 ? size : 1);
	if (NULL == pointer) mia_panic("out of memory (allocating %zu bytes)", size);
	return pointer;
}

void* mia_calloc(const size_t count, const size_t size) {
	void* const pointer = calloc(count > 0 ? count : 1, size > 0 ? size : 1);
	if (NULL == pointer) mia_panic("out of memory (allocating %zu x %zu bytes)", count, size);
	return pointer;
}

void* mia_realloc(void* const pointer, const size_t size) {
	void* const result = realloc(pointer, size > 0 ? size : 1);
	if (NULL == result) mia_panic("out of memory (reallocating %zu bytes)", size);
	return result;
}

void mia_free(void* const pointer) {
	free(pointer);
}

char* mia_strdup(const char* const text) {
	mia_assert(text != NULL);
	const size_t length = strlen(text);
	char* const copy = mia_malloc(length + 1);
	(void)memcpy(copy, text, length + 1);
	return copy;
}
