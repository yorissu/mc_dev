
/// the system layer on windows, with msvc or mingw (see mia/system.h).

#include "mia/string.h"
#include "mia/system.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <string.h>
#include <stdlib.h>
#include <direct.h>
#include <fcntl.h>
#include <io.h>

// --- shared libraries -----------------------------------------------------------------------

mia_system_library_s* mia_system_library_open(const char* const path, char** const error) {
	// LoadLibrary searches for a name without a directory (the program's
	// directory, the system's, PATH) instead of opening the file: the file is
	// meant, by its full path, and the libraries it needs are looked for beside it
	const DWORD length = GetFullPathNameA(path, 0, NULL, NULL);
	char* const full = (length > 0) ? mia_malloc(length) : NULL;
	HMODULE library = NULL;
	if (full != NULL && GetFullPathNameA(path, length, full, NULL) > 0) library = LoadLibraryExA(full, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
	const DWORD failure = (NULL == library) ? GetLastError() : 0;
	mia_free(full);
	if (NULL == library) *error = mia_string_format("cannot load it (error %lu)", (unsigned long)failure);
	return (mia_system_library_s*)library;
}

void* mia_system_library_symbol(mia_system_library_s* const library, const char* const name) {
	FARPROC const symbol = GetProcAddress((HMODULE)library, name);
	void* result = NULL;
	memcpy(&result, &symbol, sizeof(result) < sizeof(symbol) ? sizeof(result) : sizeof(symbol));
	return result;
}

void mia_system_library_close(mia_system_library_s* const library) {
	(void)FreeLibrary((HMODULE)library);
}

const char* mia_system_library_extension(void) {
	return ".dll";
}

// --- files and directories ------------------------------------------------------------------

bool mia_system_file_exists(const char* const path) {
	const DWORD attributes = GetFileAttributesA(path);
	return attributes != INVALID_FILE_ATTRIBUTES && 0 == (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

bool mia_system_directory_exists(const char* const path) {
	const DWORD attributes = GetFileAttributesA(path);
	return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

static int _mia_system_compare_paths(const void* const left, const void* const right) {
	return strcmp(*(const char* const*)left, *(const char* const*)right);
}

bool mia_system_directory_list(const char* const directory, mia_system_paths_s* const paths) {
	*paths = (mia_system_paths_s) {0};
	char* const pattern = mia_string_format("%s\\*", directory);
	WIN32_FIND_DATAA found;
	HANDLE const handle = FindFirstFileA(pattern, &found);
	mia_free(pattern);
	if (INVALID_HANDLE_VALUE == handle) return false;
	do {
		if ('.' == found.cFileName[0]) continue;
		mia_vector_push(paths, mia_string_format("%s\\%s", directory, found.cFileName));
	} while (FindNextFileA(handle, &found));
	(void)FindClose(handle);
	if (paths->count > 0) qsort(paths->data, paths->count, sizeof(*paths->data), _mia_system_compare_paths);
	return true;
}

// --- the process ----------------------------------------------------------------------------

char* mia_system_current_directory(void) {
	char directory[4096];
	if (NULL == _getcwd(directory, (int)sizeof(directory))) return NULL;
	return mia_strdup(directory);
}

char* mia_system_executable_directory(const char* const argv0) {
	mia_unused(argv0);
	char path[MAX_PATH * 4];
	const DWORD length = GetModuleFileNameA(NULL, path, (DWORD)sizeof(path));
	if (0 == length || length >= sizeof(path)) return NULL;
	char* const separator = strrchr(path, '\\');
	if (separator != NULL) *separator = '\0';
	return mia_strdup(path);
}

// --- terminals and streams ------------------------------------------------------------------

bool mia_system_terminal(FILE* const stream) {
	return _isatty(_fileno(stream)) != 0;
}

void mia_system_binary_stream(FILE* const stream) {
	(void)_setmode(_fileno(stream), _O_BINARY);
}
