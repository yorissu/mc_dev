
#ifndef __mia__listing_h__
#define __mia__listing_h__

/// bytecode as text, for people: the listing of a module ('mia dis') and the
/// report of a profiled run ('mia run --profile').
///
/// a listing shows the files, types and plugin functions of a module, then
/// every function's instructions, each with what its operand names (the
/// constant, the called function), under the source line they come from. a
/// jump target is marked with '>'. source lines are taken from 'sources' (or
/// read into it), only of '.mia' files; with 'sources' null, only the file
/// and line are shown.

#include "mia/bytecode.h"
#include "mia/source.h"
#include "mia/vm.h"

/// appends an instruction: its name with its type codes, then its operands ('LOAD_S64 0 8').
void mia_listing_instruction(const mia_bytecode_instruction_s* const instruction, mia_string_s* const out);

/// appends the listing of a module.
void mia_listing_module(const mia_bytecode_module_s* const module, mia_source_table_s* const sources, mia_string_s* const out);

/// appends the report of a profiled run: the functions, source lines,
/// instructions and pairs of instructions (one, then the next one of the
/// code right after it) that ran most, at most 'top' of each (0: all).
void mia_listing_profile(const mia_bytecode_module_s* const module, const mia_vm_profile_s* const profile, mia_source_table_s* const sources,
	const size_t top, mia_string_s* const out);

#endif
