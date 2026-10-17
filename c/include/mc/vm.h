
#ifndef __mc__vm_h__
#define __mc__vm_h__

/// the virtual machine: runs a bytecode module.
///
/// data is laid out the way c lays it out (see bytecode.h): the locals of a
/// call live in a byte area of its frame, heap memory in blocks from
/// 'alloc', constants in read-only blocks. slices and locations name a
/// block by a handle (its table, index and generation) and an offset in it,
/// so every access checks that the memory is still alive: a block's
/// generation changes when it is freed or its call returns, and a slice of
/// it stops working. a mistake stops the program with a runtime error
/// instead of corrupting memory. nothing is garbage collected: frames are
/// freed when their call returns, heap blocks by 'free' (and all the rest
/// when the program ends).
///
/// functions the machine does not run itself ('[plugin]' functions) are
/// intrinsics: they are linked by their qualified name to the plugin
/// functions that provide them before the program starts.

#include "mc/bytecode.h"
#include "mc/plugins.h"

#include <stdio.h>

typedef struct {
	FILE*               out    ;   // what the program prints (stdout when null)
	uint64_t            limit  ;   // stop after this many instructions; 0: no limit
	uint64_t            call_depth;   // the calls that may be running at once; 0: the default, 10000
	const mc_plugins_s* plugins;   // the plugin functions; null: only the built-in ones
	const char* const*  arguments;   // the command line for 'main', the program first (it is read-only to the program)
	size_t              argument_count;
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
	int64_t                        result    ;
	uint64_t                       steps     ;   // instructions executed
	uint64_t                       allocations;  // heap blocks allocated
} mc_vm_result_s;

/// the default limit of nested calls.
#define mc_vm_call_depth_default 10000

/// runs the module's entry function; false when it stops with a runtime error
/// (described in 'result').
bool mc_vm_run(const mc_module_bc_s* const module, const mc_vm_options_s* const options, mc_vm_result_s* const result);

void mc_vm_result_free(mc_vm_result_s* const result);

/// the exit status of a finished run: main's int result & 0xFF, else 0.
int mc_vm_exit_status(const mc_vm_result_s* const result);

#endif
