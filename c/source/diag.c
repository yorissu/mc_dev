
#include "mc/diag.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#define _mc_tab_width 4

#define _mc_reset  "\033[0m"
#define _mc_bold   "\033[1m"
#define _mc_red    "\033[1;31m"
#define _mc_yellow "\033[1;33m"
#define _mc_blue   "\033[1;34m"
#define _mc_cyan   "\033[1;36m"

const mc_span_s mc_no_span = { .file = UINT32_MAX, .begin = 0, .end = 0, };

static const char* const _mc_code_texts[] = {
#define mc_diagnostic(_code, _name, _summary) #_code,
#include "mc/diagnostics.def"
#undef mc_diagnostic
};

static const char* const _mc_code_names[] = {
#define mc_diagnostic(_code, _name, _summary) #_name,
#include "mc/diagnostics.def"
#undef mc_diagnostic
};

static const char* const _mc_code_summaries[] = {
#define mc_diagnostic(_code, _name, _summary) _summary,
#include "mc/diagnostics.def"
#undef mc_diagnostic
};

const char* mc_code_text(const mc_code_e code) {
	mc_assert(code < mc_code_count_);
	return _mc_code_texts[code];
}

const char* mc_code_name(const mc_code_e code) {
	mc_assert(code < mc_code_count_);
	return _mc_code_names[code];
}

const char* mc_code_summary(const mc_code_e code) {
	mc_assert(code < mc_code_count_);
	return _mc_code_summaries[code];
}

mc_code_e mc_code_parse(const char* const text) {
	for (size_t code = 0; code < mc_code_count_; ++code) {
		const char* const candidate = _mc_code_texts[code];
		if (strlen(text) != strlen(candidate)) continue;
		bool equal = true;
		for (size_t i = 0; candidate[i] != '\0'; ++i) {
			if (toupper((unsigned char)text[i]) != candidate[i]) equal = false;
		}
		if (equal) return (mc_code_e)code;
	}
	return mc_code_count_;
}

static bool _mc_has_span(const mc_span_s span) {
	return span.file != UINT32_MAX;
}

static mc_diagnostic_s* _mc_report(mc_diag_s* const diag, const mc_level_e level, const mc_code_e code,
								   const mc_span_s span, const char* const format, va_list args) mc_printf_like(5, 0);

static mc_diagnostic_s* _mc_report(mc_diag_s* const diag, const mc_level_e level, const mc_code_e code,
								   const mc_span_s span, const char* const format, va_list args) {
	mc_assert(diag != NULL);
	mc_diagnostic_s* const diagnostic = mc_calloc(1, sizeof(*diagnostic));
	diagnostic->level   = level;
	diagnostic->code    = code;
	diagnostic->message = mc_vformat(format, args);
	if (_mc_has_span(span)) {
		const mc_label_s label = { .span = span, .primary = true, .message = mc_strdup(""), };
		mc_vector_push(&diagnostic->labels, label);
	}
	// generic bodies are checked once per instantiation; a mistake in one is
	// reported once.
	for (size_t i = 0; i < diag->items.count; ++i) {
		const mc_diagnostic_s* const other = diag->items.data[i];
		const bool other_span = other->labels.count > 0 && other->labels.data[0].primary;
		const bool same_span = (other_span == _mc_has_span(span)) && (!other_span
			|| (other->labels.data[0].span.file == span.file && other->labels.data[0].span.begin == span.begin));
		if (other->code == code && same_span && 0 == strcmp(other->message, diagnostic->message)) {
			mc_vector_push(&diag->discarded, diagnostic);
			return diagnostic;
		}
	}
	mc_vector_push(&diag->items, diagnostic);
	if (mc_level_warning == level) diag->warning_count++;
	else diag->error_count++;
	return diagnostic;
}

mc_diagnostic_s* mc_error(mc_diag_s* const diag, const mc_code_e code, const mc_span_s span,
						  const char* const format, ...) {
	va_list args; va_start(args, format);
	mc_diagnostic_s* const diagnostic = _mc_report(diag, mc_level_error, code, span, format, args);
	va_end(args);
	return diagnostic;
}

mc_diagnostic_s* mc_warning(mc_diag_s* const diag, const mc_code_e code, const mc_span_s span,
							const char* const format, ...) {
	va_list args; va_start(args, format);
	mc_diagnostic_s* const diagnostic = _mc_report(diag, mc_level_warning, code, span, format, args);
	va_end(args);
	return diagnostic;
}

