
"""checker: name resolution, type checking and generic instantiation.

generics are blueprints: every use with new type arguments creates an
instance (a copy of the declaration with the type parameters bound), which is
checked like ordinary code. operators on non-scalar operands call functions
marked '[op._+_]' etc. that are visible from the module, from its imports, or
from the modules that define the operand types. constants are evaluated at
compile time (see evaluate.py)."""

from __future__ import annotations

import copy
from dataclasses import dataclass, field

from . import ast
from .diag import Diagnostics
from .loader import Loader, ModuleInfo
from .types import (BOOL, ERROR, FLOAT, INT, NOTHING, NULL, U8, UINT, UNTYPED_FLOAT, UNTYPED_INTEGER, VARIADIC, WORD,
	SCALARS, Array, FunctionType, Reference, Scalar, Slice, StructType, Type, TypeParameter, contains_parameter,
	is_integer, is_numeric, is_untyped, substitute, unify)

INSTANTIATION_LIMIT = 64

ATTRIBUTES = {
	'function': {'builtin', 'extern', 'op', 'wrap'},
	'struct': set(),
	'constant': {'wrap'},
	'field': set(),
	'let': {'uninit', 'wrap'},
	'statement': {'wrap'},
}
KNOWN_ATTRIBUTES = {'builtin', 'extern', 'op', 'wrap', 'uninit'}

# operator patterns: pattern -> number of operands.
OPERATOR_PATTERNS = {
	'_+_': 2, '_-_': 2, '_*_': 2, '_/_': 2, '_%_': 2, '_==_': 2, '_<_': 2,
	'_&_': 2, '_|_': 2, '_^_': 2, '_<<_': 2, '_>>_': 2,
	'-_': 1, '!_': 1, '~_': 1, '_[_]': 2,
}
DERIVED_PATTERNS = {
	'_!=_': '`!=` is `!(a == b)`', '_>_': '`a > b` is `b < a`', '_<=_': '`a <= b` is `!(b < a)`',
	'_>=_': '`a >= b` is `!(a < b)`',
}
for _operator in ('+', '-', '*', '/', '%', '&', '|', '^', '<<', '>>'):
	DERIVED_PATTERNS[f'_{_operator}=_'] = f'`a {_operator}= b` is `a = a {_operator} b`'

ARITHMETIC = {'+', '-', '*', '/', '%'}
BITWISE = {'&', '|', '^'}
SHIFTS = {'<<', '>>'}
EQUALITY = {'==', '!='}
ORDERING = {'<', '>', '<=', '>='}


# --- symbols -------------------------------------------------------------------

@dataclass(eq=False)
class Variable:
	name: str
	type: Type                   # for reference parameters: the referenced type
	kind: str                    # 'let', 'param' or 'for'
	mutable: bool                # for reference parameters: writable through
	span: object
	reference: bool = False
	level: int = 0
	mutated: bool = False
	loops: int = 0               # loops around the declaration (late initialization must not repeat)


@dataclass(eq=False)
class FunctionInfo:
	node: ast.Function
	module: ModuleInfo
	parent: object = None        # the enclosing context of a local function
	instances: dict = field(default_factory=dict)
	operator: str | None = None

	@property
	def name(self) -> str:
		return self.node.name

	@property
	def generics(self) -> list[str]:
		return [generic.name for generic in self.node.generics]

	@property
	def builtin(self) -> bool:
		return any(attribute.name in ('builtin', 'extern') for attribute in self.node.attributes)

	@property
	def public(self) -> bool:
		return self.node.public


@dataclass(eq=False)
class Instance:
	function: FunctionInfo
	arguments: tuple
	bindings: dict
	node: ast.Function           # a copy for generic functions
	parameters: list = field(default_factory=list)
	result: Type | None = None
	depth: int = 0
	state: str = 'pending'       # 'pending', 'checking', 'checked'
	variables: list = field(default_factory=list)

	def __str__(self) -> str:
		if not self.arguments:
			return self.function.name
		return self.function.name + '<' + ', '.join(str(argument) for argument in self.arguments) + '>'

	@property
	def type(self) -> FunctionType:
		return FunctionType(tuple(self.parameters), self.result)


@dataclass(eq=False)
class StructInfo:
	node: ast.Struct
	module: ModuleInfo
	instances: dict = field(default_factory=dict)

	@property
	def name(self) -> str:
		return self.node.name

	@property
	def generics(self) -> list[str]:
		return [generic.name for generic in self.node.generics]

	@property
	def public(self) -> bool:
		return self.node.public


@dataclass(eq=False)
class FieldInfo:
	name: str
	type: Type
	mutable: bool
	default: ast.Node | None
	node: ast.Field


@dataclass(eq=False)
class Layout:
	type: StructType
	fields: dict = field(default_factory=dict)


@dataclass(eq=False)
class ConstantInfo:
	node: ast.Constant
	module: ModuleInfo
	context: object = None       # for local constants: the enclosing context
	state: str = 'pending'       # 'pending', 'checking', 'done'
	type: Type = ERROR
	value: object = None

	@property
	def name(self) -> str:
		return self.node.name

	@property
	def public(self) -> bool:
		return self.node.public


@dataclass(eq=False)
class ModulePrefix:
	"""the first parts of a dotted module name ('a' of 'import a.b')."""
	module: ModuleInfo
	prefix: str


class Context:
	"""where an expression is checked: a module, type parameter bindings and,
	inside a function, its scopes. lambdas and local functions get their own
	context whose parent is the enclosing one."""

	def __init__(self, module: ModuleInfo, bindings: dict | None = None, result: Type | None = None,
			parent: Context | None = None, instance: Instance | None = None, function: bool = False) -> None:
		self.module = module
		self.bindings = dict(bindings or {})
		self.result = result
		self.parent = parent
		self.instance = instance
		self.function = function
		self.scopes: list[dict] = [{}] if function else []
		self.loops = 0
		self.unassigned: set = set()
		self.variables: list[Variable] = []
		self.wrap = parent.wrap if parent else False
		self.level = (parent.level + 1) if parent else 0
		self.depth = instance.depth if instance else (parent.depth if parent else 0)

	def declare(self, name: str, symbol: object) -> object | None:
		"""adds 'symbol' to the innermost scope; returns a previous one of that scope."""
		scope = self.scopes[-1]
		previous = scope.get(name)
		scope[name] = symbol
		return previous


@dataclass
class Program:
	modules: list[ModuleInfo]
	entry: ModuleInfo | None
	instances: list[Instance]
	layouts: list[Layout]
	constants: list[ConstantInfo]
	diagnostics: Diagnostics
	checker: object = None


