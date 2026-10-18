
#include "mia/diag.h"

#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#define _mia_diag_tab_width 4

#define _mia_diag_reset  "\033[0m"
#define _mia_diag_bold   "\033[1m"
#define _mia_diag_red    "\033[1;31m"
#define _mia_diag_yellow "\033[1;33m"
#define _mia_diag_blue   "\033[1;34m"
#define _mia_diag_cyan   "\033[1;36m"

static const char* const _mia_diag_code_texts[] = {
#define mia_diag_code(_code, _name, _summary) #_code,
#include "mia/diagnostics.def"
#undef mia_diag_code
};

static const char* const _mia_diag_code_names[] = {
#define mia_diag_code(_code, _name, _summary) #_name,
#include "mia/diagnostics.def"
#undef mia_diag_code
};

static const char* const _mia_diag_code_summaries[] = {
#define mia_diag_code(_code, _name, _summary) _summary,
#include "mia/diagnostics.def"
#undef mia_diag_code
};

const char* mia_diag_code_text(const mia_diag_code_e code) {
	mia_assert(code < mia_diag_code_count);
	return _mia_diag_code_texts[code];
}

const char* mia_diag_code_name(const mia_diag_code_e code) {
	mia_assert(code < mia_diag_code_count);
	return _mia_diag_code_names[code];
}

const char* mia_diag_code_summary(const mia_diag_code_e code) {
	mia_assert(code < mia_diag_code_count);
	return _mia_diag_code_summaries[code];
}

mia_diag_code_e mia_diag_code_parse(const char* const text) {
	for (size_t code = 0; code < mia_diag_code_count; ++code) {
		const char* const candidate = _mia_diag_code_texts[code];
		if (strlen(text) != strlen(candidate)) continue;
		bool equal = true;
		for (size_t i = 0; candidate[i] != '\0'; ++i) {
			if (toupper((unsigned char)text[i]) != candidate[i]) equal = false;
		}
		if (equal) return (mia_diag_code_e)code;
	}
	return mia_diag_code_count;
}

static bool _mia_diag_has_span(const mia_span_s span) {
	return span.file != UINT32_MAX;
}

static mia_diag_entry_s* _mia_diag_report(mia_diag_s* const diag, const mia_diag_level_e level, const mia_diag_code_e code,
										  const mia_span_s span, const char* const format, va_list args) mia_printf_like(5, 0);

static mia_diag_entry_s* _mia_diag_report(mia_diag_s* const diag, const mia_diag_level_e level, const mia_diag_code_e code,
										  const mia_span_s span, const char* const format, va_list args) {
	mia_assert(diag != NULL);
	mia_diag_entry_s* const diagnostic = mia_calloc(1, sizeof(*diagnostic));
	diagnostic->level   = level;
	diagnostic->code    = code;
	diagnostic->message = mia_string_vformat(format, args);
	if (_mia_diag_has_span(span)) {
		const mia_diag_label_s label = { .span = span, .primary = true, .message = mia_strdup(""), };
		mia_vector_push(&diagnostic->labels, label);
	}
	// generic bodies are checked once per instantiation; a mistake in one is
	// reported once.
	for (size_t i = 0; i < diag->items.count; ++i) {
		const mia_diag_entry_s* const other = diag->items.data[i];
		const bool other_span = other->labels.count > 0 && other->labels.data[0].primary;
		const bool same_span = (other_span == _mia_diag_has_span(span)) && (!other_span
			|| (other->labels.data[0].span.file == span.file && other->labels.data[0].span.begin == span.begin));
		if (other->code == code && same_span && 0 == strcmp(other->message, diagnostic->message)) {
			mia_vector_push(&diag->discarded, diagnostic);
			return diagnostic;
		}
	}
	mia_vector_push(&diag->items, diagnostic);
	if (mia_diag_level_warning == level) diag->warning_count++;
	else diag->error_count++;
	return diagnostic;
}

mia_diag_entry_s* mia_diag_error(mia_diag_s* const diag, const mia_diag_code_e code, const mia_span_s span,
								 const char* const format, ...) {
	va_list args; va_start(args, format);
	mia_diag_entry_s* const diagnostic = _mia_diag_report(diag, mia_diag_level_error, code, span, format, args);
	va_end(args);
	return diagnostic;
}

