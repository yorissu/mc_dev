
"""compiler: a checked program -> a bytecode module.

only what 'main' can reach is compiled. every function instance becomes one
bytecode function, every lambda too; operator uses become direct calls,
constants become entries of the constant pool. struct values are lists of
their fields in declaration order."""

from __future__ import annotations

from . import ast
from .bytecode import SLICE_HIGH, SLICE_INCLUSIVE, SLICE_LOW, SLICE_READ_ONLY, SLICE_STATIC, Function, Module
from .checker import Instance, Program
from .evaluate import FunctionValue as ConstantFunction
from .evaluate import SliceValue as ConstantSlice
from .evaluate import wrap
from .types import (UNTYPED_FLOAT, UNTYPED_INTEGER, VARIADIC, Array, FunctionType, Reference, Scalar, Slice, StructType,
	Type, is_untyped)
from .vm import NULL_SLICE, Block, FunctionValue
from .vm import Slice as SliceRuntime

OPCODES = {
	'+': 'ADD', '-': 'SUB', '*': 'MUL', '/': 'DIV', '%': 'MOD', '&': 'AND', '|': 'OR', '^': 'XOR', '<<': 'SHL', '>>': 'SHR',
	'==': 'EQ', '!=': 'NE', '<': 'LT', '>': 'GT', '<=': 'LE', '>=': 'GE',
}


class CompileError(Exception):
	"""a construct the compiler cannot translate (a checker bug, or unsupported)."""


def type_code(type_: Type) -> str:
	if isinstance(type_, Scalar):
		return type_.name.upper()
	if type_ is UNTYPED_INTEGER:
		return 'INT'
	if type_ is UNTYPED_FLOAT:
		return 'FLOAT'
	if isinstance(type_, FunctionType):
		return 'FN'
	raise CompileError(f'no opcode type for `{type_}`')


