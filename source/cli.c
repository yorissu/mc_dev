
/// the command line: 'mia <command> [options] <file>'.
///
/// every command is a row of _mia_cli_commands: its name, a one-line summary, the
/// options it takes and the function that runs it. a new command is a new
/// function and a new row.

#include "mia/bytecode.h"
#include "mia/cli.h"
#include "mia/compiler.h"
#include "mia/diag.h"
#include "mia/format.h"
#include "mia/parser.h"
#include "mia/system.h"
#include "mia/plugins.h"
#include "mia/printer.h"
#include "mia/program.h"
#include "mia/vm.h"

#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>

#if !defined(mia_cli_version)
#	error "mia_cli_version is not defined: build with the makefile or build.bat"
#endif

enum {
	_mia_cli_status_ok      =   0,
	_mia_cli_status_error   =   1,
	_mia_cli_status_usage   =   2,
	_mia_cli_status_runtime = 101,
};

typedef struct {
	const char*                command     ;
	const char*                path        ;
	mia_vector_of(const char*) module_dirs ;   // -M, --module-dir: where else to look modules up
	mia_vector_of(const char*) plugin_dirs ;   // -P, --plugin-dir: where else to look plugins up
	mia_vector_of(const char*) arguments   ;   // run: the program's command line (the file first)
	const char*                installation;   // the folder of mia and its stdlib/ (null: unknown)
	const char*                stdlib      ;
	const char*                output      ;
	mia_diag_format_e          format      ;
	bool                       color       ;
	bool                       werror      ;
	bool                       stats       ;
	uint64_t                   limit       ;
	uint64_t                   eval_steps  ;
	uint64_t                   call_depth  ;
} _mia_cli_options_s;

/// what every command shares: the files read and the diagnostics about them.
typedef struct {
	_mia_cli_options_s* options;
	mia_source_table_s  sources;
	mia_diag_s          diag   ;
	mia_arena_s         arena  ;
	mia_plugins_s       plugins;
} _mia_cli_session_s;

typedef int (*_mia_cli_command_f)(_mia_cli_session_s* const session);

typedef struct {
	const char*        name   ;
	const char*        summary;
	const char*        details;   // the options it takes, more about it
	_mia_cli_command_f run    ;
	bool               file   ;   // takes a file
} _mia_cli_command_s;

// --- reporting -----------------------------------------------------------------------------

/// prints the diagnostics and, in the human format, a summary line.
static void _mia_cli_report(_mia_cli_session_s* const s) {
	mia_diag_print(&s->diag, stderr, s->options->format, s->options->color);
	const size_t errors = s->diag.error_count, warnings = s->diag.warning_count;
	if (mia_diag_format_human == s->options->format && (errors > 0 || warnings > 0)) {
		(void)fprintf(stderr, "%zu error(s), %zu warning(s)\n", errors, warnings);
	}
}

/// the exit status after the diagnostics: 1 with errors (or warnings under --werror).
static int _mia_cli_status(const _mia_cli_session_s* const s) {
	if (s->diag.error_count > 0) return _mia_cli_status_error;
	if (s->options->werror && s->diag.warning_count > 0) return _mia_cli_status_error;
	return _mia_cli_status_ok;
}

// --- commands ------------------------------------------------------------------------------

static const mia_source_s* _mia_cli_read(_mia_cli_session_s* const s) {
	const mia_source_s* const source = mia_source_load(&s->sources, s->options->path);
	if (NULL == source) {
		mia_diag_help(mia_diag_error(&s->diag, mia_diag_E0601, mia_span_none, "cannot read `%s`", s->options->path), "check the path and that the file is readable");
	}
	return source;
}

static int _mia_cli_ast(_mia_cli_session_s* const s) {
	const mia_source_s* const source = _mia_cli_read(s);
	if (source != NULL) {
		mia_string_s out = {0};
		mia_printer_print(mia_parser_parse(source, &s->arena, &s->diag), NULL, &out);
		(void)fputs(mia_string_cstr(&out), stdout);
		mia_string_free(&out);
	}
	_mia_cli_report(s);
	return _mia_cli_status(s);
}

