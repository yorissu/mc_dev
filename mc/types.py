
"""semantic types. types are immutable and compared by value; a struct type
is its declaration plus its type arguments (one per generic instance)."""

from __future__ import annotations

from dataclasses import dataclass

WORD = 8   # bytes in a machine word: the size of int, uint, float and function values

SCALARS = {
	# name: (kind, bits, signed)
	's8': ('integer', 8, True), 's16': ('integer', 16, True), 's32': ('integer', 32, True), 's64': ('integer', 64, True),
	'u8': ('integer', 8, False), 'u16': ('integer', 16, False), 'u32': ('integer', 32, False), 'u64': ('integer', 64, False),
	'int': ('integer', WORD * 8, True), 'uint': ('integer', WORD * 8, False),
	'f32': ('float', 32, True), 'f64': ('float', 64, True), 'float': ('float', WORD * 8, True),
	'bool': ('bool', 8, False),
}


class Type:
	pass


@dataclass(frozen=True)
class Scalar(Type):
	name: str

	@property
	def kind(self) -> str:
		return SCALARS[self.name][0]

	@property
	def bits(self) -> int:
		return SCALARS[self.name][1]

	@property
	def signed(self) -> bool:
		return SCALARS[self.name][2]

	@property
	def is_integer(self) -> bool:
		return self.kind == 'integer'

	@property
	def is_float(self) -> bool:
		return self.kind == 'float'

	@property
	def minimum(self) -> int:
		return -(1 << (self.bits - 1)) if self.signed else 0

	@property
	def maximum(self) -> int:
		return (1 << (self.bits - 1)) - 1 if self.signed else (1 << self.bits) - 1

	def __str__(self) -> str:
		return self.name


@dataclass(frozen=True)
class Array(Type):
	element: Type
	length: int | None           # none while '[_]' waits for its initializer

	def __str__(self) -> str:
		return f'[{"_" if self.length is None else self.length}]{self.element}'


@dataclass(frozen=True)
class Slice(Type):
	element: Type

	def __str__(self) -> str:
		return f'[]{self.element}'


@dataclass(frozen=True)
class Reference(Type):
	"""only the type of a parameter; reading the parameter gives the element."""
	element: Type
	mutable: bool = False

	def __str__(self) -> str:
		return f'{"mut " if self.mutable else ""}&{self.element}'


@dataclass(frozen=True)
class FunctionType(Type):
	parameters: tuple[Type, ...]
	result: Type | None

	def __str__(self) -> str:
		text = '(' + ', '.join(str(parameter) for parameter in self.parameters) + ')'
		return text + (f' -> {self.result}' if self.result is not None else '')


@dataclass(frozen=True)
class StructType(Type):
	info: object                 # checker.StructInfo, compared by identity
	arguments: tuple[Type, ...] = ()

	def __str__(self) -> str:
		name = self.info.name
		if self.arguments:
			name += '<' + ', '.join(str(argument) for argument in self.arguments) + '>'
		return name


@dataclass(frozen=True)
class TypeParameter(Type):
	"""a type parameter while a generic signature is matched against arguments."""
	name: str
	owner: object

	def __str__(self) -> str:
		return self.name


@dataclass(frozen=True)
class Special(Type):
	name: str

	def __str__(self) -> str:
		return self.name


UNTYPED_INTEGER = Special('{integer}')
UNTYPED_FLOAT = Special('{float}')
NULL = Special('null')
NOTHING = Special('nothing')     # what a function without '->' returns
ERROR = Special('{error}')       # after an error; silences follow-up errors
VARIADIC = Special('...')        # the last parameter of a variadic builtin: any number of values

BOOL = Scalar('bool')
INT = Scalar('int')
UINT = Scalar('uint')
FLOAT = Scalar('float')
U8 = Scalar('u8')


def is_untyped(type_: Type) -> bool:
	return type_ is UNTYPED_INTEGER or type_ is UNTYPED_FLOAT


def is_numeric(type_: Type) -> bool:
	return isinstance(type_, Scalar) and type_.kind in ('integer', 'float')


def is_integer(type_: Type) -> bool:
	return (isinstance(type_, Scalar) and type_.is_integer) or type_ is UNTYPED_INTEGER


def contains_parameter(type_: Type) -> bool:
	if isinstance(type_, TypeParameter):
		return True
	if isinstance(type_, (Array, Slice, Reference)):
		return contains_parameter(type_.element)
	if isinstance(type_, FunctionType):
		return any(contains_parameter(parameter) for parameter in type_.parameters) or \
			(type_.result is not None and contains_parameter(type_.result))
	if isinstance(type_, StructType):
		return any(contains_parameter(argument) for argument in type_.arguments)
	return False


def substitute(type_: Type, bindings: dict[str, Type]) -> Type:
	if isinstance(type_, TypeParameter):
		return bindings.get(type_.name, type_)
	if isinstance(type_, Array):
		return Array(substitute(type_.element, bindings), type_.length)
	if isinstance(type_, Slice):
		return Slice(substitute(type_.element, bindings))
	if isinstance(type_, Reference):
		return Reference(substitute(type_.element, bindings), type_.mutable)
	if isinstance(type_, FunctionType):
		return FunctionType(tuple(substitute(parameter, bindings) for parameter in type_.parameters),
			None if type_.result is None else substitute(type_.result, bindings))
	if isinstance(type_, StructType):
		return StructType(type_.info, tuple(substitute(argument, bindings) for argument in type_.arguments))
	return type_


def unify(pattern: Type, actual: Type, bindings: dict[str, Type]) -> bool:
	"""matches 'pattern' (which may contain type parameters) against 'actual',
	binding the parameters; false when they cannot match."""
	if isinstance(pattern, TypeParameter):
		bound = bindings.get(pattern.name)
		if bound is None:
			bindings[pattern.name] = actual
			return True
		return bound == actual
	if isinstance(pattern, Array) and isinstance(actual, Array):
		return pattern.length == actual.length and unify(pattern.element, actual.element, bindings)
	if isinstance(pattern, Slice) and isinstance(actual, Slice):
		return unify(pattern.element, actual.element, bindings)
	if isinstance(pattern, Reference) and isinstance(actual, Reference):
		return unify(pattern.element, actual.element, bindings)
	if isinstance(pattern, FunctionType) and isinstance(actual, FunctionType):
		if len(pattern.parameters) != len(actual.parameters) or (pattern.result is None) != (actual.result is None):
			return False
		for left, right in zip(pattern.parameters, actual.parameters):
			if not unify(left, right, bindings):
				return False
		return pattern.result is None or unify(pattern.result, actual.result, bindings)
	if isinstance(pattern, StructType) and isinstance(actual, StructType):
		if pattern.info is not actual.info:
			return False
		return all(unify(left, right, bindings) for left, right in zip(pattern.arguments, actual.arguments))
	return pattern == actual
