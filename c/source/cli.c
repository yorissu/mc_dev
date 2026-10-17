
/// the command line: 'mc <command> [options] <file>'.
///
/// every command is a row of _mc_commands: its name, a one-line summary, the
/// options it takes and the function that runs it. a new command is a new
/// function and a new row.

#include "mc/bytecode.h"
#include "mc/cli.h"
#include "mc/compiler.h"
#include "mc/diag.h"
#include "mc/format.h"
#include "mc/parser.h"
#include "mc/platform.h"
#include "mc/plugins.h"
#include "mc/printer.h"
#include "mc/program.h"
#include "mc/vm.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define mc_version "0.1.0"

enum {
	mc_status_ok      = 0,
	mc_status_error   = 1,
	mc_status_usage   = 2,
	mc_status_runtime = 101,
};

typedef struct {
	const char*                 command ;
	const char*                 path    ;
	mc_vector_of(const char*)   include ;
	mc_vector_of(const char*)   plugins ;   // --plugin and --plugin-dir, in order
	mc_vector_of(const char*)   arguments;  // run: the program's command line (the file first)
	const char*                 stdlib  ;
	const char*                 output  ;
	mc_diag_format_e            format  ;
	bool                        color   ;
	bool                        werror  ;
	bool                        stats   ;
	uint64_t                    limit   ;
} _mc_options_s;

/// what every command shares: the files read and the diagnostics about them.
typedef struct {
	_mc_options_s* options;
	mc_sources_s   sources;
	mc_diag_s      diag   ;
	mc_arena_s     arena  ;
	mc_plugins_s   plugins;
} _mc_session_s;

typedef int (*_mc_command_f)(_mc_session_s* const session);

typedef struct {
	const char*   name   ;
	const char*   summary;
	const char*   details;   // the options it takes, more about it
	_mc_command_f run    ;
	bool          file   ;   // takes a file
} _mc_command_s;

// --- reporting -----------------------------------------------------------------------------

/// prints the diagnostics and, in the human format, a summary line.
static void _mc_report(_mc_session_s* const s) {
	mc_diag_print(&s->diag, stderr, s->options->format, s->options->color);
	const size_t errors = s->diag.error_count, warnings = s->diag.warning_count;
	if (mc_diag_format_human == s->options->format && (errors > 0 || warnings > 0)) {
		(void)fprintf(stderr, "%zu error(s), %zu warning(s)\n", errors, warnings);
	}
}

/// the exit status after the diagnostics: 1 with errors (or warnings under --werror).
static int _mc_status(const _mc_session_s* const s) {
	if (s->diag.error_count > 0) return mc_status_error;
	if (s->options->werror && s->diag.warning_count > 0) return mc_status_error;
	return mc_status_ok;
}

static const char* _mc_token_category(const mc_token_kind_e kind) {
	switch (kind) {
		case mc_token_eof:        return "eof";
		case mc_token_identifier: return "identifier";
		case mc_token_type:       return "type";
		case mc_token_integer:    return "integer";
		case mc_token_float:      return "float";
		case mc_token_char:       return "char";
		case mc_token_string:     return "string";
		default: break;
	}
	return (kind <= mc_token_assert) ? "keyword" : "punctuator";
}

// --- commands ------------------------------------------------------------------------------

static const mc_source_s* _mc_read(_mc_session_s* const s) {
	const mc_source_s* const source = mc_sources_load(&s->sources, s->options->path);
	if (NULL == source) {
		mc_help(mc_error(&s->diag, mc_E0601, mc_no_span, "cannot read `%s`", s->options->path), "check the path and that the file is readable");
	}
	return source;
}