class Checker:
	def __init__(self, diagnostics: Diagnostics, loader: Loader) -> None:
		from .evaluate import Evaluator
		self.diagnostics = diagnostics
		self.loader = loader
		self.queue: list[Instance] = []
		self.instances: list[Instance] = []
		self.layouts: list[Layout] = []
		self.constants: list[ConstantInfo] = []
		self.evaluator = Evaluator(self)
		self.sizing: set = set()
		self.recursive_reported: set = set()

	def error(self, code: str, message: str, span, label: str = ''):
		return self.diagnostics.error(code, message, span, label)

	def warning(self, code: str, message: str, span, label: str = ''):
		return self.diagnostics.warning(code, message, span, label)

	# --- program ---------------------------------------------------------------

	def run(self, entry_path: str) -> Program:
		entry = self.loader.load_entry(entry_path)
		modules = self.loader.order
		for module in modules:
			self.collect(module)
		for module in modules:
			self.resolve_imports(module)
		for module in modules:
			self.declarations(module)
		for module in modules:
			for symbol in list(module.symbols.values()):
				if isinstance(symbol, ConstantInfo):
					self.constant(symbol)
				elif isinstance(symbol, StructInfo) and not symbol.generics:
					self.struct_type(symbol, (), symbol.node.name_span)
				elif isinstance(symbol, FunctionInfo) and not symbol.generics:
					self.instance(symbol, (), None, symbol.node.name_span)
		self.drain()
		if entry is not None:
			self.check_main(entry)
		return Program(modules, entry, self.instances, self.layouts, self.constants, self.diagnostics, self)

	def drain(self) -> None:
		while self.queue:
			instance = self.queue.pop(0)
			self.check_instance(instance)

	def collect(self, module: ModuleInfo) -> None:
		for node in module.tree.declarations:
			if isinstance(node, ast.Function):
				symbol = FunctionInfo(node, module)
			elif isinstance(node, ast.Struct):
				symbol = StructInfo(node, module)
			else:
				symbol = ConstantInfo(node, module)
			previous = module.symbols.get(node.name)
			if previous is not None:
				self.error('E0401', f'`{node.name}` is declared twice', node.name_span, 'declared again here') \
					.label(previous.node.name_span, 'first declared here')
				continue
			module.symbols[node.name] = symbol

	def resolve_imports(self, module: ModuleInfo) -> None:
		for node, target in module.imports:
			if isinstance(node, ast.Import):
				alias = node.alias or '.'.join(node.path)
				if alias in module.qualifiers and module.qualifiers[alias] is not target:
					self.error('E0306', f'`{alias}` is imported twice', node.span, 'already imported')
				module.qualifiers[alias] = target
			elif node.star:
				module.star.append(target)
			else:
				for name in node.names:
					symbol = target.symbols.get(name.name)
					if symbol is None:
						self.error('E0305', f'module `{target.name}` has no `{name.name}`', name.span, 'not declared there')
						continue
					if not symbol.public:
						self.error('E0303', f'`{name.name}` is not public in `{target.name}`', name.span, 'not `pub`') \
							.label(symbol.node.name_span, 'declared here without `pub`')
						continue
					alias = name.alias or name.name
					previous = module.imported.get(alias)
					if previous is not None and previous is not symbol:
						self.error('E0306', f'`{alias}` is imported twice', name.span, 'already imported')
						continue
					module.imported[alias] = symbol

	# --- lookup ----------------------------------------------------------------

	def lookup(self, name: str, context: Context, span, report: bool = True) -> object | None:
		"""the symbol 'name' refers to: a variable, constant, function, struct,
		module or module prefix. reports undefined names and captures."""
		current = context
		while current is not None:
			for scope in reversed(current.scopes):
				symbol = scope.get(name)
				if symbol is not None:
					if isinstance(symbol, Variable) and current is not context:
						if report:
							self.error('E0516', f'`{name}` is a variable of the enclosing function', span,
								'lambdas and local functions cannot capture variables') \
								.help('pass it as an argument instead')
						return None
					return symbol
			current = current.parent
		module = context.module
		symbol = module.symbols.get(name) or module.imported.get(name)
		if symbol is not None:
			return symbol
		found = []
		for target in module.star:
			candidate = target.symbols.get(name)
			if candidate is not None and candidate.public and candidate not in found:
				found.append(candidate)
		if len(found) == 1:
			return found[0]
		if len(found) > 1:
			if report:
				diagnostic = self.error('E0304', f'`{name}` is ambiguous', span, 'imported by several `*` imports')
				for candidate in found:
					diagnostic.note(f'`{name}` from `{candidate.module.name}`')
			return None
		if name in module.qualifiers:
			return module.qualifiers[name]
		if any(alias.startswith(name + '.') for alias in module.qualifiers):
			return ModulePrefix(module, name)
		if report:
			self.error('E0501', f'cannot find `{name}`', span, 'not declared')
		return None

	def member_of(self, base: object, name: str, span, context: Context) -> object | None:
		"""'name' inside a module or module prefix."""
		if isinstance(base, ModulePrefix):
			dotted = f'{base.prefix}.{name}'
			if dotted in base.module.qualifiers:
				return base.module.qualifiers[dotted]
			if any(alias.startswith(dotted + '.') for alias in base.module.qualifiers):
				return ModulePrefix(base.module, dotted)
			self.error('E0301', f'module `{dotted}` is not imported', span, 'no such module')
			return None
		symbol = base.symbols.get(name)
		if symbol is None:
			self.error('E0305', f'module `{base.name}` has no `{name}`', span, 'not declared there')
			return None
		if not symbol.public and base is not context.module:
			self.error('E0303', f'`{name}` is not public in `{base.name}`', span, 'not `pub`') \
				.label(symbol.node.name_span, 'declared here without `pub`')
			return None
		return symbol

	def qualified(self, node: ast.Node, context: Context, report: bool = True) -> object | None:
		"""the symbol a name or a 'module.name' chain refers to, or none when it is a value expression."""
		if isinstance(node, ast.Name):
			return self.lookup(node.name, context, node.span, report)
		if isinstance(node, ast.Member):
			base = self.qualified(node.target, context, report=False) if isinstance(node.target, (ast.Name, ast.Member)) else None
			if isinstance(base, (ModuleInfo, ModulePrefix)):
				return self.member_of(base, node.name, node.name_span, context)
		return None

	# --- types -----------------------------------------------------------------

	def resolve_type(self, node: ast.Node, context: Context, reference: bool = False, infer: bool = False) -> Type:
		if isinstance(node, ast.TypeName):
			if len(node.path) == 1 and node.path[0] in SCALARS:
				return Scalar(node.path[0])
			if len(node.path) == 1 and node.path[0] in context.bindings:
				if node.arguments:
					self.error('E0521', f'type parameter `{node.path[0]}` takes no type arguments', node.span)
				return context.bindings[node.path[0]]
			if len(node.path) == 1:
				symbol = self.lookup(node.path[0], context, node.span)
			else:
				base = self.lookup(node.path[0], context, node.span)
				for part in node.path[1:]:
					if not isinstance(base, (ModuleInfo, ModulePrefix)):
						break
					base = self.member_of(base, part, node.span, context)
				symbol = base
			if symbol is None:
				return ERROR
			if not isinstance(symbol, StructInfo):
				self.error('E0503', f'`{".".join(node.path)}` is not a type', node.span, 'expected a type')
				return ERROR
			arguments = tuple(self.resolve_type(argument, context) for argument in node.arguments)
			if len(arguments) != len(symbol.generics):
				if symbol.generics:
					self.error('E0520', f'`{symbol.name}` takes {len(symbol.generics)} type argument(s), found {len(arguments)}',
						node.span, f'expected `{symbol.name}<{", ".join(symbol.generics)}>`')
				else:
					self.error('E0521', f'`{symbol.name}` is not generic', node.span, 'remove the type arguments')
				return ERROR
			if ERROR in arguments:
				return ERROR
			return self.struct_type(symbol, arguments, node.span)
		if isinstance(node, ast.TypeSlice):
			return Slice(self.resolve_type(node.element, context))
		if isinstance(node, ast.TypeArray):
			element = self.resolve_type(node.element, context)
			if node.length is None:
				if not infer:
					self.error('E0506', '`[_]` needs an initializer to take its length from', node.span,
						'only allowed in `let` with a value')
					return ERROR
				return Array(element, None)
			length = self.constant_integer(node.length, context, 'an array length')
			if length is None:
				return ERROR
			if length < 0:
				self.error('E0502', 'an array length cannot be negative', node.length.span, f'length is {length}')
				return ERROR
			return Array(element, length)
		if isinstance(node, ast.TypeReference):
			element = self.resolve_type(node.element, context)
			if not reference:
				self.error('E0413', 'references are only allowed as parameter types', node.span,
					'a reference cannot be stored').help('use a slice to keep a view of an array')
				return element
			return Reference(element, node.mutable)
		if isinstance(node, ast.TypeFunction):
			parameters = tuple(self.resolve_type(parameter, context, reference=True) for parameter in node.parameters)
			result = self.resolve_type(node.result, context) if node.result is not None else None
			return FunctionType(parameters, result)
		raise AssertionError(node)

	def struct_type(self, info: StructInfo, arguments: tuple, span) -> StructType:
		type_ = StructType(info, arguments)
		if any(contains_parameter(argument) for argument in arguments) or arguments in info.instances:
			return type_
		layout = Layout(type_)
		info.instances[arguments] = layout
		self.layouts.append(layout)
		context = Context(info.module, dict(zip(info.generics, arguments)))
		for node in info.node.fields:
			self.attributes(node.attributes, 'field', context)
			if node.name in layout.fields:
				self.error('E0401', f'field `{node.name}` is declared twice', node.span, 'declared again here') \
					.label(layout.fields[node.name].node.span, 'first declared here')
				continue
			field_type = self.resolve_type(node.type, context)
			default = copy.deepcopy(node.default) if node.default is not None else None
			layout.fields[node.name] = FieldInfo(node.name, field_type, node.mutable, default, node)
		for item in layout.fields.values():
			if item.default is not None:
				actual = self.expression(item.default, context, item.type)
				self.coerce(item.default, actual, item.type)
				self.require_constant(item.default, context, 'a field default')
		self.size_of(type_)
		return type_

	def layout(self, type_: StructType) -> Layout:
		layout = type_.info.instances.get(type_.arguments)
		if layout is None:
			self.struct_type(type_.info, type_.arguments, type_.info.node.name_span)
			layout = type_.info.instances[type_.arguments]
		return layout

	def size_of(self, type_: Type) -> int:
		return self.size_and_alignment(type_)[0]

	def size_and_alignment(self, type_: Type) -> tuple[int, int]:
		if isinstance(type_, Scalar):
			size = 1 if type_.name == 'bool' else type_.bits // 8
			return size, size
		if isinstance(type_, Slice):
			return 2 * WORD, WORD
		if isinstance(type_, FunctionType):
			return WORD, WORD
		if isinstance(type_, Array):
			size, alignment = self.size_and_alignment(type_.element)
			return size * (type_.length or 0), alignment
		if isinstance(type_, StructType):
			if type_ in self.sizing:
				info = type_.info
				if info not in self.recursive_reported:
					self.recursive_reported.add(info)
					self.error('E0414', f'struct `{info.name}` contains itself', info.node.name_span,
						'it would have infinite size').help('store it through a slice, for example `next: []Node`')
				return 0, 1
			self.sizing.add(type_)
			try:
				offset, alignment = 0, 1
				for item in self.layout(type_).fields.values():
					size, item_alignment = self.size_and_alignment(item.type)
					offset = (offset + item_alignment - 1) // item_alignment * item_alignment + size
					alignment = max(alignment, item_alignment)
				return (offset + alignment - 1) // alignment * alignment, alignment
			finally:
				self.sizing.discard(type_)
		return 0, 1

	# --- attributes and declarations ---------------------------------------------

	def attributes(self, attributes: list[ast.Attribute], where: str, context: Context | None = None) -> dict:
		found: dict = {}
		for attribute in attributes:
			if attribute.name not in KNOWN_ATTRIBUTES:
				self.error('E0402', f'unknown attribute `{attribute.name}`', attribute.span, 'not an attribute') \
					.note('attributes: ' + ', '.join(sorted(KNOWN_ATTRIBUTES)))
				continue
			if attribute.name not in ATTRIBUTES[where]:
				self.error('E0403', f'`{attribute.name}` cannot be used on a {where.replace("_", " ")}', attribute.span, 'not applicable here')
				continue
			if attribute.name in found:
				self.error('E0403', f'attribute `{attribute.name}` is given twice', attribute.span, 'repeated')
				continue
			if attribute.name == 'extern' and not isinstance(attribute.value, str):
				self.error('E0403', '`extern` needs the name of the C function', attribute.span, 'write `extern = "name"`')
				continue
			if attribute.name in ('builtin', 'wrap', 'uninit') and attribute.value is not None:
				self.error('E0403', f'`{attribute.name}` takes no value', attribute.span, 'remove the value')
				continue
			found[attribute.name] = attribute
		if 'builtin' in found and 'extern' in found:
			self.error('E0403', 'a function cannot be both `builtin` and `extern`', found['extern'].span, 'choose one')
		return found

	def declarations(self, module: ModuleInfo) -> None:
		context = Context(module)
		for symbol in module.symbols.values():
			if isinstance(symbol, StructInfo):
				self.attributes(symbol.node.attributes, 'struct')
				self.duplicate_generics(symbol.node)
			elif isinstance(symbol, ConstantInfo):
				pass
			else:
				self.function_declaration(symbol, context)

	def duplicate_generics(self, node) -> None:
		seen = {}
		for generic in node.generics:
			if generic.name in seen or generic.name in SCALARS:
				self.error('E0401', f'type parameter `{generic.name}` is declared twice' if generic.name in seen
					else f'type parameter `{generic.name}` hides a built-in type', generic.span, 'rename it')
			seen[generic.name] = generic

	def function_declaration(self, info: FunctionInfo, context: Context) -> None:
		node = info.node
		self.duplicate_generics(node)
		found = self.attributes(node.attributes, 'function')
		bodyless = 'builtin' in found or 'extern' in found
		if bodyless and node.body is not None:
			self.error('E0405', f'`{node.name}` is `{"builtin" if "builtin" in found else "extern"}` and cannot have a body',
				node.body.span, 'remove the body')
		if not bodyless and node.body is None:
			self.error('E0404', f'`{node.name}` has no body', node.name_span, 'expected `{ ... }`') \
				.help('only `[builtin]` and `[extern = "name"]` functions are declared without a body')
		seen_default = None
		names = set()
		for parameter in node.parameters:
			if parameter.name in names:
				self.error('E0401', f'parameter `{parameter.name}` is declared twice', parameter.span, 'declared again')
			names.add(parameter.name)
			if parameter.default is not None:
				seen_default = parameter
			elif seen_default is not None:
				self.error('E0412', 'parameters without defaults cannot follow parameters with defaults', parameter.span,
					'needs a default').label(seen_default.span, 'has a default')
		for index, parameter in enumerate(node.parameters):
			if not parameter.variadic:
				continue
			if not bodyless:
				self.error('E0415', 'only `[builtin]` and `[extern]` functions can take `...`', parameter.span,
					'variadic parameter').help('take a slice instead: `values: []T`')
			elif index != len(node.parameters) - 1:
				self.error('E0415', '`...` must be the last parameter', parameter.span, 'not last')
			elif parameter.mutable or seen_default is not None:
				self.error('E0415', 'a variadic function cannot have `mut` or default parameters', parameter.span, 'variadic')
		if 'op' in found:
			self.operator_declaration(info, found['op'], context)

	def operator_declaration(self, info: FunctionInfo, attribute: ast.Attribute, context: Context) -> None:
		pattern = attribute.value
		node = info.node
		if pattern in DERIVED_PATTERNS:
			self.error('E0408', f'operator `{pattern}` cannot be declared', attribute.span, 'it is derived') \
				.note(DERIVED_PATTERNS[pattern])
			return
		if pattern not in OPERATOR_PATTERNS:
			self.error('E0406', f'`{pattern}` is not an operator pattern', attribute.span, 'unknown pattern') \
				.note('patterns: ' + ' '.join(OPERATOR_PATTERNS))
			return
		arity = OPERATOR_PATTERNS[pattern]
		if any(parameter.variadic for parameter in node.parameters):
			self.error('E0409', 'operator functions cannot be variadic', node.name_span, 'remove `...`')
			return
		if len(node.parameters) != arity:
			self.error('E0407', f'`{pattern}` takes {arity} operand(s), but `{node.name}` has {len(node.parameters)} parameter(s)',
				node.name_span, 'parameter count does not match the pattern') \
				.help('every `_` in the pattern is one parameter, in order')
			return
		if info.generics:
			self.error('E0409', 'operator functions cannot be generic', node.name_span, 'remove the type parameters')
			return
		instance = self.instance(info, (), None, node.name_span)
		if instance is None:
			return
		if any(isinstance(parameter, Reference) for parameter in instance.parameters):
			self.error('E0409', 'operator functions take their operands by value', node.name_span, 'remove `&` from the parameters')
			return
		if instance.result is None:
			self.error('E0409', f'operator function `{node.name}` must return a value', node.name_span, 'add `-> type`')
			return
		if pattern in ('_==_', '_<_') and instance.result != BOOL:
			self.error('E0409', f'`{pattern}` must return `bool`', node.result.span, f'returns `{instance.result}`')
			return
		for other in info.module.operators.get(pattern, []):
			other_instance = other.instances.get(())
			if other_instance is not None and other_instance.parameters == instance.parameters:
				self.error('E0410', f'operator `{pattern}` for ({", ".join(map(str, instance.parameters))}) is declared twice',
					node.name_span, 'declared again').label(other.node.name_span, 'first declared here')
				return
		info.operator = pattern
		info.module.operators.setdefault(pattern, []).append(info)

	def check_main(self, entry: ModuleInfo) -> None:
		symbol = entry.symbols.get('main')
		if not isinstance(symbol, FunctionInfo):
			self.error('E0411', f'the entry module `{entry.path}` has no `main` function', None) \
				.help('declare `pub main :: () { ... }`')
			return
		node = symbol.node
		if not node.public:
			self.error('E0411', '`main` must be `pub`', node.name_span, 'add `pub`').note('`main` is called from outside the program')
		if symbol.generics or node.parameters:
			self.error('E0411', '`main` takes no parameters and no type parameters', node.name_span, 'remove them')
		instance = symbol.instances.get(())
		if instance is not None and instance.result not in (None, INT):
			self.error('E0411', '`main` returns nothing or `int`', node.result.span, f'returns `{instance.result}`')

	# --- constants ---------------------------------------------------------------

	def constant(self, info: ConstantInfo) -> Type:
		if info.state == 'done':
			return info.type
		if info.state == 'checking':
			self.error('E0702', f'constant `{info.name}` depends on itself', info.node.name_span, 'its value needs its own value')
			info.state = 'done'
			info.type = ERROR
			return ERROR
		info.state = 'checking'
		context = info.context or Context(info.module)
		found = self.attributes(info.node.attributes, 'constant')
		saved = context.wrap
		context.wrap = saved or 'wrap' in found
		type_ = self.expression(info.node.value, context)
		if type_ is NOTHING:
			self.error('E0523', 'a constant needs a value', info.node.value.span, 'this has no value')
			type_ = ERROR
		elif type_ is NULL:
			self.error('E0506', 'cannot infer the type of `null`', info.node.value.span, 'write `(T){}` or use a typed slice')
			type_ = ERROR
		value = self.require_constant(info.node.value, context, f'constant `{info.name}`') if type_ is not ERROR else None
		context.wrap = saved
		info.type = type_ if value is not None else ERROR
		info.value = value
		info.state = 'done'
		if info.context is None and info not in self.constants:
			self.constants.append(info)
		return info.type

	def require_constant(self, node: ast.Node, context: Context, what: str):
		from .evaluate import EvaluationError, NotConstant
		try:
			return self.evaluator.constant(node, context)
		except NotConstant as problem:
			diagnostic = self.error('E0701', f'{what} must be known at compile time', node.span, 'evaluated at compile time')
			if problem.node is not None and problem.node is not node:
				diagnostic.label(problem.node.span, problem.reason)
			else:
				diagnostic.note(problem.reason)
			return None
		except EvaluationError:
			return None

	def constant_integer(self, node: ast.Node, context: Context, what: str) -> int | None:
		type_ = self.expression(node, context)
		if type_ is ERROR:
			return None
		if not is_integer(type_):
			self.error('E0502', f'{what} must be an integer', node.span, f'found `{type_}`')
			return None
		if type_ is UNTYPED_INTEGER:
			self.finalize(node, UINT if self.try_value(node, context, 0) >= 0 else INT)
		value = self.require_constant(node, context, what)
		return value if isinstance(value, int) else None

	def try_value(self, node: ast.Node, context: Context, fallback=None):
		"""the compile-time value of 'node', or 'fallback' when it is not constant."""
		from .evaluate import EvaluationError, NotConstant
		try:
			return self.evaluator.constant(node, context, quiet=True)
		except (NotConstant, EvaluationError):
			return fallback

	# --- functions and instances --------------------------------------------------

	def instance(self, info: FunctionInfo, arguments: tuple, context: Context | None, span) -> Instance | None:
		existing = info.instances.get(arguments)
		if existing is not None:
			return existing
		depth = (context.depth + 1) if (context is not None and info.generics) else 0
		if depth > INSTANTIATION_LIMIT:
			self.error('E0527', f'too many nested instantiations of `{info.name}`', span,
				f'more than {INSTANTIATION_LIMIT} levels deep').note('a generic function probably instantiates itself with ever larger types')
			return None
		node = copy.deepcopy(info.node) if info.generics else info.node
		bindings = dict(zip(info.generics, arguments))
		instance = Instance(info, arguments, bindings, node, depth=depth)
		info.instances[arguments] = instance
		declaration_context = Context(info.module, bindings, parent=info.parent)
		if info.parent is not None:
			declaration_context.bindings = {**info.parent.bindings, **bindings}
		for parameter in node.parameters:
			if parameter.variadic:
				instance.parameters.append(VARIADIC)
				continue
			type_ = self.resolve_type(parameter.type, declaration_context, reference=True)
			if isinstance(type_, Reference):
				type_ = Reference(type_.element, parameter.mutable)
			instance.parameters.append(type_)
		instance.result = self.resolve_type(node.result, declaration_context) if node.result is not None else None
		self.instances.append(instance)
		self.queue.append(instance)
		return instance

	def signature(self, info: FunctionInfo) -> tuple[list[Type], Type | None]:
		"""the parameter and result types with the type parameters left open."""
		placeholders = {name: TypeParameter(name, info) for name in info.generics}
		context = Context(info.module, placeholders, parent=info.parent)
		parameters = []
		for parameter in info.node.parameters:
			if parameter.variadic:
				parameters.append(VARIADIC)
				continue
			type_ = self.resolve_type(parameter.type, context, reference=True)
			parameters.append(Reference(type_.element, parameter.mutable) if isinstance(type_, Reference) else type_)
		result = self.resolve_type(info.node.result, context) if info.node.result is not None else None
		return parameters, result

	def check_instance(self, instance: Instance) -> None:
		if instance.state != 'pending':
			return
		instance.state = 'checking'
		info = instance.function
		node = instance.node
		bindings = dict(instance.bindings)
		if info.parent is not None:
			bindings = {**info.parent.bindings, **bindings}
		context = Context(info.module, bindings, instance.result, parent=info.parent, instance=instance, function=True)
		context.wrap = context.wrap or any(attribute.name == 'wrap' for attribute in node.attributes)
		for parameter, type_ in zip(node.parameters, instance.parameters):
			is_reference = isinstance(type_, Reference)
			variable = Variable(parameter.name, type_.element if is_reference else type_, 'param', parameter.mutable,
				parameter.span, reference=is_reference, level=context.level)
			parameter.variable = variable
			instance.variables.append(variable)
			if parameter.default is not None:
				target = type_.element if is_reference else type_
				actual = self.expression(parameter.default, context, target)
				self.coerce(parameter.default, actual, target)
				self.require_constant(parameter.default, context, 'a default argument')
			context.declare(parameter.name, variable)
			context.variables.append(variable)
		if node.body is not None:
			self.block(node.body, context, new_scope=False)
			if instance.result not in (None, ERROR) and not terminates(node.body):
				end = node.body.span
				self.error('E0514', f'`{info.name}` does not return a value on every path', node.name_span,
					f'must return `{instance.result}`').label(type(end)(end.source, end.end - 1, end.end), 'the end of the body can be reached')
			self.unused_mut(context)
		instance.state = 'checked'

	def unused_mut(self, context: Context) -> None:
		for variable in context.variables:
			if variable.mutable and not variable.mutated and variable.kind in ('let', 'param'):
				what = 'reference parameter' if variable.reference else 'variable' if variable.kind == 'let' else 'parameter'
				self.warning('W0503', f'{what} `{variable.name}` is `mut` but never changed', variable.span, 'remove `mut`')

	# --- statements ----------------------------------------------------------------

	def block(self, node: ast.Block, context: Context, new_scope: bool = True) -> None:
		if new_scope:
			context.scopes.append({})
		local_functions = []
		for statement in node.statements:
			if not isinstance(statement, ast.DeclarationStatement):
				continue
			declaration = statement.declaration
			if isinstance(declaration, ast.Function):
				symbol = FunctionInfo(declaration, context.module, parent=context)
				local_functions.append(symbol)
			elif isinstance(declaration, ast.Constant):
				symbol = ConstantInfo(declaration, context.module, context=context)
			else:
				continue
			statement.symbol = symbol
			previous = context.declare(declaration.name, symbol)
			if previous is not None:
				self.error('E0401', f'`{declaration.name}` is declared twice in this block', declaration.name_span, 'declared again here') \
					.label(previous.node.name_span if hasattr(previous, 'node') else previous.span, 'first declared here')
		for statement in node.statements:
			self.statement(statement, context)
		if new_scope:
			context.scopes.pop()

	def statement(self, node: ast.Node, context: Context) -> None:
		getattr(self, 's_' + type(node).__name__)(node, context)

	def s_DeclarationStatement(self, node: ast.DeclarationStatement, context: Context) -> None:
		symbol = getattr(node, 'symbol', None)
		if isinstance(symbol, ConstantInfo):
			self.constant(symbol)
		elif isinstance(symbol, FunctionInfo):
			# checked where it is declared, so that earlier variables are in scope
			# (and reported as captures when the body uses them).
			self.function_declaration(symbol, context)
			if not symbol.generics:
				instance = self.instance(symbol, (), context, symbol.node.name_span)
				if instance is not None:
					self.check_instance(instance)

	def s_Let(self, node: ast.Let, context: Context) -> None:
		found = self.attributes(node.attributes, 'let')
		saved = context.wrap
		context.wrap = saved or 'wrap' in found
		declared = self.resolve_type(node.type, context, infer=True) if node.type is not None else None
		type_ = declared
		if node.value is not None:
			if declared is not None:
				actual = self.expression(node.value, context, declared)
				if isinstance(declared, Array) and declared.length is None:
					if isinstance(actual, Array) and actual.element == declared.element:
						type_ = actual
					elif actual is not ERROR:
						self.error('E0502', 'mismatched types', node.value.span, f'expected `{declared}`, found `{actual}`')
						type_ = ERROR
				else:
					self.coerce(node.value, actual, declared)
			else:
				type_ = self.settle(node.value, self.expression(node.value, context))
		elif declared is None:
			self.error('E0506', f'variable `{node.name}` needs a type or a value', node.name_span, 'cannot infer its type') \
				.help(f'write `let {node.name}: T;` or `let {node.name} = value;`')
			type_ = ERROR
		elif isinstance(declared, Array) and declared.length is None:
			self.error('E0506', '`[_]` needs an initializer to take its length from', node.type.span, 'no value given')
			type_ = ERROR
		context.wrap = saved
		variable = Variable(node.name, type_ if type_ is not None else ERROR, 'let', node.mutable, node.name_span,
			level=context.level, loops=context.loops)
		node.variable = variable
		if node.value is None and declared is not None and 'uninit' not in found:
			context.unassigned.add(variable)
		previous = context.scopes[-1].get(node.name)
		if isinstance(previous, Variable):
			self.error('E0401', f'`{node.name}` is already declared in this scope', node.name_span, 'declared again') \
				.label(previous.span, 'first declared here').help('use another name, or assign with `=`')
		context.declare(node.name, variable)
		context.variables.append(variable)

	def s_Assign(self, node: ast.Assign, context: Context) -> None:
		found = self.attributes(node.attributes, 'statement')
		saved = context.wrap
		context.wrap = saved or 'wrap' in found
		try:
			self.assign(node, context)
		finally:
			context.wrap = saved

	def assign(self, node: ast.Assign, context: Context) -> None:
		target = node.target
		if isinstance(target, ast.Name) and target.name == '_':
			if node.operator != '=':
				self.error('E0508', '`_` can only be assigned with `=`', target.span, 'discards a value')
			self.settle(node.value, self.expression(node.value, context))
			return
		if node.operator == '=' and isinstance(target, ast.Name):
			symbol = self.lookup(target.name, context, target.span)
			if isinstance(symbol, Variable):
				target.variable = symbol
				target.type_ = target_type = symbol.type
			else:
				target_type = self.symbol_value(symbol, target, context, None) if symbol is not None else ERROR
				target.type_ = target_type
		else:
			target_type = self.expression(target, context)
		place = self.place(target, context)
		# the first assignment of 'let a: T;' initializes it, even when it is not 'mut'.
		initializing = (node.operator == '=' and isinstance(target, ast.Name) and place is not None
			and place.variable is not None and place.variable in context.unassigned and place.variable.kind == 'let'
			and place.variable.loops == context.loops)
		if target_type is not ERROR and not initializing:
			if place is None:
				self.error('E0508', f'cannot assign to `{target.span.text}`', target.span, 'not a variable, field or element')
			elif not place.mutable:
				self.report_immutable(place, target, 'assign to')
			elif place.needs_root and place.variable is not None:
				place.variable.mutated = True
		if node.operator == '=':
			actual = self.expression(node.value, context, target_type)
			self.coerce(node.value, actual, target_type)
			if isinstance(target, ast.Name) and place is not None and place.variable is not None:
				context.unassigned.discard(place.variable)
			return
		operator = node.operator[:-1]
		right = self.expression(node.value, context)
		result = self.binary_operation(node, operator, target, target_type, node.value, right, context, node.span)
		if result is not ERROR and target_type is not ERROR and result != target_type:
			self.error('E0502', f'`{operator}` gives `{result}`, which cannot be stored in `{target_type}`', node.span, 'mismatched types')

	def report_immutable(self, place: Place, node: ast.Node, action: str) -> None:
		diagnostic = self.error('E0508', f'cannot {action} `{node.span.text}`', node.span, place.reason)
		if place.help:
			diagnostic.help(place.help)
		if place.variable is not None:
			diagnostic.label(place.variable.span, 'declared here')

	def place(self, node: ast.Node, context: Context) -> Place | None:
		"""what an assignment target writes to and whether it may."""
		if isinstance(node, ast.Paren):
			return self.place(node.inner, context)
		if isinstance(node, ast.Name):
			variable = getattr(node, 'variable', None)
			if variable is None:
				return Place(None, False, f'`{node.name}` is not a variable', (), False)
			if variable.reference:
				return Place(variable, variable.mutable, f'`{node.name}` is a read-only reference', (variable,), True,
					f'declare the parameter as `mut {node.name}: &...`')
			if variable.kind == 'for':
				return Place(variable, False, 'a loop variable cannot be changed', (variable,), True, 'copy it into a `let mut` variable')
			how = 'with `let mut`' if variable.kind == 'let' else f'as `mut {node.name}`'
			return Place(variable, variable.mutable, f'`{node.name}` is not mutable', (variable,), True, f'declare it {how}')
		if isinstance(node, ast.Member):
			item = getattr(node, 'field', None)
			if item is None:
				return Place(None, False, 'not a field of a variable', (), False)
			base = self.place(node.target, context)
			if base is None or (base.variable is None and not base.mutable):
				return Place(None, False, 'part of a temporary value', (), False)
			path = base.path + (node.name,)
			if not item.mutable:
				return Place(base.variable, False, f'field `{node.name}` is not `mut`', path, base.needs_root,
					f'declare the field with `let mut {node.name}`')
			return Place(base.variable, base.mutable, base.reason, path, base.needs_root, base.help)
		if isinstance(node, ast.Index):
			if getattr(node, 'operator_call', None) is not None:
				return Place(None, False, 'an indexing operator function returns a copy', (), False)
			value = self.try_value(node.index, context)
			component = value if isinstance(value, int) else '*'
			base = self.place(node.target, context)
			if isinstance(getattr(node.target, 'type_', None), Slice):
				# the elements of a slice can always be written.
				root = base.variable if base is not None else None
				return Place(root, True, '', (base.path if base is not None else ()) + (component,), False)
			if base is None or (base.variable is None and not base.mutable):
				return Place(None, False, 'part of a temporary value', (), False)
			return Place(base.variable, base.mutable, base.reason, base.path + (component,), base.needs_root, base.help)
		return None

	def condition(self, node: ast.Node, context: Context) -> None:
		self.coerce(node, self.expression(node, context, BOOL), BOOL)

	def s_If(self, node: ast.If, context: Context) -> None:
		self.condition(node.condition, context)
		before = set(context.unassigned)
		self.block(node.then, context)
		after_then = set(context.unassigned)
		context.unassigned = set(before)
		if node.otherwise is not None:
			if isinstance(node.otherwise, ast.If):
				self.s_If(node.otherwise, context)
			else:
				self.block(node.otherwise, context)
			after_else = set(context.unassigned)
		else:
			after_else = before
		then_ends = terminates(node.then)
		else_ends = node.otherwise is not None and terminates(node.otherwise)
		if then_ends and not else_ends:
			context.unassigned = after_else
		elif else_ends and not then_ends:
			context.unassigned = after_then
		else:
			context.unassigned = after_then | after_else

	def s_While(self, node: ast.While, context: Context) -> None:
		self.condition(node.condition, context)
		before = set(context.unassigned)
		context.loops += 1
		self.block(node.body, context)
		context.loops -= 1
		context.unassigned |= before

	def s_For(self, node: ast.For, context: Context) -> None:
		start = self.expression(node.start, context)
		end = self.expression(node.end, context)
		type_ = self.common_integer(node.start, start, node.end, end)
		context.scopes.append({})
		variable = Variable(node.name, type_, 'for', False, node.name_span, level=context.level)
		node.variable = variable
		context.declare(node.name, variable)
		before = set(context.unassigned)
		context.loops += 1
		self.block(node.body, context)
		context.loops -= 1
		context.unassigned |= before
		context.scopes.pop()

	def common_integer(self, left_node: ast.Node, left: Type, right_node: ast.Node, right: Type) -> Type:
		if left is ERROR or right is ERROR:
			return ERROR
		if is_untyped(left) and is_untyped(right):
			left = self.settle(left_node, left)
			right = self.settle(right_node, right)
		elif is_untyped(left):
			self.coerce(left_node, left, right)
			left = right
		elif is_untyped(right):
			self.coerce(right_node, right, left)
			right = left
		if not (isinstance(left, Scalar) and left.is_integer):
			self.error('E0502', 'a range needs integers', left_node.span, f'found `{left}`')
			return ERROR
		if left != right:
			self.error('E0502', 'both ends of a range need the same type', right_node.span, f'expected `{left}`, found `{right}`') \
				.label(left_node.span, f'this is `{left}`')
			return ERROR
		return left

	def s_Return(self, node: ast.Return, context: Context) -> None:
		if context.result is None:
			if node.value is not None:
				actual = self.expression(node.value, context)
				if actual not in (NOTHING, ERROR):
					self.error('E0524', 'this function returns nothing', node.value.span, 'remove the value') \
						.help('declare a result type with `-> type`')
			return
		if node.value is None:
			self.error('E0524', f'this function must return `{context.result}`', node.span, 'missing value')
			return
		self.coerce(node.value, self.expression(node.value, context, context.result), context.result)

	def s_Break(self, node: ast.Break, context: Context) -> None:
		if context.loops == 0:
			self.error('E0515', '`break` outside of a loop', node.span, 'not inside `while` or `for`')

	def s_Continue(self, node: ast.Continue, context: Context) -> None:
		if context.loops == 0:
			self.error('E0515', '`continue` outside of a loop', node.span, 'not inside `while` or `for`')

	def s_Block(self, node: ast.Block, context: Context) -> None:
		self.block(node, context)

	def s_ExpressionStatement(self, node: ast.ExpressionStatement, context: Context) -> None:
		found = self.attributes(node.attributes, 'statement')
		saved = context.wrap
		context.wrap = saved or 'wrap' in found
		type_ = self.expression(node.expression, context)
		context.wrap = saved
		if is_untyped(type_):
			type_ = self.settle(node.expression, type_)
		if type_ not in (NOTHING, ERROR):
			self.error('E0509', 'the result of this expression is not used', node.expression.span, f'a `{type_}` is discarded') \
				.help(f'write `_ = {node.expression.span.text};` to discard it on purpose')

	# --- expressions ---------------------------------------------------------------

	def expression(self, node: ast.Node, context: Context, expected: Type | None = None) -> Type:
		type_ = getattr(self, 'e_' + type(node).__name__)(node, context, expected)
		node.type_ = type_
		return type_

	def checked(self, node: ast.Node, context: Context, expected: Type | None) -> Type:
		"""the type of 'node', checking it unless inference already did."""
		if getattr(node, 'prechecked', False):
			return node.type_
		return self.expression(node, context, expected)

	def loose(self, nodes: list[ast.Node], context: Context) -> None:
		"""checks arguments of a call that failed, to find their own errors."""
		for node in nodes:
			operand = node.operand if isinstance(node, ast.Reference) else node
			if getattr(operand, 'prechecked', False):
				continue
			type_ = self.expression(operand, context)
			if is_untyped(type_):
				self.settle(operand, type_)

	def e_IntegerLiteral(self, node: ast.IntegerLiteral, context: Context, expected: Type | None) -> Type:
		if node.suffix is None:
			return UNTYPED_INTEGER
		type_ = Scalar(node.suffix)
		value = -node.value if getattr(node, 'negated', False) else node.value
		if not (type_.minimum <= value <= type_.maximum):
			self.error('E0511', f'`{value}` does not fit in `{type_}`', node.span, f'`{type_}` holds {type_.minimum} to {type_.maximum}')
		return type_

	def e_FloatLiteral(self, node: ast.FloatLiteral, context: Context, expected: Type | None) -> Type:
		return Scalar(node.suffix) if node.suffix else UNTYPED_FLOAT

	def e_CharLiteral(self, node: ast.CharLiteral, context: Context, expected: Type | None) -> Type:
		return U8

	def e_StringLiteral(self, node: ast.StringLiteral, context: Context, expected: Type | None) -> Type:
		return Array(U8, len(node.data))

	def e_BoolLiteral(self, node: ast.BoolLiteral, context: Context, expected: Type | None) -> Type:
		return BOOL

	def e_NullLiteral(self, node: ast.NullLiteral, context: Context, expected: Type | None) -> Type:
		return NULL

	def e_Paren(self, node: ast.Paren, context: Context, expected: Type | None) -> Type:
		return self.expression(node.inner, context, expected)

	def e_Name(self, node: ast.Name, context: Context, expected: Type | None) -> Type:
		if node.name == '_':
			self.error('E0501', '`_` is not a value', node.span, 'only `_ = value;` discards')
			return ERROR
		symbol = self.lookup(node.name, context, node.span)
		return self.symbol_value(symbol, node, context, expected)

	def symbol_value(self, symbol: object, node: ast.Node, context: Context, expected: Type | None) -> Type:
		if symbol is None:
			return ERROR
		if isinstance(symbol, Variable):
			node.variable = symbol
			if symbol in context.unassigned:
				context.unassigned.discard(symbol)
				self.warning('W0502', f'`{symbol.name}` may be read before it is assigned', node.span, 'read here') \
					.label(symbol.span, 'declared without a value').help('assign it first, or mark the declaration `[uninit]`')
			return symbol.type
		if isinstance(symbol, ConstantInfo):
			node.constant = symbol
			return self.constant(symbol)
		if isinstance(symbol, FunctionInfo):
			if symbol.generics:
				if isinstance(expected, FunctionType):
					parameters, result = self.signature(symbol)
					bindings: dict = {}
					if unify(FunctionType(tuple(parameters), result), expected, bindings) and all(name in bindings for name in symbol.generics):
						instance = self.instance(symbol, tuple(bindings[name] for name in symbol.generics), context, node.span)
						if instance is None:
							return ERROR
						node.instance = instance
						return instance.type
				self.error('E0506', f'generic function `{symbol.name}` needs type arguments here', node.span, 'cannot infer them') \
					.help(f'write `{symbol.name}<{", ".join(symbol.generics)}>`')
				return ERROR
			instance = self.instance(symbol, (), context, node.span)
			if instance is None:
				return ERROR
			if VARIADIC in instance.parameters:
				self.error('E0529', f'variadic function `{symbol.name}` can only be called', node.span, 'not a value')
				return ERROR
			node.instance = instance
			return instance.type
		if isinstance(symbol, StructInfo):
			self.error('E0529', f'`{symbol.name}` is a type, not a value', node.span, 'expected a value') \
				.help(f'create a value with a literal: `({symbol.name}){{ ... }}`')
			return ERROR
		self.error('E0529', f'`{node.span.text}` is a module, not a value', node.span, 'expected a value')
		return ERROR

	def e_Member(self, node: ast.Member, context: Context, expected: Type | None) -> Type:
		if isinstance(node.target, (ast.Name, ast.Member)):
			base = self.qualified(node.target, context, report=False)
			if isinstance(base, (ModuleInfo, ModulePrefix)):
				symbol = self.member_of(base, node.name, node.name_span, context)
				return self.symbol_value(symbol, node, context, expected)
		target = self.expression(node.target, context)
		if target is ERROR:
			return ERROR
		if isinstance(target, StructType):
			layout = self.layout(target)
			item = layout.fields.get(node.name)
			if item is None:
				self.error('E0512', f'`{target}` has no field `{node.name}`', node.name_span, 'unknown field') \
					.note('fields: ' + (', '.join(layout.fields) or 'none'))
				return ERROR
			if not target.info.public and target.info.module is not context.module:
				self.error('E0522', f'field `{node.name}` of `{target}` is private', node.name_span, 'not accessible here') \
					.note(f'`{target.info.name}` is not `pub` in `{target.info.module.name}`, so its fields are private')
			node.field = item
			return item.type
		self.error('E0512', f'`{target}` has no fields', node.name_span, f'`.{node.name}` on a `{target}`')
		return ERROR

	def e_Specialize(self, node: ast.Specialize, context: Context, expected: Type | None) -> Type:
		symbol = self.qualified(node.target, context)
		if symbol is None:
			return ERROR
		if not isinstance(symbol, FunctionInfo) or not symbol.generics:
			self.error('E0521', f'`{node.target.span.text}` is not a generic function', node.span, 'remove the type arguments')
			return ERROR
		arguments = tuple(self.resolve_type(argument, context) for argument in node.type_arguments)
		if len(arguments) != len(symbol.generics):
			self.error('E0520', f'`{symbol.name}` takes {len(symbol.generics)} type argument(s), found {len(arguments)}', node.span, 'wrong count')
			return ERROR
		if ERROR in arguments:
			return ERROR
		instance = self.instance(symbol, arguments, context, node.span)
		if instance is None:
			return ERROR
		node.instance = instance
		return instance.type

	def e_Call(self, node: ast.Call, context: Context, expected: Type | None) -> Type:
		callee = node.callee
		symbol = None
		if isinstance(callee, ast.Name):
			symbol = self.lookup(callee.name, context, callee.span)
			if symbol is None:
				self.loose(node.arguments, context)
				return ERROR
		elif isinstance(callee, ast.Member) and isinstance(callee.target, (ast.Name, ast.Member)):
			base = self.qualified(callee.target, context, report=False)
			if isinstance(base, (ModuleInfo, ModulePrefix)):
				symbol = self.member_of(base, callee.name, callee.name_span, context)
				if symbol is None:
					self.loose(node.arguments, context)
					return ERROR
		if isinstance(symbol, FunctionInfo):
			instance = self.call_instance(symbol, node, context, expected)
			if instance is None:
				self.loose(node.arguments, context)
				return ERROR
			callee.type_ = instance.type
			callee.instance = instance
			node.instance = instance
			self.arguments(node, instance.parameters, instance.node.parameters, context, str(instance))
			return instance.result if instance.result is not None else NOTHING
		if isinstance(symbol, StructInfo):
			self.error('E0504', f'`{symbol.name}` is a struct, not a function', callee.span, 'cannot be called') \
				.help(f'create a value with a literal: `({symbol.name}){{ ... }}`')
			self.loose(node.arguments, context)
			return ERROR
		if node.type_arguments is not None:
			self.error('E0521', f'`{callee.span.text}` is not a generic function', callee.span, 'only generic functions take type arguments')
		callee_type = self.symbol_value(symbol, callee, context, None) if symbol is not None else self.expression(callee, context)
		callee.type_ = callee_type
		if callee_type is ERROR:
			self.loose(node.arguments, context)
			return ERROR
		if not isinstance(callee_type, FunctionType):
			self.error('E0504', f'`{callee.span.text}` is not a function', callee.span, f'it is a `{callee_type}`')
			self.loose(node.arguments, context)
			return ERROR
		self.arguments(node, list(callee_type.parameters), None, context, callee.span.text)
		return callee_type.result if callee_type.result is not None else NOTHING

	def call_instance(self, info: FunctionInfo, node: ast.Call, context: Context, expected: Type | None) -> Instance | None:
		if node.type_arguments is not None:
			if not info.generics:
				self.error('E0521', f'`{info.name}` is not generic', node.callee.span, 'remove the type arguments')
				return None
			arguments = tuple(self.resolve_type(argument, context) for argument in node.type_arguments)
			if len(arguments) != len(info.generics):
				self.error('E0520', f'`{info.name}` takes {len(info.generics)} type argument(s), found {len(arguments)}',
					node.callee.span, f'expected `{info.name}<{", ".join(info.generics)}>`')
				return None
			if ERROR in arguments:
				return None
			return self.instance(info, arguments, context, node.span)
		if not info.generics:
			return self.instance(info, (), context, node.span)
		parameters, result = self.signature(info)
		bindings: dict = {}
		for argument, parameter in zip(node.arguments, parameters):
			operand = argument.operand if isinstance(argument, ast.Reference) else argument
			pattern = parameter.element if isinstance(parameter, Reference) else parameter
			if not contains_parameter(pattern) or context_typed(operand):
				continue
			if isinstance(argument, ast.Reference) and argument.mutable and isinstance(operand, ast.Name):
				variable = self.lookup(operand.name, context, operand.span, report=False)
				context.unassigned.discard(variable)
			actual = self.expression(operand, context)
			operand.prechecked = True
			if actual is ERROR or is_untyped(actual) or actual is NULL:
				continue
			if not unify(pattern, actual, bindings):
				self.error('E0502', f'mismatched types in the call of `{info.name}`', operand.span,
					f'expected `{substitute(pattern, bindings)}`, found `{actual}`')
				return None
		if expected is not None and expected is not ERROR and result is not None and contains_parameter(result):
			trial = dict(bindings)
			if unify(result, expected, trial):
				bindings = trial
		for argument, parameter in zip(node.arguments, parameters):
			operand = argument.operand if isinstance(argument, ast.Reference) else argument
			pattern = parameter.element if isinstance(parameter, Reference) else parameter
			if isinstance(pattern, TypeParameter) and pattern.name not in bindings:
				kind = untyped_kind(operand) if context_typed(operand) else (
					'integer' if getattr(operand, 'type_', None) is UNTYPED_INTEGER else
					'float' if getattr(operand, 'type_', None) is UNTYPED_FLOAT else None)
				if kind is not None:
					bindings[pattern.name] = INT if kind == 'integer' else FLOAT
		missing = [name for name in info.generics if name not in bindings]
		if missing:
			self.error('E0506', f'cannot infer the type argument `{missing[0]}` of `{info.name}`', node.span,
				'not determined by the arguments').help(f'write it: `{info.name}<{", ".join(info.generics)}>(...)`')
			return None
		return self.instance(info, tuple(bindings[name] for name in info.generics), context, node.span)

	def arguments(self, node: ast.Call, parameters: list[Type], parameter_nodes: list[ast.Param] | None,
			context: Context, name: str) -> None:
		arguments = node.arguments
		variadic = bool(parameters) and parameters[-1] is VARIADIC
		if variadic:
			parameters = list(parameters[:-1])
			if len(arguments) < len(parameters):
				self.error('E0505', f'`{name}` takes at least {len(parameters)} argument(s), found {len(arguments)}', node.span,
					'too few arguments')
				self.loose(arguments, context)
				return
			self.variadic_arguments(node, parameters, context)
			arguments = arguments[:len(parameters)]
			parameter_nodes = None
		required = len(parameters) if parameter_nodes is None else sum(1 for item in parameter_nodes if item.default is None)
		if len(arguments) > len(parameters) or len(arguments) < required:
			expected_text = str(len(parameters)) if required == len(parameters) else f'{required} to {len(parameters)}'
			self.error('E0505', f'`{name}` takes {expected_text} argument(s), found {len(arguments)}', node.span,
				'wrong number of arguments')
			self.loose(arguments, context)
			return
		references = []
		for argument, parameter in zip(arguments, parameters):
			if isinstance(parameter, Reference):
				prefix = 'mut &' if parameter.mutable else '&'
				if not isinstance(argument, ast.Reference):
					actual = self.checked(argument, context, parameter.element)
					self.coerce(argument, actual, parameter.element)
					self.error('E0517', 'this parameter is a reference', argument.span, f'expected `{prefix}...`') \
						.help(f'write `{prefix}{argument.span.text}`')
					continue
				if argument.mutable != parameter.mutable:
					if parameter.mutable:
						self.error('E0518', 'this parameter is a writable reference', argument.span, 'passed read-only') \
							.help(f'write `mut &{argument.operand.span.text}`')
					else:
						self.error('E0518', 'this parameter is a read-only reference', argument.span, 'passed as writable') \
							.help(f'write `&{argument.operand.span.text}`')
				operand = argument.operand
				if argument.mutable and isinstance(operand, ast.Name):
					variable = self.lookup(operand.name, context, operand.span, report=False)
					context.unassigned.discard(variable)
				actual = self.checked(operand, context, parameter.element)
				argument.type_ = parameter
				self.coerce(operand, actual, parameter.element)
				place = self.place(operand, context)
				if argument.mutable and actual is not ERROR:
					if place is None:
						self.error('E0508', 'cannot pass a temporary value as `mut &`', operand.span, 'not a variable')
					elif not place.mutable:
						self.report_immutable(place, operand, 'pass as `mut &`')
					elif place.needs_root and place.variable is not None:
						place.variable.mutated = True
				if place is not None and place.variable is not None:
					references.append((argument, place))
				continue
			if isinstance(argument, ast.Reference):
				self.error('E0519', 'this parameter is not a reference', argument.span, 'remove the `&`')
				actual = self.checked(argument.operand, context, parameter)
				self.coerce(argument.operand, actual, parameter)
				continue
			if getattr(argument, 'format', False):
				continue
			self.coerce(argument, self.checked(argument, context, parameter), parameter)
		for index, (first, first_place) in enumerate(references):
			for second, second_place in references[index + 1:]:
				if (first.mutable or second.mutable) and overlaps(first_place.path, second_place.path):
					self.error('E0528', 'two references of this call overlap', second.span, 'overlaps the other reference') \
						.label(first.span, 'this one').help('a `mut &` argument must not share memory with another reference argument')

	def variadic_arguments(self, node: ast.Call, parameters: list[Type], context: Context) -> None:
		"""checks the values given for '...'; when the parameter before it is a
		'[]u8', it is a format string whose '{}' must match the values."""
		format_node = None
		if parameters and parameters[-1] == Slice(U8) and len(node.arguments) >= len(parameters):
			format_node = node.arguments[len(parameters) - 1]
		types_ = []
		for argument in node.arguments[len(parameters):]:
			if isinstance(argument, ast.Reference):
				self.error('E0519', 'values for `...` are passed by value', argument.span, 'remove the `&`')
				self.loose([argument], context)
				types_.append(ERROR)
				continue
			types_.append(self.settle(argument, self.expression(argument, context)))
		node.variadic_types = types_
		if format_node is None or isinstance(format_node, ast.Reference):
			return
		actual = self.expression(format_node, context, Slice(U8))
		if isinstance(actual, Array) and actual.element == U8:
			format_node.format = True             # a string literal: the compiler takes a slice of it
		elif not self.coerce(format_node, actual, Slice(U8)):
			format_node.format = True
			return
		format_node.format = True
		value = self.require_constant(format_node, context, 'the format string')
		if value is None:
			return
		from .evaluate import SliceValue
		data = bytes(value.data[value.start:value.start + value.length]) if isinstance(value, SliceValue) else bytes(value)
		count, problem = format_placeholders(data)
		if problem is not None:
			self.error('E0530', 'invalid format string', format_node.span, problem) \
				.help('`{}` is replaced by the next value; write `{{` and `}}` for braces')
		elif count != len(types_):
			self.error('E0530', f'the format string has {count} placeholder(s), but {len(types_)} value(s) are given',
				node.span, 'they must match').label(format_node.span, f'{count} `{{}}`')

	def e_Index(self, node: ast.Index, context: Context, expected: Type | None) -> Type:
		target = self.expression(node.target, context)
		if target is ERROR:
			self.expression(node.index, context)
			return ERROR
		if isinstance(target, StructType):
			index = self.expression(node.index, context)
			result = self.operator_call(node, '_[_]', [node.target, node.index], [target, index], context)
			if result is not None:
				return result
			self.error('E0513', f'`{target}` cannot be indexed', node.span, 'no `[op._[_]]` function for it')
			return ERROR
		if not isinstance(target, (Array, Slice)):
			self.error('E0513', f'`{target}` cannot be indexed', node.target.span, 'not an array or a slice')
			self.expression(node.index, context)
			return ERROR
		self.index(node.index, context, target)
		return target.element

	def index(self, node: ast.Node, context: Context, target: Type, end: bool = False) -> None:
		type_ = self.expression(node, context)
		if type_ is ERROR:
			return
		if type_ is UNTYPED_INTEGER:
			self.settle(node, type_)
		elif not (isinstance(type_, Scalar) and type_.is_integer):
			self.error('E0502', 'an index must be an integer', node.span, f'found `{type_}`')
			return
		value = self.try_value(node, context)
		if isinstance(value, int):
			limit = target.length if isinstance(target, Array) else None
			if value < 0 or (limit is not None and (value > limit if end else value >= limit)):
				self.error('E0706', f'index {value} is out of bounds', node.span,
					f'the array has {limit} elements' if limit is not None else 'indexes cannot be negative')

	def e_SliceOf(self, node: ast.SliceOf, context: Context, expected: Type | None) -> Type:
		target = self.expression(node.target, context)
		if target is ERROR:
			return ERROR
		if not isinstance(target, (Array, Slice)):
			self.error('E0513', f'`{target}` cannot be sliced', node.target.span, 'not an array or a slice')
			return ERROR
		if node.low is not None:
			self.index(node.low, context, target, end=True)
		if node.high is not None:
			self.index(node.high, context, target, end=not node.inclusive)
		return Slice(target.element)

	def e_Unary(self, node: ast.Unary, context: Context, expected: Type | None) -> Type:
		operator = node.operator
		if operator == '-' and isinstance(node.operand, ast.IntegerLiteral):
			node.operand.negated = True
		operand = self.expression(node.operand, context, BOOL if operator == '!' else expected)
		if operand is ERROR:
			return ERROR
		if operator == '!' and operand == BOOL:
			return BOOL
		if operator == '-' and (is_untyped(operand) or (isinstance(operand, Scalar) and (operand.is_float or (operand.is_integer and operand.signed)))):
			return operand
		if operator == '~' and (operand is UNTYPED_INTEGER or (isinstance(operand, Scalar) and operand.is_integer)):
			return operand
		result = self.operator_call(node, operator + '_', [node.operand], [operand], context)
		if result is not None:
			return result
		diagnostic = self.error('E0507', f'cannot apply unary `{operator}` to `{operand}`', node.span, 'no such operation')
		if operator == '-' and isinstance(operand, Scalar) and operand.is_integer:
			diagnostic.help('unsigned values cannot be negated; cast to a signed type first')
		return ERROR

	def e_Binary(self, node: ast.Binary, context: Context, expected: Type | None) -> Type:
		if node.operator in ('&&', '||'):
			for side in (node.left, node.right):
				self.coerce(side, self.expression(side, context, BOOL), BOOL)
			return BOOL
		left = self.expression(node.left, context)
		right = self.expression(node.right, context)
		return self.binary_operation(node, node.operator, node.left, left, node.right, right, context, node.operator_span)

	def binary_operation(self, node: ast.Node, operator: str, left_node: ast.Node, left: Type, right_node: ast.Node,
			right: Type, context: Context, span) -> Type:
		if left is ERROR or right is ERROR:
			return ERROR
		if is_untyped(left) and is_untyped(right):
			any_float = UNTYPED_FLOAT in (left, right)
			if operator in EQUALITY | ORDERING:
				self.finalize(left_node, FLOAT if any_float else INT)
				self.finalize(right_node, FLOAT if any_float else INT)
				return BOOL
			if (operator in BITWISE | SHIFTS or operator == '%') and any_float:
				self.error('E0507', f'`{operator}` needs integers', span, 'a float operand')
				return ERROR
			if operator in ('/', '%') and not any_float and self.untyped_value(right_node) == 0:
				self.error('E0704', 'division by zero', node.span, 'the divisor is zero at compile time')
				return ERROR
			return UNTYPED_FLOAT if any_float else UNTYPED_INTEGER
		if is_untyped(left) and accepts_literal(right, left):
			self.coerce(left_node, left, right)
			left = right
		elif is_untyped(right) and operator in SHIFTS and right is UNTYPED_INTEGER and isinstance(left, Scalar) and left.is_integer:
			self.coerce(right_node, right, UINT)
			right = UINT
		elif is_untyped(right) and accepts_literal(left, right):
			self.coerce(right_node, right, left)
			right = left
		if operator in EQUALITY and ((left is NULL and isinstance(right, Slice)) or (right is NULL and isinstance(left, Slice))):
			self.coerce(left_node if left is NULL else right_node, NULL, right if left is NULL else left)
			return BOOL
		if isinstance(left, Scalar) and isinstance(right, Scalar):
			if operator in ARITHMETIC and left == right and is_numeric(left) and (operator != '%' or left.is_integer):
				self.check_overflow(node, operator, left_node, right_node, left, context)
				return left
			if operator in BITWISE and left == right and left.is_integer:
				return left
			if operator in SHIFTS and left.is_integer and right.is_integer:
				self.check_overflow(node, operator, left_node, right_node, left, context)
				return left
			if operator in EQUALITY and left == right:
				return BOOL
			if operator in ORDERING and left == right and is_numeric(left):
				return BOOL
		if isinstance(left, FunctionType) and left == right and operator in EQUALITY:
			return BOOL
		pattern, swapped, negated = OPERATOR_FOR[operator]
		nodes, types_ = [left_node, right_node], [left, right]
		if swapped:
			nodes.reverse()
			types_.reverse()
		result = self.operator_call(node, pattern, nodes, types_, context)
		if result is ERROR:
			return ERROR
		if result is not None:
			node.operator_swapped = swapped
			node.operator_negated = negated
			return BOOL if operator in EQUALITY | ORDERING else result
		diagnostic = self.error('E0507', f'cannot apply `{operator}` to `{left}` and `{right}`', span, 'no such operation')
		if is_numeric(left) and is_numeric(right):
			diagnostic.help(f'mixed types need an explicit cast, for example `({left}){right_node.span.text}`')
		elif isinstance(left, StructType) or isinstance(right, StructType):
			diagnostic.help(f'declare a function marked `[op.{pattern}]` for ({", ".join(map(str, types_))})')
		elif is_untyped(left) or is_untyped(right):
			diagnostic.help('a float literal cannot become an integer')
		return ERROR

	def check_overflow(self, node: ast.Node, operator: str, left_node: ast.Node, right_node: ast.Node, type_: Scalar,
			context: Context) -> None:
		"""warns when integer arithmetic on known values wraps around."""
		if not type_.is_integer:
			return
		left = self.try_value(left_node, context)
		right = self.try_value(right_node, context)
		if not isinstance(left, int) or not isinstance(right, int) or isinstance(left, bool):
			return
		if operator in ('/', '%') and right == 0:
			self.error('E0704', 'division by zero', node.span, 'the divisor is zero at compile time')
			return
		if context.wrap or (operator in SHIFTS and right < 0):
			return
		from .evaluate import arithmetic, wrap
		exact = arithmetic(operator, left, right)
		if not (type_.minimum <= exact <= type_.maximum):
			self.warning('W0701', f'`{node.span.text}` overflows `{type_}`', node.span, f'wraps around to {wrap(exact, type_)}') \
				.help('mark the statement or declaration `[wrap]` if the wrap-around is intended')

	def visible_operators(self, pattern: str, types_: list[Type], context: Context) -> list[FunctionInfo]:
		modules = [context.module] + [target for _, target in context.module.imports]
		for type_ in types_:
			if isinstance(type_, StructType):
				modules.append(type_.info.module)
		result, seen = [], []
		for module in modules:
			if module in seen:
				continue
			seen.append(module)
			for info in module.operators.get(pattern, []):
				if module is context.module or info.public:
					result.append(info)
		return result

	def operator_call(self, node: ast.Node, pattern: str, operand_nodes: list[ast.Node], types_: list[Type],
			context: Context) -> Type | None:
		"""the result of an operator function for 'types_', none when there is none."""
		matches = []
		for info in self.visible_operators(pattern, types_, context):
			instance = info.instances.get(())
			if instance is None or len(instance.parameters) != len(types_):
				continue
			if all(parameter == actual or accepts_literal(parameter, actual) or (actual is NULL and isinstance(parameter, Slice))
					for parameter, actual in zip(instance.parameters, types_)):
				matches.append(instance)
		if not matches:
			return None
		chosen = matches[0]
		if len(matches) > 1:
			defaults = [match for match in matches if all(parameter == literal_default(actual)
				for parameter, actual in zip(match.parameters, types_) if is_untyped(actual))]
			if len(defaults) != 1:
				diagnostic = self.error('E0507', f'several operator functions `{pattern}` match', node.span, 'ambiguous')
				for match in matches:
					diagnostic.note(f'candidate: `{match}` ({", ".join(map(str, match.parameters))})')
				return ERROR
			chosen = defaults[0]
			diagnostic = self.warning('W0501', 'ambiguous literal: several operator functions match', node.span,
				'the literal fits more than one')
			for match in matches:
				diagnostic.note(f'candidate: `{match}` ({", ".join(map(str, match.parameters))})')
			diagnostic.note(f'using `{chosen}`, the default type of the literal').help('cast the literal to choose, for example `(f32)2`')
		for operand, parameter, actual in zip(operand_nodes, chosen.parameters, types_):
			if is_untyped(actual) or actual is NULL:
				self.coerce(operand, actual, parameter)
		node.operator_call = chosen
		return chosen.result

	def e_Cast(self, node: ast.Cast, context: Context, expected: Type | None) -> Type:
		target = self.resolve_type(node.type, context)
		operand = self.expression(node.operand, context)
		if operand is ERROR or target is ERROR:
			return target
		if is_untyped(operand):
			value = self.untyped_value(node.operand)
			if isinstance(target, Scalar) and accepts_literal(target, operand) and (not isinstance(value, int) or target.is_float
					or target.minimum <= value <= target.maximum):
				self.finalize(node.operand, target)
				operand = target
			else:
				operand = self.settle(node.operand, operand)
		if isinstance(target, Scalar) and isinstance(operand, Scalar) and (target == operand or (is_numeric(target) and is_numeric(operand))):
			return target
		self.error('E0510', f'cannot cast `{operand}` to `{target}`', node.span, 'invalid cast').help('casts convert between number types')
		return target

	def e_Reference(self, node: ast.Reference, context: Context, expected: Type | None) -> Type:
		self.error('E0519', '`&` is only allowed on a call argument', node.span, 'not an argument of a reference parameter') \
			.help('references are passed to functions; use a slice to keep a view of an array')
		self.loose([node.operand], context)
		return ERROR

	def e_Composite(self, node: ast.Composite, context: Context, expected: Type | None) -> Type:
		target = expected
		if node.type is not None:
			target = self.resolve_type(node.type, context, infer=True)
		if target is None or target is ERROR:
			if target is None:
				self.error('E0506', 'cannot infer the type of this literal', node.span, 'no type known here') \
					.help('write the type, `(T){ ... }`, or give the variable a type')
			self.loose([entry.value for entry in node.entries], context)
			return ERROR
		return self.composite(node, target, context)

	def composite(self, node: ast.Composite, target: Type, context: Context) -> Type:
		entries = node.entries
		named = bool(entries) and entries[0].name is not None
		if not entries:
			if not self.zeroable(target, set()):
				self.error('E0526', f'`{target}` has no zero value', node.span, 'a function value cannot be empty')
				return ERROR
			if isinstance(target, Array) and target.length is None:
				self.error('E0506', '`[_]` cannot take its length from `{}`', node.span, 'no elements')
				return ERROR
			return target
		if isinstance(target, StructType):
			if not named:
				self.error('E0525', 'struct literals need field names', node.span, 'positional values') \
					.help('write `{ field: value, ... }`')
				self.loose([entry.value for entry in entries], context)
				return target
			layout = self.layout(target)
			if not target.info.public and target.info.module is not context.module:
				self.error('E0522', f'the fields of `{target}` are private', node.span, 'cannot be initialized here') \
					.note(f'`{target.info.name}` is not `pub` in `{target.info.module.name}`')
			given = set()
			for entry in entries:
				if entry.name is None:
					# a positional entry among named ones (reported by the parser).
					self.loose([entry.value], context)
					continue
				item = layout.fields.get(entry.name)
				if item is None:
					self.error('E0512', f'`{target}` has no field `{entry.name}`', entry.span, 'unknown field') \
						.note('fields: ' + ', '.join(layout.fields))
					self.loose([entry.value], context)
					continue
				if entry.name in given:
					self.error('E0525', f'field `{entry.name}` is given twice', entry.span, 'repeated')
					continue
				given.add(entry.name)
				self.coerce(entry.value, self.expression(entry.value, context, item.type), item.type)
			for name, item in layout.fields.items():
				if name in given or item.default is not None:
					continue
				if not self.zeroable(item.type, set()):
					self.error('E0526', f'field `{name}` must be given', node.span, 'a function value has no zero value')
				else:
					self.warning('W0504', f'field `{name}` is not initialized', node.span, 'it will be zero') \
						.help(f'give it a value, or a default in `{target.info.name}`')
			return target
		if isinstance(target, (Array, Slice)):
			if named:
				self.error('E0525', 'array literals take values without names', node.span, 'named entries')
				self.loose([entry.value for entry in entries], context)
				return target
			for entry in entries:
				self.coerce(entry.value, self.expression(entry.value, context, target.element), target.element)
			if isinstance(target, Array):
				if target.length is None:
					return Array(target.element, len(entries))
				if len(entries) != target.length:
					self.error('E0525', f'expected {target.length} elements, found {len(entries)}', node.span, f'for `{target}`') \
						.help('write `[_]` in the type to take the length from the literal')
			return target
		self.error('E0525', f'a `{target}` cannot be written as `{{ ... }}`', node.span, 'only `{}` (zero) is allowed') \
			.help('`{ ... }` with values is for structs, arrays and slices')
		return ERROR

	def zeroable(self, type_: Type, seen: set) -> bool:
		if isinstance(type_, FunctionType):
			return False
		if isinstance(type_, Array):
			return self.zeroable(type_.element, seen)
		if isinstance(type_, StructType):
			if type_ in seen:
				return True
			seen.add(type_)
			return all(item.default is not None or self.zeroable(item.type, seen) for item in self.layout(type_).fields.values())
		return True

	def e_Lambda(self, node: ast.Lambda, context: Context, expected: Type | None) -> Type:
		inner = Context(context.module, context.bindings, parent=context, function=True)
		parameters = []
		for parameter in node.parameters:
			if parameter.variadic:
				self.error('E0415', 'lambdas cannot take `...`', parameter.span, 'variadic parameter')
				parameters.append(ERROR)
				continue
			type_ = self.resolve_type(parameter.type, inner, reference=True)
			if isinstance(type_, Reference):
				type_ = Reference(type_.element, parameter.mutable)
			if parameter.default is not None:
				self.error('E0412', 'lambda parameters cannot have defaults', parameter.default.span, 'remove the default')
			parameters.append(type_)
		result = self.resolve_type(node.result, inner) if node.result is not None else None
		inner.result = result
		for parameter, type_ in zip(node.parameters, parameters):
			is_reference = isinstance(type_, Reference)
			variable = Variable(parameter.name, type_.element if is_reference else type_, 'param', parameter.mutable,
				parameter.span, reference=is_reference, level=inner.level)
			parameter.variable = variable
			inner.declare(parameter.name, variable)
			inner.variables.append(variable)
		self.block(node.body, inner, new_scope=False)
		if result is not None and not terminates(node.body):
			self.error('E0514', 'this lambda does not return a value on every path', node.span, f'must return `{result}`')
		self.unused_mut(inner)
		return FunctionType(tuple(parameters), result)

	def e_SizeOf(self, node: ast.SizeOf, context: Context, expected: Type | None) -> Type:
		type_ = self.resolve_type(node.type, context)
		node.size = self.size_of(type_) if type_ is not ERROR else 0
		return UINT

	def e_LenOf(self, node: ast.LenOf, context: Context, expected: Type | None) -> Type:
		operand = self.expression(node.operand, context)
		if operand is ERROR:
			return UINT
		if not isinstance(operand, (Array, Slice)):
			self.error('E0507', f'`lenof` needs an array or a slice, found `{operand}`', node.operand.span, 'no length')
		return UINT

	def e_Assert(self, node: ast.Assert, context: Context, expected: Type | None) -> Type:
		actual = self.expression(node.condition, context, BOOL)
		self.coerce(node.condition, actual, BOOL)
		if node.message is not None:
			if not isinstance(node.message, ast.StringLiteral):
				self.error('E0502', 'the message of `assert` must be a string literal', node.message.span, 'not a string literal')
			else:
				node.message.type_ = Array(U8, len(node.message.data))
		if actual == BOOL and self.try_value(node.condition, context) is False:
			diagnostic = self.error('E0703', 'assertion is always false', node.condition.span, 'checked at compile time')
			if isinstance(node.message, ast.StringLiteral):
				diagnostic.note(node.message.data.decode('utf-8', 'replace'))
		return NOTHING

	# --- conversions -------------------------------------------------------------

	def untyped_value(self, node: ast.Node):
		from .evaluate import EvaluationError, NotConstant
		try:
			return self.evaluator.constant(node, None, quiet=True)
		except (NotConstant, EvaluationError):
			return None

	def finalize(self, node: ast.Node, target: Type) -> None:
		"""gives the untyped parts of 'node' the type 'target'."""
		current = getattr(node, 'type_', None)
		if current is not None and not is_untyped(current):
			return
		node.type_ = target
		if isinstance(node, ast.Paren):
			self.finalize(node.inner, target)
		elif isinstance(node, ast.Unary):
			self.finalize(node.operand, target)
		elif isinstance(node, ast.Binary) and node.operator not in EQUALITY | ORDERING | {'&&', '||'}:
			self.finalize(node.left, target)
			self.finalize(node.right, target)

	def settle(self, node: ast.Node, actual: Type) -> Type:
		"""the type of a value without context: untyped literals become int or float."""
		if actual is UNTYPED_INTEGER:
			value = self.untyped_value(node)
			if isinstance(value, int) and not (INT.minimum <= value <= INT.maximum):
				self.error('E0511', f'`{value}` does not fit in `int`', node.span, 'too large for the default type') \
					.help('give it a type, for example with the suffix `u64`')
			self.finalize(node, INT)
			return INT
		if actual is UNTYPED_FLOAT:
			self.finalize(node, FLOAT)
			return FLOAT
		if actual is NULL:
			self.error('E0506', 'cannot infer the type of `null`', node.span, 'no slice type known here')
			return ERROR
		if actual is NOTHING:
			self.error('E0523', 'this has no value', node.span, 'the function returns nothing')
			return ERROR
		return actual

	def coerce(self, node: ast.Node, actual: Type, target: Type | None) -> bool:
		"""checks that 'actual' can be used as 'target'; literals take the target type."""
		if target is None or actual is ERROR or target is ERROR or actual == target:
			return True
		if accepts_literal(target, actual):
			value = self.untyped_value(node)
			if isinstance(value, int) and target.is_integer and not (target.minimum <= value <= target.maximum):
				self.error('E0511', f'`{value}` does not fit in `{target}`', node.span, f'`{target}` holds {target.minimum} to {target.maximum}')
				self.finalize(node, target)
				return False
			self.finalize(node, target)
			return True
		if actual is NULL and isinstance(target, Slice):
			node.type_ = target
			return True
		if actual is NOTHING:
			self.error('E0523', 'this has no value', node.span, 'the function returns nothing')
			return False
		diagnostic = self.error('E0502', 'mismatched types', node.span, f'expected `{target}`, found `{actual}`')
		if isinstance(actual, Array) and isinstance(target, Slice) and actual.element == target.element:
			diagnostic.help(f'take a slice of the array: `{node.span.text}[..]`')
		elif is_numeric(actual) and is_numeric(target):
			diagnostic.help(f'convert explicitly: `({target}){node.span.text}`')
		elif actual is UNTYPED_FLOAT and isinstance(target, Scalar) and target.is_integer:
			diagnostic.help('a float literal cannot become an integer')
		return False


