
/// the platform layer on posix systems (see mc/platform.h).

#if !defined(_WIN32)

#include "mc/platform.h"
#include "mc/string.h"
#include "mc/vector.h"

#include <dirent.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void* mc_library_open(const char* const path, char** const error) {
	void* const library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	if (NULL == library) {
		const char* const reason = dlerror();
		*error = mc_strdup(reason != NULL ? reason : "cannot open it");
	}
	return library;
}

void* mc_library_symbol(void* const library, const char* const name) {
	return dlsym(library, name);
}

void mc_library_close(void* const library) {
	(void)dlclose(library);
}

static int _mc_compare_names(const void* const left, const void* const right) {
	return strcmp(*(const char* const*)left, *(const char* const*)right);
}

bool mc_directory_each(const char* const directory, void (*visit)(const char* const path, void* const context), void* const context) {
	DIR* const handle = opendir(directory);
	if (NULL == handle) return false;
	mc_vector_of(char*) names = {0};
	for (const struct dirent* entry = readdir(handle); entry != NULL; entry = readdir(handle)) {
		if ('.' == entry->d_name[0]) continue;
		mc_vector_push(&names, mc_strdup(entry->d_name));
	}
	(void)closedir(handle);
	if (names.count > 0) qsort(names.data, names.count, sizeof(*names.data), _mc_compare_names);
	for (size_t i = 0; i < names.count; ++i) {
		char* const path = mc_format("%s/%s", directory, names.data[i]);
		visit(path, context);
		mc_free(path);
		mc_free(names.data[i]);
	}
	mc_vector_free(&names);
	return true;
}

bool mc_is_directory(const char* const path) {
	struct stat status;
	return 0 == stat(path, &status) && S_ISDIR(status.st_mode);
}

bool mc_is_file(const char* const path) {
	struct stat status;
	return 0 == stat(path, &status) && S_ISREG(status.st_mode);
}

bool mc_is_terminal(FILE* const stream) {
	return isatty(fileno(stream)) != 0;
}

void mc_binary_stream(FILE* const stream) {
	mc_unused(stream);
}

char* mc_current_directory(void) {
	char directory[4096];
	if (NULL == getcwd(directory, sizeof(directory))) return NULL;
	return mc_strdup(directory);
}

char* mc_temporary_directory(void) {
	const char* const base = getenv("TMPDIR");
	char* const pattern = mc_format("%s/mc_XXXXXX", (base != NULL && base[0] != '\0') ? base : "/tmp");
	if (NULL == mkdtemp(pattern)) {
		mc_free(pattern);
		return NULL;
	}
	return pattern;
}

static void _mc_remove_visit(const char* const path, void* const context) {
	mc_unused(context);
	mc_remove_tree(path);
}

void mc_remove_tree(const char* const path) {
	if (mc_is_directory(path)) {
		(void)mc_directory_each(path, _mc_remove_visit, NULL);
		(void)rmdir(path);
	}
	else {
		(void)unlink(path);
	}
}

bool mc_is_path_separator(const char c) {
	return '/' == c;
}

size_t mc_path_root(const char* const path) {
	return ('/' == path[0]) ? 1 : 0;
}

char mc_path_list_separator(void) {
	return ':';
}

#else

// this file is empty on windows (see platform_windows.c); iso c wants a declaration.
typedef int mc_platform_posix_unused_;

#endif
