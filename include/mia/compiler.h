
#ifndef __mia__compiler_h__
#define __mia__compiler_h__

/// the compiler: a checked program -> a bytecode module. only what 'main'
/// can reach is compiled; every function instance and every lambda becomes
/// one bytecode function, and constants become entries of the constant pool.

#include "mia/bytecode.h"
#include "mia/program.h"

/// compiles a program without errors; false (with 'error' set, caller frees)
/// for constructs that cannot be compiled.
bool mia_compiler_compile(mia_program_s* const program, mia_bytecode_module_s* const module, char** const error);

#endif
