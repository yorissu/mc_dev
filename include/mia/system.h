
#ifndef __mia__system_h__
#define __mia__system_h__

/// what differs between operating systems, behind one interface. each
/// system has its own file in source/system/ (posix.c: linux, macos, bsd,
/// wsl; windows.c: windows with msvc or mingw), and the build compiles the
/// one for the system it builds on. adding a system is adding a file.
///
/// paths only differ in a few characters, so their helpers are here.

#include "mia/global.h"
#include "mia/vector.h"

#include <stdio.h>

// --- shared libraries (plugins) -----------------------------------------------------------

typedef struct mia_system_library_s mia_system_library_s;

/// opens a shared library; null (with 'error' set, caller frees) when it cannot.
mia_system_library_s* mia_system_library_open(const char* const path, char** const error);

/// a symbol of a library, or null.
void* mia_system_library_symbol(mia_system_library_s* const library, const char* const name);

void mia_system_library_close(mia_system_library_s* const library);

/// the file name ending of shared libraries: '.so', '.dylib' or '.dll'.
const char* mia_system_library_extension(void);

// --- files and directories ----------------------------------------------------------------

/// whether 'path' is a regular file / a directory.
bool mia_system_file_exists(const char* const path);

bool mia_system_directory_exists(const char* const path);

typedef mia_vector_of(char*) mia_system_paths_s;

/// the paths of what 'directory' holds (hidden names left out), sorted by
/// name; false when it cannot be read. free them with mia_system_paths_free.
bool mia_system_directory_list(const char* const directory, mia_system_paths_s* const paths);

static inline void mia_system_paths_free(mia_system_paths_s* const paths) {
	for (size_t i = 0; i < paths->count; ++i) mia_free(paths->data[i]);
	mia_vector_free(paths);
}

// --- the process ----------------------------------------------------------------------------

/// the current directory (caller frees), or null.
char* mia_system_current_directory(void);

/// the directory of the running executable, without a separator at the
/// end (caller frees), or null; 'argv0' helps where the system cannot say.
char* mia_system_executable_directory(const char* const argv0);

// --- terminals and streams ------------------------------------------------------------------

/// whether 'stream' is a terminal.
bool mia_system_terminal(FILE* const stream);

/// makes 'stream' write bytes as they are (windows turns '\n' into '\r\n'
/// in text mode), so programs print the same bytes on every system.
void mia_system_binary_stream(FILE* const stream);

// --- paths ------------------------------------------------------------------------------------

#if defined(_WIN32)
#	define mia_system_path_separators "/\\"
#	define mia_system_path_list_separator ';'
#else
#	define mia_system_path_separators "/"
#	define mia_system_path_list_separator ':'
#endif

/// whether 'c' separates the parts of a path: '/', and '\' on windows.
static inline bool mia_system_path_is_separator(const char c) {
	return '/' == c || ('\\' == c && '\\' == mia_system_path_separators[1]);
}

/// the length of the root that starts an absolute path ('/', or 'C:\' on
/// windows); 0 for a relative path.
static inline size_t mia_system_path_root(const char* const path) {
#if defined(_WIN32)
	const bool letter = (path[0] >= 'a' && path[0] <= 'z') || (path[0] >= 'A' && path[0] <= 'Z');
	if (letter && ':' == path[1] && mia_system_path_is_separator(path[2])) return 3;
#endif
	return mia_system_path_is_separator(path[0]) ? 1 : 0;
}

/// how much of 'path' is its directory: up to its last separator, a root
/// kept whole ('/x.mia' -> '/', 'C:\x.mia' -> 'C:\', 'a/b.mia' -> 'a');
/// 0 when it has none ('x.mia').
static inline size_t mia_system_path_directory(const char* const path) {
	size_t end = 0;
	for (size_t i = 0; path[i] != '\0'; ++i) if (mia_system_path_is_separator(path[i])) end = i + 1;
	if (0 == end) return 0;
	const size_t root = mia_system_path_root(path);
	return (end - 1 < root) ? root : end - 1;
}

#endif
