
#ifndef __mc__tests_helpers_h__
#define __mc__tests_helpers_h__

/// helpers of the unit tests: running a whole program from its source text.

#include "mc/compiler.h"
#include "mc/diag.h"
#include "mc/platform.h"
#include "mc/plugins.h"
#include "mc/program.h"
#include "mc/vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
	bool           compiled   ;   // no errors, and compiled
	char*          diagnostics;   // in the short format
	char*          out        ;   // what the program printed
	bool           finished   ;   // ran without a runtime error
	char*          message    ;   // the runtime error
	int            status     ;   // the exit status
	uint64_t       allocations;
	mc_module_bc_s module     ;
} mc_test_run_s;

/// writes 'text' to the file 'name' of 'directory'; returns its path (caller frees).
static inline char* mc_test_write_file(const char* const directory, const char* const name, const char* const text) {
	char* const path = mc_format("%s/%s", directory, name);
	FILE* const file = fopen(path, "wb");
	if (file != NULL) {
		(void)fputs(text, file);
		(void)fclose(file);
	}
	return path;
}

/// the whole content of the file 'path' (caller frees), or null.
static inline char* mc_test_read_file(const char* const path) {
	FILE* const file = fopen(path, "rb");
	if (NULL == file) return NULL;
	mc_string_s text = {0};
	char buffer[4096];
	size_t read = 0;
	while ((read = fread(buffer, 1, sizeof(buffer), file)) > 0) mc_string_append(&text, mc_view_new(buffer, read));
	(void)fclose(file);
	if (NULL == text.data) mc_string_append_cstr(&text, "");
	return mc_string_take(&text);
}

/// checks, compiles and runs the program 'text' with 'plugins' (null: the
/// built-in ones) and the module directory 'include' (or null).
static inline mc_test_run_s mc_test_run_with(const char* const text, const uint64_t limit, const mc_plugins_s* const plugins,
											 const char* const include) {
	mc_test_run_s run = {0};
	char* const directory = mc_temporary_directory();
	if (NULL == directory) {
		run.diagnostics = mc_strdup("cannot make a temporary directory");
		return run;
	}
	char* const path = mc_test_write_file(directory, "main.mc", text);
	mc_sources_s sources = {0};
	mc_diag_s diag = { .sources = &sources, };
	const char* const includes[1] = { include, };
	const mc_check_options_s options = { .stdlib = MC_DEFAULT_STDLIB, .plugins = plugins, .include = includes, .include_count = (include != NULL) ? 1 : 0, };
	mc_program_s* const program = mc_check(path, &sources, &diag, &options);
	if (!mc_diag_has_errors(&diag)) {
		char* error = NULL;
		run.compiled = mc_compile(program, &run.module, &error);
		mc_free(error);
	}
	mc_string_s rendered = {0};
	for (size_t i = 0; i < diag.items.count; ++i) mc_diag_render(&diag, diag.items.data[i], mc_diag_format_short, false, &rendered);
	run.diagnostics = mc_string_take(&rendered);
	mc_program_free(program);
	mc_diag_free(&diag);
	mc_sources_free(&sources);
	mc_free(path);
	if (run.compiled) {
		// the output goes through a file: iso c has no stream into memory
		char* const out_path = mc_format("%s/out.txt", directory);
		FILE* const out = fopen(out_path, "wb");
		const mc_vm_options_s vm_options = { .out = out, .limit = limit, .plugins = plugins, };
		mc_vm_result_s result = {0};
		run.finished = out != NULL && mc_vm_run(&run.module, &vm_options, &result);
		if (out != NULL) (void)fclose(out);
		run.out = mc_test_read_file(out_path);
		mc_free(out_path);
		run.message = (result.message != NULL) ? mc_strdup(result.message) : NULL;
		run.status = run.finished ? mc_vm_exit_status(&result) : 101;
		run.allocations = result.allocations;
		mc_vm_result_free(&result);
	}
	mc_remove_tree(directory);
	mc_free(directory);
	return run;
}

/// checks, compiles and runs the program 'text' (instruction limit 'limit', 0: none).
static inline mc_test_run_s mc_test_run_limited(const char* const text, const uint64_t limit) {
	return mc_test_run_with(text, limit, NULL, NULL);
}

static inline mc_test_run_s mc_test_run(const char* const text) {
	return mc_test_run_limited(text, 0);
}

static inline void mc_test_run_free(mc_test_run_s* const run) {
	mc_free(run->diagnostics);
	mc_free(run->out);
	mc_free(run->message);
	mc_module_bc_free(&run->module);
}

#endif
