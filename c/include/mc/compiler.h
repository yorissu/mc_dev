
#ifndef __mc__compiler_h__
#define __mc__compiler_h__

/// the compiler: a checked program -> a bytecode module. only what 'main'
/// can reach is compiled; every function instance and every lambda becomes
/// one bytecode function, operator uses become direct calls and constants
/// become entries of the constant pool.

#include "mc/bytecode.h"
#include "mc/program.h"

/// compiles a program without errors; false (with 'error' set, caller frees)
/// for constructs that cannot be compiled.
bool mc_compile(mc_program_s* const program, mc_module_bc_s* const module, char** const error);

#endif