/// checks the program; a 'library' (check, mono) need not have 'main'.
static mia_program_s* _mia_cli_check_program(_mia_cli_session_s* const s, const bool library) {
	const mia_program_check_options_s options = {
		.module_dirs = s->options->module_dirs.data, .module_dir_count = s->options->module_dirs.count, .stdlib = s->options->stdlib, .plugins = &s->plugins,
		.eval_steps = s->options->eval_steps, .library = library,
	};
	return mia_program_check(s->options->path, &s->sources, &s->diag, &options);
}

static int _mia_cli_check(_mia_cli_session_s* const s) {
	mia_program_free(_mia_cli_check_program(s, true));
	_mia_cli_report(s);
	return _mia_cli_status(s);
}

static bool _mia_cli_ends_with(const char* const text, const char* const suffix) {
	const size_t length = strlen(text), size = strlen(suffix);
	return length >= size && 0 == strcmp(text + length - size, suffix);
}

/// the bytecode of the program: compiled from a '.mia' file, or loaded from a '.miab' file.
static bool _mia_cli_module(_mia_cli_session_s* const s, mia_bytecode_module_s* const module) {
	const char* const path = s->options->path;
	if (_mia_cli_ends_with(path, ".miab")) {
		char* error = NULL;
		if (mia_bytecode_load(path, module, &error)) return true;
		mia_diag_error(&s->diag, mia_diag_E0601, mia_span_none, "cannot load `%s`: %s", path, error != NULL ? error : "unreadable");
		mia_free(error);
		return false;
	}
	mia_program_s* const program = _mia_cli_check_program(s, false);
	bool compiled = false;
	if (!mia_diag_has_errors(&s->diag)) {
		char* error = NULL;
		compiled = mia_compiler_compile(program, module, &error);
		if (!compiled) {
			mia_diag_note(mia_diag_error(&s->diag, mia_diag_E0602, mia_span_none, "cannot compile: %s", error),
				"this is a bug of the compiler: the checker accepted the program");
			mia_free(error);
		}
	}
	mia_program_free(program);
	return compiled;
}

static int _mia_cli_build(_mia_cli_session_s* const s) {
	mia_bytecode_module_s module = {0};
	if (_mia_cli_module(s, &module)) {
		char* target = NULL;
		if (s->options->output != NULL) {
			target = mia_strdup(s->options->output);
			if (_mia_cli_ends_with(target, ".mia")) {
				mia_diag_help(mia_diag_error(&s->diag, mia_diag_E0602, mia_span_none, "the output `%s` is named like a source file (`.mia`)", target),
					"bytecode goes to a `.miab` file; a source file is never written over");
				mia_free(target);
				mia_bytecode_free(&module);
				_mia_cli_report(s);
				return _mia_cli_status(s);
			}
		}
		else {
			const char* const path = s->options->path;
			const size_t length = strlen(path) - (_mia_cli_ends_with(path, ".mia") ? 4 : 0);
			target = mia_string_format("%.*s.miab", (int)length, path);
		}
		if (!mia_bytecode_save(&module, target)) mia_diag_error(&s->diag, mia_diag_E0601, mia_span_none, "cannot write `%s`", target);
		mia_free(target);
	}
	mia_bytecode_free(&module);
	_mia_cli_report(s);
	return _mia_cli_status(s);
}