mc_diagnostic_s* mc_runtime_error(mc_diag_s* const diag, const mc_span_s span, const char* const format, ...) {
	va_list args; va_start(args, format);
	mc_diagnostic_s* const diagnostic = _mc_report(diag, mc_level_runtime, mc_code_count_, span, format, args);
	va_end(args);
	return diagnostic;
}

mc_diagnostic_s* mc_label_main(mc_diagnostic_s* const diagnostic, const char* const format, ...) {
	for (size_t i = 0; i < diagnostic->labels.count; ++i) {
		if (!diagnostic->labels.data[i].primary) continue;
		va_list args; va_start(args, format);
		mc_free(diagnostic->labels.data[i].message);
		diagnostic->labels.data[i].message = mc_vformat(format, args);
		va_end(args);
		break;
	}
	return diagnostic;
}

mc_diagnostic_s* mc_label(mc_diagnostic_s* const diagnostic, const mc_span_s span, const char* const format, ...) {
	if (!_mc_has_span(span)) return diagnostic;
	va_list args; va_start(args, format);
	const mc_label_s label = { .span = span, .primary = false, .message = mc_vformat(format, args), };
	va_end(args);
	mc_vector_push(&diagnostic->labels, label);
	return diagnostic;
}

static mc_diagnostic_s* _mc_footer(mc_diagnostic_s* const diagnostic, const bool help,
								   const char* const format, va_list args) mc_printf_like(3, 0);

static mc_diagnostic_s* _mc_footer(mc_diagnostic_s* const diagnostic, const bool help,
								   const char* const format, va_list args) {
	const mc_footer_s footer = { .help = help, .message = mc_vformat(format, args), };
	mc_vector_push(&diagnostic->footers, footer);
	return diagnostic;
}

mc_diagnostic_s* mc_note(mc_diagnostic_s* const diagnostic, const char* const format, ...) {
	va_list args; va_start(args, format);
	_mc_footer(diagnostic, false, format, args);
	va_end(args);
	return diagnostic;
}

mc_diagnostic_s* mc_help(mc_diagnostic_s* const diagnostic, const char* const format, ...) {
	va_list args; va_start(args, format);
	_mc_footer(diagnostic, true, format, args);
	va_end(args);
	return diagnostic;
}

bool mc_diag_has_errors(const mc_diag_s* const diag) {
	return diag->error_count > 0;
}

static void _mc_diagnostic_free(mc_diagnostic_s* const diagnostic) {
	for (size_t i = 0; i < diagnostic->labels.count; ++i) mc_free(diagnostic->labels.data[i].message);
	for (size_t i = 0; i < diagnostic->footers.count; ++i) mc_free(diagnostic->footers.data[i].message);
	mc_vector_free(&diagnostic->labels);
	mc_vector_free(&diagnostic->footers);
	mc_free(diagnostic->message);
	mc_free(diagnostic);
}

void mc_diag_free(mc_diag_s* const diag) {
	for (size_t i = 0; i < diag->items.count; ++i) _mc_diagnostic_free(diag->items.data[i]);
	for (size_t i = 0; i < diag->discarded.count; ++i) _mc_diagnostic_free(diag->discarded.data[i]);
	mc_vector_free(&diag->items);
	mc_vector_free(&diag->discarded);
	diag->error_count = diag->warning_count = 0;
}

// --- rendering ---------------------------------------------------------------

typedef struct {
	const mc_diag_s*       diag      ;
	const mc_diagnostic_s* diagnostic;
	mc_string_s*           out       ;
	bool                   color     ;
	size_t                 width     ;   // of the line number gutter
} _mc_renderer_s;

static void _mc_paint(const _mc_renderer_s* const r, const char* const color) {
	if (r->color) mc_string_append_cstr(r->out, color);
}

static const char* _mc_level_color(const mc_level_e level) {
	return (mc_level_warning == level) ? _mc_yellow : _mc_red;
}

static size_t _mc_digits(uint32_t number) {
	size_t digits = 1;
	while (number >= 10) {
		number /= 10;
		++digits;
	}
	return digits;
}

