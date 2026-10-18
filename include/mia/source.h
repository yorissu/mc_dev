
#ifndef __mia__source_h__
#define __mia__source_h__

/// source files. every loaded file gets an id. lines and columns are
/// 1-based; columns count code points.

#include "mia/arena.h"
#include "mia/string.h"
#include "mia/view.h"
#include "mia/vector.h"

typedef struct {
	uint32_t line  ;
	uint32_t column;
} mia_source_location_s;

typedef mia_vector_of(uint32_t) mia_source_line_starts_s;

typedef struct {
	uint32_t                 id   ;
	const char*              path ;
	mia_view_s               text ;
	mia_source_line_starts_s lines;
} mia_source_s;

typedef mia_vector_of(mia_source_s*) mia_source_list_s;

typedef struct {
	mia_arena_s       arena;
	mia_source_list_s files;
} mia_source_table_s;

/// adds a copy of 'text' as the file 'path'.
mia_source_s* mia_source_add(mia_source_table_s* const sources, const char* const path, const mia_view_s text);

/// reads and adds the file at 'path'; null when it cannot be read (or
/// takes 4 GiB or more: spans are 32-bit).
mia_source_s* mia_source_load(mia_source_table_s* const sources, const char* const path);

const mia_source_s* mia_source_get(const mia_source_table_s* const sources, const uint32_t id);

void mia_source_table_free(mia_source_table_s* const sources);

mia_source_location_s mia_source_location(const mia_source_s* const source, const uint32_t offset);

/// the text of a line without its line break.
mia_view_s mia_source_line(const mia_source_s* const source, const uint32_t line);

uint32_t mia_source_line_start(const mia_source_s* const source, const uint32_t line);

/// reads a whole file into a caller-owned buffer; false when it cannot.
bool mia_source_read_file(const char* const path, mia_string_s* const content);

#endif