/// the span of a bytecode position (a column and a length in code points).
static bool _mia_cli_position_span(_mia_cli_session_s* const s, const char* const path, const mia_bytecode_position_s position, mia_span_s* const span) {
	// the path comes from the bytecode: only a source file is shown, not any file it names
	if (!_mia_cli_ends_with(path, ".mia")) return false;
	const mia_source_s* source = NULL;
	for (size_t i = 0; i < s->sources.files.count && NULL == source; ++i) {
		if (0 == strcmp(s->sources.files.data[i]->path, path)) source = s->sources.files.data[i];
	}
	if (NULL == source) source = mia_source_load(&s->sources, path);
	if (NULL == source || 0 == position.line || position.line > source->lines.count) return false;
	const mia_view_s line = mia_source_line(source, position.line);
	const uint32_t start = mia_source_line_start(source, position.line);
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
	*span = (mia_span_s) { .file = source->id, .begin = start + (uint32_t)begin, .end = start + (uint32_t)(end > begin ? end : begin + 1), };
	return true;
}

/// advice for the runtime errors that have a usual cause.
static const struct {
	const char* prefix;
	const char* help  ;
} _mia_cli_runtime_hints[] = {
	{ "use of freed heap memory", "the memory was given back by `free` or `realloc` before this access" },
	{ "use of a local array", "a slice of a local array must not outlive the call; use `alloc` for memory that outlives it" },
	{ "write through a read-only slice", "slices of constants and string literals are read-only" },
	{ "stack overflow", "a function calls itself without reaching an end, or calls nest too deeply; `--call-depth <n>` raises the limit" },
	{ "stopped after", "the instruction limit (`--limit <n>`) was reached" },
	{ "`free` of memory", "only a slice returned by `alloc` or `realloc` can be freed, and only once" },
	{ "`realloc` of memory", "only a slice returned by `alloc` or `realloc` can be resized" },
};

/// a runtime error: the source line where it happened and the calls that led there.
static void _mia_cli_runtime_report(_mia_cli_session_s* const s, const mia_vm_result_s* const result) {
	mia_span_s span = mia_span_none;
	const mia_vm_trace_entry_s* const innermost = (result->trace.count > 0) ? &result->trace.data[0] : NULL;
	bool located = false;
	if (innermost != NULL && innermost->has_position) located = _mia_cli_position_span(s, innermost->path, innermost->position, &span);
	mia_diag_entry_s* const diagnostic = mia_diag_runtime_error(&s->diag, located ? span : mia_span_none, "%s", result->message);
	if (located) mia_diag_label_main(diagnostic, "here");
	// the calls, innermost first; a run of the same call (deep recursion) is shown once
	for (size_t i = 0; i < result->trace.count;) {
		const mia_vm_trace_entry_s* const entry = &result->trace.data[i];
		size_t repeats = 1;
		while (i + repeats < result->trace.count) {
			const mia_vm_trace_entry_s* const next = &result->trace.data[i + repeats];
			const bool same = 0 == strcmp(next->function, entry->function) && next->has_position == entry->has_position
				&& (!entry->has_position || (next->position.line == entry->position.line && next->position.column == entry->position.column
					&& 0 == strcmp(next->path, entry->path)));
			if (!same || 0 == i) break;
			++repeats;
		}
		mia_string_s note = {0};
		mia_string_appendf(&note, "%s`%s`", (0 == i) ? "in " : "called from ", entry->function);
		if (entry->has_position) mia_string_appendf(&note, " at %s:%u:%u", entry->path, entry->position.line, entry->position.column);
		if (repeats > 1) mia_string_appendf(&note, " (%zu times)", repeats);
		mia_diag_note(diagnostic, "%s", mia_string_cstr(&note));
		mia_string_free(&note);
		i += repeats;
	}
	for (size_t i = 0; i < mia_count_of(_mia_cli_runtime_hints); ++i) {
		const char* const prefix = _mia_cli_runtime_hints[i].prefix;
		if (0 == strncmp(result->message, prefix, strlen(prefix))) mia_diag_help(diagnostic, "%s", _mia_cli_runtime_hints[i].help);
	}
	mia_diag_print(&s->diag, stderr, s->options->format, s->options->color);
}