/// the screen width of 'text': code points, tabs expanded.
static size_t _mc_display_width(const mc_view_s text) {
	size_t width = 0;
	for (size_t i = 0; i < text.length; ++i) {
		if ('\t' == text.data[i]) width += _mc_tab_width;
		else if (0x80 != ((uint8_t)text.data[i] & 0xC0)) ++width;
	}
	return width;
}

static void _mc_gutter(const _mc_renderer_s* const r, const char* const tail) {
	_mc_paint(r, _mc_blue);
	mc_string_append_repeat(r->out, ' ', r->width);
	mc_string_append_cstr(r->out, tail);
	_mc_paint(r, _mc_reset);
}

static void _mc_source_line(const _mc_renderer_s* const r, const mc_source_s* const source, const uint32_t line) {
	_mc_paint(r, _mc_blue);
	mc_string_appendf(r->out, "%*u |", (int)r->width, line);
	_mc_paint(r, _mc_reset);
	const mc_view_s text = mc_source_line(source, line);
	if (text.length > 0) mc_string_append_char(r->out, ' ');
	for (size_t i = 0; i < text.length; ++i) {
		if ('\t' == text.data[i]) mc_string_append_repeat(r->out, ' ', _mc_tab_width);
		else mc_string_append_char(r->out, text.data[i]);
	}
	mc_string_append_char(r->out, '\n');
}

/// one label as it appears on one line: columns on screen.
typedef struct {
	const mc_label_s* label  ;
	size_t            begin  ;
	size_t            end    ;   // exclusive, at least begin + 1
	bool              message;   // whether its message is shown on this line
} _mc_mark_s;

static int _mc_mark_compare(const void* const a, const void* const b) {
	const _mc_mark_s* const left = a;
	const _mc_mark_s* const right = b;
	if (left->begin != right->begin) return (left->begin < right->begin) ? -1 : 1;
	return (left->label->primary == right->label->primary) ? 0 : (left->label->primary ? -1 : 1);
}

static const char* _mc_mark_color(const _mc_renderer_s* const r, const mc_label_s* const label) {
	return label->primary ? _mc_level_color(r->diagnostic->level) : _mc_blue;
}

/// the underline rows below one source line: all markers on the first row,
/// the message of the rightmost label next to them, the other messages
/// hanging below on connectors.
static void _mc_underlines(const _mc_renderer_s* const r, _mc_mark_s* const marks, const size_t count) {
	qsort(marks, count, sizeof(*marks), _mc_mark_compare);
	size_t width = 0;
	for (size_t i = 0; i < count; ++i) if (marks[i].end > width) width = marks[i].end;

	// the markers row.
	_mc_gutter(r, " | ");
	size_t column = 0;
	const char* painted = NULL;   // the color of the run of markers being written
	for (size_t position = 0; position < width; ++position) {
		const _mc_mark_s* owner = NULL;
		for (size_t i = 0; i < count; ++i) {
			if (position >= marks[i].begin && position < marks[i].end && (NULL == owner || marks[i].label->primary)) owner = &marks[i];
		}
		if (NULL == owner) continue;
		const char* const color = _mc_mark_color(r, owner->label);
		if (position > column || color != painted) {
			if (painted != NULL) _mc_paint(r, _mc_reset);
			painted = NULL;
		}
		mc_string_append_repeat(r->out, ' ', position - column);
		if (NULL == painted) {
			_mc_paint(r, color);
			painted = color;
		}
		mc_string_append_char(r->out, owner->label->primary ? '^' : '-');
		column = position + 1;
	}
	if (painted != NULL) _mc_paint(r, _mc_reset);
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
			_mc_paint(r, _mc_mark_color(r, marks[hanging].label));
			mc_string_appendf(r->out, " %s", marks[hanging].label->message);
			_mc_paint(r, _mc_reset);
			marks[hanging].message = false;
		}
	}
	mc_string_append_char(r->out, '\n');

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
			_mc_gutter(r, " | ");
			size_t at = 0;
			for (size_t i = 0; i <= next; ++i) {
				if (!(marks[i].message && marks[i].label->message[0] != '\0')) continue;
				if (marks[i].begin < at) continue;
				mc_string_append_repeat(r->out, ' ', marks[i].begin - at);
				_mc_paint(r, _mc_mark_color(r, marks[i].label));
				if (1 == pass && i == next) {
					mc_string_append_cstr(r->out, marks[i].label->message);
					at = marks[i].begin + strlen(marks[i].label->message);
				}
				else {
					mc_string_append_char(r->out, '|');
					at = marks[i].begin + 1;
				}
				_mc_paint(r, _mc_reset);
			}
			mc_string_append_char(r->out, '\n');
		}
		marks[next].message = false;
	}
}