class Compiler:
	def __init__(self, program: Program) -> None:
		self.program = program
		self.checker = program.checker
		self.module = Module()
		self.function_indexes: dict[int, int] = {}
		self.pending: list[tuple[object, int]] = []
		self.type_indexes: dict[Type, int] = {}
		self.builtin_indexes: dict[tuple, int] = {}
		self.native_indexes: dict[str, int] = {}
		self.file_indexes: dict[str, int] = {}
		self.scalar_constants: dict[tuple, int] = {}

	def compile(self) -> Module:
		entry = self.program.entry
		main = entry.symbols['main'].instances[()]
		self.module.entry = self.function(main)
		self.module.entry_returns = main.result is not None
		while self.pending:
			target, index = self.pending.pop(0)
			self.compile_function(target, self.module.functions[index])
		return self.module

	# --- tables ------------------------------------------------------------------

	def function(self, target: object) -> int:
		"""the index of an instance or lambda, compiled later."""
		key = id(target)
		if key in self.function_indexes:
			return self.function_indexes[key]
		if isinstance(target, Instance):
			name = str(target)
			parameters = len(target.parameters)
		else:
			line, _ = target.span.source.location(target.span.start)
			name = f'lambda at {target.span.source.path.replace(chr(92), "/").split("/")[-1]}:{line}'
			parameters = len(target.parameters)
		index = len(self.module.functions)
		self.module.functions.append(Function(name, parameters))
		self.function_indexes[key] = index
		self.pending.append((target, index))
		return index

	def constant(self, value: object) -> int:
		if isinstance(value, (bool, int, float, str)) or value is None:
			key = (type(value).__name__, repr(value))
			if key in self.scalar_constants:
				return self.scalar_constants[key]
			self.scalar_constants[key] = len(self.module.constants)
		self.module.constants.append(value)
		return len(self.module.constants) - 1

	def builtin(self, instance: Instance, extra: tuple = ()) -> int:
		"""a builtin with its type arguments (and the types of its '...' values)."""
		key = (instance.function.name, tuple(self.describe(argument) for argument in tuple(instance.arguments) + tuple(extra)))
		if key not in self.builtin_indexes:
			self.builtin_indexes[key] = len(self.module.builtins)
			self.module.builtins.append([key[0], list(key[1])])
		return self.builtin_indexes[key]

	def native(self, name: str) -> int:
		if name not in self.native_indexes:
			self.native_indexes[name] = len(self.module.natives)
			self.module.natives.append(name)
		return self.native_indexes[name]

	def position(self, span) -> list | None:
		if span is None:
			return None
		path = span.source.path
		if path not in self.file_indexes:
			self.file_indexes[path] = len(self.module.files)
			self.module.files.append(path)
		line, column = span.source.location(span.start)
		end_line, end_column = span.source.location(max(span.start, span.end - 1))
		length = end_column - column + 1 if end_line == line else len(span.source.line_text(line)) - column + 1
		return [self.file_indexes[path], line, column, max(1, length)]

	def describe(self, type_: Type) -> int:
		"""the index of a type descriptor (used for zero values and printing)."""
		if type_ in self.type_indexes:
			return self.type_indexes[type_]
		index = len(self.module.types)
		self.type_indexes[type_] = index
		descriptor: dict = {'name': str(type_)}
		self.module.types.append(descriptor)
		if isinstance(type_, Scalar):
			descriptor['kind'] = 'bool' if type_.name == 'bool' else 'float' if type_.is_float else 'int'
		elif isinstance(type_, Array):
			descriptor.update(kind='array', element=self.describe(type_.element), length=type_.length)
		elif isinstance(type_, Slice):
			descriptor.update(kind='slice', element=self.describe(type_.element))
		elif isinstance(type_, StructType):
			fields = self.checker.layout(type_).fields
			descriptor.update(kind='struct', fields=[[name, self.describe(item.type)] for name, item in fields.items()])
		elif isinstance(type_, FunctionType):
			descriptor['kind'] = 'function'
		else:
			raise CompileError(f'cannot describe `{type_}`')
		descriptor['zero'] = None if isinstance(type_, FunctionType) else self.constant(self.zero(type_))
		return index

	# --- values ------------------------------------------------------------------

	def zero(self, type_: Type) -> object:
		if isinstance(type_, Scalar):
			return False if type_.name == 'bool' else 0.0 if type_.is_float else 0
		if isinstance(type_, Slice):
			return NULL_SLICE
		if isinstance(type_, Array):
			return [self.zero(type_.element) for _ in range(type_.length or 0)]
		if isinstance(type_, StructType):
			value = []
			for item in self.checker.layout(type_).fields.values():
				if item.default is not None:
					value.append(self.convert(self.checker.evaluator.constant(item.default, None), item.type))
				else:
					value.append(self.zero(item.type))
			return value
		return None

	def convert(self, value: object, type_: Type) -> object:
		"""a compile-time value (evaluate.py) as a machine value."""
		if is_untyped(type_):
			return float(value) if type_ is UNTYPED_FLOAT else value
		if isinstance(type_, Scalar):
			if type_.name == 'bool':
				return bool(value)
			return wrap(float(value) if type_.is_float else value, type_)
		if isinstance(type_, Array):
			return [self.convert(item, type_.element) for item in value]
		if isinstance(type_, Slice):
			if not isinstance(value, ConstantSlice) or (not value.data and value.length == 0):
				return NULL_SLICE
			data = [self.convert(item, type_.element) for item in value.data]
			return SliceRuntime(Block(data, None, 'static'), value.start, value.length, True)
		if isinstance(type_, StructType):
			fields = self.checker.layout(type_).fields
			return [self.convert(value[name], item.type) for name, item in fields.items()]
		if isinstance(type_, FunctionType):
			if isinstance(value, ConstantFunction):
				target = value.target
				if isinstance(target, Instance) and target.function.builtin:
					return FunctionValue('builtin', self.builtin(target))
				return FunctionValue('function', self.function(target))
		raise CompileError(f'cannot convert a constant of type `{type_}`')

	def compile_function(self, target: object, function: Function) -> None:
		if isinstance(target, Instance):
			parameters, body, returns = target.node.parameters, target.node.body, target.result is not None
		else:
			parameters, body, returns = target.parameters, target.body, target.result is not None
		function.returns = returns
		FunctionCompiler(self, function, parameters).run(body)