mia_diag_entry_s* mia_diag_warning(mia_diag_s* const diag, const mia_diag_code_e code, const mia_span_s span,
								   const char* const format, ...) {
	va_list args; va_start(args, format);
	mia_diag_entry_s* const diagnostic = _mia_diag_report(diag, mia_diag_level_warning, code, span, format, args);
	va_end(args);
	return diagnostic;
}

mia_diag_entry_s* mia_diag_runtime_error(mia_diag_s* const diag, const mia_span_s span, const char* const format, ...) {
	va_list args; va_start(args, format);
	mia_diag_entry_s* const diagnostic = _mia_diag_report(diag, mia_diag_level_runtime, mia_diag_code_count, span, format, args);
	va_end(args);
	return diagnostic;
}

mia_diag_entry_s* mia_diag_label_main(mia_diag_entry_s* const diagnostic, const char* const format, ...) {
	for (size_t i = 0; i < diagnostic->labels.count; ++i) {
		if (!diagnostic->labels.data[i].primary) continue;
		va_list args; va_start(args, format);
		mia_free(diagnostic->labels.data[i].message);
		diagnostic->labels.data[i].message = mia_string_vformat(format, args);
		va_end(args);
		break;
	}
	return diagnostic;
}

mia_diag_entry_s* mia_diag_label(mia_diag_entry_s* const diagnostic, const mia_span_s span, const char* const format, ...) {
	if (!_mia_diag_has_span(span)) return diagnostic;
	va_list args; va_start(args, format);
	const mia_diag_label_s label = { .span = span, .primary = false, .message = mia_string_vformat(format, args), };
	va_end(args);
	mia_vector_push(&diagnostic->labels, label);
	return diagnostic;
}

static mia_diag_entry_s* _mia_diag_footer(mia_diag_entry_s* const diagnostic, const bool help,
										  const char* const format, va_list args) mia_printf_like(3, 0);

static mia_diag_entry_s* _mia_diag_footer(mia_diag_entry_s* const diagnostic, const bool help,
										  const char* const format, va_list args) {
	const mia_diag_footer_s footer = { .help = help, .message = mia_string_vformat(format, args), };
	mia_vector_push(&diagnostic->footers, footer);
	return diagnostic;
}

mia_diag_entry_s* mia_diag_note(mia_diag_entry_s* const diagnostic, const char* const format, ...) {
	va_list args; va_start(args, format);
	_mia_diag_footer(diagnostic, false, format, args);
	va_end(args);
	return diagnostic;
}

mia_diag_entry_s* mia_diag_help(mia_diag_entry_s* const diagnostic, const char* const format, ...) {
	va_list args; va_start(args, format);
	_mia_diag_footer(diagnostic, true, format, args);
	va_end(args);
	return diagnostic;
}

bool mia_diag_has_errors(const mia_diag_s* const diag) {
	return diag->error_count > 0;
}

static void _mia_diag_diagnostic_free(mia_diag_entry_s* const diagnostic) {
	for (size_t i = 0; i < diagnostic->labels.count; ++i) mia_free(diagnostic->labels.data[i].message);
	for (size_t i = 0; i < diagnostic->footers.count; ++i) mia_free(diagnostic->footers.data[i].message);
	mia_vector_free(&diagnostic->labels);
	mia_vector_free(&diagnostic->footers);
	mia_free(diagnostic->message);
	mia_free(diagnostic);
}

void mia_diag_free(mia_diag_s* const diag) {
	for (size_t i = 0; i < diag->items.count; ++i) _mia_diag_diagnostic_free(diag->items.data[i]);
	for (size_t i = 0; i < diag->discarded.count; ++i) _mia_diag_diagnostic_free(diag->discarded.data[i]);
	mia_vector_free(&diag->items);
	mia_vector_free(&diag->discarded);
	diag->error_count = diag->warning_count = 0;
}

// --- rendering ---------------------------------------------------------------

