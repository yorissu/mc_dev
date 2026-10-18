
#ifndef __mia__span_h__
#define __mia__span_h__

/// spans: byte ranges in one source file.

#include "mia/source.h"

typedef struct {
	uint32_t file ;
	uint32_t begin;
	uint32_t end  ;
} mia_span_s;

/// a span that points nowhere (diagnostics about the command line).
extern const mia_span_s mia_span_none;

/// the smallest span covering both ('a' when they are in different files).
mia_span_s mia_span_join(const mia_span_s a, const mia_span_s b);

mia_view_s mia_span_text(const mia_source_table_s* const sources, const mia_span_s span);

#endif
