
#ifndef __mia__opcode_h__
#define __mia__opcode_h__

/// the instructions of the machine (see opcodes.def; bytecode.h describes them).

typedef enum {
#define mia_opcode(_name, _types, _operands) mia_opcode_##_name,
#include "mia/opcodes.def"
#undef mia_opcode
	mia_opcode_count,
} mia_opcode_e;

/// the name of an opcode: 'LOAD', 'CALL_INTRINSIC'.
const char* mia_opcode_name(const mia_opcode_e opcode);

int mia_opcode_operands(const mia_opcode_e opcode);

#endif
