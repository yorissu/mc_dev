
#ifndef __mc__vm_h__
#define __mc__vm_h__

/// the virtual machine: runs a bytecode module.
///
/// integers, floats and bools are plain values; arrays and structs are
/// lists, copied on every assignment and written into in place, so slices of
/// an array keep seeing it; slices are views of a block of memory; references
/// and assignment targets are locations. every access through a slice or a
/// location checks that its memory is still alive and that the index is in
/// bounds: a mistake stops the program with a runtime error instead of
/// corrupting memory. a mark-and-sweep collector reclaims unreachable lists,
/// blocks and locations.
///
/// builtins ('[builtin]' functions) and natives ('[extern = "name"]'
/// functions) are listed in source/vm_builtins.c; adding one there is all it
/// takes to make it callable.

#include "mc/bytecode.h"

#include <stdio.h>

typedef struct {
	FILE*    out  ;   // what the program prints (stdout when null)
	uint64_t limit;   // stop after this many instructions; 0: no limit
} mc_vm_options_s;

/// one call of a runtime error's trace.
typedef struct {
	const char*   function    ;   // owned by the module
	bool          has_position;
	mc_position_s position    ;
	const char*   path        ;   // of the position, owned by the module
} mc_trace_entry_s;

typedef struct {
	bool                           failed    ;
	char*                          message   ;   // the runtime error
	mc_vector_of(mc_trace_entry_s) trace     ;   // innermost call first
	bool                           has_result;   // 'main' returned an int
	__int128                       result    ;
	uint64_t                       steps     ;   // instructions executed
	uint64_t                       collections;  // garbage collections
} mc_vm_result_s;

/// runs the module's entry function; false when it stops with a runtime error
/// (described in 'result').
bool mc_vm_run(const mc_module_bc_s* const module, const mc_vm_options_s* const options, mc_vm_result_s* const result);

void mc_vm_result_free(mc_vm_result_s* const result);

/// the exit status of a finished run: main's int result & 0xFF, else 0.
int mc_vm_exit_status(const mc_vm_result_s* const result);

#endif