static int _mc_tokens(_mc_session_s* const s) {
	const mc_source_s* const source = _mc_read(s);
	if (source != NULL) {
		mc_tokens_s tokens = {0};
		mc_lex(source, &s->arena, &s->diag, &tokens);
		mc_string_s out = {0};
		for (size_t i = 0; i < tokens.count; ++i) {
			const mc_token_s* const token = &tokens.data[i];
			const mc_location_s at = mc_source_location(source, token->span.begin);
			mc_string_appendf(&out, "%u:%u %s ", at.line, at.column, _mc_token_category(token->kind));
			mc_format_repr_text(&out, token->text);
			switch (token->kind) {
				case mc_token_integer:
				case mc_token_char:
					mc_string_append_char(&out, ' ');
					mc_format_uint128(&out, token->integer);
					break;
				case mc_token_float:
					mc_string_append_char(&out, ' ');
					mc_format_float_repr(&out, token->real);
					break;
				case mc_token_string:
					mc_string_append_cstr(&out, " b");
					mc_format_repr_bytes(&out, token->bytes);
					break;
				default:
					break;
			}
			const bool number = mc_token_integer == token->kind || mc_token_float == token->kind;
			if (number && token->suffix != mc_scalar_none_) mc_string_appendf(&out, " suffix=%s", mc_scalar_name(token->suffix));
			else if (mc_token_string == token->kind && token->c) mc_string_append_cstr(&out, " suffix=c");
			mc_string_append_char(&out, '\n');
		}
		(void)fputs(mc_string_cstr(&out), stdout);
		mc_string_free(&out);
		mc_vector_free(&tokens);
	}
	_mc_report(s);
	return _mc_status(s);
}

static int _mc_ast(_mc_session_s* const s) {
	const mc_source_s* const source = _mc_read(s);
	if (source != NULL) {
		mc_string_s out = {0};
		mc_print_tree(mc_parse(source, &s->arena, &s->diag), NULL, &out);
		(void)fputs(mc_string_cstr(&out), stdout);
		mc_string_free(&out);
	}
	_mc_report(s);
	return _mc_status(s);
}

static mc_program_s* _mc_check_program(_mc_session_s* const s) {
	const mc_check_options_s options = {
		.include = s->options->include.data, .include_count = s->options->include.count, .stdlib = s->options->stdlib, .plugins = &s->plugins,
	};
	return mc_check(s->options->path, &s->sources, &s->diag, &options);
}

static int _mc_check(_mc_session_s* const s) {
	mc_program_free(_mc_check_program(s));
	_mc_report(s);
	return _mc_status(s);
}

static int _mc_mono(_mc_session_s* const s) {
	mc_program_s* const program = _mc_check_program(s);
	if (!mc_diag_has_errors(&s->diag)) {
		mc_string_s out = {0};
		mc_program_text(program, &out);
		(void)fputs(mc_string_cstr(&out), stdout);
		mc_string_free(&out);
	}
	mc_program_free(program);
	_mc_report(s);
	return _mc_status(s);
}

static bool _mc_ends_with(const char* const text, const char* const suffix) {
	const size_t length = strlen(text), size = strlen(suffix);
	return length >= size && 0 == strcmp(text + length - size, suffix);
}

/// the bytecode of the program: compiled from a '.mc' file, or loaded from a '.mcb' file.
static bool _mc_module(_mc_session_s* const s, mc_module_bc_s* const module) {
	const char* const path = s->options->path;
	if (_mc_ends_with(path, ".mcb")) {
		char* error = NULL;
		if (mc_module_bc_load(path, module, &error)) return true;
		mc_error(&s->diag, mc_E0601, mc_no_span, "cannot load `%s`: %s", path, error != NULL ? error : "unreadable");
		mc_free(error);
		return false;
	}
	mc_program_s* const program = _mc_check_program(s);
	bool compiled = false;
	if (!mc_diag_has_errors(&s->diag)) {
		char* error = NULL;
		compiled = mc_compile(program, module, &error);
		if (!compiled) {
			mc_note(mc_error(&s->diag, mc_E0602, mc_no_span, "cannot compile: %s", error),
				"this is a bug of the compiler: the checker accepted the program");
			mc_free(error);
		}
	}
	mc_program_free(program);
	return compiled;
}

