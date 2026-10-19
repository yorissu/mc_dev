
#ifndef __mia__vm_h__
#define __mia__vm_h__

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
/// functions the machine does not run itself (plugin functions) are
/// intrinsics: they are linked by their qualified name to the plugin
/// functions that provide them before the program starts.

#include "mia/bytecode.h"
#include "mia/plugins.h"

#include <stdio.h>

/// what a profiled run executed in one function of the module.
typedef struct {
	uint64_t  calls   ;
	size_t    count   ;   // its instructions
	uint64_t* runs    ;   // per instruction: how often it ran
	uint64_t* followed;   // per instruction: how often the next one of the code ran right after it, in the same call
} mia_vm_profile_function_s;

/// what a profiled run executed: the module's functions, in their order.
typedef struct {
	mia_vector_of(mia_vm_profile_function_s) functions;
} mia_vm_profile_s;

typedef struct {
	FILE*                out           ;   // what the program prints (stdout when null)
	mia_vm_profile_s*    profile       ;   // counts what runs (one instruction at a time, slower); null: nothing counted
	uint64_t             limit         ;   // stop after this many instructions; 0: no limit
	uint64_t             call_depth    ;   // the calls that may be running at once; 0: the default, 10000
	const mia_plugins_s* plugins       ;   // the plugin functions; null: only the built-in ones
	const char* const*   arguments     ;   // the command line for 'main', the program first (it is read-only to the program)
	size_t               argument_count;
} mia_vm_options_s;

/// one call of a runtime error's trace.
typedef struct {
	const char*             function    ;   // owned by the module
	bool                    has_position;
	mia_bytecode_position_s position    ;
	const char*             path        ;   // of the position, owned by the module
} mia_vm_trace_entry_s;

typedef struct {
	bool                                failed     ;
	char*                               message    ;   // the runtime error
	mia_vector_of(mia_vm_trace_entry_s) trace      ;   // innermost call first
	bool                                has_result ;   // 'main' returned an int
	int64_t                             result     ;
	uint64_t                            steps      ;   // instructions executed
	uint64_t                            allocations;   // heap blocks allocated
} mia_vm_result_s;

/// the default limit of nested calls.
#define mia_vm_call_depth_default 10000

/// runs the module's entry function; false when it stops with a runtime error
/// (described in 'result').
bool mia_vm_run(const mia_bytecode_module_s* const module, const mia_vm_options_s* const options, mia_vm_result_s* const result);

void mia_vm_result_free(mia_vm_result_s* const result);

void mia_vm_profile_free(mia_vm_profile_s* const profile);

/// the exit status of a finished run: main's int result & 0xFF, else 0.
int mia_vm_exit_status(const mia_vm_result_s* const result);

/// the machine's part of the plugin api's function table (calls at run time).
void mia_vm_plugin_table(mia_plugin_interface_s* const table);

#endif