static int _mia_cli_run(_mia_cli_session_s* const s) {
	mia_bytecode_module_s module = {0};
	if (!_mia_cli_module(s, &module)) {
		mia_bytecode_free(&module);
		_mia_cli_report(s);
		return _mia_cli_status_error;
	}
	if (s->diag.items.count > 0) {
		// warnings: shown before the program's output
		mia_diag_print(&s->diag, stderr, s->options->format, s->options->color);
		if (mia_diag_format_human == s->options->format) (void)fprintf(stderr, "%zu error(s), %zu warning(s)\n", s->diag.error_count, s->diag.warning_count);
		if (_mia_cli_status(s) != _mia_cli_status_ok) {
			mia_bytecode_free(&module);
			return _mia_cli_status_error;
		}
		mia_diag_free(&s->diag);
		s->diag = (mia_diag_s) { .sources = &s->sources, };
	}
	// every plugin function the program calls must be there before it starts
	for (size_t i = 0; i < module.intrinsics.count; ++i) {
		const mia_bytecode_intrinsic_s* const intrinsic = &module.intrinsics.data[i];
		if (mia_plugins_find(&s->plugins, intrinsic->name) != NULL) continue;
		char* error = NULL;
		if (mia_plugins_require(&s->plugins, intrinsic->plugin, &error) && mia_plugins_find(&s->plugins, intrinsic->name) != NULL) continue;
		if (error != NULL) {
			mia_diag_error(&s->diag, mia_diag_E0603, mia_span_none, "%s", error);
			mia_free(error);
			continue;
		}
		mia_diag_help(mia_diag_note(mia_diag_error(&s->diag, mia_diag_E0416, mia_span_none, "no plugin provides `%s`", intrinsic->name),
			"it comes from the plugin `%s`", intrinsic->plugin), "pass the directory of its library with `-P <dir>`");
	}
	if (mia_diag_has_errors(&s->diag)) {
		mia_bytecode_free(&module);
		_mia_cli_report(s);
		return _mia_cli_status_error;
	}
	const mia_vm_options_s options = {
		.out = stdout, .limit = s->options->limit, .call_depth = s->options->call_depth, .plugins = &s->plugins,
		.arguments = s->options->arguments.data, .argument_count = s->options->arguments.count,
	};
	mia_vm_result_s result = {0};
	const bool finished = mia_vm_run(&module, &options, &result);
	(void)fflush(stdout);
	int status = mia_vm_exit_status(&result);
	if (!finished) {
		_mia_cli_runtime_report(s, &result);
		status = _mia_cli_status_runtime;
	}
	if (s->options->stats) {
		(void)fprintf(stderr, "%llu instruction(s), %llu allocation(s), exit status %d\n", (unsigned long long)result.steps,
			(unsigned long long)result.allocations, status);
	}
	mia_vm_result_free(&result);
	mia_bytecode_free(&module);
	return status;
}

static int _mia_cli_explain_code(const char* const text) {
	const mia_diag_code_e code = mia_diag_code_parse(text);
	if (mia_diag_code_count == code) {
		(void)fprintf(stderr, "error[E0602]: `%s` is not a diagnostic code\n = help: `mia explain` lists them all\n", text);
		return _mia_cli_status_usage;
	}
	(void)printf("%s (%s)\n\n  %s\n", mia_diag_code_text(code), mia_diag_code_name(code), mia_diag_code_summary(code));
	return _mia_cli_status_ok;
}

static int _mia_cli_explain(_mia_cli_session_s* const s) {
	if (s->options->path != NULL) return _mia_cli_explain_code(s->options->path);
	for (int code = 0; code < (int)mia_diag_code_count; ++code) {
		(void)printf("%s  %-24s %s\n", mia_diag_code_text((mia_diag_code_e)code), mia_diag_code_name((mia_diag_code_e)code), mia_diag_code_summary((mia_diag_code_e)code));
	}
	return _mia_cli_status_ok;
}