@dataclass
class Place:
	variable: Variable | None
	mutable: bool
	reason: str
	path: tuple
	needs_root: bool
	help: str = ''


OPERATOR_FOR = {
	'+': ('_+_', False, False), '-': ('_-_', False, False), '*': ('_*_', False, False), '/': ('_/_', False, False),
	'%': ('_%_', False, False), '&': ('_&_', False, False), '|': ('_|_', False, False), '^': ('_^_', False, False),
	'<<': ('_<<_', False, False), '>>': ('_>>_', False, False),
	'==': ('_==_', False, False), '!=': ('_==_', False, True),
	'<': ('_<_', False, False), '>': ('_<_', True, False), '<=': ('_<_', True, True), '>=': ('_<_', False, True),
}


def accepts_literal(target: Type, actual: Type) -> bool:
	"""whether an untyped literal of type 'actual' can become a 'target'."""
	if not isinstance(target, Scalar):
		return False
	if actual is UNTYPED_INTEGER:
		return is_numeric(target)
	if actual is UNTYPED_FLOAT:
		return target.is_float
	return False


def literal_default(type_: Type) -> Type:
	return INT if type_ is UNTYPED_INTEGER else FLOAT if type_ is UNTYPED_FLOAT else type_


def context_typed(node: ast.Node) -> bool:
	"""whether the type of 'node' comes from where it is used (literals, '{...}', null)."""
	if isinstance(node, (ast.IntegerLiteral, ast.FloatLiteral)):
		return node.suffix is None
	if isinstance(node, ast.NullLiteral):
		return True
	if isinstance(node, ast.Composite):
		return node.type is None
	if isinstance(node, ast.Paren):
		return context_typed(node.inner)
	if isinstance(node, ast.Unary):
		return node.operator in ('-', '~') and context_typed(node.operand)
	if isinstance(node, ast.Binary):
		return node.operator in ARITHMETIC | BITWISE | SHIFTS and context_typed(node.left) and context_typed(node.right)
	return False