typedef struct {
	const mia_diag_s*       diag      ;
	const mia_diag_entry_s* diagnostic;
	mia_string_s*           out       ;
	bool                    color     ;
	size_t                  width     ;   // of the line number gutter
} _mia_diag_renderer_s;

static void _mia_diag_paint(const _mia_diag_renderer_s* const r, const char* const color) {
	if (r->color) mia_string_append_cstr(r->out, color);
}

static const char* _mia_diag_level_color(const mia_diag_level_e level) {
	return (mia_diag_level_warning == level) ? _mia_diag_yellow : _mia_diag_red;
}

static size_t _mia_diag_digits(const uint32_t number) {
	size_t digits = 1;
	for (uint32_t rest = number; rest >= 10; rest /= 10) ++digits;
	return digits;
}

/// the screen width of 'text': code points, tabs expanded.
static size_t _mia_diag_display_width(const mia_view_s text) {
	size_t width = 0;
	for (size_t i = 0; i < text.length; ++i) {
		if ('\t' == text.data[i]) width += _mia_diag_tab_width;
		else if (0x80 != ((uint8_t)text.data[i] & 0xC0)) ++width;
	}
	return width;
}

static void _mia_diag_gutter(const _mia_diag_renderer_s* const r, const char* const tail) {
	_mia_diag_paint(r, _mia_diag_blue);
	mia_string_append_repeat(r->out, ' ', r->width);
	mia_string_append_cstr(r->out, tail);
	_mia_diag_paint(r, _mia_diag_reset);
}

static void _mia_diag_source_line(const _mia_diag_renderer_s* const r, const mia_source_s* const source, const uint32_t line) {
	_mia_diag_paint(r, _mia_diag_blue);
	mia_string_appendf(r->out, "%*u |", (int)r->width, line);
	_mia_diag_paint(r, _mia_diag_reset);
	const mia_view_s text = mia_source_line(source, line);
	if (text.length > 0) mia_string_append_char(r->out, ' ');
	for (size_t i = 0; i < text.length; ++i) {
		if ('\t' == text.data[i]) mia_string_append_repeat(r->out, ' ', _mia_diag_tab_width);
		else mia_string_append_char(r->out, text.data[i]);
	}
	mia_string_append_char(r->out, '\n');
}

/// one label as it appears on one line: columns on screen.
typedef struct {
	const mia_diag_label_s* label  ;
	size_t                  begin  ;
	size_t                  end    ;   // exclusive, at least begin + 1
	bool                    message;   // whether its message is shown on this line
} _mia_diag_mark_s;

static int _mia_diag_mark_compare(const void* const a, const void* const b) {
	const _mia_diag_mark_s* const left = a;
	const _mia_diag_mark_s* const right = b;
	if (left->begin != right->begin) return (left->begin < right->begin) ? -1 : 1;
	return (left->label->primary == right->label->primary) ? 0 : (left->label->primary ? -1 : 1);
}

static const char* _mia_diag_mark_color(const _mia_diag_renderer_s* const r, const mia_diag_label_s* const label) {
	return label->primary ? _mia_diag_level_color(r->diagnostic->level) : _mia_diag_blue;
}

