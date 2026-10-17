
"""virtual machine: runs a bytecode module.

values: integers, floats and bools are python values; arrays and structs are
lists (copied on every assignment, written into in place so that slices of
an array keep seeing it); slices are views (block, start, length) of a block
of memory; references and assignment targets are locations.

memory model: a block belongs to the heap (alloc, until free or realloc),
to a function call (local arrays, until the call returns), to another block
(arrays inside it), or to the program (constants). every access through a
slice or a location checks that its block is still alive and that the index
is in bounds. slices of constants and of non-mut arrays are read-only."""

from __future__ import annotations

import math
import struct
import sys
from typing import TextIO

from .bytecode import SLICE_HIGH, SLICE_INCLUSIVE, SLICE_LOW, SLICE_READ_ONLY, SLICE_STATIC, TYPE_CODES, Module
from .evaluate import integer_divide, wrap
from .types import SCALARS, Scalar

CALL_DEPTH_LIMIT = 10_000


class MachineError(Exception):
	"""a runtime error: the program stops."""

	def __init__(self, message: str, trace: list | None = None) -> None:
		super().__init__(message)
		self.message = message
		self.trace = trace or []      # [(function name, position)] innermost first


class Frame:
	__slots__ = ('function', 'code', 'locals', 'pc', 'alive')

	def __init__(self, function, code, local_values) -> None:
		self.function = function
		self.code = code
		self.locals = local_values
		self.pc = 0
		self.alive = True


class Block:
	"""memory that slices view. 'owner' is the call, block or nothing it lives in."""
	__slots__ = ('data', 'owner', 'kind', 'freed')

	def __init__(self, data: list, owner: object, kind: str) -> None:
		self.data = data
		self.owner = owner
		self.kind = kind              # 'heap', 'stack' or 'static'
		self.freed = False

	@property
	def alive(self) -> bool:
		return not self.freed and (self.owner is None or self.owner.alive)


class Slice:
	__slots__ = ('block', 'start', 'length', 'read_only')

	def __init__(self, block: Block | None, start: int, length: int, read_only: bool = False) -> None:
		self.block = block
		self.start = start
		self.length = length
		self.read_only = read_only


NULL_SLICE = Slice(None, 0, 0, False)


class Location:
	"""a place: an assignment target or a reference argument."""
	__slots__ = ('container', 'key', 'owner', 'block', 'read_only')

	def __init__(self, container, key, owner, block: Block | None = None, read_only: bool = False) -> None:
		self.container = container
		self.key = key
		self.owner = owner
		self.block = block
		self.read_only = read_only


class FunctionValue:
	__slots__ = ('kind', 'index')

	def __init__(self, kind: str, index: int) -> None:
		self.kind = kind              # 'function' or 'builtin'
		self.index = index

	def __eq__(self, other: object) -> bool:
		return isinstance(other, FunctionValue) and (other.kind, other.index) == (self.kind, self.index)

	def __hash__(self) -> int:
		return hash((self.kind, self.index))


def copy(value: object) -> object:
	if type(value) is list:
		return [copy(item) for item in value]
	return value


def assign(old: object, new: object) -> object:
	"""the new value of a place: arrays and structs are written in place."""
	if type(old) is list and type(new) is list and len(old) == len(new):
		for index, item in enumerate(new):
			old[index] = assign(old[index], item)
		return old
	return copy(new)


def format_constant(value: object) -> str:
	if isinstance(value, Slice):
		data = value.block.data[value.start:value.start + value.length] if value.block else []
		return f'slice {format_constant(data)}'
	if isinstance(value, FunctionValue):
		return f'{value.kind} {value.index}'
	if isinstance(value, list):
		return '{' + ', '.join(format_constant(item) for item in value) + '}'
	if isinstance(value, bool):
		return 'true' if value else 'false'
	return repr(value)