#define _mia_cli_check_options                                                                             \
	"  -M, --module-dir <dir>  also look for modules in <dir> (after the file's and the stdlib's)\n"       \
	"  -P, --plugin-dir <dir>  also look for plugins in <dir> (after the file's and the stdlib's)\n"       \
	"  --stdlib <dir>          the standard library (default: the stdlib/ of mia's installation folder)\n" \
	"  --eval-steps <n>        the steps compile-time evaluation may take (default 1000000)\n"

static const _mia_cli_command_s _mia_cli_commands[] = {
	{ "ast", "print the syntax tree of a file", "", _mia_cli_ast, true, },
	{ "check", "check a program: the file and every module it imports", _mia_cli_check_options, _mia_cli_check, true, },
	{ "build", "compile a program to bytecode",
		_mia_cli_check_options "  -o <file>               where to write it (default: next to the source, as .miab)\n", _mia_cli_build, true, },
	{ "run", "run a program (.mia or .miab); exits with what main returns",
		_mia_cli_check_options
		"  --limit <n>             stop after <n> instructions\n"
		"  --call-depth <n>        the calls that may run at once (default 10000)\n"
		"  --stats                 print the instructions run and the heap allocations made\n"
		"  -- <file> <arguments>   ends mia's options; the program gets the file and what follows it\n", _mia_cli_run, true, },
	{ "explain", "explain a diagnostic code ('mia explain E0502'), or list them all", "", _mia_cli_explain, false, },
};

// --- the command line ----------------------------------------------------------------------

static const _mia_cli_command_s* _mia_cli_find_command(const char* const name) {
	for (size_t i = 0; i < mia_count_of(_mia_cli_commands); ++i) if (0 == strcmp(_mia_cli_commands[i].name, name)) return &_mia_cli_commands[i];
	return NULL;
}

static void _mia_cli_help(FILE* const stream, const _mia_cli_command_s* const command) {
	if (command != NULL) {
		(void)fprintf(stream, "mia %s: %s\n\nusage: mia %s [options] %s\n\noptions:\n%s", command->name, command->summary, command->name,
			command->file ? "<file>" : "[code]", command->details);
		(void)fprintf(stream,
			"  --format <format>       diagnostics as snippets ('human', the default) or one line each ('short')\n"
			"  --color[=<when>]        color diagnostics: 'auto' (a terminal without NO_COLOR), 'always' or 'never'\n"
			"  --no-color              the same as --color=never\n"
			"  --werror                treat warnings as errors\n");
		return;
	}
	(void)fprintf(stream, "mia %s: the compiler and virtual machine of the mia language\n\nusage: mia <command> [options] <file>\n\ncommands:\n",
		mia_cli_version);
	for (size_t i = 0; i < mia_count_of(_mia_cli_commands); ++i) (void)fprintf(stream, "  %-9s %s\n", _mia_cli_commands[i].name, _mia_cli_commands[i].summary);
	(void)fprintf(stream, "\n'mia <command> --help' shows the options of a command.\n\n"
		"exit status: 0 success, 1 errors, 2 bad usage, 101 runtime error; 'run' exits with main's result.\n");
}

static int _mia_cli_usage(const char* const format, ...) mia_printf_like(1, 2);

static int _mia_cli_usage(const char* const format, ...) {
	va_list args; va_start(args, format);
	(void)fprintf(stderr, "error[E0602]: ");
	(void)vfprintf(stderr, format, args);
	va_end(args);
	(void)fprintf(stderr, "\n = help: 'mia --help' lists the commands and 'mia <command> --help' their options\n");
	return _mia_cli_status_usage;
}

static bool _mia_cli_color_auto(void) {
	const char* const no_color = getenv("NO_COLOR");
	return mia_system_terminal(stderr) && (NULL == no_color || '\0' == no_color[0]);
}