def untyped_kind(node: ast.Node) -> str | None:
	if isinstance(node, ast.IntegerLiteral) and node.suffix is None:
		return 'integer'
	if isinstance(node, ast.FloatLiteral) and node.suffix is None:
		return 'float'
	if isinstance(node, ast.Paren):
		return untyped_kind(node.inner)
	if isinstance(node, ast.Unary):
		return untyped_kind(node.operand)
	if isinstance(node, ast.Binary):
		kinds = {untyped_kind(node.left), untyped_kind(node.right)}
		if None in kinds:
			return None
		return 'float' if 'float' in kinds else 'integer'
	return None


def format_placeholders(data: bytes) -> tuple[int, str | None]:
	"""the number of '{}' in a format string, or a problem."""
	count, index = 0, 0
	while index < len(data):
		character = data[index:index + 1]
		following = data[index + 1:index + 2]
		if character == b'{':
			if following == b'{':
				index += 2
				continue
			if following == b'}':
				count += 1
				index += 2
				continue
			return count, '`{` must be followed by `}` (format options are not supported yet)'
		if character == b'}':
			if following == b'}':
				index += 2
				continue
			return count, 'a single `}` must be written `}}`'
		index += 1
	return count, None


def overlaps(first: tuple, second: tuple) -> bool:
	if not first or not second or first[0] is not second[0]:
		return False
	for left, right in zip(first[1:], second[1:]):
		if left == '*' or right == '*':
			continue
		if left != right:
			return False
	return True


def terminates(node: ast.Node | None) -> bool:
	"""whether control never continues after 'node' (it returns on every path)."""
	if isinstance(node, ast.Return):
		return True
	if isinstance(node, ast.Block):
		return any(terminates(statement) for statement in node.statements)
	if isinstance(node, ast.If):
		return node.otherwise is not None and terminates(node.then) and terminates(node.otherwise)
	if isinstance(node, ast.While):
		return isinstance(node.condition, ast.BoolLiteral) and node.condition.value and not breaks(node.body)
	return False


def breaks(node: ast.Node) -> bool:
	if isinstance(node, ast.Break):
		return True
	if isinstance(node, (ast.While, ast.For, ast.Lambda)):
		return False
	if isinstance(node, ast.Block):
		return any(breaks(statement) for statement in node.statements)
	if isinstance(node, ast.If):
		return breaks(node.then) or (node.otherwise is not None and breaks(node.otherwise))
	return False


def check(entry: str, diagnostics: Diagnostics, include: list[str] | None = None, stdlib: str | None = None,
		files: dict[str, str] | None = None) -> Program:
	loader = Loader(diagnostics, include, stdlib)
	if files:
		loader.overrides.update(files)
	return Checker(diagnostics, loader).run(entry)