static int _mc_build(_mc_session_s* const s) {
	mc_module_bc_s module = {0};
	if (_mc_module(s, &module)) {
		char* target = NULL;
		if (s->options->output != NULL) {
			target = mc_strdup(s->options->output);
		}
		else {
			const char* const path = s->options->path;
			const size_t length = strlen(path) - (_mc_ends_with(path, ".mc") ? 3 : 0);
			target = mc_format("%.*s.mcb", (int)length, path);
		}
		if (!mc_module_bc_save(&module, target)) mc_error(&s->diag, mc_E0601, mc_no_span, "cannot write `%s`", target);
		mc_free(target);
	}
	mc_module_bc_free(&module);
	_mc_report(s);
	return _mc_status(s);
}

static int _mc_dis(_mc_session_s* const s) {
	mc_module_bc_s module = {0};
	if (_mc_module(s, &module)) {
		mc_string_s out = {0};
		mc_disassemble(&module, &out);
		(void)fputs(mc_string_cstr(&out), stdout);
		mc_string_free(&out);
	}
	mc_module_bc_free(&module);
	_mc_report(s);
	return _mc_status(s);
}

/// the span of a bytecode position (a column and a length in code points).
static bool _mc_position_span(_mc_session_s* const s, const char* const path, const mc_position_s position, mc_span_s* const span) {
	const mc_source_s* source = NULL;
	for (size_t i = 0; i < s->sources.files.count && NULL == source; ++i) {
		if (0 == strcmp(s->sources.files.data[i]->path, path)) source = s->sources.files.data[i];
	}
	if (NULL == source) source = mc_sources_load(&s->sources, path);
	if (NULL == source || 0 == position.line || position.line > source->lines.count) return false;
	const mc_view_s line = mc_source_line(source, position.line);
	const uint32_t start = mc_source_line_start(source, position.line);
	size_t begin = 0, points = 1;
	while (begin < line.length && points < position.column) {
		++begin;
		while (begin < line.length && 0x80 == ((uint8_t)line.data[begin] & 0xC0)) ++begin;
		++points;
	}
	size_t end = begin;
	for (uint32_t taken = 0; taken < position.length && end < line.length; ++taken) {
		++end;
		while (end < line.length && 0x80 == ((uint8_t)line.data[end] & 0xC0)) ++end;
	}
	*span = (mc_span_s) { .file = source->id, .begin = start + (uint32_t)begin, .end = start + (uint32_t)(end > begin ? end : begin + 1), };
	return true;
}

/// advice for the runtime errors that have a usual cause.
static const struct {
	const char* prefix;
	const char* help  ;
} _mc_runtime_hints[] = {
	{ "use of freed heap memory", "the memory was given back by `free` or `realloc` before this access" },
	{ "use of a local array", "a slice of a local array must not outlive the call; use `alloc` for memory that outlives it" },
	{ "write through a read-only slice", "slices of constants, string literals and arrays that are not `mut` are read-only" },
	{ "stack overflow", "a function calls itself without reaching an end, or calls nest too deeply" },
	{ "stopped after", "the instruction limit (`--limit`) was reached" },
	{ "`free` of memory", "only a slice returned by `alloc` or `realloc` can be freed, and only once" },
	{ "`realloc` of memory", "only a slice returned by `alloc` or `realloc` can be resized" },
};

