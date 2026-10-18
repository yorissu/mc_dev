
#include "mia/source.h"

#include <string.h>
#include <stdio.h>

mia_source_s* mia_source_add(mia_source_table_s* const sources, const char* const path, const mia_view_s text) {
	mia_assert(sources != NULL);
	mia_assert(text.length < UINT32_MAX);
	mia_source_s* const source = mia_arena_new(&sources->arena, mia_source_s);
	source->id   = (uint32_t)sources->files.count;
	source->path = mia_arena_strndup(&sources->arena, path, strlen(path));
	source->text = mia_view_new(mia_arena_strndup(&sources->arena, text.data, text.length), text.length);
	mia_vector_push(&source->lines, 0u);
	for (uint32_t i = 0; i < (uint32_t)text.length; ++i) {
		if ('\n' == source->text.data[i]) mia_vector_push(&source->lines, i + 1);
	}
	mia_vector_push(&sources->files, source);
	return source;
}

bool mia_source_read_file(const char* const path, mia_string_s* const content) {
	FILE* const file = fopen(path, "rb");
	if (NULL == file) return false;
	char buffer[8192];
	size_t read = 0;
	while ((read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
		mia_string_append(content, mia_view_new(buffer, read));
	}
	const bool failed = (0 != ferror(file));
	(void)fclose(file);
	return !failed;
}

mia_source_s* mia_source_load(mia_source_table_s* const sources, const char* const path) {
	mia_string_s content = {0};
	mia_source_s* source = NULL;
	// spans are 32-bit: a file of 4 GiB or more cannot be a source (it is refused like one that cannot be read)
	if (mia_source_read_file(path, &content) && content.length < UINT32_MAX) source = mia_source_add(sources, path, mia_string_view(&content));
	mia_string_free(&content);
	return source;
}

const mia_source_s* mia_source_get(const mia_source_table_s* const sources, const uint32_t id) {
	mia_assert(id < sources->files.count);
	return sources->files.data[id];
}

void mia_source_table_free(mia_source_table_s* const sources) {
	for (size_t i = 0; i < sources->files.count; ++i) mia_vector_free(&sources->files.data[i]->lines);
	mia_vector_free(&sources->files);
	mia_arena_free(&sources->arena);
}

mia_source_location_s mia_source_location(const mia_source_s* const source, const uint32_t offset) {
	mia_assert(offset <= source->text.length);
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
	return (const mia_source_location_s) { .line = (uint32_t)low + 1, .column = column, };
}

mia_view_s mia_source_line(const mia_source_s* const source, const uint32_t line) {
	mia_assert((line > 0) && (line <= source->lines.count));
	const size_t begin = source->lines.data[line - 1];
	size_t end = (line < source->lines.count) ? source->lines.data[line] : source->text.length;
	if ((end > begin) && ('\n' == source->text.data[end - 1])) --end;
	if ((end > begin) && ('\r' == source->text.data[end - 1])) --end;
	return mia_view_slice(source->text, begin, end);
}

uint32_t mia_source_line_start(const mia_source_s* const source, const uint32_t line) {
	mia_assert((line > 0) && (line <= source->lines.count));
	return source->lines.data[line - 1];
}
