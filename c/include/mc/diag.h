
#ifndef __mc__diag_h__
#define __mc__diag_h__

/// diagnostics: errors and warnings with labelled source spans, notes and
/// help, rendered as annotated source snippets (or as one line each).

#include "mc/source.h"
#include "mc/string.h"
#include "mc/vector.h"

#include <stdio.h>

typedef enum {
#define mc_diagnostic(_code, _name, _summary) mc_##_code,
#include "mc/diagnostics.def"
#undef mc_diagnostic
	mc_code_count_,
} mc_code_e;

typedef enum {
	mc_level_error  ,
	mc_level_warning,
	mc_level_runtime,   // a program stopped while running (no code)
} mc_level_e;

typedef struct {
	mc_span_s span   ;
	bool      primary;
	char*     message;
} mc_label_s;

typedef struct {
	bool  help   ;
	char* message;
} mc_footer_s;

typedef struct {
	mc_level_e                   level  ;
	mc_code_e                    code   ;
	char*                        message;
	mc_vector_of(mc_label_s)     labels ;
	mc_vector_of(mc_footer_s)    footers;
} mc_diagnostic_s;

typedef enum {
	mc_diag_format_human,   // headers, annotated snippets, notes
	mc_diag_format_short,   // one line each: path:line:column: error[E0502]: message
} mc_diag_format_e;

typedef struct {
	const mc_sources_s*            sources      ;
	mc_vector_of(mc_diagnostic_s*) items        ;
	mc_vector_of(mc_diagnostic_s*) discarded    ;   // duplicates, freed with the rest
	size_t                         error_count  ;
	size_t                         warning_count;
} mc_diag_s;

/// a span that points nowhere (diagnostics about the command line).
extern const mc_span_s mc_no_span;

const char* mc_code_text(const mc_code_e code);

const char* mc_code_name(const mc_code_e code);

const char* mc_code_summary(const mc_code_e code);

/// the code named "E0502" (or "e0502"), or mc_code_count_.
mc_code_e mc_code_parse(const char* const text);

/// reports an error at 'span' (mc_no_span for none). the same code, message
/// and position are reported once; later duplicates are discarded silently.
mc_diagnostic_s* mc_error(mc_diag_s* const diag, const mc_code_e code, const mc_span_s span,
						  const char* const format, ...) mc_printf_like(4, 5);

mc_diagnostic_s* mc_warning(mc_diag_s* const diag, const mc_code_e code, const mc_span_s span,
							const char* const format, ...) mc_printf_like(4, 5);

/// a runtime error: rendered as 'runtime error: message' (no code).
mc_diagnostic_s* mc_runtime_error(mc_diag_s* const diag, const mc_span_s span, const char* const format, ...) mc_printf_like(3, 4);

/// the message under the primary span.
mc_diagnostic_s* mc_label_main(mc_diagnostic_s* const diagnostic, const char* const format, ...) mc_printf_like(2, 3);

/// a secondary span with a message.
mc_diagnostic_s* mc_label(mc_diagnostic_s* const diagnostic, const mc_span_s span, const char* const format, ...) mc_printf_like(3, 4);

mc_diagnostic_s* mc_note(mc_diagnostic_s* const diagnostic, const char* const format, ...) mc_printf_like(2, 3);

mc_diagnostic_s* mc_help(mc_diagnostic_s* const diagnostic, const char* const format, ...) mc_printf_like(2, 3);

bool mc_diag_has_errors(const mc_diag_s* const diag);

/// appends one diagnostic, rendered, to 'out'.
void mc_diag_render(const mc_diag_s* const diag, const mc_diagnostic_s* const diagnostic,
					const mc_diag_format_e format, const bool color, mc_string_s* const out);

/// writes every diagnostic to 'stream' (warnings as errors when 'werror').
void mc_diag_print(const mc_diag_s* const diag, FILE* const stream, const mc_diag_format_e format, const bool color);

void mc_diag_free(mc_diag_s* const diag);

#endif
