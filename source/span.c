
#include "mia/span.h"

const mia_span_s mia_span_none = { .file = UINT32_MAX, .begin = 0, .end = 0, };

mia_span_s mia_span_join(const mia_span_s a, const mia_span_s b) {
	if (a.file != b.file) return a;
	return (const mia_span_s) {
		.file  = a.file,
		.begin = (a.begin < b.begin) ? a.begin : b.begin,
		.end   = (a.end > b.end) ? a.end : b.end,
	};
}

mia_view_s mia_span_text(const mia_source_table_s* const sources, const mia_span_s span) {
	const mia_source_s* const source = mia_source_get(sources, span.file);
	return mia_view_slice(source->text, span.begin, span.end);
}
