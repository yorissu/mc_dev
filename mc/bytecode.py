
"""bytecode: the compiled form of a program, its file format and a
disassembler.

the machine is stack based. every function has numbered local slots
(parameters first) and a list of instructions; an instruction is an opcode
name and an optional operand. arithmetic and comparison opcodes are typed:
the type is part of the name ('ADD_S32', 'LT_F64', 'CAST_INT_F32'), so the
machine never inspects values to decide what to do.

opcodes (operand in brackets, stack effect after the colon):

	CONST [k]            : -> constants[k] (a copy)
	NONE                 : -> nothing (what a function without a result returns)
	POP                  : x ->
	DUP                  : x -> x x
	LOAD [slot]          : -> local
	LOAD_DEREF [slot]    : -> value of the reference parameter in the slot
	STORE [slot]         : x -> (assigns into the slot's value, keeping arrays in place)
	STORE_NEW [slot]     : x -> (a new variable)
	LOC_LOCAL [slot]     : -> location of a local
	LOC_FIELD [i]        : location -> location of field i
	LOC_INDEX_ARRAY      : location index -> location of the element
	LOC_INDEX_SLICE      : slice index -> location of the element
	TEMP_LOC             : x -> location of a temporary holding x
	LOAD_LOC             : location -> value
	STORE_LOC            : location x ->
	FIELD [i]            : struct -> field i
	SET_FIELD [i]        : struct x -> struct (field i set)
	MAKE_ARRAY [n]       : x1 .. xn -> array
	INDEX_ARRAY          : array index -> element
	INDEX_SLICE          : slice index -> element
	LEN_SLICE            : slice -> uint
	SLICE_LOC [flags]    : location [low] [high] -> slice of the array at the location
	SLICE_SLICE [flags]  : slice [low] [high] -> slice
	                       flags: 1 low given, 2 high given, 4 inclusive, 8 read-only,
	                       16 static (constant data that lives as long as the program)
	SLICE_IS_NULL        : slice -> bool
	JUMP [target]
	JUMP_IF_FALSE [target] : bool ->
	JUMP_IF_TRUE [target]  : bool ->
	RANGE_INIT [slot, inclusive] : start end -> (slots slot..slot+2: current, step, stop)
	RANGE_TEST [slot, target]    : jumps to target when the range is done
	RANGE_STEP [slot]
	CALL [function, n]           : x1 .. xn -> result
	CALL_VALUE [n]               : function x1 .. xn -> result
	CALL_BUILTIN [builtin, n]    : x1 .. xn -> result
	CALL_NATIVE [native, n]      : x1 .. xn -> result
	FUNCTION [function]          : -> function value
	FUNCTION_BUILTIN [builtin]   : -> function value
	RETURN                       : x ->
	ASSERT_FAIL [k]              : stops with the message constants[k]
	typed: ADD SUB MUL DIV MOD AND OR XOR SHL SHR (x y -> z), EQ NE LT GT LE GE
	(x y -> bool), NEG BITNOT (x -> y), NOT (bool -> bool), CAST_<from>_<to>.
	type codes: S8 S16 S32 S64 U8 U16 U32 U64 INT UINT F32 F64 FLOAT BOOL, and FN
	for function values (EQ_FN, NE_FN).

file format ('.mcb'): the line 'mc-bytecode 1' followed by the module as json."""

from __future__ import annotations

import json
from dataclasses import dataclass, field

MAGIC = 'mc-bytecode'
VERSION = 1

TYPE_CODES = ('S8', 'S16', 'S32', 'S64', 'U8', 'U16', 'U32', 'U64', 'INT', 'UINT', 'F32', 'F64', 'FLOAT', 'BOOL', 'FN')

SLICE_LOW, SLICE_HIGH, SLICE_INCLUSIVE, SLICE_READ_ONLY, SLICE_STATIC = 1, 2, 4, 8, 16


@dataclass
class Function:
	name: str
	parameters: int
	locals: int = 0
	returns: bool = False
	code: list = field(default_factory=list)          # [opcode, operand]
	positions: list = field(default_factory=list)     # [file, line, column, length] per instruction