/// sets the plugin directories: the program's, the standard library's, then
/// those of '-P'; false (reported) when one of '-P' does not exist.
static bool _mia_cli_load_plugins(_mia_cli_session_s* const s) {
	bool loaded = true;
	// the program's directory and the standard library first, as for modules
	const char* const path = s->options->path;
	const size_t length = mia_system_path_directory(path);
	char* const program = (length > 0) ? mia_string_format("%.*s", (int)length, path) : mia_strdup(".");
	mia_plugins_add_directory(&s->plugins, program);
	mia_free(program);
	if (s->options->stdlib != NULL) mia_plugins_add_directory(&s->plugins, s->options->stdlib);
	for (size_t i = 0; i < s->options->plugin_dirs.count; ++i) {
		const char* const directory = s->options->plugin_dirs.data[i];
		if (mia_system_directory_exists(directory)) {
			mia_plugins_add_directory(&s->plugins, directory);
			continue;
		}
		mia_diag_error(&s->diag, mia_diag_E0603, mia_span_none, "the plugin directory `%s` does not exist", directory);
		loaded = false;
	}
	if (!loaded) _mia_cli_report(s);
	return loaded;
}

/// parses the arguments; returns -1 to go on, or the status to exit with.
static int _mia_cli_parse(_mia_cli_options_s* const options, const int argc, const char* const argv[]) {
	bool help = false;
	bool options_end = false;
	for (int i = 1; i < argc; ++i) {
		const char* const argument = argv[i];
		const bool has_next = i + 1 < argc;
		// 'run': what follows the file is the program's command line; '--' ends mia's options
		if (options->path != NULL && options->command != NULL && 0 == strcmp(options->command, "run")) {
			if (0 == options->arguments.count) mia_vector_push(&options->arguments, options->path);
			mia_vector_push(&options->arguments, argument);
			continue;
		}
		if (!options_end && 0 == strcmp(argument, "--")) {
			options_end = true;
			continue;
		}
		if (options_end) {
			if (NULL == options->command) options->command = argument;
			else if (NULL == options->path) options->path = argument;
			else return _mia_cli_usage("unexpected argument `%s`", argument);
			continue;
		}
		if (0 == strcmp(argument, "-h") || 0 == strcmp(argument, "--help")) help = true;
		else if (0 == strcmp(argument, "--version")) {
			(void)printf("mia %s\n", mia_cli_version);
			return _mia_cli_status_ok;
		}
		else if ((0 == strcmp(argument, "-M") || 0 == strcmp(argument, "--module-dir")) && has_next) mia_vector_push(&options->module_dirs, argv[++i]);
		else if (0 == strncmp(argument, "-M", 2) && argument[2] != '\0') mia_vector_push(&options->module_dirs, argument + 2);
		else if ((0 == strcmp(argument, "-P") || 0 == strcmp(argument, "--plugin-dir")) && has_next) mia_vector_push(&options->plugin_dirs, argv[++i]);
		else if (0 == strncmp(argument, "-P", 2) && argument[2] != '\0') mia_vector_push(&options->plugin_dirs, argument + 2);
		else if (0 == strcmp(argument, "--stdlib") && has_next) options->stdlib = argv[++i];
		else if (0 == strcmp(argument, "-o") && has_next) options->output = argv[++i];
		else if (0 == strcmp(argument, "--format") && has_next) {
			const char* const format = argv[++i];
			if (0 == strcmp(format, "short")) options->format = mia_diag_format_short;
			else if (0 == strcmp(format, "human")) options->format = mia_diag_format_human;
			else return _mia_cli_usage("unknown format `%s` (human or short)", format);
		}
		else if (0 == strcmp(argument, "--color") || 0 == strcmp(argument, "--color=always")) options->color = true;
		else if (0 == strcmp(argument, "--no-color") || 0 == strcmp(argument, "--color=never")) options->color = false;
		else if (0 == strcmp(argument, "--color=auto")) options->color = _mia_cli_color_auto();
		else if (0 == strcmp(argument, "--werror")) options->werror = true;
		else if (0 == strcmp(argument, "--stats")) options->stats = true;
		else if ((0 == strcmp(argument, "--limit") || 0 == strcmp(argument, "--eval-steps") || 0 == strcmp(argument, "--call-depth")) && has_next) {
			char* end = NULL;
			const char* const text = argv[++i];
			const unsigned long long value = strtoull(text, &end, 10);
			if (NULL == end || *end != '\0' || '\0' == text[0] || '-' == text[0]) return _mia_cli_usage("`%s` needs a number, not `%s`", argument, text);
			if (0 == strcmp(argument, "--limit")) options->limit = value;
			else if (0 == value) return _mia_cli_usage("`%s` needs a number above 0", argument);
			else if (0 == strcmp(argument, "--eval-steps")) options->eval_steps = value;
			else options->call_depth = value;
		}
		else if ('-' == argument[0] && argument[1] != '\0') {
			const bool needs_value = 0 == strcmp(argument, "-M") || 0 == strcmp(argument, "--module-dir") || 0 == strcmp(argument, "-P")
				|| 0 == strcmp(argument, "--plugin-dir") || 0 == strcmp(argument, "--stdlib") || 0 == strcmp(argument, "-o")
				|| 0 == strcmp(argument, "--format") || 0 == strcmp(argument, "--limit") || 0 == strcmp(argument, "--eval-steps")
				|| 0 == strcmp(argument, "--call-depth");
			if (needs_value) return _mia_cli_usage("`%s` needs a value", argument);
			return _mia_cli_usage("unknown option `%s`", argument);
		}
		else if (NULL == options->command) options->command = argument;
		else if (NULL == options->path) options->path = argument;
		else return _mia_cli_usage("unexpected argument `%s`", argument);
	}
	const _mia_cli_command_s* const command = (options->command != NULL) ? _mia_cli_find_command(options->command) : NULL;
	if (help) {
		_mia_cli_help(stdout, command);
		return _mia_cli_status_ok;
	}
	if (NULL == options->command) {
		_mia_cli_help(stderr, NULL);
		return _mia_cli_status_usage;
	}
	if (NULL == command) return _mia_cli_usage("unknown command `%s`", options->command);
	if (command->file && NULL == options->path) return _mia_cli_usage("`mia %s` needs a file", command->name);
	if (0 == options->arguments.count && options->path != NULL) mia_vector_push(&options->arguments, options->path);
	return -1;
}