/// the source lines of 'source' that labels point at, with their underlines.
static void _mc_snippet(const _mc_renderer_s* const r, const mc_source_s* const source) {
	const mc_diagnostic_s* const diagnostic = r->diagnostic;
	uint32_t previous = 0;
	for (;;) {
		// the next line after 'previous' where a label starts or ends.
		uint32_t line = UINT32_MAX;
		for (size_t i = 0; i < diagnostic->labels.count; ++i) {
			const mc_label_s* const label = &diagnostic->labels.data[i];
			if (label->span.file != source->id) continue;
			const uint32_t first = mc_source_location(source, label->span.begin).line;
			const uint32_t last = mc_source_location(source, label->span.end > label->span.begin ? label->span.end - 1 : label->span.begin).line;
			if (first > previous && first < line) line = first;
			if (last != first && last > previous && last < line) line = last;
		}
		if (UINT32_MAX == line) return;
		if (previous > 0 && line > previous + 1) {
			_mc_paint(r, _mc_blue);
			mc_string_append_repeat(r->out, ' ', r->width > 1 ? r->width - 1 : 0);
			mc_string_append_cstr(r->out, "...\n");
			_mc_paint(r, _mc_reset);
		}
		_mc_source_line(r, source, line);

		const mc_view_s text = mc_source_line(source, line);
		const uint32_t start = mc_source_line_start(source, line);
		mc_vector_of(_mc_mark_s) marks = {0};
		for (size_t i = 0; i < diagnostic->labels.count; ++i) {
			const mc_label_s* const label = &diagnostic->labels.data[i];
			if (label->span.file != source->id) continue;
			const uint32_t last_offset = label->span.end > label->span.begin ? label->span.end - 1 : label->span.begin;
			const uint32_t first = mc_source_location(source, label->span.begin).line;
			const uint32_t last = mc_source_location(source, last_offset).line;
			if (first != line && last != line) continue;
			// a span over several lines: underline the rest of its first line
			// and the start of its last line; the message goes on the last.
			const size_t from = (first == line) ? label->span.begin - start : 0;
			size_t to = (last == line) ? (size_t)label->span.end - start : text.length;
			if (to > text.length) to = text.length;
			const size_t begin = _mc_display_width(mc_view_slice(text, 0, from));
			size_t end = _mc_display_width(mc_view_slice(text, 0, to > from ? to : from));
			if (end <= begin) end = begin + 1;
			const _mc_mark_s mark = { .label = label, .begin = begin, .end = end, .message = (last == line), };
			mc_vector_push(&marks, mark);
		}
		_mc_underlines(r, marks.data, marks.count);
		mc_vector_free(&marks);
		previous = line;
	}
}

/// 'error[E0502]', 'warning[W0503]' or 'runtime error'.
static void _mc_header(const _mc_renderer_s* const r) {
	const mc_diagnostic_s* const diagnostic = r->diagnostic;
	if (mc_level_runtime == diagnostic->level) mc_string_append_cstr(r->out, "runtime error");
	else mc_string_appendf(r->out, "%s[%s]", mc_level_error == diagnostic->level ? "error" : "warning", mc_code_text(diagnostic->code));
}