/// the underline rows below one source line: all markers on the first row,
/// the message of the rightmost label next to them, the other messages
/// hanging below on connectors.
static void _mia_diag_underlines(const _mia_diag_renderer_s* const r, _mia_diag_mark_s* const marks, const size_t count) {
	qsort(marks, count, sizeof(*marks), _mia_diag_mark_compare);
	size_t width = 0;
	for (size_t i = 0; i < count; ++i) if (marks[i].end > width) width = marks[i].end;

	// the markers row.
	_mia_diag_gutter(r, " | ");
	size_t column = 0;
	const char* painted = NULL;   // the color of the run of markers being written
	for (size_t position = 0; position < width; ++position) {
		const _mia_diag_mark_s* owner = NULL;
		for (size_t i = 0; i < count; ++i) {
			if (position >= marks[i].begin && position < marks[i].end && (NULL == owner || marks[i].label->primary)) owner = &marks[i];
		}
		if (NULL == owner) continue;
		const char* const color = _mia_diag_mark_color(r, owner->label);
		if (position > column || color != painted) {
			if (painted != NULL) _mia_diag_paint(r, _mia_diag_reset);
			painted = NULL;
		}
		mia_string_append_repeat(r->out, ' ', position - column);
		if (NULL == painted) {
			_mia_diag_paint(r, color);
			painted = color;
		}
		mia_string_append_char(r->out, owner->label->primary ? '^' : '-');
		column = position + 1;
	}
	if (painted != NULL) _mia_diag_paint(r, _mia_diag_reset);
	// the last label with a message ends the row with its message.
	size_t hanging = count;
	for (size_t i = count; i > 0; --i) {
		if (marks[i - 1].message && marks[i - 1].label->message[0] != '\0') {
			hanging = i - 1;
			break;
		}
	}
	if (hanging < count) {
		bool alone = true;
		for (size_t i = 0; i < count; ++i) if (i != hanging && marks[i].end > marks[hanging].begin && i > hanging) alone = false;
		if (alone) {
			_mia_diag_paint(r, _mia_diag_mark_color(r, marks[hanging].label));
			mia_string_appendf(r->out, " %s", marks[hanging].label->message);
			_mia_diag_paint(r, _mia_diag_reset);
			marks[hanging].message = false;
		}
	}
	mia_string_append_char(r->out, '\n');

	// the other messages, rightmost first, each below a connector.
	for (;;) {
		size_t next = count;
		for (size_t i = count; i > 0; --i) {
			if (marks[i - 1].message && marks[i - 1].label->message[0] != '\0') {
				next = i - 1;
				break;
			}
		}
		if (next == count) break;
		for (int pass = 0; pass < 2; ++pass) {
			_mia_diag_gutter(r, " | ");
			size_t at = 0;
			for (size_t i = 0; i <= next; ++i) {
				if (!(marks[i].message && marks[i].label->message[0] != '\0')) continue;
				if (marks[i].begin < at) continue;
				mia_string_append_repeat(r->out, ' ', marks[i].begin - at);
				_mia_diag_paint(r, _mia_diag_mark_color(r, marks[i].label));
				if (1 == pass && i == next) {
					mia_string_append_cstr(r->out, marks[i].label->message);
					at = marks[i].begin + strlen(marks[i].label->message);
				}
				else {
					mia_string_append_char(r->out, '|');
					at = marks[i].begin + 1;
				}
				_mia_diag_paint(r, _mia_diag_reset);
			}
			mia_string_append_char(r->out, '\n');
		}
		marks[next].message = false;
	}
}

/// the source lines of 'source' that labels point at, with their underlines.
/// the next line after 'previous' where a label of 'source' starts or ends (UINT32_MAX: none).
static uint32_t _mia_diag_next_labelled_line(const mia_diag_entry_s* const diagnostic, const mia_source_s* const source, const uint32_t previous) {
	uint32_t line = UINT32_MAX;
	for (size_t i = 0; i < diagnostic->labels.count; ++i) {
		const mia_diag_label_s* const label = &diagnostic->labels.data[i];
		if (label->span.file != source->id) continue;
		const uint32_t first = mia_source_location(source, label->span.begin).line;
		const uint32_t last = mia_source_location(source, label->span.end > label->span.begin ? label->span.end - 1 : label->span.begin).line;
		if (first > previous && first < line) line = first;
		if (last != first && last > previous && last < line) line = last;
	}
	return line;
}

