
#include "mia/opcode.h"

static const char* const _mia_opcode_names[] = {
#define mia_opcode(_name, _types, _operands) #_name,
#include "mia/opcodes.def"
#undef mia_opcode
};

const char* mia_opcode_name(const mia_opcode_e opcode) {
	return _mia_opcode_names[opcode];
}