@dataclass
class Module:
	functions: list[Function] = field(default_factory=list)
	constants: list = field(default_factory=list)
	types: list = field(default_factory=list)         # type descriptors (see compiler.describe)
	builtins: list = field(default_factory=list)      # [name, [type indexes]]
	natives: list = field(default_factory=list)       # extern function names
	files: list = field(default_factory=list)
	entry: int = -1
	entry_returns: bool = False


# --- serialization -----------------------------------------------------------

def encode_value(value: object) -> object:
	from .vm import FunctionValue, Slice
	if isinstance(value, list):
		return [encode_value(item) for item in value]
	if isinstance(value, Slice):
		data = value.block.data if value.block is not None else []
		return {'slice': encode_value(data), 'start': value.start, 'length': value.length, 'read_only': value.read_only}
	if isinstance(value, FunctionValue):
		return {'function': value.index} if value.kind == 'function' else {'builtin': value.index}
	return value


def decode_value(value: object) -> object:
	from .vm import Block, FunctionValue, Slice
	if isinstance(value, list):
		return [decode_value(item) for item in value]
	if isinstance(value, dict):
		if 'slice' in value:
			data = decode_value(value['slice'])
			block = Block(data, None, 'static') if data or value['length'] else None
			return Slice(block, value['start'], value['length'], value['read_only'])
		if 'function' in value:
			return FunctionValue('function', value['function'])
		return FunctionValue('builtin', value['builtin'])
	return value


def save(module: Module, path: str) -> None:
	data = {
		'functions': [vars(function) for function in module.functions],
		'constants': [encode_value(value) for value in module.constants],
		'types': module.types, 'builtins': module.builtins, 'natives': module.natives,
		'files': module.files, 'entry': module.entry, 'entry_returns': module.entry_returns,
	}
	with open(path, 'w', encoding='utf-8', newline='\n') as file:
		file.write(f'{MAGIC} {VERSION}\n')
		json.dump(data, file, separators=(',', ':'))
		file.write('\n')


def load(path: str) -> Module:
	with open(path, encoding='utf-8') as file:
		header = file.readline().split()
		if header != [MAGIC, str(VERSION)]:
			raise ValueError(f'`{path}` is not an mc bytecode file (version {VERSION})')
		data = json.load(file)
	module = Module()
	module.functions = [Function(**function) for function in data['functions']]
	module.constants = [decode_value(value) for value in data['constants']]
	module.types = data['types']
	module.builtins = data['builtins']
	module.natives = data['natives']
	module.files = data['files']
	module.entry = data['entry']
	module.entry_returns = data['entry_returns']
	return module


# --- disassembler ------------------------------------------------------------

def disassemble(module: Module) -> str:
	from .vm import format_constant
	out = []
	for index, value in enumerate(module.constants):
		out.append(f'constant {index}: {format_constant(value)}\n')
	if module.constants:
		out.append('\n')
	for index, function in enumerate(module.functions):
		marker = '  (entry)' if index == module.entry else ''
		out.append(f'function {index} {function.name}: {function.parameters} parameter(s), {function.locals} local(s)'
			f'{", returns a value" if function.returns else ""}{marker}\n')
		for offset, (opcode, operand) in enumerate(function.code):
			text = opcode if operand is None else f'{opcode} {operand}'
			comment = ''
			if opcode in ('CALL', 'FUNCTION'):
				target = operand[0] if isinstance(operand, list) else operand
				comment = module.functions[target].name
			elif opcode in ('CALL_BUILTIN', 'FUNCTION_BUILTIN'):
				target = operand[0] if isinstance(operand, list) else operand
				comment = f'builtin {module.builtins[target][0]}'
			elif opcode == 'CALL_NATIVE':
				comment = f'extern {module.natives[operand[0]]}'
			elif opcode in ('CONST', 'ASSERT_FAIL'):
				comment = format_constant(module.constants[operand])
			if len(comment) > 40:
				comment = comment[:37] + '...'
			position = function.positions[offset]
			where = f'{module.files[position[0]].replace(chr(92), "/").split("/")[-1]}:{position[1]}' if position else ''
			out.append(f'  {offset:4}  {text:<28} {where:<16} {comment}'.rstrip() + '\n')
		out.append('\n')
	return ''.join(out)
