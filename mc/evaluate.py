
"""compile-time evaluation of checked expressions.

constants ('NAME :: value;'), array lengths, default arguments and field
defaults must be known at compile time. the evaluator computes them from the
checked tree, and it can run user functions with constant arguments (pure
code: builtin and extern functions cannot run at compile time). evaluation
stops after a fixed number of steps.

values: integers, floats and bools are python values; arrays are lists and
structs dicts (both copied on assignment, like every value); slices are
views into a list; function values name an instance or a lambda."""

from __future__ import annotations

import math
import struct
from dataclasses import dataclass

from . import ast
from .types import (NULL, UNTYPED_FLOAT, UNTYPED_INTEGER, Array, FunctionType, Scalar, Slice, StructType, Type)

STEP_LIMIT = 1_000_000
CALL_DEPTH_LIMIT = 100


class NotConstant(Exception):
	def __init__(self, node: ast.Node | None, reason: str) -> None:
		super().__init__(reason)
		self.node = node
		self.reason = reason


class EvaluationError(Exception):
	"""evaluation failed; the diagnostic was reported already (unless quiet)."""


class _Return(Exception):
	def __init__(self, value: object) -> None:
		self.value = value


class _Break(Exception):
	pass


class _Continue(Exception):
	pass


@dataclass(eq=False)
class SliceValue:
	data: list
	start: int
	length: int

	def __repr__(self) -> str:
		return repr(self.data[self.start:self.start + self.length])


@dataclass(eq=False)
class FunctionValue:
	target: object               # an Instance or an ast.Lambda

	def __eq__(self, other: object) -> bool:
		return isinstance(other, FunctionValue) and other.target is self.target

	def __hash__(self) -> int:
		return id(self.target)

	def __repr__(self) -> str:
		return f'function {self.target}' if not isinstance(self.target, ast.Lambda) else 'lambda'


@dataclass(eq=False)
class RefValue:
	container: object
	key: object

	def get(self) -> object:
		return self.container[self.key]

	def set(self, value: object) -> None:
		self.container[self.key] = value


def copy_value(value: object) -> object:
	if isinstance(value, list):
		return [copy_value(item) for item in value]
	if isinstance(value, dict):
		return {key: copy_value(item) for key, item in value.items()}
	if isinstance(value, SliceValue):
		return SliceValue(value.data, value.start, value.length)
	return value


def wrap(value: object, type_: Type | None) -> object:
	"""'value' as a 'type_' value: integers wrap around, f32 rounds."""
	if isinstance(type_, Scalar):
		if type_.is_integer and isinstance(value, int) and not isinstance(value, bool):
			mask = (1 << type_.bits) - 1
			value &= mask
			if type_.signed and value >= 1 << (type_.bits - 1):
				value -= 1 << type_.bits
			return value
		if type_.is_float:
			value = float(value)
			if type_.bits == 32 and math.isfinite(value):
				try:
					value = struct.unpack('f', struct.pack('f', value))[0]
				except OverflowError:
					value = math.copysign(math.inf, value)
			return value
	return value


def integer_divide(left: int, right: int) -> int:
	"""division that truncates toward zero, like c."""
	quotient = abs(left) // abs(right)
	return quotient if (left >= 0) == (right >= 0) else -quotient


def arithmetic(operator: str, left: object, right: object) -> object:
	"""the exact result of 'operator' (no wrapping); raises ZeroDivisionError."""
	if operator == '+':
		return left + right
	if operator == '-':
		return left - right
	if operator == '*':
		return left * right
	if operator == '/':
		if isinstance(left, int) and isinstance(right, int):
			return integer_divide(left, right)
		if right == 0:
			return math.nan if left == 0 or math.isnan(left) else math.copysign(math.inf, left) * math.copysign(1.0, right)
		return left / right
	if operator == '%':
		return left - right * integer_divide(left, right)
	if operator == '&':
		return left & right
	if operator == '|':
		return left | right
	if operator == '^':
		return left ^ right
	if operator == '<<':
		return left << right if right < 512 else 0
	if operator == '>>':
		return left >> right
	raise AssertionError(operator)


