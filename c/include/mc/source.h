
#ifndef __mc__source_h__
#define __mc__source_h__

/// source files and spans. every loaded file gets an id; a span is a byte
/// range in one file. lines and columns are 1-based; columns count code points.

#include "mc/arena.h"
#include "mc/string.h"
#include "mc/vector.h"

typedef struct {
	uint32_t file ;
	uint32_t begin;
	uint32_t end  ;
} mc_span_s;

typedef struct {
	uint32_t line  ;
	uint32_t column;
} mc_location_s;

typedef mc_vector_of(uint32_t) mc_line_starts_s;

typedef struct {
	uint32_t          id     ;
	const char*       path   ;
	mc_view_s         text   ;
	mc_line_starts_s  lines  ;
} mc_source_s;

typedef mc_vector_of(mc_source_s*) mc_source_list_s;

typedef struct {
	mc_arena_s       arena;
	mc_source_list_s files;
} mc_sources_s;

/// adds a copy of 'text' as the file 'path'.
mc_source_s* mc_sources_add(mc_sources_s* const sources, const char* const path, const mc_view_s text);

/// reads and adds the file at 'path'; null when it cannot be read.
mc_source_s* mc_sources_load(mc_sources_s* const sources, const char* const path);

const mc_source_s* mc_sources_get(const mc_sources_s* const sources, const uint32_t id);

void mc_sources_free(mc_sources_s* const sources);

mc_location_s mc_source_location(const mc_source_s* const source, const uint32_t offset);

/// the text of a line without its line break.
mc_view_s mc_source_line(const mc_source_s* const source, const uint32_t line);

uint32_t mc_source_line_start(const mc_source_s* const source, const uint32_t line);

mc_span_s mc_span_join(const mc_span_s a, const mc_span_s b);

mc_view_s mc_span_text(const mc_sources_s* const sources, const mc_span_s span);

/// reads a whole file into a caller-owned buffer; false when it cannot.
bool mc_read_file(const char* const path, mc_string_s* const content);

#endif