def format_float(value: float, bits: int) -> str:
	if math.isnan(value):
		return 'nan'
	if math.isinf(value):
		return 'inf' if value > 0 else '-inf'
	if bits == 32:
		for precision in range(1, 10):
			text = f'{value:.{precision}g}'
			if struct.unpack('f', struct.pack('f', float(text)))[0] == value:
				break
	else:
		text = repr(value)
	if 'e' not in text and '.' not in text and 'n' not in text:
		text += '.0'
	return text


def scalar_of(code: str) -> Scalar:
	return Scalar(code.lower())


# --- natives (functions declared '[extern = "name"]') ---------------------------

def native_write(machine: Machine, arguments: list) -> None:
	machine.write(machine.text(arguments[0]))


def native_write_line(machine: Machine, arguments: list) -> None:
	machine.write(machine.text(arguments[0]) + '\n')


NATIVES = {
	'mc_write': native_write,
	'mc_write_line': native_write_line,
}


class Machine:
	def __init__(self, module: Module, out: TextIO | None = None, natives: dict | None = None,
			limit: int | None = None) -> None:
		self.module = module
		self.limit = limit            # stop after this many instructions (none: no limit)
		self.out = out if out is not None else sys.stdout
		self.natives = dict(NATIVES)
		self.natives.update(natives or {})
		self.stack: list = []
		self.frames: list[Frame] = []
		self.result: object = None
		self.code = [self.decode(function) for function in module.functions]
		missing = [name for name in module.natives if name not in self.natives]
		if missing:
			raise MachineError(f'unknown extern function `{missing[0]}`')
		for name, _ in module.builtins:
			if name not in BUILTINS:
				raise MachineError(f'unknown builtin function `{name}`')

	# --- running -------------------------------------------------------------------

	def run(self) -> object:
		"""runs the entry function; returns its result (none or an int)."""
		self.call(self.module.entry, [])
		budget = self.limit
		try:
			while self.frames:
				frame = self.frames[-1]
				code = frame.code
				if budget is None:
					while True:
						handler, operand = code[frame.pc]
						frame.pc += 1
						if handler(frame, operand):
							break
				else:
					while True:
						budget -= 1
						if budget < 0:
							raise self.fail(f'stopped after {self.limit} instructions')
						handler, operand = code[frame.pc]
						frame.pc += 1
						if handler(frame, operand):
							break
		except MachineError as error:
			error.trace = self.trace()
			raise
		except RecursionError:
			raise MachineError('values nest too deeply', self.trace()) from None
		return self.result

	def trace(self) -> list:
		trace = []
		for frame in reversed(self.frames):
			function = self.module.functions[frame.function]
			index = max(0, frame.pc - 1)
			position = function.positions[index] if index < len(function.positions) else None
			trace.append((function.name, position))
		return trace

	def fail(self, message: str) -> MachineError:
		return MachineError(message)

	def call(self, index: int, arguments: list) -> None:
		if len(self.frames) >= CALL_DEPTH_LIMIT:
			raise self.fail(f'stack overflow: more than {CALL_DEPTH_LIMIT} nested calls')
		function = self.module.functions[index]
		local_values = arguments + [None] * (function.locals - len(arguments))
		self.frames.append(Frame(index, self.code[index], local_values))

	def write(self, text: str) -> None:
		self.out.write(text)

	def text(self, value: object) -> str:
		data = self.elements(value) if isinstance(value, Slice) else value
		return bytes(data).decode('utf-8', 'replace')

	# --- memory ----------------------------------------------------------------------

	def check(self, block: Block | None) -> None:
		if block is not None and not block.alive:
			if block.kind == 'heap' or (block.owner is not None and isinstance(block.owner, Block) and block.owner.kind == 'heap'):
				raise self.fail('use of freed heap memory')
			raise self.fail('use of a local array of a function that has returned')

	def elements(self, value: Slice) -> list:
		if value.block is None:
			return []
		self.check(value.block)
		return value.block.data[value.start:value.start + value.length]

	def get(self, location: Location) -> object:
		if location.block is not None:
			self.check(location.block)
		return location.container[location.key]

	def set(self, location: Location, value: object) -> None:
		if location.block is not None:
			self.check(location.block)
		if location.read_only:
			raise self.fail('write through a read-only slice')
		location.container[location.key] = assign(location.container[location.key], value)

	def slice_of(self, data: list, owner: object, read_only: bool, flags: int, low: int | None, high: int | None,
			start: int = 0, length: int | None = None, block: Block | None = None) -> Slice:
		length = len(data) if length is None else length
		low = 0 if low is None else low
		high = length if high is None else high + (1 if flags & SLICE_INCLUSIVE else 0)
		if not (0 <= low <= high <= length):
			raise self.fail(f'slice {low}..{high} is out of bounds (length {length})')
		if block is None:
			block = Block(data, owner, 'stack' if owner is not None else 'static')
		return Slice(block, start + low, high - low, read_only)

	def zero(self, type_index: int) -> object:
		zero = self.module.types[type_index]['zero']
		return copy(self.module.constants[zero])

	def format(self, value: object, type_index: int) -> str:
		descriptor = self.module.types[type_index]
		kind = descriptor['kind']
		if kind == 'int':
			return str(value)
		if kind == 'float':
			return format_float(value, SCALARS[descriptor['name']][1])
		if kind == 'bool':
			return 'true' if value else 'false'
		if kind == 'function':
			return f'<function {self.module.functions[value.index].name if value.kind == "function" else self.module.builtins[value.index][0]}>'
		if kind in ('array', 'slice'):
			items = self.elements(value) if kind == 'slice' else value
			element = self.module.types[descriptor['element']]
			if element['name'] == 'u8':
				if items and items[-1] == 0:
					items = items[:-1]
				return bytes(items).decode('utf-8', 'replace')
			return '{' + ', '.join(self.format(item, descriptor['element']) for item in items) + '}'
		fields = descriptor['fields']
		return '{' + ', '.join(f'{name}: {self.format(item, field_type)}' for (name, field_type), item in zip(fields, value)) + '}'

	# --- decoding --------------------------------------------------------------------

	def decode(self, function) -> list:
		code = []
		for opcode, operand in function.code:
			handler = getattr(self, 'op_' + opcode, None)
			if handler is None:
				handler = self.typed(opcode)
			code.append((handler, operand))
		return code

	def typed(self, opcode: str):
		stack = self.stack
		if opcode.startswith('CAST_'):
			source, target = opcode[5:].split('_')
			return self.cast(source, target)
		base, _, code = opcode.rpartition('_')
		if code not in TYPE_CODES:
			raise MachineError(f'unknown opcode `{opcode}`')
		type_ = scalar_of(code) if code != 'FN' else None
		is_float = type_ is not None and type_.is_float

		def binary(operation):
			def handler(frame, operand):
				right = stack.pop()
				stack[-1] = operation(stack[-1], right)
			return handler

		if base in ('EQ', 'NE', 'LT', 'GT', 'LE', 'GE'):
			compare = {'EQ': lambda a, b: a == b, 'NE': lambda a, b: a != b, 'LT': lambda a, b: a < b,
				'GT': lambda a, b: a > b, 'LE': lambda a, b: a <= b, 'GE': lambda a, b: a >= b}[base]
			return binary(compare)
		if base == 'ADD':
			return binary(lambda a, b: wrap(a + b, type_))
		if base == 'SUB':
			return binary(lambda a, b: wrap(a - b, type_))
		if base == 'MUL':
			return binary(lambda a, b: wrap(a * b, type_))
		if base == 'DIV':
			if is_float:
				def divide(a, b):
					if b == 0:
						return math.nan if a == 0 or math.isnan(a) else math.copysign(math.inf, a) * math.copysign(1.0, b)
					return wrap(a / b, type_)
				return binary(divide)

			def integer(a, b):
				if b == 0:
					raise self.fail('division by zero')
				return wrap(integer_divide(a, b), type_)
			return binary(integer)
		if base == 'MOD':
			def remainder(a, b):
				if b == 0:
					raise self.fail('division by zero')
				return wrap(a - b * integer_divide(a, b), type_)
			return binary(remainder)
		if base in ('AND', 'OR', 'XOR'):
			operation = {'AND': lambda a, b: a & b, 'OR': lambda a, b: a | b, 'XOR': lambda a, b: a ^ b}[base]
			return binary(lambda a, b: wrap(operation(a, b), type_))
		if base in ('SHL', 'SHR'):
			def shift(a, b):
				if b < 0:
					raise self.fail(f'negative shift count {b}')
				b = min(b, 4096)
				return wrap(a << b if base == 'SHL' else a >> b, type_)
			return binary(shift)
		if base == 'NEG':
			def negate(frame, operand):
				stack[-1] = wrap(-stack[-1], type_)
			return negate
		if base == 'BITNOT':
			def invert(frame, operand):
				stack[-1] = wrap(~stack[-1], type_)
			return invert
		raise MachineError(f'unknown opcode `{opcode}`')

	def cast(self, source: str, target: str):
		stack = self.stack
		target_type = scalar_of(target)
		source_type = scalar_of(source)

		def handler(frame, operand):
			value = stack[-1]
			if target_type.is_integer:
				if source_type.is_float:
					if not math.isfinite(value):
						raise self.fail(f'cannot convert {format_float(value, 64)} to `{target_type}`')
					value = int(value)
				stack[-1] = wrap(int(value), target_type)
			elif target_type.is_float:
				stack[-1] = wrap(float(value), target_type)
		return handler

	# --- opcodes -----------------------------------------------------------------------

	def op_CONST(self, frame, operand):
		self.stack.append(copy(self.module.constants[operand]))

	def op_NONE(self, frame, operand):
		self.stack.append(None)

	def op_POP(self, frame, operand):
		self.stack.pop()

	def op_DUP(self, frame, operand):
		self.stack.append(self.stack[-1])

	def op_LOAD(self, frame, operand):
		self.stack.append(frame.locals[operand])

	def op_LOAD_DEREF(self, frame, operand):
		self.stack.append(self.get(frame.locals[operand]))

	def op_STORE(self, frame, operand):
		frame.locals[operand] = assign(frame.locals[operand], self.stack.pop())

	def op_STORE_NEW(self, frame, operand):
		frame.locals[operand] = copy(self.stack.pop())

	def op_LOC_LOCAL(self, frame, operand):
		self.stack.append(Location(frame.locals, operand, frame))

	def op_LOC_FIELD(self, frame, operand):
		parent = self.stack[-1]
		self.stack[-1] = Location(self.get(parent), operand, parent.owner, parent.block, parent.read_only)

	def op_LOC_INDEX_ARRAY(self, frame, operand):
		index = self.stack.pop()
		parent = self.stack[-1]
		array = self.get(parent)
		if not 0 <= index < len(array):
			raise self.fail(f'index {index} is out of bounds (length {len(array)})')
		self.stack[-1] = Location(array, index, parent.owner, parent.block, parent.read_only)

	def op_LOC_INDEX_SLICE(self, frame, operand):
		index = self.stack.pop()
		value = self.stack[-1]
		if not 0 <= index < value.length:
			raise self.fail(f'index {index} is out of bounds (length {value.length})')
		self.check(value.block)
		self.stack[-1] = Location(value.block.data, value.start + index, value.block, value.block, value.read_only)

	def op_TEMP_LOC(self, frame, operand):
		self.stack[-1] = Location([self.stack[-1]], 0, frame)

	def op_LOAD_LOC(self, frame, operand):
		self.stack[-1] = self.get(self.stack[-1])

	def op_STORE_LOC(self, frame, operand):
		value = self.stack.pop()
		self.set(self.stack.pop(), value)

	def op_FIELD(self, frame, operand):
		self.stack[-1] = self.stack[-1][operand]

	def op_SET_FIELD(self, frame, operand):
		value = self.stack.pop()
		self.stack[-1][operand] = copy(value)

	def op_MAKE_ARRAY(self, frame, operand):
		if operand:
			items = [copy(item) for item in self.stack[-operand:]]
			del self.stack[-operand:]
		else:
			items = []
		self.stack.append(items)

	def op_INDEX_ARRAY(self, frame, operand):
		index = self.stack.pop()
		array = self.stack[-1]
		if not 0 <= index < len(array):
			raise self.fail(f'index {index} is out of bounds (length {len(array)})')
		self.stack[-1] = array[index]

	def op_INDEX_SLICE(self, frame, operand):
		index = self.stack.pop()
		value = self.stack[-1]
		if not 0 <= index < value.length:
			raise self.fail(f'index {index} is out of bounds (length {value.length})')
		self.check(value.block)
		self.stack[-1] = value.block.data[value.start + index]

	def op_LEN_SLICE(self, frame, operand):
		self.stack[-1] = self.stack[-1].length

	def slice_bounds(self, flags: int) -> tuple:
		high = self.stack.pop() if flags & SLICE_HIGH else None
		low = self.stack.pop() if flags & SLICE_LOW else None
		return low, high

	def op_SLICE_LOC(self, frame, operand):
		low, high = self.slice_bounds(operand)
		location = self.stack.pop()
		array = self.get(location)
		owner = location.block if location.block is not None else location.owner
		if operand & SLICE_STATIC:
			owner = None
		read_only = bool(operand & SLICE_READ_ONLY) or location.read_only
		self.stack.append(self.slice_of(array, owner, read_only, operand, low, high))

	def op_SLICE_SLICE(self, frame, operand):
		low, high = self.slice_bounds(operand)
		value = self.stack.pop()
		if value.block is None:
			self.stack.append(self.slice_of([], None, value.read_only, operand, low, high, block=None) if (low or high) else value)
			return
		self.check(value.block)
		self.stack.append(self.slice_of(value.block.data, None, value.read_only, operand, low, high,
			start=value.start, length=value.length, block=value.block))

	def op_SLICE_IS_NULL(self, frame, operand):
		self.stack[-1] = self.stack[-1].length == 0

	def op_JUMP(self, frame, operand):
		frame.pc = operand

	def op_JUMP_IF_FALSE(self, frame, operand):
		if not self.stack.pop():
			frame.pc = operand

	def op_JUMP_IF_TRUE(self, frame, operand):
		if self.stack.pop():
			frame.pc = operand

	def op_NOT(self, frame, operand):
		self.stack[-1] = not self.stack[-1]

	def op_RANGE_INIT(self, frame, operand):
		slot, inclusive = operand
		end = self.stack.pop()
		start = self.stack.pop()
		step = 1 if end >= start else -1
		frame.locals[slot] = start
		frame.locals[slot + 1] = step
		frame.locals[slot + 2] = end + step if inclusive else end

	def op_RANGE_TEST(self, frame, operand):
		slot, target = operand
		if frame.locals[slot] == frame.locals[slot + 2]:
			frame.pc = target

	def op_RANGE_STEP(self, frame, operand):
		frame.locals[operand] += frame.locals[operand + 1]

	def arguments(self, count: int) -> list:
		if not count:
			return []
		values = [value if isinstance(value, Location) else copy(value) for value in self.stack[-count:]]
		del self.stack[-count:]
		return values

	def op_CALL(self, frame, operand):
		index, count = operand
		self.call(index, self.arguments(count))
		return True

	def op_CALL_VALUE(self, frame, operand):
		arguments = self.arguments(operand)
		function = self.stack.pop()
		if function.kind == 'builtin':
			self.stack.append(self.builtin(function.index, arguments))
			return False
		self.call(function.index, arguments)
		return True

	def builtin(self, index: int, arguments: list) -> object:
		name, type_arguments = self.module.builtins[index]
		return BUILTINS[name](self, arguments, type_arguments)

	def op_CALL_BUILTIN(self, frame, operand):
		index, count = operand
		self.stack.append(self.builtin(index, self.arguments(count)))

	def op_CALL_NATIVE(self, frame, operand):
		index, count = operand
		name = self.module.natives[index]
		self.stack.append(self.natives[name](self, self.arguments(count)))

	def op_FUNCTION(self, frame, operand):
		self.stack.append(FunctionValue('function', operand))

	def op_FUNCTION_BUILTIN(self, frame, operand):
		self.stack.append(FunctionValue('builtin', operand))

	def op_RETURN(self, frame, operand):
		value = self.stack.pop()
		frame.alive = False
		self.frames.pop()
		if self.frames:
			self.stack.append(value)
		else:
			self.result = value
		return True

	def op_ASSERT_FAIL(self, frame, operand):
		raise self.fail(self.module.constants[operand])