/// a runtime error: the source line where it happened and the calls that led there.
static void _mc_runtime_report(_mc_session_s* const s, const mc_vm_result_s* const result) {
	mc_span_s span = mc_no_span;
	const mc_trace_entry_s* const innermost = (result->trace.count > 0) ? &result->trace.data[0] : NULL;
	bool located = false;
	if (innermost != NULL && innermost->has_position) located = _mc_position_span(s, innermost->path, innermost->position, &span);
	mc_diagnostic_s* const diagnostic = mc_runtime_error(&s->diag, located ? span : mc_no_span, "%s", result->message);
	if (located) mc_label_main(diagnostic, "here");
	// the calls, innermost first; a run of the same call (deep recursion) is shown once
	for (size_t i = 0; i < result->trace.count;) {
		const mc_trace_entry_s* const entry = &result->trace.data[i];
		size_t repeats = 1;
		while (i + repeats < result->trace.count) {
			const mc_trace_entry_s* const next = &result->trace.data[i + repeats];
			const bool same = 0 == strcmp(next->function, entry->function) && next->has_position == entry->has_position
				&& (!entry->has_position || (next->position.line == entry->position.line && next->position.column == entry->position.column
					&& 0 == strcmp(next->path, entry->path)));
			if (!same || 0 == i) break;
			++repeats;
		}
		mc_string_s note = {0};
		mc_string_appendf(&note, "%s`%s`", (0 == i) ? "in " : "called from ", entry->function);
		if (entry->has_position) mc_string_appendf(&note, " at %s:%u:%u", entry->path, entry->position.line, entry->position.column);
		if (repeats > 1) mc_string_appendf(&note, " (%zu times)", repeats);
		mc_note(diagnostic, "%s", mc_string_cstr(&note));
		mc_string_free(&note);
		i += repeats;
	}
	for (size_t i = 0; i < mc_count_of(_mc_runtime_hints); ++i) {
		const char* const prefix = _mc_runtime_hints[i].prefix;
		if (0 == strncmp(result->message, prefix, strlen(prefix))) mc_help(diagnostic, "%s", _mc_runtime_hints[i].help);
	}
	mc_diag_print(&s->diag, stderr, s->options->format, s->options->color);
}

static int _mc_run(_mc_session_s* const s) {
	mc_module_bc_s module = {0};
	if (!_mc_module(s, &module)) {
		mc_module_bc_free(&module);
		_mc_report(s);
		return mc_status_error;
	}
	if (s->diag.items.count > 0) {
		// warnings: shown before the program's output
		mc_diag_print(&s->diag, stderr, s->options->format, s->options->color);
		if (mc_diag_format_human == s->options->format) (void)fprintf(stderr, "%zu error(s), %zu warning(s)\n", s->diag.error_count, s->diag.warning_count);
		if (_mc_status(s) != mc_status_ok) {
			mc_module_bc_free(&module);
			return mc_status_error;
		}
		mc_diag_free(&s->diag);
		s->diag = (mc_diag_s) { .sources = &s->sources, };
	}
	// every plugin function the program calls must be there before it starts
	for (size_t i = 0; i < module.intrinsics.count; ++i) {
		const char* const name = module.intrinsics.data[i].name;
		if (mc_plugins_find(&s->plugins, name) != NULL) continue;
		mc_help(mc_error(&s->diag, mc_E0416, mc_no_span, "no plugin provides `%s`", name), "load a plugin that provides it with `--plugin <file>`");
	}
	if (mc_diag_has_errors(&s->diag)) {
		mc_module_bc_free(&module);
		_mc_report(s);
		return mc_status_error;
	}
	const mc_vm_options_s options = {
		.out = stdout, .limit = s->options->limit, .plugins = &s->plugins,
		.arguments = s->options->arguments.data, .argument_count = s->options->arguments.count,
	};
	mc_vm_result_s result = {0};
	const bool finished = mc_vm_run(&module, &options, &result);
	(void)fflush(stdout);
	int status = mc_vm_exit_status(&result);
	if (!finished) {
		_mc_runtime_report(s, &result);
		status = mc_status_runtime;
	}
	if (s->options->stats) {
		(void)fprintf(stderr, "%llu instruction(s), %llu allocation(s), exit status %d\n", (unsigned long long)result.steps,
			(unsigned long long)result.allocations, status);
	}
	mc_vm_result_free(&result);
	mc_module_bc_free(&module);
	return status;
}

