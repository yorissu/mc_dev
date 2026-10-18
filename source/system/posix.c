
/// the system layer on posix systems: linux, macos, the bsds, wsl (see
/// mia/system.h).

// realpath is x/open
#if !defined(_XOPEN_SOURCE)
#	define _XOPEN_SOURCE 700
#endif

#include "mia/string.h"
#include "mia/system.h"

#if defined(__APPLE__)
#	include <mach-o/dyld.h>
#endif

#include <sys/stat.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>
#include <dlfcn.h>

// --- shared libraries -----------------------------------------------------------------------

mia_system_library_s* mia_system_library_open(const char* const path, char** const error) {
	// dlopen searches for a name without a '/' (LD_LIBRARY_PATH, the system's
	// libraries) instead of opening the file: the file is meant
	char* const file = (strchr(path, '/') != NULL) ? mia_strdup(path) : mia_string_format("./%s", path);
	void* const library = dlopen(file, RTLD_NOW | RTLD_LOCAL);
	mia_free(file);
	if (NULL == library) {
		const char* const reason = dlerror();
		*error = mia_strdup(reason != NULL ? reason : "cannot open it");
	}
	return library;
}

void* mia_system_library_symbol(mia_system_library_s* const library, const char* const name) {
	return dlsym(library, name);
}

void mia_system_library_close(mia_system_library_s* const library) {
	(void)dlclose(library);
}

const char* mia_system_library_extension(void) {
#if defined(__APPLE__)
	return ".dylib";
#else
	return ".so";
#endif
}

// --- files and directories ------------------------------------------------------------------

bool mia_system_file_exists(const char* const path) {
	struct stat status;
	return 0 == stat(path, &status) && S_ISREG(status.st_mode);
}

bool mia_system_directory_exists(const char* const path) {
	struct stat status;
	return 0 == stat(path, &status) && S_ISDIR(status.st_mode);
}

static int _mia_system_compare_paths(const void* const left, const void* const right) {
	return strcmp(*(const char* const*)left, *(const char* const*)right);
}

bool mia_system_directory_list(const char* const directory, mia_system_paths_s* const paths) {
	*paths = (mia_system_paths_s) {0};
	DIR* const handle = opendir(directory);
	if (NULL == handle) return false;
	for (const struct dirent* entry = readdir(handle); entry != NULL; entry = readdir(handle)) {
		if ('.' == entry->d_name[0]) continue;
		mia_vector_push(paths, mia_string_format("%s/%s", directory, entry->d_name));
	}
	(void)closedir(handle);
	if (paths->count > 0) qsort(paths->data, paths->count, sizeof(*paths->data), _mia_system_compare_paths);
	return true;
}

// --- the process ----------------------------------------------------------------------------

char* mia_system_current_directory(void) {
	char directory[4096];
	if (NULL == getcwd(directory, sizeof(directory))) return NULL;
	return mia_strdup(directory);
}

/// the path of the running executable (caller frees), or null.
static char* _mia_system_executable(const char* const argv0) {
	char path[4096];
#if defined(__APPLE__)
	uint32_t size = sizeof(path);
	if (0 == _NSGetExecutablePath(path, &size)) {
		char* const resolved = realpath(path, NULL);
		if (resolved != NULL) return resolved;
	}
#else
	const ssize_t length = readlink("/proc/self/exe", path, sizeof(path) - 1);
	if (length > 0) {
		path[length] = '\0';
		return mia_strdup(path);
	}
#endif
	// started with a path ('./mia', 'build/release/mia'): resolve it
	if (argv0 != NULL && strchr(argv0, '/') != NULL) return realpath(argv0, NULL);
	return NULL;
}

char* mia_system_executable_directory(const char* const argv0) {
	char* const path = _mia_system_executable(argv0);
	if (NULL == path) return NULL;
	char* const slash = strrchr(path, '/');
	if (slash != NULL) *slash = '\0';
	return path;
}

// --- terminals and streams ------------------------------------------------------------------

bool mia_system_terminal(FILE* const stream) {
	return isatty(fileno(stream)) != 0;
}

void mia_system_binary_stream(FILE* const stream) {
	// posix streams write bytes as they are
	mia_unused(stream);
}