# --- builtins (functions declared '[builtin]'): (machine, arguments, type arguments) ---

def heap_allocation(machine: Machine, value: Slice, what: str) -> Block:
	block = value.block
	if block is None or block.kind != 'heap':
		raise machine.fail(f'{what} of memory that `alloc` did not return')
	machine.check(block)
	if value.start != 0 or value.length != len(block.data):
		raise machine.fail(f'{what} needs the whole allocation, not a part of it')
	return block


def builtin_alloc(machine: Machine, arguments: list, types_: list) -> Slice:
	count = arguments[0]
	data = [machine.zero(types_[0]) for _ in range(count)]
	return Slice(Block(data, None, 'heap'), 0, count)


def builtin_realloc(machine: Machine, arguments: list, types_: list) -> Slice:
	value, count = arguments
	if value.block is None:
		return builtin_alloc(machine, [count], types_)
	block = heap_allocation(machine, value, '`realloc`')
	data = block.data[:count] + [machine.zero(types_[0]) for _ in range(max(0, count - len(block.data)))]
	block.freed = True
	return Slice(Block(data, None, 'heap'), 0, count)


def builtin_free(machine: Machine, arguments: list, types_: list) -> Slice:
	value = arguments[0]
	if value.block is not None:
		heap_allocation(machine, value, '`free`').freed = True
	return NULL_SLICE