/// the lines of a snippet: every labelled line with the line before and the
/// line after it (real source, without markers); '...' where lines are left out.
static void _mia_diag_snippet(const _mia_diag_renderer_s* const r, const mia_source_s* const source) {
	const mia_diag_entry_s* const diagnostic = r->diagnostic;
	const uint32_t lines = (uint32_t)source->lines.count;
	uint32_t shown = 0;   // the last line shown
	for (uint32_t line = _mia_diag_next_labelled_line(diagnostic, source, 0); line != UINT32_MAX; line = _mia_diag_next_labelled_line(diagnostic, source, line)) {
		const uint32_t next = _mia_diag_next_labelled_line(diagnostic, source, line);
		const uint32_t before = (line > 1) ? line - 1 : line;
		if (shown > 0 && before > shown + 1) {
			_mia_diag_paint(r, _mia_diag_blue);
			mia_string_append_repeat(r->out, ' ', r->width > 1 ? r->width - 1 : 0);
			mia_string_append_cstr(r->out, "...\n");
			_mia_diag_paint(r, _mia_diag_reset);
		}
		for (uint32_t context = (before > shown) ? before : shown + 1; context < line; ++context) _mia_diag_source_line(r, source, context);
		_mia_diag_source_line(r, source, line);

		const mia_view_s text = mia_source_line(source, line);
		const uint32_t start = mia_source_line_start(source, line);
		mia_vector_of(_mia_diag_mark_s) marks = {0};
		for (size_t i = 0; i < diagnostic->labels.count; ++i) {
			const mia_diag_label_s* const label = &diagnostic->labels.data[i];
			if (label->span.file != source->id) continue;
			const uint32_t last_offset = label->span.end > label->span.begin ? label->span.end - 1 : label->span.begin;
			const uint32_t first = mia_source_location(source, label->span.begin).line;
			const uint32_t last = mia_source_location(source, last_offset).line;
			if (first != line && last != line) continue;
			// a span over several lines: underline the rest of its first line
			// and the start of its last line; the message goes on the last.
			const size_t from = (first == line) ? label->span.begin - start : 0;
			size_t to = (last == line) ? (size_t)label->span.end - start : text.length;
			if (to > text.length) to = text.length;
			const size_t begin = _mia_diag_display_width(mia_view_slice(text, 0, from));
			size_t end = _mia_diag_display_width(mia_view_slice(text, 0, to > from ? to : from));
			if (end <= begin) end = begin + 1;
			const _mia_diag_mark_s mark = { .label = label, .begin = begin, .end = end, .message = (last == line), };
			mia_vector_push(&marks, mark);
		}
		_mia_diag_underlines(r, marks.data, marks.count);
		mia_vector_free(&marks);
		shown = line;
		// the line after, unless it is labelled itself (then it comes next)
		if (line < lines && line + 1 != next) {
			_mia_diag_source_line(r, source, line + 1);
			shown = line + 1;
		}
	}
}

/// 'error[E0502]', 'warning[W0502]' or 'runtime error'.
static void _mia_diag_header(const _mia_diag_renderer_s* const r) {
	const mia_diag_entry_s* const diagnostic = r->diagnostic;
	if (mia_diag_level_runtime == diagnostic->level) mia_string_append_cstr(r->out, "runtime error");
	else mia_string_appendf(r->out, "%s[%s]", mia_diag_level_error == diagnostic->level ? "error" : "warning", mia_diag_code_text(diagnostic->code));
}

