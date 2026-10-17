
#ifndef __mc__platform_h__
#define __mc__platform_h__

/// what differs between operating systems, behind one interface:
/// source/platform_posix.c (linux, macos, bsd, wsl, mingw's posix parts)
/// and source/platform_windows.c (windows, with msvc or mingw).

#include "mc/global.h"

#include <stdio.h>

/// a shared library: opened, its symbols, closed. null (with 'error' set,
/// caller frees) when it cannot be opened.
void* mc_library_open(const char* const path, char** const error);

void* mc_library_symbol(void* const library, const char* const name);

void mc_library_close(void* const library);

/// calls 'visit' with the path of every file in 'directory' (sorted by
/// name); false when the directory cannot be read.
bool mc_directory_each(const char* const directory, void (*visit)(const char* const path, void* const context), void* const context);

/// whether 'path' is a directory.
bool mc_is_directory(const char* const path);

/// whether 'path' is a regular file.
bool mc_is_file(const char* const path);

/// whether 'stream' is a terminal.
bool mc_is_terminal(FILE* const stream);

/// makes 'stream' write bytes as they are: windows turns '\\n' into '\\r\\n'
/// in text mode, and programs print the same bytes on every system.
void mc_binary_stream(FILE* const stream);

/// the current directory (caller frees), or null.
char* mc_current_directory(void);

/// a new empty directory for temporary files (caller frees the path;
/// mc_remove_tree removes it); null when it cannot be made.
char* mc_temporary_directory(void);

/// removes a file, or a directory and everything in it.
void mc_remove_tree(const char* const path);

/// the separator of path lists in environment variables (':' or ';').
char mc_path_list_separator(void);

/// whether 'c' separates the parts of a path: '/', and '\\' on windows.
bool mc_is_path_separator(const char c);

/// the length of the root that starts an absolute path ('/', or 'C:\\' on
/// windows); 0 for a relative path.
size_t mc_path_root(const char* const path);

#endif