def builtin_copy(machine: Machine, arguments: list, types_: list) -> int:
	target, source = arguments
	if target.read_only and target.length:
		raise machine.fail('`copy` into a read-only slice')
	items = machine.elements(source)
	count = min(target.length, len(items))
	if count:
		machine.check(target.block)
		for index in range(count):
			target.block.data[target.start + index] = copy(items[index])
	return count


def formatted(machine: Machine, arguments: list, types_: list) -> str:
	"""the format string (the first argument) with each '{}' replaced by the
	next value. the compiler has checked that they match."""
	data = machine.elements(arguments[0])
	values = iter(zip(arguments[1:], types_))
	text = bytearray()
	parts = []
	index = 0
	while index < len(data):
		character = data[index]
		following = data[index + 1] if index + 1 < len(data) else None
		if character == 0x7B and following == 0x7D:
			parts.append(text.decode('utf-8', 'replace'))
			text = bytearray()
			value = next(values, None)
			if value is not None:
				parts.append(machine.format(*value))
			index += 2
			continue
		if character in (0x7B, 0x7D) and following == character:
			index += 1
		text.append(character)
		index += 1
	parts.append(text.decode('utf-8', 'replace'))
	return ''.join(parts)


def builtin_print(machine: Machine, arguments: list, types_: list) -> None:
	machine.write(formatted(machine, arguments, types_))


def builtin_println(machine: Machine, arguments: list, types_: list) -> None:
	machine.write(formatted(machine, arguments, types_) + '\n')


BUILTINS = {
	'alloc': builtin_alloc,
	'realloc': builtin_realloc,
	'free': builtin_free,
	'copy': builtin_copy,
	'print': builtin_print,
	'println': builtin_println,
}


def run(module: Module, out: TextIO | None = None) -> object:
	return Machine(module, out).run()