static void _mia_diag_render_human(const _mia_diag_renderer_s* const r) {
	const mia_diag_entry_s* const diagnostic = r->diagnostic;
	_mia_diag_paint(r, _mia_diag_level_color(diagnostic->level));
	_mia_diag_header(r);
	_mia_diag_paint(r, _mia_diag_reset);
	_mia_diag_paint(r, _mia_diag_bold);
	mia_string_appendf(r->out, ": %s", diagnostic->message);
	_mia_diag_paint(r, _mia_diag_reset);
	mia_string_append_char(r->out, '\n');

	const mia_source_table_s* const sources = r->diag->sources;
	const mia_diag_label_s* primary = NULL;
	for (size_t i = 0; i < diagnostic->labels.count; ++i) {
		if (diagnostic->labels.data[i].primary) primary = &diagnostic->labels.data[i];
	}
	if (NULL == primary && diagnostic->labels.count > 0) primary = &diagnostic->labels.data[0];

	if (primary != NULL && sources != NULL) {
		// the files with labels: the primary label's first.
		uint32_t files[64];
		size_t file_count = 0;
		files[file_count++] = primary->span.file;
		for (size_t i = 0; i < diagnostic->labels.count && file_count < mia_count_of(files); ++i) {
			bool seen = false;
			for (size_t j = 0; j < file_count; ++j) seen = seen || files[j] == diagnostic->labels.data[i].span.file;
			if (!seen) files[file_count++] = diagnostic->labels.data[i].span.file;
		}
		for (size_t f = 0; f < file_count; ++f) {
			const mia_source_s* const source = mia_source_get(sources, files[f]);
			const mia_diag_label_s* anchor = (0 == f) ? primary : NULL;
			for (size_t i = 0; i < diagnostic->labels.count && NULL == anchor; ++i) {
				if (diagnostic->labels.data[i].span.file == files[f]) anchor = &diagnostic->labels.data[i];
			}
			if (NULL == anchor) continue;   // (every file listed has a label)
			const mia_source_location_s location = mia_source_location(source, anchor->span.begin);
			_mia_diag_gutter(r, (0 == f) ? "--> " : "::: ");
			mia_string_appendf(r->out, "%s:%u:%u\n", source->path, location.line, location.column);
			_mia_diag_gutter(r, " |\n");
			_mia_diag_snippet(r, source);
		}
		if (diagnostic->footers.count > 0) _mia_diag_gutter(r, " |\n");
	}
	for (size_t i = 0; i < diagnostic->footers.count; ++i) {
		const mia_diag_footer_s* const footer = &diagnostic->footers.data[i];
		_mia_diag_gutter(r, " = ");
		_mia_diag_paint(r, footer->help ? _mia_diag_cyan : _mia_diag_bold);
		mia_string_append_cstr(r->out, footer->help ? "help" : "note");
		_mia_diag_paint(r, _mia_diag_reset);
		mia_string_appendf(r->out, ": %s\n", footer->message);
	}
}

static void _mia_diag_render_short(const _mia_diag_renderer_s* const r) {
	const mia_diag_entry_s* const diagnostic = r->diagnostic;
	const mia_source_table_s* const sources = r->diag->sources;
	for (size_t i = 0; i < diagnostic->labels.count; ++i) {
		const mia_diag_label_s* const label = &diagnostic->labels.data[i];
		if (!label->primary || NULL == sources) continue;
		const mia_source_s* const source = mia_source_get(sources, label->span.file);
		const mia_source_location_s location = mia_source_location(source, label->span.begin);
		mia_string_appendf(r->out, "%s:%u:%u: ", source->path, location.line, location.column);
		break;
	}
	_mia_diag_paint(r, _mia_diag_level_color(diagnostic->level));
	_mia_diag_header(r);
	_mia_diag_paint(r, _mia_diag_reset);
	mia_string_appendf(r->out, ": %s\n", diagnostic->message);
}

void mia_diag_render(const mia_diag_s* const diag, const mia_diag_entry_s* const diagnostic,
					 const mia_diag_format_e format, const bool color, mia_string_s* const out) {
	_mia_diag_renderer_s renderer = { .diag = diag, .diagnostic = diagnostic, .out = out, .color = color, .width = 1, };
	for (size_t i = 0; i < diagnostic->labels.count && diag->sources != NULL; ++i) {
		const mia_diag_label_s* const label = &diagnostic->labels.data[i];
		const mia_source_s* const source = mia_source_get(diag->sources, label->span.file);
		const uint32_t last = label->span.end > label->span.begin ? label->span.end - 1 : label->span.begin;
		// the line after the last one is shown too
		const uint32_t line = mia_source_location(source, last).line;
		const size_t digits = _mia_diag_digits(line < source->lines.count ? line + 1 : line);
		if (digits > renderer.width) renderer.width = digits;
	}
	if (mia_diag_format_short == format) _mia_diag_render_short(&renderer);
	else _mia_diag_render_human(&renderer);
}

void mia_diag_print(const mia_diag_s* const diag, FILE* const stream, const mia_diag_format_e format, const bool color) {
	mia_string_s out = {0};
	for (size_t i = 0; i < diag->items.count; ++i) {
		mia_diag_render(diag, diag->items.data[i], format, color, &out);
		if (mia_diag_format_human == format) mia_string_append_char(&out, '\n');
	}
	(void)fputs(mia_string_cstr(&out), stream);
	mia_string_free(&out);
}