static int _mc_explain_code(const char* const text) {
	const mc_code_e code = mc_code_parse(text);
	if (mc_code_count_ == code) {
		(void)fprintf(stderr, "error[E0602]: `%s` is not a diagnostic code\n = help: `mc explain` lists them all\n", text);
		return mc_status_usage;
	}
	(void)printf("%s (%s)\n\n  %s\n", mc_code_text(code), mc_code_name(code), mc_code_summary(code));
	return mc_status_ok;
}

static int _mc_explain(_mc_session_s* const s) {
	if (s->options->path != NULL) return _mc_explain_code(s->options->path);
	for (int code = 0; code < (int)mc_code_count_; ++code) {
		(void)printf("%s  %-24s %s\n", mc_code_text((mc_code_e)code), mc_code_name((mc_code_e)code), mc_code_summary((mc_code_e)code));
	}
	return mc_status_ok;
}

#define mc_check_options                                                                       \
	"  -I <dir>            also look for modules in <dir> (after the file's own directory)\n" \
	"  --stdlib <dir>      the standard library (default: $MC_STDLIB, or the bundled one)\n" \
	"  --plugin <file>     load a plugin (a shared library); repeatable\n"                      \
	"  --plugin-dir <dir>  load every plugin in <dir>; repeatable (also: $MC_PLUGINS)\n"

static const _mc_command_s _mc_commands[] = {
	{ "tokens", "print the tokens of a file", "", _mc_tokens, true, },
	{ "ast", "print the syntax tree of a file", "", _mc_ast, true, },
	{ "check", "check a program: the file and every module it imports", mc_check_options, _mc_check, true, },
	{ "mono", "check a program and print it with every generic instantiated and every expression typed",
		mc_check_options, _mc_mono, true, },
	{ "build", "compile a program to bytecode",
		mc_check_options "  -o <file>           where to write it (default: next to the source, as .mcb)\n", _mc_build, true, },
	{ "dis", "print the bytecode of a program (.mc or .mcb)", mc_check_options, _mc_dis, true, },
	{ "run", "run a program (.mc or .mcb); exits with what main returns. what follows the file is given to\n"
		"  `main :: (arguments: [][]u8)`, the file first ('--' ends mc's own options)",
		mc_check_options
		"  --limit <n>         stop after <n> instructions\n"
		"  --stats             print how many instructions ran\n", _mc_run, true, },
	{ "explain", "explain a diagnostic code ('mc explain E0502'), or list them all", "", _mc_explain, false, },
};

// --- the command line ----------------------------------------------------------------------

static const _mc_command_s* _mc_find_command(const char* const name) {
	for (size_t i = 0; i < mc_count_of(_mc_commands); ++i) if (0 == strcmp(_mc_commands[i].name, name)) return &_mc_commands[i];
	return NULL;
}

static void _mc_help(FILE* const stream, const _mc_command_s* const command) {
	if (command != NULL) {
		(void)fprintf(stream, "mc %s: %s\n\nusage: mc %s [options] %s\n\noptions:\n%s", command->name, command->summary, command->name,
			command->file ? "<file>" : "[code]", command->details);
		(void)fprintf(stream,
			"  --format <format>   diagnostics as snippets ('human', the default) or one line each ('short')\n"
			"  --color[=<when>]    color diagnostics: 'auto' (a terminal without NO_COLOR), 'always' or 'never'\n"
			"  --no-color          the same as --color=never\n"
			"  --werror            treat warnings as errors\n");
		return;
	}
	(void)fprintf(stream, "mc %s: the compiler and virtual machine of the mc language\n\nusage: mc <command> [options] <file>\n\ncommands:\n",
		mc_version);
	for (size_t i = 0; i < mc_count_of(_mc_commands); ++i) (void)fprintf(stream, "  %-9s %s\n", _mc_commands[i].name, _mc_commands[i].summary);
	(void)fprintf(stream, "\n'mc <command> --help' shows the options of a command.\n\n"
		"exit status: 0 success, 1 errors, 2 bad usage, 101 runtime error; 'run' exits with main's result.\n");
}

