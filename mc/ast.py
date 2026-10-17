
"""syntax tree nodes. every node has a span; the checker adds annotations
(for example 'type' on expressions) as plain attributes, so they are not part
of the dataclass fields that the printer walks."""

from __future__ import annotations

from dataclasses import dataclass, field

from .source import Span


@dataclass(eq=False)
class Node:
	span: Span


# --- types -------------------------------------------------------------------

@dataclass(eq=False)
class TypeName(Node):
	path: list[str]
	arguments: list[Node] = field(default_factory=list)


@dataclass(eq=False)
class TypeSlice(Node):
	element: Node


@dataclass(eq=False)
class TypeArray(Node):
	length: Node | None          # none for '[_]', the length of the initializer
	element: Node


@dataclass(eq=False)
class TypeReference(Node):
	element: Node
	mutable: bool = False        # 'mut &T', only inside function types


@dataclass(eq=False)
class TypeFunction(Node):
	parameters: list[Node]
	result: Node | None


# --- expressions -------------------------------------------------------------

@dataclass(eq=False)
class IntegerLiteral(Node):
	value: int
	suffix: str | None = None


@dataclass(eq=False)
class FloatLiteral(Node):
	value: float
	suffix: str | None = None


@dataclass(eq=False)
class CharLiteral(Node):
	value: int


@dataclass(eq=False)
class StringLiteral(Node):
	data: bytes
	c: bool = False


@dataclass(eq=False)
class BoolLiteral(Node):
	value: bool


@dataclass(eq=False)
class NullLiteral(Node):
	pass


@dataclass(eq=False)
class Name(Node):
	name: str


@dataclass(eq=False)
class Member(Node):
	target: Node
	name: str
	name_span: Span


@dataclass(eq=False)
class Call(Node):
	callee: Node
	type_arguments: list[Node] | None
	arguments: list[Node]


@dataclass(eq=False)
class Specialize(Node):
	"""a generic function with explicit type arguments that is not called."""
	target: Node
	type_arguments: list[Node]


@dataclass(eq=False)
class Index(Node):
	target: Node
	index: Node


@dataclass(eq=False)
class SliceOf(Node):
	target: Node
	low: Node | None
	high: Node | None
	inclusive: bool = False


@dataclass(eq=False)
class Unary(Node):
	operator: str
	operand: Node


@dataclass(eq=False)
class Binary(Node):
	operator: str
	left: Node
	right: Node
	operator_span: Span


@dataclass(eq=False)
class Paren(Node):
	inner: Node


@dataclass(eq=False)
class Cast(Node):
	type: Node
	operand: Node


@dataclass(eq=False)
class Reference(Node):
	operand: Node
	mutable: bool = False


@dataclass(eq=False)
class Entry(Node):
	name: str | None
	value: Node


@dataclass(eq=False)
class Composite(Node):
	type: Node | None
	entries: list[Entry]


@dataclass(eq=False)
class Lambda(Node):
	parameters: list[Param]
	result: Node | None
	body: Block


@dataclass(eq=False)
class SizeOf(Node):
	type: Node


@dataclass(eq=False)
class LenOf(Node):
	operand: Node


@dataclass(eq=False)
class Assert(Node):
	condition: Node
	message: Node | None


# --- statements --------------------------------------------------------------

@dataclass(eq=False)
class Attribute(Node):
	name: str
	value: object = None         # string, integer, name or operator pattern


@dataclass(eq=False)
class Let(Node):
	name: str
	name_span: Span
	mutable: bool
	type: Node | None
	value: Node | None
	attributes: list[Attribute] = field(default_factory=list)


@dataclass(eq=False)
class DeclarationStatement(Node):
	declaration: Node


@dataclass(eq=False)
class If(Node):
	condition: Node
	then: Block
	otherwise: Node | None       # a Block, or an If for 'else if'


@dataclass(eq=False)
class While(Node):
	condition: Node
	body: Block


@dataclass(eq=False)
class For(Node):
	name: str
	name_span: Span
	start: Node
	end: Node
	inclusive: bool
	body: Block


@dataclass(eq=False)
class Return(Node):
	value: Node | None


@dataclass(eq=False)
class Break(Node):
	pass


@dataclass(eq=False)
class Continue(Node):
	pass


@dataclass(eq=False)
class Block(Node):
	statements: list[Node]
	braceless: bool = False


@dataclass(eq=False)
class ExpressionStatement(Node):
	expression: Node
	attributes: list[Attribute] = field(default_factory=list)


@dataclass(eq=False)
class Assign(Node):
	operator: str                # '=', '+=', ...
	target: Node
	value: Node
	attributes: list[Attribute] = field(default_factory=list)


# --- declarations ------------------------------------------------------------

@dataclass(eq=False)
class Generic(Node):
	name: str


@dataclass(eq=False)
class Param(Node):
	name: str
	mutable: bool
	type: Node | None            # none for 'values: ...'
	default: Node | None = None
	variadic: bool = False


@dataclass(eq=False)
class Field(Node):
	name: str
	mutable: bool
	type: Node
	default: Node | None = None
	attributes: list[Attribute] = field(default_factory=list)


@dataclass(eq=False)
class Function(Node):
	name: str
	name_span: Span
	generics: list[Generic]
	parameters: list[Param]
	result: Node | None
	body: Block | None
	attributes: list[Attribute] = field(default_factory=list)
	public: bool = False


@dataclass(eq=False)
class Struct(Node):
	name: str
	name_span: Span
	generics: list[Generic]
	fields: list[Field]
	attributes: list[Attribute] = field(default_factory=list)
	public: bool = False


@dataclass(eq=False)
class Constant(Node):
	name: str
	name_span: Span
	value: Node
	attributes: list[Attribute] = field(default_factory=list)
	public: bool = False


@dataclass(eq=False)
class ImportName(Node):
	name: str
	alias: str | None = None


@dataclass(eq=False)
class Import(Node):
	path: list[str]
	alias: str | None = None


@dataclass(eq=False)
class FromImport(Node):
	path: list[str]
	names: list[ImportName]
	star: bool = False


@dataclass(eq=False)
class Module(Node):
	imports: list[Node]
	declarations: list[Node]


DECLARATIONS = (Function, Struct, Constant)
