
/// the platform layer on windows (see mc/platform.h).

#if defined(_WIN32)

#include "mc/platform.h"
#include "mc/string.h"
#include "mc/vector.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <direct.h>
#include <io.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

void* mc_library_open(const char* const path, char** const error) {
	HMODULE const library = LoadLibraryA(path);
	if (NULL == library) *error = mc_format("cannot load it (error %lu)", (unsigned long)GetLastError());
	return (void*)library;
}

void* mc_library_symbol(void* const library, const char* const name) {
	FARPROC const symbol = GetProcAddress((HMODULE)library, name);
	void* result = NULL;
	memcpy(&result, &symbol, sizeof(result) < sizeof(symbol) ? sizeof(result) : sizeof(symbol));
	return result;
}

void mc_library_close(void* const library) {
	(void)FreeLibrary((HMODULE)library);
}

static int _mc_compare_names(const void* const left, const void* const right) {
	return strcmp(*(const char* const*)left, *(const char* const*)right);
}

bool mc_directory_each(const char* const directory, void (*visit)(const char* const path, void* const context), void* const context) {
	char* const pattern = mc_format("%s\\*", directory);
	WIN32_FIND_DATAA found;
	HANDLE const handle = FindFirstFileA(pattern, &found);
	mc_free(pattern);
	if (INVALID_HANDLE_VALUE == handle) return false;
	mc_vector_of(char*) names = {0};
	do {
		if ('.' == found.cFileName[0]) continue;
		mc_vector_push(&names, mc_strdup(found.cFileName));
	} while (FindNextFileA(handle, &found));
	(void)FindClose(handle);
	if (names.count > 0) qsort(names.data, names.count, sizeof(*names.data), _mc_compare_names);
	for (size_t i = 0; i < names.count; ++i) {
		char* const path = mc_format("%s\\%s", directory, names.data[i]);
		visit(path, context);
		mc_free(path);
		mc_free(names.data[i]);
	}
	mc_vector_free(&names);
	return true;
}

bool mc_is_directory(const char* const path) {
	const DWORD attributes = GetFileAttributesA(path);
	return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool mc_is_file(const char* const path) {
	const DWORD attributes = GetFileAttributesA(path);
	return attributes != INVALID_FILE_ATTRIBUTES && 0 == (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

bool mc_is_terminal(FILE* const stream) {
	return _isatty(_fileno(stream)) != 0;
}

char* mc_current_directory(void) {
	char directory[4096];
	if (NULL == _getcwd(directory, (int)sizeof(directory))) return NULL;
	return mc_strdup(directory);
}

char* mc_temporary_directory(void) {
	char base[MAX_PATH + 1];
	const DWORD length = GetTempPathA((DWORD)sizeof(base), base);
	if (0 == length || length > MAX_PATH) return NULL;
	for (unsigned attempt = 0; attempt < 1000; ++attempt) {
		char* const path = mc_format("%smc_%lu_%u", base, (unsigned long)GetCurrentProcessId(), attempt);
		if (CreateDirectoryA(path, NULL)) return path;
		mc_free(path);
	}
	return NULL;
}

static void _mc_remove_visit(const char* const path, void* const context) {
	mc_unused(context);
	mc_remove_tree(path);
}

void mc_remove_tree(const char* const path) {
	if (mc_is_directory(path)) {
		(void)mc_directory_each(path, _mc_remove_visit, NULL);
		(void)RemoveDirectoryA(path);
	}
	else {
		(void)DeleteFileA(path);
	}
}

char mc_path_list_separator(void) {
	return ';';
}

#else

// this file is empty on other systems (see platform_posix.c); iso c wants a declaration.
typedef int mc_platform_windows_unused_;

#endif