static int _mc_usage(const char* const format, ...) mc_printf_like(1, 2);

static int _mc_usage(const char* const format, ...) {
	va_list args; va_start(args, format);
	(void)fprintf(stderr, "error[E0602]: ");
	(void)vfprintf(stderr, format, args);
	va_end(args);
	(void)fprintf(stderr, "\n = help: 'mc --help' lists the commands and 'mc <command> --help' their options\n");
	return mc_status_usage;
}

static bool _mc_color_auto(void) {
	const char* const no_color = getenv("NO_COLOR");
	return mc_is_terminal(stderr) && (NULL == no_color || '\0' == no_color[0]);
}

/// loads one plugin path (a file or a directory) into the session.
static bool _mc_load_plugin(_mc_session_s* const s, const char* const path) {
	char* error = NULL;
	if (mc_plugins_load(&s->plugins, path, &error)) return true;
	mc_help(mc_error(&s->diag, mc_E0603, mc_no_span, "%s", error), "a plugin is a shared library with an `mc_plugin_init` function (see mc/plugin.h)");
	mc_free(error);
	return false;
}

/// the plugins of $MC_PLUGINS (a list of files and directories), then of
/// '--plugin' and '--plugin-dir'; false (reported) when one cannot be loaded.
static bool _mc_load_plugins(_mc_session_s* const s) {
	bool loaded = true;
	const char* const list = getenv("MC_PLUGINS");
	if (list != NULL) {
		const char separator = mc_path_list_separator();
		const char* start = list;
		for (const char* cursor = list;; ++cursor) {
			if (*cursor != separator && *cursor != '\0') continue;
			if (cursor > start) {
				char* const path = mc_format("%.*s", (int)(cursor - start), start);
				loaded = _mc_load_plugin(s, path) && loaded;
				mc_free(path);
			}
			if ('\0' == *cursor) break;
			start = cursor + 1;
		}
	}
	for (size_t i = 0; i < s->options->plugins.count; ++i) loaded = _mc_load_plugin(s, s->options->plugins.data[i]) && loaded;
	if (!loaded) _mc_report(s);
	return loaded;
}