class FunctionCompiler:
	def __init__(self, compiler: Compiler, function: Function, parameters: list[ast.Param]) -> None:
		self.compiler = compiler
		self.function = function
		self.slots: dict = {}
		self.count = 0
		self.span = None
		self.loops: list[dict] = []
		for parameter in parameters:
			self.slots[parameter.variable] = self.new_slot()

	def new_slot(self, size: int = 1) -> int:
		slot = self.count
		self.count += size
		return slot

	def slot(self, variable) -> int:
		if variable not in self.slots:
			self.slots[variable] = self.new_slot()
		return self.slots[variable]

	def emit(self, opcode: str, operand: object = None) -> int:
		self.function.code.append([opcode, operand])
		self.function.positions.append(self.compiler.position(self.span))
		return len(self.function.code) - 1

	def here(self) -> int:
		return len(self.function.code)

	def patch(self, index: int, target: int) -> None:
		instruction = self.function.code[index]
		if instruction[0] == 'RANGE_TEST':
			instruction[1][1] = target
		else:
			instruction[1] = target

	def constant(self, value: object) -> None:
		self.emit('CONST', self.compiler.constant(value))

	def run(self, body: ast.Block) -> None:
		self.span = body.span
		self.block(body)
		self.span = type(body.span)(body.span.source, body.span.end - 1, body.span.end)
		self.emit('NONE')
		self.emit('RETURN')
		self.function.locals = self.count

	# --- statements ----------------------------------------------------------------

	def block(self, node: ast.Block) -> None:
		for statement in node.statements:
			self.statement(statement)

	def statement(self, node: ast.Node) -> None:
		saved = self.span
		self.span = node.span
		getattr(self, 's_' + type(node).__name__)(node)
		self.span = saved

	def s_DeclarationStatement(self, node: ast.DeclarationStatement) -> None:
		pass

	def s_Block(self, node: ast.Block) -> None:
		self.block(node)

	def s_Let(self, node: ast.Let) -> None:
		variable = node.variable
		if node.value is not None:
			self.value(node.value)
		else:
			self.constant(self.compiler.zero(variable.type))
		self.emit('STORE_NEW', self.slot(variable))

	def s_ExpressionStatement(self, node: ast.ExpressionStatement) -> None:
		self.value(node.expression)
		self.emit('POP')

	def s_Assign(self, node: ast.Assign) -> None:
		target = node.target
		if isinstance(target, ast.Name) and target.name == '_':
			self.value(node.value)
			self.emit('POP')
			return
		if node.operator == '=':
			variable = getattr(target, 'variable', None)
			if isinstance(target, ast.Name) and variable is not None and not variable.reference:
				self.value(node.value)
				self.emit('STORE', self.slot(variable))
				return
			self.location(target)
			self.value(node.value)
			self.emit('STORE_LOC')
			return
		self.location(target)
		self.emit('DUP')
		self.emit('LOAD_LOC')
		self.value(node.value)
		call = getattr(node, 'operator_call', None)
		if call is not None:
			self.emit_call(call, 2)
		else:
			self.emit(f'{OPCODES[node.operator[:-1]]}_{type_code(target.type_)}')
		self.emit('STORE_LOC')

	def s_If(self, node: ast.If) -> None:
		self.value(node.condition)
		skip = self.emit('JUMP_IF_FALSE', None)
		self.block(node.then)
		if node.otherwise is None:
			self.patch(skip, self.here())
			return
		end = self.emit('JUMP', None)
		self.patch(skip, self.here())
		if isinstance(node.otherwise, ast.If):
			self.statement(node.otherwise)
		else:
			self.block(node.otherwise)
		self.patch(end, self.here())

	def s_While(self, node: ast.While) -> None:
		start = self.here()
		self.value(node.condition)
		leave = self.emit('JUMP_IF_FALSE', None)
		self.loops.append({'breaks': [leave], 'continues': [], 'continue_target': start})
		self.block(node.body)
		self.emit('JUMP', start)
		self.finish_loop(self.here())

	def s_For(self, node: ast.For) -> None:
		base = self.new_slot(3)
		self.value(node.start)
		self.value(node.end)
		self.emit('RANGE_INIT', [base, node.inclusive])
		test = self.here()
		leave = self.emit('RANGE_TEST', [base, None])
		self.emit('LOAD', base)
		self.emit('STORE_NEW', self.slot(node.variable))
		self.loops.append({'breaks': [leave], 'continues': [], 'continue_target': None})
		self.block(node.body)
		step = self.here()
		for index in self.loops[-1]['continues']:
			self.patch(index, step)
		self.emit('RANGE_STEP', base)
		self.emit('JUMP', test)
		self.finish_loop(self.here())

	def finish_loop(self, end: int) -> None:
		loop = self.loops.pop()
		for index in loop['breaks']:
			self.patch(index, end)

	def s_Break(self, node: ast.Break) -> None:
		self.loops[-1]['breaks'].append(self.emit('JUMP', None))

	def s_Continue(self, node: ast.Continue) -> None:
		loop = self.loops[-1]
		if loop['continue_target'] is not None:
			self.emit('JUMP', loop['continue_target'])
		else:
			loop['continues'].append(self.emit('JUMP', None))

	def s_Return(self, node: ast.Return) -> None:
		if node.value is not None:
			self.value(node.value)
		else:
			self.emit('NONE')
		self.emit('RETURN')

	# --- places ------------------------------------------------------------------

	def location(self, node: ast.Node) -> None:
		"""pushes the location of a place (a temporary for other values)."""
		if isinstance(node, ast.Paren):
			self.location(node.inner)
			return
		variable = getattr(node, 'variable', None)
		if isinstance(node, ast.Name) and variable is not None:
			self.emit('LOAD' if variable.reference else 'LOC_LOCAL', self.slot(variable))
			return
		if isinstance(node, ast.Member) and getattr(node, 'field', None) is not None:
			self.location(node.target)
			self.emit('LOC_FIELD', self.field_index(node))
			return
		if isinstance(node, ast.Index) and getattr(node, 'operator_call', None) is None:
			if isinstance(node.target.type_, Array):
				self.location(node.target)
				self.value(node.index)
				self.emit('LOC_INDEX_ARRAY')
			else:
				self.value(node.target)
				self.value(node.index)
				self.emit('LOC_INDEX_SLICE')
			return
		self.value(node)
		self.emit('TEMP_LOC')

	def read_only(self, node: ast.Node) -> bool:
		"""whether a slice of the array 'node' must not be written through."""
		if isinstance(node, ast.Paren):
			return self.read_only(node.inner)
		variable = getattr(node, 'variable', None)
		if isinstance(node, ast.Name):
			if variable is None:
				return True                       # a constant
			return not variable.mutable
		if isinstance(node, ast.Member):
			item = getattr(node, 'field', None)
			if item is None:
				return True
			return not item.mutable or self.read_only(node.target)
		if isinstance(node, ast.Index) and isinstance(node.target.type_, Array):
			return self.read_only(node.target)
		return isinstance(node, ast.StringLiteral)

	def static(self, node: ast.Node) -> bool:
		"""whether 'node' is constant data (a string literal or a constant)."""
		if isinstance(node, ast.Paren):
			return self.static(node.inner)
		if isinstance(node, ast.StringLiteral):
			return True
		if isinstance(node, (ast.Name, ast.Member)):
			return getattr(node, 'constant', None) is not None
		return False

	def field_index(self, node: ast.Member) -> int:
		layout = self.compiler.checker.layout(node.target.type_)
		return list(layout.fields).index(node.name)

	# --- expressions ---------------------------------------------------------------

	def value(self, node: ast.Node) -> None:
		saved = self.span
		self.span = node.span
		getattr(self, 'v_' + type(node).__name__)(node)
		self.span = saved

	def scalar(self, value: object, type_: Type) -> None:
		if isinstance(type_, Scalar):
			value = bool(value) if type_.name == 'bool' else wrap(float(value) if type_.is_float else value, type_)
		elif type_ is UNTYPED_FLOAT:
			value = float(value)
		self.constant(value)

	def v_IntegerLiteral(self, node: ast.IntegerLiteral) -> None:
		self.scalar(node.value, node.type_)

	def v_FloatLiteral(self, node: ast.FloatLiteral) -> None:
		self.scalar(node.value, node.type_)

	def v_CharLiteral(self, node: ast.CharLiteral) -> None:
		self.constant(node.value)

	def v_BoolLiteral(self, node: ast.BoolLiteral) -> None:
		self.constant(node.value)

	def v_StringLiteral(self, node: ast.StringLiteral) -> None:
		self.constant(list(node.data))

	def v_NullLiteral(self, node: ast.NullLiteral) -> None:
		self.constant(NULL_SLICE)

	def v_Paren(self, node: ast.Paren) -> None:
		self.value(node.inner)

	def v_Name(self, node: ast.Name) -> None:
		self.symbol(node)

	def symbol(self, node: ast.Node) -> None:
		variable = getattr(node, 'variable', None)
		if variable is not None:
			if variable not in self.slots:
				raise CompileError(f'`{variable.name}` is not a local of this function')
			self.emit('LOAD_DEREF' if variable.reference else 'LOAD', self.slots[variable])
			return
		constant = getattr(node, 'constant', None)
		if constant is not None:
			type_ = node.type_ if not is_untyped(node.type_) else constant.type
			self.constant(self.compiler.convert(constant.value, type_))
			return
		instance = getattr(node, 'instance', None)
		if instance is not None:
			self.function_value(instance)
			return
		raise CompileError(f'cannot compile `{node.span.text}`')

	def function_value(self, instance: Instance) -> None:
		attributes = {attribute.name: attribute for attribute in instance.function.node.attributes}
		if 'builtin' in attributes:
			self.emit('FUNCTION_BUILTIN', self.compiler.builtin(instance))
		elif 'extern' in attributes:
			raise CompileError('extern functions cannot be used as values yet; wrap them in a function')
		else:
			self.emit('FUNCTION', self.compiler.function(instance))

	def v_Member(self, node: ast.Member) -> None:
		if getattr(node, 'field', None) is None:
			self.symbol(node)
			return
		self.value(node.target)
		self.emit('FIELD', self.field_index(node))

	def v_Specialize(self, node: ast.Specialize) -> None:
		self.function_value(node.instance)

	def v_Lambda(self, node: ast.Lambda) -> None:
		self.emit('FUNCTION', self.compiler.function(node))

	def emit_call(self, instance: Instance, count: int, extra: tuple = ()) -> None:
		attributes = {attribute.name: attribute for attribute in instance.function.node.attributes}
		if 'builtin' in attributes:
			self.emit('CALL_BUILTIN', [self.compiler.builtin(instance, extra), count])
		elif 'extern' in attributes:
			self.emit('CALL_NATIVE', [self.compiler.native(attributes['extern'].value), count])
		else:
			self.emit('CALL', [self.compiler.function(instance), count])

	def argument(self, argument: ast.Node, parameter: Type) -> None:
		if isinstance(parameter, Reference):
			self.location(argument.operand if isinstance(argument, ast.Reference) else argument)
		else:
			self.value(argument)

	def v_Call(self, node: ast.Call) -> None:
		instance = getattr(node, 'instance', None)
		if instance is not None and VARIADIC in instance.parameters:
			for argument, parameter in zip(node.arguments, instance.parameters[:-1]):
				if isinstance(parameter, Slice) and isinstance(argument.type_, Array):
					self.location(argument)      # a string literal given as the format: slice all of it
					flags = (SLICE_READ_ONLY if self.read_only(argument) else 0) | (SLICE_STATIC if self.static(argument) else 0)
					self.emit('SLICE_LOC', flags)
				else:
					self.argument(argument, parameter)
			for argument in node.arguments[len(instance.parameters) - 1:]:
				self.value(argument)
			self.emit_call(instance, len(node.arguments), tuple(node.variadic_types))
			return
		if instance is not None:
			for index, parameter in enumerate(instance.parameters):
				if index < len(node.arguments):
					self.argument(node.arguments[index], parameter)
				else:
					self.value(instance.node.parameters[index].default)
			self.emit_call(instance, len(instance.parameters))
			return
		self.value(node.callee)
		for argument, parameter in zip(node.arguments, node.callee.type_.parameters):
			self.argument(argument, parameter)
		self.emit('CALL_VALUE', len(node.arguments))

	def operator(self, node: ast.Node, operands: list[ast.Node]) -> bool:
		call = getattr(node, 'operator_call', None)
		if call is None:
			return False
		if getattr(node, 'operator_swapped', False):
			operands = list(reversed(operands))
		for operand in operands:
			self.value(operand)
		self.emit_call(call, len(operands))
		if getattr(node, 'operator_negated', False):
			self.emit('NOT')
		return True

	def v_Index(self, node: ast.Index) -> None:
		if self.operator(node, [node.target, node.index]):
			return
		self.value(node.target)
		self.value(node.index)
		self.emit('INDEX_ARRAY' if isinstance(node.target.type_, Array) else 'INDEX_SLICE')

	def v_SliceOf(self, node: ast.SliceOf) -> None:
		flags = (SLICE_LOW if node.low is not None else 0) | (SLICE_HIGH if node.high is not None else 0) \
			| (SLICE_INCLUSIVE if node.inclusive else 0)
		if isinstance(node.target.type_, Slice):
			self.value(node.target)
			opcode = 'SLICE_SLICE'
		else:
			self.location(node.target)
			if self.read_only(node.target):
				flags |= SLICE_READ_ONLY
			if self.static(node.target):
				flags |= SLICE_STATIC
			opcode = 'SLICE_LOC'
		if node.low is not None:
			self.value(node.low)
		if node.high is not None:
			self.value(node.high)
		self.emit(opcode, flags)

	def v_Unary(self, node: ast.Unary) -> None:
		if self.operator(node, [node.operand]):
			return
		if node.operator == '-' and isinstance(node.operand, (ast.IntegerLiteral, ast.FloatLiteral)):
			self.scalar(-node.operand.value, node.type_)
			return
		self.value(node.operand)
		if node.operator == '!':
			self.emit('NOT')
		elif node.operator == '-':
			self.emit(f'NEG_{type_code(node.type_)}')
		else:
			self.emit(f'BITNOT_{type_code(node.type_)}')

	def v_Binary(self, node: ast.Binary) -> None:
		operator = node.operator
		if operator in ('&&', '||'):
			self.value(node.left)
			self.emit('DUP')
			jump = self.emit('JUMP_IF_FALSE' if operator == '&&' else 'JUMP_IF_TRUE', None)
			self.emit('POP')
			self.value(node.right)
			self.patch(jump, self.here())
			return
		if self.operator(node, [node.left, node.right]):
			return
		left, right = node.left.type_, node.right.type_
		if operator in ('==', '!=') and (isinstance(left, Slice) or isinstance(right, Slice)):
			self.value(node.right if isinstance(right, Slice) and not isinstance(left, Slice) else node.left)
			self.emit('SLICE_IS_NULL')
			if operator == '!=':
				self.emit('NOT')
			return
		self.value(node.left)
		self.value(node.right)
		self.emit(f'{OPCODES[operator]}_{type_code(left)}')

	def v_Cast(self, node: ast.Cast) -> None:
		self.value(node.operand)
		source, target = type_code(node.operand.type_), type_code(node.type_)
		if source != target:
			self.emit(f'CAST_{source}_{target}')

	def v_Reference(self, node: ast.Reference) -> None:
		raise CompileError('a reference outside of a call argument')

	def v_Composite(self, node: ast.Composite) -> None:
		type_ = node.type_
		if not node.entries:
			self.constant(self.compiler.zero(type_))
			return
		if isinstance(type_, StructType):
			fields = list(self.compiler.checker.layout(type_).fields)
			self.constant(self.compiler.zero(type_))
			for entry in node.entries:
				self.value(entry.value)
				self.emit('SET_FIELD', fields.index(entry.name))
			return
		for entry in node.entries:
			self.value(entry.value)
		self.emit('MAKE_ARRAY', len(node.entries))
		if isinstance(type_, Slice):
			self.emit('TEMP_LOC')
			self.emit('SLICE_LOC', 0)

	def v_SizeOf(self, node: ast.SizeOf) -> None:
		self.constant(node.size)

	def v_LenOf(self, node: ast.LenOf) -> None:
		type_ = node.operand.type_
		if isinstance(type_, Array):
			if not isinstance(node.operand, (ast.Name, ast.StringLiteral, ast.Member, ast.Paren)):
				self.value(node.operand)
				self.emit('POP')
			self.constant(type_.length)
			return
		self.value(node.operand)
		self.emit('LEN_SLICE')

	def v_Assert(self, node: ast.Assert) -> None:
		self.value(node.condition)
		skip = self.emit('JUMP_IF_TRUE', None)
		if isinstance(node.message, ast.StringLiteral):
			message = node.message.data.decode('utf-8', 'replace')
		else:
			message = f'assertion failed: {node.condition.span.text}'
		self.emit('ASSERT_FAIL', self.compiler.constant(message))
		self.patch(skip, self.here())
		self.emit('NONE')


def compile_program(program: Program) -> Module:
	return Compiler(program).compile()