/// the installation folder: where the executable is, with its stdlib/
/// (modules and their plugins; caller frees), or null.
static char* _mia_cli_installation(const char* const argv0) {
	return mia_system_executable_directory(argv0);
}

int mia_cli_main(const int argc, const char* const argv[]) {
	char* const installation = _mia_cli_installation(argc > 0 ? argv[0] : NULL);
	char* const stdlib = (installation != NULL) ? mia_string_format("%s/stdlib", installation) : NULL;
	_mia_cli_options_s options = {
		.installation = installation,
		.stdlib = (stdlib != NULL && mia_system_directory_exists(stdlib)) ? stdlib : NULL,
		.format = mia_diag_format_human,
		.color = _mia_cli_color_auto(),
	};
	int status = _mia_cli_parse(&options, argc, argv);
	const _mia_cli_command_s* const command = (status < 0) ? _mia_cli_find_command(options.command) : NULL;
	if (command != NULL) {
		_mia_cli_session_s session = { .options = &options, };
		session.diag.sources = &session.sources;
		mia_plugins_init(&session.plugins);
		if (command->file && _mia_cli_load_plugins(&session)) status = command->run(&session);
		else if (command->file) status = _mia_cli_status_error;
		else status = command->run(&session);
		mia_plugins_free(&session.plugins);
		mia_diag_free(&session.diag);
		mia_arena_free(&session.arena);
		mia_source_table_free(&session.sources);
	}
	mia_vector_free(&options.module_dirs);
	mia_vector_free(&options.plugin_dirs);
	mia_vector_free(&options.arguments);
	mia_free(stdlib);
	mia_free(installation);
	return status;
}