/// parses the arguments; returns -1 to go on, or the status to exit with.
static int _mc_parse(_mc_options_s* const options, const int argc, const char* const argv[]) {
	bool help = false;
	bool options_end = false;
	for (int i = 1; i < argc; ++i) {
		const char* const argument = argv[i];
		const bool has_next = i + 1 < argc;
		// 'run': what follows the file is the program's command line; '--' ends mc's options
		if (options->path != NULL && options->command != NULL && 0 == strcmp(options->command, "run")) {
			if (0 == options->arguments.count) mc_vector_push(&options->arguments, options->path);
			mc_vector_push(&options->arguments, argument);
			continue;
		}
		if (!options_end && 0 == strcmp(argument, "--")) {
			options_end = true;
			continue;
		}
		if (options_end) {
			if (NULL == options->command) options->command = argument;
			else if (NULL == options->path) options->path = argument;
			else return _mc_usage("unexpected argument `%s`", argument);
			continue;
		}
		if (0 == strcmp(argument, "-h") || 0 == strcmp(argument, "--help")) help = true;
		else if (0 == strcmp(argument, "--version")) {
			(void)printf("mc %s\n", mc_version);
			return mc_status_ok;
		}
		else if (0 == strcmp(argument, "-I") && has_next) mc_vector_push(&options->include, argv[++i]);
		else if (0 == strncmp(argument, "-I", 2) && argument[2] != '\0') mc_vector_push(&options->include, argument + 2);
		else if (0 == strcmp(argument, "--stdlib") && has_next) options->stdlib = argv[++i];
		else if ((0 == strcmp(argument, "--plugin") || 0 == strcmp(argument, "--plugin-dir")) && has_next) mc_vector_push(&options->plugins, argv[++i]);
		else if (0 == strcmp(argument, "-o") && has_next) options->output = argv[++i];
		else if (0 == strcmp(argument, "--format") && has_next) {
			const char* const format = argv[++i];
			if (0 == strcmp(format, "short")) options->format = mc_diag_format_short;
			else if (0 == strcmp(format, "human")) options->format = mc_diag_format_human;
			else return _mc_usage("unknown format `%s` (human or short)", format);
		}
		else if (0 == strcmp(argument, "--color") || 0 == strcmp(argument, "--color=always")) options->color = true;
		else if (0 == strcmp(argument, "--no-color") || 0 == strcmp(argument, "--color=never")) options->color = false;
		else if (0 == strcmp(argument, "--color=auto")) options->color = _mc_color_auto();
		else if (0 == strcmp(argument, "--werror")) options->werror = true;
		else if (0 == strcmp(argument, "--stats")) options->stats = true;
		else if (0 == strcmp(argument, "--limit") && has_next) {
			char* end = NULL;
			options->limit = strtoull(argv[++i], &end, 10);
			if (NULL == end || *end != '\0') return _mc_usage("`--limit` needs a number, not `%s`", argv[i]);
		}
		else if ('-' == argument[0] && argument[1] != '\0') {
			const bool needs_value = 0 == strcmp(argument, "-I") || 0 == strcmp(argument, "--stdlib") || 0 == strcmp(argument, "-o")
				|| 0 == strcmp(argument, "--format") || 0 == strcmp(argument, "--limit") || 0 == strcmp(argument, "--plugin")
				|| 0 == strcmp(argument, "--plugin-dir");
			if (needs_value) return _mc_usage("`%s` needs a value", argument);
			return _mc_usage("unknown option `%s`", argument);
		}
		else if (NULL == options->command) options->command = argument;
		else if (NULL == options->path) options->path = argument;
		else return _mc_usage("unexpected argument `%s`", argument);
	}
	const _mc_command_s* const command = (options->command != NULL) ? _mc_find_command(options->command) : NULL;
	if (help) {
		_mc_help(stdout, command);
		return mc_status_ok;
	}
	if (NULL == options->command) {
		_mc_help(stderr, NULL);
		return mc_status_usage;
	}
	if (NULL == command) return _mc_usage("unknown command `%s`", options->command);
	if (command->file && NULL == options->path) return _mc_usage("`mc %s` needs a file", command->name);
	if (0 == options->arguments.count && options->path != NULL) mc_vector_push(&options->arguments, options->path);
	return -1;
}

int mc_cli_main(const int argc, const char* const argv[]) {
	const char* const stdlib = getenv("MC_STDLIB");
	_mc_options_s options = {
		.stdlib = (stdlib != NULL && stdlib[0] != '\0') ? stdlib : MC_DEFAULT_STDLIB,
		.format = mc_diag_format_human,
		.color = _mc_color_auto(),
	};
	int status = _mc_parse(&options, argc, argv);
	const _mc_command_s* const command = (status < 0) ? _mc_find_command(options.command) : NULL;
	if (command != NULL) {
		_mc_session_s session = { .options = &options, };
		session.diag.sources = &session.sources;
		mc_plugins_init(&session.plugins);
		if (command->file && _mc_load_plugins(&session)) status = command->run(&session);
		else if (command->file) status = mc_status_error;
		else status = command->run(&session);
		mc_plugins_free(&session.plugins);
		mc_diag_free(&session.diag);
		mc_arena_free(&session.arena);
		mc_sources_free(&session.sources);
	}
	mc_vector_free(&options.include);
	mc_vector_free(&options.plugins);
	mc_vector_free(&options.arguments);
	return status;
}
