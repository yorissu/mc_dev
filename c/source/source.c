
#include "mc/source.h"

#include <stdio.h>
#include <string.h>

mc_source_s* mc_sources_add(mc_sources_s* const sources, const char* const path, const mc_view_s text) {
	mc_assert(sources != NULL);
	mc_assert(text.length < UINT32_MAX);
	mc_source_s* const source = mc_arena_new(&sources->arena, mc_source_s);
	source->id   = (uint32_t)sources->files.count;
	source->path = mc_arena_strndup(&sources->arena, path, strlen(path));
	source->text = mc_view_new(mc_arena_strndup(&sources->arena, text.data, text.length), text.length);
	mc_vector_push(&source->lines, 0u);
	for (uint32_t i = 0; i < (uint32_t)text.length; ++i) {
		if ('\n' == source->text.data[i]) mc_vector_push(&source->lines, i + 1);
	}
	mc_vector_push(&sources->files, source);
	return source;
}

bool mc_read_file(const char* const path, mc_string_s* const content) {
	FILE* const file = fopen(path, "rb");
	if (NULL == file) return false;
	char buffer[8192];
	size_t read = 0;
	while ((read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
		mc_string_append(content, mc_view_new(buffer, read));
	}
	const bool failed = (0 != ferror(file));
	(void)fclose(file);
	return !failed;
}

mc_source_s* mc_sources_load(mc_sources_s* const sources, const char* const path) {
	mc_string_s content = {0};
	mc_source_s* source = NULL;
	if (mc_read_file(path, &content)) source = mc_sources_add(sources, path, mc_string_view(&content));
	mc_string_free(&content);
	return source;
}

const mc_source_s* mc_sources_get(const mc_sources_s* const sources, const uint32_t id) {
	mc_assert(id < sources->files.count);
	return sources->files.data[id];
}

void mc_sources_free(mc_sources_s* const sources) {
	for (size_t i = 0; i < sources->files.count; ++i) mc_vector_free(&sources->files.data[i]->lines);
	mc_vector_free(&sources->files);
	mc_arena_free(&sources->arena);
}

mc_location_s mc_source_location(const mc_source_s* const source, const uint32_t offset) {
	mc_assert(offset <= source->text.length);
	size_t low = 0, high = source->lines.count;
	while ((high - low) > 1) {
		const size_t middle = low + ((high - low) / 2);
		if (source->lines.data[middle] <= offset) low = middle;
		else high = middle;
	}
	uint32_t column = 1;
	for (uint32_t i = source->lines.data[low]; i < offset; ++i) {
		if (0x80 != ((uint8_t)source->text.data[i] & 0xC0)) ++column;
	}
	return (const mc_location_s) { .line = (uint32_t)low + 1, .column = column, };
}

mc_view_s mc_source_line(const mc_source_s* const source, const uint32_t line) {
	mc_assert((line > 0) && (line <= source->lines.count));
	const size_t begin = source->lines.data[line - 1];
	size_t end = (line < source->lines.count) ? source->lines.data[line] : source->text.length;
	if ((end > begin) && ('\n' == source->text.data[end - 1])) --end;
	if ((end > begin) && ('\r' == source->text.data[end - 1])) --end;
	return mc_view_slice(source->text, begin, end);
}

uint32_t mc_source_line_start(const mc_source_s* const source, const uint32_t line) {
	mc_assert((line > 0) && (line <= source->lines.count));
	return source->lines.data[line - 1];
}

mc_span_s mc_span_join(const mc_span_s a, const mc_span_s b) {
	if (a.file != b.file) return a;
	return (const mc_span_s) {
		.file  = a.file,
		.begin = (a.begin < b.begin) ? a.begin : b.begin,
		.end   = (a.end > b.end) ? a.end : b.end,
	};
}

mc_view_s mc_span_text(const mc_sources_s* const sources, const mc_span_s span) {
	const mc_source_s* const source = mc_sources_get(sources, span.file);
	return mc_view_slice(source->text, span.begin, span.end);
}
