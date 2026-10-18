
#ifndef __mia__diag_h__
#define __mia__diag_h__

/// diagnostics: errors and warnings with labelled source spans, notes and
/// help, rendered as annotated source snippets (or as one line each).

#include "mia/source.h"
#include "mia/span.h"
#include "mia/string.h"
#include "mia/vector.h"

#include <stdio.h>

typedef enum {
#define mia_diag_code(_code, _name, _summary) mia_diag_##_code,
#include "mia/diagnostics.def"
#undef mia_diag_code
	mia_diag_code_count,
} mia_diag_code_e;

typedef enum {
	mia_diag_level_error  ,
	mia_diag_level_warning,
	mia_diag_level_runtime,   // a program stopped while running (no code)
} mia_diag_level_e;

typedef struct {
	mia_span_s span   ;
	bool       primary;
	char*      message;
} mia_diag_label_s;

typedef struct {
	bool  help   ;
	char* message;
} mia_diag_footer_s;

typedef struct {
	mia_diag_level_e                 level  ;
	mia_diag_code_e                  code   ;
	char*                            message;
	mia_vector_of(mia_diag_label_s)  labels ;
	mia_vector_of(mia_diag_footer_s) footers;
} mia_diag_entry_s;

typedef enum {
	mia_diag_format_human,   // headers, annotated snippets, notes
	mia_diag_format_short,   // one line each: path:line:column: error[E0502]: message
} mia_diag_format_e;

typedef struct {
	const mia_source_table_s*        sources      ;
	mia_vector_of(mia_diag_entry_s*) items        ;
	mia_vector_of(mia_diag_entry_s*) discarded    ;   // duplicates, freed with the rest
	size_t                           error_count  ;
	size_t                           warning_count;
} mia_diag_s;

/// a code as text ("E0502"), its name ("type_mismatch") and its summary (what 'mia explain' shows).
const char* mia_diag_code_text(const mia_diag_code_e code);

const char* mia_diag_code_name(const mia_diag_code_e code);

const char* mia_diag_code_summary(const mia_diag_code_e code);

/// the code named "E0502" (or "e0502"), or mia_diag_code_count.
mia_diag_code_e mia_diag_code_parse(const char* const text);

/// reports an error at 'span' (mia_span_none for none). the same code, message
/// and position are reported once; later duplicates are discarded silently.
mia_diag_entry_s* mia_diag_error(mia_diag_s* const diag, const mia_diag_code_e code, const mia_span_s span,
								 const char* const format, ...) mia_printf_like(4, 5);

/// the same as a warning.
mia_diag_entry_s* mia_diag_warning(mia_diag_s* const diag, const mia_diag_code_e code, const mia_span_s span,
								   const char* const format, ...) mia_printf_like(4, 5);

/// a runtime error: rendered as 'runtime error: message' (no code).
mia_diag_entry_s* mia_diag_runtime_error(mia_diag_s* const diag, const mia_span_s span, const char* const format, ...) mia_printf_like(3, 4);

/// the message under the primary span.
mia_diag_entry_s* mia_diag_label_main(mia_diag_entry_s* const diagnostic, const char* const format, ...) mia_printf_like(2, 3);

/// a secondary span with a message.
mia_diag_entry_s* mia_diag_label(mia_diag_entry_s* const diagnostic, const mia_span_s span, const char* const format, ...) mia_printf_like(3, 4);

mia_diag_entry_s* mia_diag_note(mia_diag_entry_s* const diagnostic, const char* const format, ...) mia_printf_like(2, 3);

mia_diag_entry_s* mia_diag_help(mia_diag_entry_s* const diagnostic, const char* const format, ...) mia_printf_like(2, 3);

bool mia_diag_has_errors(const mia_diag_s* const diag);

/// appends one diagnostic, rendered, to 'out'.
void mia_diag_render(const mia_diag_s* const diag, const mia_diag_entry_s* const diagnostic,
					 const mia_diag_format_e format, const bool color, mia_string_s* const out);

/// writes every diagnostic to 'stream' (warnings as errors when 'werror').
void mia_diag_print(const mia_diag_s* const diag, FILE* const stream, const mia_diag_format_e format, const bool color);

void mia_diag_free(mia_diag_s* const diag);

#endif
