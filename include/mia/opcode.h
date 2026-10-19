
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

/// how many type codes it carries in its name (0, 1 or 2), and how many
/// operands it takes (inline: the bytecode reader asks for every instruction).
static inline int mia_opcode_types(const mia_opcode_e opcode) {
	switch (opcode) {
#define mia_opcode(_name, _types, _operands) case mia_opcode_##_name: return _types;
#include "mia/opcodes.def"
#undef mia_opcode
		default: return 0;
	}
}

static inline int mia_opcode_operands(const mia_opcode_e opcode) {
	switch (opcode) {
#define mia_opcode(_name, _types, _operands) case mia_opcode_##_name: return _operands;
#include "mia/opcodes.def"
#undef mia_opcode
		default: return 0;
	}
}

#endif