static void _mc_render_human(const _mc_renderer_s* const r) {
	const mc_diagnostic_s* const diagnostic = r->diagnostic;
	_mc_paint(r, _mc_level_color(diagnostic->level));
	_mc_header(r);
	_mc_paint(r, _mc_reset);
	_mc_paint(r, _mc_bold);
	mc_string_appendf(r->out, ": %s", diagnostic->message);
	_mc_paint(r, _mc_reset);
	mc_string_append_char(r->out, '\n');

	const mc_sources_s* const sources = r->diag->sources;
	const mc_label_s* primary = NULL;
	for (size_t i = 0; i < diagnostic->labels.count; ++i) {
		if (diagnostic->labels.data[i].primary) primary = &diagnostic->labels.data[i];
	}
	if (NULL == primary && diagnostic->labels.count > 0) primary = &diagnostic->labels.data[0];

	if (primary != NULL && sources != NULL) {
		// the files with labels: the primary label's first.
		uint32_t files[64];
		size_t file_count = 0;
		files[file_count++] = primary->span.file;
		for (size_t i = 0; i < diagnostic->labels.count && file_count < mc_count_of(files); ++i) {
			bool seen = false;
			for (size_t j = 0; j < file_count; ++j) seen = seen || files[j] == diagnostic->labels.data[i].span.file;
			if (!seen) files[file_count++] = diagnostic->labels.data[i].span.file;
		}
		for (size_t f = 0; f < file_count; ++f) {
			const mc_source_s* const source = mc_sources_get(sources, files[f]);
			const mc_label_s* anchor = (0 == f) ? primary : NULL;
			for (size_t i = 0; i < diagnostic->labels.count && NULL == anchor; ++i) {
				if (diagnostic->labels.data[i].span.file == files[f]) anchor = &diagnostic->labels.data[i];
			}
			if (NULL == anchor) continue;   // (every file listed has a label)
			const mc_location_s location = mc_source_location(source, anchor->span.begin);
			_mc_gutter(r, (0 == f) ? "--> " : "::: ");
			mc_string_appendf(r->out, "%s:%u:%u\n", source->path, location.line, location.column);
			_mc_gutter(r, " |\n");
			_mc_snippet(r, source);
		}
		if (diagnostic->footers.count > 0) _mc_gutter(r, " |\n");
	}
	for (size_t i = 0; i < diagnostic->footers.count; ++i) {
		const mc_footer_s* const footer = &diagnostic->footers.data[i];
		_mc_gutter(r, " = ");
		_mc_paint(r, footer->help ? _mc_cyan : _mc_bold);
		mc_string_append_cstr(r->out, footer->help ? "help" : "note");
		_mc_paint(r, _mc_reset);
		mc_string_appendf(r->out, ": %s\n", footer->message);
	}
}

static void _mc_render_short(const _mc_renderer_s* const r) {
	const mc_diagnostic_s* const diagnostic = r->diagnostic;
	const mc_sources_s* const sources = r->diag->sources;
	for (size_t i = 0; i < diagnostic->labels.count; ++i) {
		const mc_label_s* const label = &diagnostic->labels.data[i];
		if (!label->primary || NULL == sources) continue;
		const mc_source_s* const source = mc_sources_get(sources, label->span.file);
		const mc_location_s location = mc_source_location(source, label->span.begin);
		mc_string_appendf(r->out, "%s:%u:%u: ", source->path, location.line, location.column);
		break;
	}
	_mc_paint(r, _mc_level_color(diagnostic->level));
	_mc_header(r);
	_mc_paint(r, _mc_reset);
	mc_string_appendf(r->out, ": %s\n", diagnostic->message);
}

void mc_diag_render(const mc_diag_s* const diag, const mc_diagnostic_s* const diagnostic,
					const mc_diag_format_e format, const bool color, mc_string_s* const out) {
	_mc_renderer_s renderer = { .diag = diag, .diagnostic = diagnostic, .out = out, .color = color, .width = 1, };
	for (size_t i = 0; i < diagnostic->labels.count && diag->sources != NULL; ++i) {
		const mc_label_s* const label = &diagnostic->labels.data[i];
		const mc_source_s* const source = mc_sources_get(diag->sources, label->span.file);
		const uint32_t last = label->span.end > label->span.begin ? label->span.end - 1 : label->span.begin;
		const size_t digits = _mc_digits(mc_source_location(source, last).line);
		if (digits > renderer.width) renderer.width = digits;
	}
	if (mc_diag_format_short == format) _mc_render_short(&renderer);
	else _mc_render_human(&renderer);
}

void mc_diag_print(const mc_diag_s* const diag, FILE* const stream, const mc_diag_format_e format, const bool color) {
	mc_string_s out = {0};
	for (size_t i = 0; i < diag->items.count; ++i) {
		mc_diag_render(diag, diag->items.data[i], format, color, &out);
		if (mc_diag_format_human == format) mc_string_append_char(&out, '\n');
	}
	(void)fputs(mc_string_cstr(&out), stream);
	mc_string_free(&out);
}
