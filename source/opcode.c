
#include "mia/opcode.h"

typedef struct {
	const char* name    ;
	int         types   ;
	int         operands;
} _mia_opcode_info_s;

static const _mia_opcode_info_s _mia_opcode_opcodes[] = {
#define mia_opcode(_name, _types, _operands) { #_name, _types, _operands, },
#include "mia/opcodes.def"
#undef mia_opcode
};

const char* mia_opcode_name(const mia_opcode_e opcode) {
	return _mia_opcode_opcodes[opcode].name;
}

int mia_opcode_operands(const mia_opcode_e opcode) {
	return _mia_opcode_opcodes[opcode].operands;
}