class Evaluator:
	def __init__(self, checker) -> None:
		self.checker = checker
		self.frames: list[dict] = []
		self.steps = 0
		self.quiet = False
		self.active = False

	# --- entry -----------------------------------------------------------------

	def constant(self, node: ast.Node, context, quiet: bool = False) -> object:
		"""the value of the checked expression 'node'. quiet evaluation runs no
		functions and reports nothing; it only probes for a known value."""
		if self.active:
			saved = (self.frames, self.quiet)
			self.frames, self.quiet = [], quiet
			try:
				return self.expression(node)
			finally:
				self.frames, self.quiet = saved
		self.active = True
		self.frames = []
		self.steps = 0
		self.quiet = quiet
		try:
			return self.expression(node)
		except (TypeError, KeyError, IndexError, AttributeError, ValueError):
			# code with errors (already reported) can leave values missing;
			# without errors this is a bug in the compiler and is raised.
			if not self.checker.diagnostics.errors:
				raise
			raise EvaluationError() from None
		finally:
			self.active = False

	def fail(self, code: str, message: str, node: ast.Node, label: str) -> EvaluationError:
		if not self.quiet:
			self.checker.error(code, message, node.span, label)
		return EvaluationError()

	def step(self, node: ast.Node) -> None:
		self.steps += 1
		if self.steps > STEP_LIMIT:
			raise self.fail('E0705', 'compile-time evaluation takes too long', node,
				f'stopped after {STEP_LIMIT} steps')

	# --- expressions -------------------------------------------------------------

	def expression(self, node: ast.Node) -> object:
		self.step(node)
		method = getattr(self, 'e_' + type(node).__name__, None)
		if method is None:
			raise NotConstant(node, 'not known at compile time')
		return method(node)

	def e_IntegerLiteral(self, node: ast.IntegerLiteral) -> object:
		type_ = getattr(node, 'type_', None)
		if isinstance(type_, Scalar) and type_.is_float:
			return wrap(float(node.value), type_)
		return node.value

	def e_FloatLiteral(self, node: ast.FloatLiteral) -> object:
		return wrap(node.value, getattr(node, 'type_', None))

	def e_CharLiteral(self, node: ast.CharLiteral) -> object:
		return node.value

	def e_StringLiteral(self, node: ast.StringLiteral) -> object:
		return list(node.data)

	def e_BoolLiteral(self, node: ast.BoolLiteral) -> object:
		return node.value

	def e_NullLiteral(self, node: ast.NullLiteral) -> object:
		return SliceValue([], 0, 0)

	def e_Paren(self, node: ast.Paren) -> object:
		return self.expression(node.inner)

	def lookup_variable(self, node: ast.Node, variable) -> object:
		for frame in reversed(self.frames[-1:]):
			if variable in frame:
				value = frame[variable]
				return value.get() if isinstance(value, RefValue) else value
		raise NotConstant(node, f'uses the variable `{variable.name}`')

	def symbol(self, node: ast.Node) -> object:
		variable = getattr(node, 'variable', None)
		if variable is not None:
			return self.lookup_variable(node, variable)
		constant = getattr(node, 'constant', None)
		if constant is not None:
			self.checker.constant(constant)
			if constant.value is None:
				raise NotConstant(node, f'`{constant.name}` has no compile-time value')
			return copy_value(constant.value)
		instance = getattr(node, 'instance', None)
		if instance is not None:
			return FunctionValue(instance)
		raise NotConstant(node, 'not known at compile time')

	def e_Name(self, node: ast.Name) -> object:
		return self.symbol(node)

	def e_Member(self, node: ast.Member) -> object:
		if getattr(node, 'field', None) is None:
			return self.symbol(node)
		target = self.expression(node.target)
		return target[node.name]

	def e_Specialize(self, node: ast.Specialize) -> object:
		return FunctionValue(node.instance)

	def e_Lambda(self, node: ast.Lambda) -> object:
		return FunctionValue(node)

	def e_SizeOf(self, node: ast.SizeOf) -> object:
		return node.size

	def e_LenOf(self, node: ast.LenOf) -> object:
		value = self.expression(node.operand)
		return value.length if isinstance(value, SliceValue) else len(value)

	def e_Cast(self, node: ast.Cast) -> object:
		value = self.expression(node.operand)
		target = node.type_
		if isinstance(target, Scalar):
			if target.is_integer:
				if isinstance(value, float):
					if not math.isfinite(value):
						raise self.fail('E0510', f'cannot convert `{value}` to `{target}`', node, 'not a finite number')
					value = int(value)
				return wrap(int(value), target)
			if target.is_float:
				return wrap(float(value), target)
		return value

	def e_Unary(self, node: ast.Unary) -> object:
		call = getattr(node, 'operator_call', None)
		if call is not None:
			return self.call(FunctionValue(call), [self.expression(node.operand)], node)
		value = self.expression(node.operand)
		type_ = node.type_
		if node.operator == '!':
			return not value
		if node.operator == '-':
			return wrap(-value, type_)
		return wrap(~value, type_)

	def e_Binary(self, node: ast.Binary) -> object:
		operator = node.operator
		if operator == '&&':
			return self.expression(node.left) and self.expression(node.right)
		if operator == '||':
			return self.expression(node.left) or self.expression(node.right)
		left = self.expression(node.left)
		right = self.expression(node.right)
		return self.binary(node, operator, left, right, node.left.type_)

	def binary(self, node: ast.Node, operator: str, left: object, right: object, operand_type: Type) -> object:
		call = getattr(node, 'operator_call', None)
		if call is not None:
			operands = [right, left] if getattr(node, 'operator_swapped', False) else [left, right]
			result = self.call(FunctionValue(call), operands, node)
			return (not result) if getattr(node, 'operator_negated', False) else result
		if operator in ('==', '!='):
			if isinstance(left, SliceValue) or isinstance(right, SliceValue):
				# slices compare with null only: equal when empty.
				equal = (left.length if isinstance(left, SliceValue) else 0) == 0 and (right.length if isinstance(right, SliceValue) else 0) == 0
			else:
				equal = left == right
			return equal if operator == '==' else not equal
		if operator in ('<', '>', '<=', '>='):
			return {'<': left < right, '>': left > right, '<=': left <= right, '>=': left >= right}[operator]
		if operator in ('/', '%') and isinstance(right, int) and not isinstance(right, bool) and right == 0 and isinstance(left, int):
			raise self.fail('E0704', 'division by zero', node, 'the divisor is zero at compile time')
		result = arithmetic(operator, left, right)
		type_ = node.type_ if isinstance(getattr(node, 'type_', None), Scalar) else operand_type
		if type_ in (UNTYPED_INTEGER, UNTYPED_FLOAT):
			return result
		return wrap(result, type_)

	def e_Index(self, node: ast.Index) -> object:
		call = getattr(node, 'operator_call', None)
		if call is not None:
			return self.call(FunctionValue(call), [self.expression(node.target), self.expression(node.index)], node)
		target = self.expression(node.target)
		index = self.expression(node.index)
		data, start, length = self.storage(target)
		if not 0 <= index < length:
			raise self.fail('E0706', f'index {index} is out of bounds', node.index, f'the length is {length}')
		return data[start + index]

	def storage(self, value: object) -> tuple[list, int, int]:
		if isinstance(value, SliceValue):
			return value.data, value.start, value.length
		return value, 0, len(value)

	def e_SliceOf(self, node: ast.SliceOf) -> object:
		target = self.location_value(node.target)
		data, start, length = self.storage(target)
		low = self.expression(node.low) if node.low is not None else 0
		high = self.expression(node.high) if node.high is not None else length
		if node.inclusive and node.high is not None:
			high += 1
		if not (0 <= low <= high <= length):
			raise self.fail('E0706', f'slice {low}..{high} is out of bounds', node, f'the length is {length}')
		return SliceValue(data, start + low, high - low)

	def e_Composite(self, node: ast.Composite) -> object:
		type_ = node.type_
		if not node.entries:
			return self.zero(type_, node)
		if isinstance(type_, StructType):
			value = self.zero(type_, node)
			for entry in node.entries:
				value[entry.name] = copy_value(self.expression(entry.value))
			return value
		items = [copy_value(self.expression(entry.value)) for entry in node.entries]
		if isinstance(type_, Slice):
			return SliceValue(items, 0, len(items))
		return items

	def zero(self, type_: Type, node: ast.Node, skip: set | None = None) -> object:
		if isinstance(type_, Scalar):
			return False if type_.name == 'bool' else 0.0 if type_.is_float else 0
		if isinstance(type_, Slice) or type_ is NULL:
			return SliceValue([], 0, 0)
		if isinstance(type_, Array):
			return [self.zero(type_.element, node) for _ in range(type_.length or 0)]
		if isinstance(type_, StructType):
			value = {}
			for name, item in self.checker.layout(type_).fields.items():
				if skip and name in skip:
					continue
				if item.default is not None:
					value[name] = copy_value(self.constant_default(item.default))
				else:
					value[name] = self.zero(item.type, node)
			return value
		raise NotConstant(node, f'`{type_}` has no zero value')

	def constant_default(self, node: ast.Node) -> object:
		saved = self.frames
		self.frames = []
		try:
			return self.expression(node)
		finally:
			self.frames = saved

	def e_Assert(self, node: ast.Assert) -> object:
		if not self.expression(node.condition):
			message = node.message.data.decode('utf-8', 'replace') if isinstance(node.message, ast.StringLiteral) else ''
			raise self.fail('E0703', 'assertion failed during compile-time evaluation' + (f': {message}' if message else ''),
				node, 'false at compile time')
		return None

	def e_Reference(self, node: ast.Reference) -> object:
		raise NotConstant(node, 'a reference is not a value')

	def e_Call(self, node: ast.Call) -> object:
		instance = getattr(node, 'instance', None)
		function = FunctionValue(instance) if instance is not None else self.expression(node.callee)
		parameters = list(instance.parameters) if instance is not None else list(node.callee.type_.parameters)
		arguments = []
		for argument, parameter in zip(node.arguments, parameters):
			if isinstance(argument, ast.Reference):
				arguments.append(self.locate(argument.operand))
			else:
				arguments.append(copy_value(self.expression(argument)))
		return self.call(function, arguments, node)

	# --- calls -------------------------------------------------------------------

	def call(self, function: FunctionValue, arguments: list, node: ast.Node) -> object:
		if self.quiet:
			raise NotConstant(node, 'calls a function')
		target = function.target
		if isinstance(target, ast.Lambda):
			parameters = target.parameters
			body = target.body
			name = 'a lambda'
			returns_value = target.result is not None
		else:
			from .checker import Instance
			assert isinstance(target, Instance)
			info = target.function
			name = f'`{target}`'
			if info.builtin:
				kind = 'builtin' if any(attribute.name == 'builtin' for attribute in info.node.attributes) else 'extern'
				raise NotConstant(node, f'calls {name}, an {kind} function, which cannot run at compile time')
			if target.state == 'pending':
				self.checker.check_instance(target)
			if target.state != 'checked':
				raise NotConstant(node, f'calls {name} while it is still being checked')
			parameters = target.node.parameters
			body = target.node.body
			returns_value = target.result is not None
			if body is None:
				raise NotConstant(node, f'calls {name}, which has no body')
		if len(self.frames) >= CALL_DEPTH_LIMIT:
			raise self.fail('E0705', 'compile-time evaluation recurses too deeply', node, f'more than {CALL_DEPTH_LIMIT} nested calls')
		frame = {}
		for index, parameter in enumerate(parameters):
			if index < len(arguments):
				value = arguments[index]
			else:
				value = copy_value(self.constant_default(parameter.default))
			frame[parameter.variable] = value
		self.frames.append(frame)
		try:
			self.block(body)
		except _Return as signal:
			return signal.value
		except (_Break, _Continue):
			raise NotConstant(node, f'{name} uses `break` or `continue` outside of a loop') from None
		finally:
			self.frames.pop()
		if returns_value:
			raise NotConstant(node, f'{name} ended without returning a value')
		return None

	# --- places ------------------------------------------------------------------

	def location_value(self, node: ast.Node) -> object:
		"""the value at a place without copying it (so slices share it)."""
		if isinstance(node, ast.Paren):
			return self.location_value(node.inner)
		if isinstance(node, ast.Name) and getattr(node, 'variable', None) is not None:
			return self.lookup_variable(node, node.variable)
		if isinstance(node, ast.Member) and getattr(node, 'field', None) is not None:
			return self.location_value(node.target)[node.name]
		if isinstance(node, ast.Index) and getattr(node, 'operator_call', None) is None:
			target = self.location_value(node.target)
			data, start, length = self.storage(target)
			index = self.expression(node.index)
			if not 0 <= index < length:
				raise self.fail('E0706', f'index {index} is out of bounds', node.index, f'the length is {length}')
			return data[start + index]
		return self.expression(node)

	def locate(self, node: ast.Node) -> RefValue:
		"""a reference to the place 'node' (a temporary for other values)."""
		if isinstance(node, ast.Paren):
			return self.locate(node.inner)
		if isinstance(node, ast.Name) and getattr(node, 'variable', None) is not None:
			frame = self.frames[-1] if self.frames else None
			if frame is None or node.variable not in frame:
				raise NotConstant(node, f'uses the variable `{node.name}`')
			value = frame[node.variable]
			return value if isinstance(value, RefValue) else RefValue(frame, node.variable)
		if isinstance(node, ast.Member) and getattr(node, 'field', None) is not None:
			return RefValue(self.location_value(node.target), node.name)
		if isinstance(node, ast.Index) and getattr(node, 'operator_call', None) is None:
			target = self.location_value(node.target)
			data, start, length = self.storage(target)
			index = self.expression(node.index)
			if not 0 <= index < length:
				raise self.fail('E0706', f'index {index} is out of bounds', node.index, f'the length is {length}')
			return RefValue(data, start + index)
		return RefValue({'value': self.expression(node)}, 'value')

	def store(self, node: ast.Node, value: object) -> None:
		self.locate(node).set(value)

	# --- statements --------------------------------------------------------------

	def block(self, node: ast.Block) -> None:
		for statement in node.statements:
			self.statement(statement)

	def statement(self, node: ast.Node) -> None:
		self.step(node)
		kind = type(node).__name__
		if kind == 'Let':
			value = self.expression(node.value) if node.value is not None else self.zero(node.variable.type, node)
			self.frames[-1][node.variable] = copy_value(value)
		elif kind == 'Assign':
			if isinstance(node.target, ast.Name) and node.target.name == '_':
				self.expression(node.value)
			elif node.operator == '=':
				self.store(node.target, copy_value(self.expression(node.value)))
			else:
				place = self.locate(node.target)
				right = self.expression(node.value)
				self.store_ref(place, self.binary(node, node.operator[:-1], place.get(), right, node.target.type_))
		elif kind == 'ExpressionStatement':
			self.expression(node.expression)
		elif kind == 'If':
			if self.expression(node.condition):
				self.block(node.then)
			elif node.otherwise is not None:
				if isinstance(node.otherwise, ast.If):
					self.statement(node.otherwise)
				else:
					self.block(node.otherwise)
		elif kind == 'While':
			while self.expression(node.condition):
				try:
					self.block(node.body)
				except _Break:
					break
				except _Continue:
					continue
		elif kind == 'For':
			start = self.expression(node.start)
			end = self.expression(node.end)
			step = 1 if end >= start else -1
			stop = end + step if node.inclusive else end
			value = start
			while value != stop:
				self.frames[-1][node.variable] = value
				try:
					self.block(node.body)
				except _Break:
					break
				except _Continue:
					pass
				value += step
				self.step(node)
		elif kind == 'Return':
			raise _Return(copy_value(self.expression(node.value)) if node.value is not None else None)
		elif kind == 'Break':
			raise _Break()
		elif kind == 'Continue':
			raise _Continue()
		elif kind == 'Block':
			self.block(node)
		elif kind == 'DeclarationStatement':
			pass
		else:
			raise NotConstant(node, 'cannot run at compile time')

	def store_ref(self, place: RefValue, value: object) -> None:
		place.set(value)
