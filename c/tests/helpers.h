
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
#include <unistd.h>

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

/// writes 'text' to a new temporary directory as 'main.mc'; returns its path (caller frees with mc_test_remove).
static inline char* mc_test_write(const char* const text) {
	char directory[] = "/tmp/mc_tests_XXXXXX";
	if (NULL == mkdtemp(directory)) return NULL;
	char* const path = mc_format("%s/main.mc", directory);
	FILE* const file = fopen(path, "wb");
	if (file != NULL) {
		(void)fputs(text, file);
		(void)fclose(file);
	}
	return path;
}

static inline void mc_test_remove(char* const path) {
	(void)unlink(path);
	char* const slash = strrchr(path, '/');
	*slash = '\0';
	(void)rmdir(path);
	mc_free(path);
}

/// checks, compiles and runs the program 'text' with 'plugins' (null: the
/// built-in ones) and the module directory 'include' (or null).
static inline mc_test_run_s mc_test_run_with(const char* const text, const uint64_t limit, const mc_plugins_s* const plugins,
											 const char* const include) {
	mc_test_run_s run = {0};
	char* const path = mc_test_write(text);
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
	mc_test_remove(path);
	if (run.compiled) {
		char* buffer = NULL;
		size_t size = 0;
		FILE* const out = open_memstream(&buffer, &size);
		const mc_vm_options_s vm_options = { .out = out, .limit = limit, .plugins = plugins, };
		mc_vm_result_s result = {0};
		run.finished = mc_vm_run(&run.module, &vm_options, &result);
		(void)fclose(out);
		run.out = buffer;
		run.message = (result.message != NULL) ? mc_strdup(result.message) : NULL;
		run.status = run.finished ? mc_vm_exit_status(&result) : 101;
		run.allocations = result.allocations;
		mc_vm_result_free(&result);
	}
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
	free(run->out);
	mc_free(run->message);
	mc_module_bc_free(&run->module);
}

#endif
