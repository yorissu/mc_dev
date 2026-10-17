
"""parser: tokens -> syntax tree (recursive descent).

declarations are 'name :: ...': a struct when the braces start with 'let', a
function when the parentheses hold parameters ('()', '(name:' or '(mut'), and
a constant otherwise. operators follow c's order but are strict: where the
order is not obvious (bit operators, shifts, mixing '&&' with '||', chained
comparisons) parentheses are required. because comparisons cannot be chained,
'name<T>(' is always a call of a generic function."""

from __future__ import annotations

from . import ast
from .diag import Diagnostics
from .lexer import SCALAR_TYPES, Token, lex
from .source import Source, Span

PRECEDENCE = {
	'||': 1, '&&': 2,
	'==': 3, '!=': 3, '<': 3, '>': 3, '<=': 3, '>=': 3,
	'|': 4, '^': 5, '&': 6, '<<': 7, '>>': 7,
	'+': 8, '-': 8, '*': 9, '/': 9, '%': 9,
}

GROUP = {
	'||': 'or', '&&': 'and',
	'==': 'compare', '!=': 'compare', '<': 'compare', '>': 'compare', '<=': 'compare', '>=': 'compare',
	'|': 'bit', '^': 'bit', '&': 'bit', '<<': 'shift', '>>': 'shift',
	'+': 'add', '-': 'add', '*': 'multiply', '/': 'multiply', '%': 'multiply',
}

ASSIGNMENTS = {'=', '+=', '-=', '*=', '/=', '%=', '&=', '|=', '^=', '<<=', '>>='}

# tokens after which 'name<types>' without a call is a generic function value.
SPECIALIZE_FOLLOWERS = {';', ')', ',', ']', '}'}


def combinable(parent: str, child: str) -> bool:
	"""whether 'child' may appear unparenthesized as an operand of 'parent'."""
	parent_group, child_group = GROUP[parent], GROUP[child]
	if parent_group == child_group:
		if parent_group == 'compare':
			return False
		if parent_group in ('add', 'multiply', 'and', 'or'):
			return True
		return parent == child
	if parent_group in ('and', 'or'):
		return child_group not in ('and', 'or')
	if parent_group == 'compare':
		return child_group in ('add', 'multiply')
	if parent_group == 'add':
		return child_group == 'multiply'
	return False


class ParseError(Exception):
	pass


class Parser:
	def __init__(self, tokens: list[Token], diagnostics: Diagnostics) -> None:
		self.tokens = tokens
		self.position = 0
		self.diagnostics = diagnostics
		self.speculating = 0
		self.composites = True     # false in conditions, where '{' starts the body

	# --- token helpers ---------------------------------------------------------

	@property
	def token(self) -> Token:
		return self.tokens[self.position]

	def peek(self, offset: int = 1) -> Token:
		return self.tokens[min(self.position + offset, len(self.tokens) - 1)]

	def at(self, *texts: str) -> bool:
		return self.token.is_(*texts)

	def advance(self) -> Token:
		token = self.token
		if token.kind != 'eof':
			self.position += 1
		return token

	def accept(self, text: str) -> Token | None:
		return self.advance() if self.at(text) else None

	def fail(self, message: str, span: Span | None = None, label: str = '', code: str = 'E0201') -> ParseError:
		if not self.speculating:
			self.diagnostics.error(code, message, span or self.token.span, label)
		return ParseError()

	def describe(self, token: Token) -> str:
		return 'end of file' if token.kind == 'eof' else f'`{token.text}`'

	def expect(self, text: str, context: str = '') -> Token:
		if self.at(text):
			return self.advance()
		where = f' {context}' if context else ''
		raise self.fail(f'expected `{text}`{where}, found {self.describe(self.token)}', label=f'expected `{text}`')

	def expect_identifier(self, what: str = 'a name') -> Token:
		if self.token.kind == 'identifier':
			return self.advance()
		raise self.fail(f'expected {what}, found {self.describe(self.token)}', label=f'expected {what}')

	def expect_close_angle(self) -> Token:
		"""'>' closing type arguments; splits '>>', '>=' and '>>=' tokens."""
		token = self.token
		if token.is_('>'):
			return self.advance()
		if token.is_('>>', '>=', '>>='):
			rest = token.text[1:]
			first = Token('punctuator', '>', Span(token.span.source, token.span.start, token.span.start + 1))
			self.tokens[self.position] = Token('punctuator', rest, Span(token.span.source, token.span.start + 1, token.span.end))
			return first
		raise self.fail(f'expected `>`, found {self.describe(token)}', label='expected `>`')

	def span_from(self, start: Span) -> Span:
		previous = self.tokens[self.position - 1] if self.position > 0 else self.token
		return Span(start.source, start.start, max(start.end, previous.span.end))

	def recover(self, stop: tuple[str, ...]) -> None:
		"""skips to just after the next 'stop' token at nesting depth zero."""
		depth = 0
		while self.token.kind != 'eof':
			if depth == 0 and self.at(*stop):
				self.advance()
				return
			if self.at('{', '(', '['):
				depth += 1
			elif self.at('}', ')', ']'):
				if depth == 0:
					return
				depth -= 1
				if depth == 0 and '}' in stop and self.token.is_('}'):
					self.advance()
					return
			self.advance()

	# --- modules ---------------------------------------------------------------

	def module(self) -> ast.Module:
		start = self.token.span
		imports: list[ast.Node] = []
		declarations: list[ast.Node] = []
		while self.token.kind != 'eof':
			before = self.position
			try:
				if self.at('import'):
					imports.append(self.import_())
				elif self.at('from'):
					imports.append(self.from_import())
				else:
					declarations.append(self.declaration(top=True))
			except ParseError:
				self.recover((';', '}'))
				if self.position == before:
					self.advance()
		return ast.Module(self.span_from(start), imports, declarations)

	def path(self) -> list[str]:
		parts = [self.expect_identifier('a module name').text]
		while self.accept('.'):
			parts.append(self.expect_identifier('a module name').text)
		return parts

	def import_(self) -> ast.Import:
		start = self.advance().span
		path = self.path()
		alias = self.expect_identifier('an alias').text if self.accept('as') else None
		return ast.Import(self.span_from(start), path, alias)

	def from_import(self) -> ast.FromImport:
		start = self.advance().span
		path = self.path()
		self.expect('import', 'after the module name')
		if self.accept('*'):
			return ast.FromImport(self.span_from(start), path, [], True)
		names = []
		while True:
			name = self.expect_identifier('a name to import')
			alias = self.expect_identifier('an alias').text if self.accept('as') else None
			names.append(ast.ImportName(self.span_from(name.span), name.text, alias))
			if not self.accept(','):
				break
		return ast.FromImport(self.span_from(start), path, names, False)

	# --- attributes ------------------------------------------------------------

	def attributes(self) -> list[ast.Attribute]:
		result = []
		while self.at('['):
			self.advance()
			while True:
				result.append(self.attribute())
				if not self.accept(','):
					break
			self.expect(']', 'to close the attribute list')
		return result

	def attribute(self) -> ast.Attribute:
		start = self.token.span
		if self.token.kind not in ('identifier', 'keyword'):
			raise self.fail(f'expected an attribute name, found {self.describe(self.token)}', label='expected an attribute', code='E0204')
		name = self.advance().text
		if name == 'op' and self.at('.'):
			self.advance()
			pattern = ''
			depth = 0
			while self.token.kind != 'eof':
				if depth == 0 and self.at(',', ']'):
					break
				if self.at('['):
					depth += 1
				elif self.at(']'):
					depth -= 1
				pattern += self.advance().text
			if not pattern:
				raise self.fail('expected an operator pattern after `op.`', label='for example `_+_`', code='E0204')
			return ast.Attribute(self.span_from(start), 'op', pattern)
		value: object = None
		if self.accept('='):
			token = self.token
			if token.kind == 'string':
				value = token.value.decode('utf-8', 'replace')
			elif token.kind in ('integer', 'identifier', 'keyword', 'type'):
				value = token.value if token.kind == 'integer' else token.text
			else:
				raise self.fail(f'expected an attribute value, found {self.describe(token)}', label='expected a value', code='E0204')
			self.advance()
		return ast.Attribute(self.span_from(start), name, value)

	# --- declarations ----------------------------------------------------------

	def is_function_start(self) -> bool:
		"""at '(': whether a parameter list (not a parenthesized expression) follows."""
		after = self.peek(1)
		return after.is_(')') or after.is_('mut') or (after.kind == 'identifier' and self.peek(2).is_(':'))

	def declaration(self, top: bool) -> ast.Node:
		attributes = self.attributes()
		start = attributes[0].span if attributes else self.token.span
		public = False
		if self.at('pub'):
			pub = self.advance()
			public = True
			if not top:
				self.diagnostics.error('E0205', 'local declarations cannot be `pub`', pub.span, 'only module-level declarations are exported')
		name = self.expect_identifier('a declaration name')
		generics: list[ast.Generic] = []
		if self.at('<'):
			self.advance()
			while True:
				generic = self.expect_identifier('a type parameter name')
				generics.append(ast.Generic(generic.span, generic.text))
				if not self.accept(','):
					break
			self.expect_close_angle()
			if not top:
				self.diagnostics.error('E0205', 'generic declarations are only allowed at module level',
					Span(name.span.source, name.span.start, self.tokens[self.position - 1].span.end), 'declared inside a function')
		self.expect('::', f'after `{name.text}`')
		if self.at('{') and (self.peek().is_('let') or self.peek().is_('[')):
			declaration = self.struct(start, name, generics, attributes, public)
			if not top:
				self.diagnostics.error('E0205', 'struct declarations are only allowed at module level', name.span, 'declared inside a function')
			return declaration
		if self.at('{') and self.peek().is_('}'):
			brace = self.token.span
			self.advance()
			self.advance()
			raise self.fail('empty structs are not allowed', brace.join(self.tokens[self.position - 1].span),
				'a struct needs at least one `let` field', code='E0203')
		if self.at('(') and self.is_function_start():
			return self.function(start, name, generics, attributes, public)
		if generics:
			self.diagnostics.error('E0205', 'only functions and structs can be generic', name.span, 'a constant cannot have type parameters')
		value = self.expression()
		self.expect(';', 'after the constant')
		return ast.Constant(self.span_from(start), name.text, name.span, value, attributes, public)

	def parameters(self) -> list[ast.Param]:
		self.expect('(')
		parameters = []
		while not self.at(')'):
			start = self.token.span
			mutable = bool(self.accept('mut'))
			name = self.expect_identifier('a parameter name')
			self.expect(':', 'after the parameter name')
			if self.accept('...'):
				parameters.append(ast.Param(self.span_from(start), name.text, mutable, None, None, True))
				if not self.accept(','):
					break
				continue
			type_ = self.type()
			default = self.expression() if self.accept('=') else None
			parameters.append(ast.Param(self.span_from(start), name.text, mutable, type_, default))
			if not self.accept(','):
				break
		self.expect(')', 'to close the parameter list')
		return parameters

	def function(self, start: Span, name: Token, generics: list[ast.Generic], attributes: list[ast.Attribute], public: bool) -> ast.Function:
		parameters = self.parameters()
		result = self.type() if self.accept('->') else None
		body = None
		if not self.accept(';'):
			if not self.at('{'):
				raise self.fail(f'expected a function body or `;`, found {self.describe(self.token)}', label='expected `{`')
			body = self.block()
		return ast.Function(self.span_from(start), name.text, name.span, generics, parameters, result, body, attributes, public)

	def struct(self, start: Span, name: Token, generics: list[ast.Generic], attributes: list[ast.Attribute], public: bool) -> ast.Struct:
		self.expect('{')
		fields = []
		while not self.at('}') and self.token.kind != 'eof':
			field_attributes = self.attributes()
			field_start = field_attributes[0].span if field_attributes else self.token.span
			self.expect('let', 'to start a field')
			mutable = bool(self.accept('mut'))
			field_name = self.expect_identifier('a field name')
			self.expect(':', 'after the field name')
			type_ = self.type()
			default = self.expression() if self.accept('=') else None
			self.expect(';', 'after the field')
			fields.append(ast.Field(self.span_from(field_start), field_name.text, mutable, type_, default, field_attributes))
		self.expect('}', 'to close the struct')
		return ast.Struct(self.span_from(start), name.text, name.span, generics, fields, attributes, public)

	# --- types -----------------------------------------------------------------

	def type(self) -> ast.Node:
		token = self.token
		start = token.span
		if token.is_('['):
			self.advance()
			if self.accept(']'):
				return ast.TypeSlice(self.span_from(start), self.type())
			if self.token.kind == 'identifier' and self.token.text == '_' and self.peek().is_(']'):
				self.advance()
				self.advance()
				return ast.TypeArray(self.span_from(start), None, self.type())
			saved = self.composites
			self.composites = True
			length = self.expression()
			self.composites = saved
			self.expect(']', 'after the array length')
			return ast.TypeArray(self.span_from(start), length, self.type())
		if token.is_('&'):
			self.advance()
			return ast.TypeReference(self.span_from(start), self.type())
		if token.is_('('):
			self.advance()
			parameters = []
			while not self.at(')'):
				if self.at('mut') and self.peek().is_('&'):
					mut_start = self.advance().span
					self.advance()
					element = self.type()
					parameters.append(ast.TypeReference(self.span_from(mut_start), element, True))
				else:
					parameters.append(self.type())
				if not self.accept(','):
					break
			self.expect(')', 'to close the function type')
			result = self.type() if self.accept('->') else None
			return ast.TypeFunction(self.span_from(start), parameters, result)
		if token.kind == 'type':
			self.advance()
			return ast.TypeName(token.span, [token.text])
		if token.kind == 'identifier':
			path = [self.advance().text]
			while self.at('.') and self.peek().kind == 'identifier':
				self.advance()
				path.append(self.advance().text)
			arguments = []
			if self.at('<'):
				self.advance()
				while True:
					arguments.append(self.type())
					if not self.accept(','):
						break
				self.expect_close_angle()
			return ast.TypeName(self.span_from(start), path, arguments)
		raise self.fail(f'expected a type, found {self.describe(token)}', label='expected a type')

	# --- statements ------------------------------------------------------------

	def block(self) -> ast.Block:
		start = self.expect('{').span
		statements = []
		saved = self.composites
		self.composites = True
		while not self.at('}') and self.token.kind != 'eof':
			before = self.position
			try:
				statements.append(self.statement())
			except ParseError:
				self.recover((';', '}'))
				if self.position == before:
					self.advance()
		self.composites = saved
		self.expect('}', 'to close the block')
		return ast.Block(self.span_from(start), statements)

	def body(self, what: str) -> ast.Block:
		"""a block, or a single statement without braces."""
		if self.at('{'):
			return self.block()
		start = self.token.span
		if self.at('let') or (self.token.kind == 'identifier' and self.peek().is_('::')):
			raise self.fail(f'the body of {what} without braces cannot be a declaration', label='wrap it in `{ }`', code='E0206')
		statement = self.statement()
		return ast.Block(self.span_from(start), [statement], braceless=True)

	def condition(self) -> ast.Node:
		saved = self.composites
		self.composites = False
		try:
			return self.expression()
		finally:
			self.composites = saved

	def statement(self) -> ast.Node:
		attributes = self.attributes() if self.at('[') else []
		token = self.token
		start = attributes[0].span if attributes else token.span
		if token.is_('let'):
			self.advance()
			mutable = bool(self.accept('mut'))
			name = self.expect_identifier('a variable name')
			type_ = self.type() if self.accept(':') else None
			value = self.expression() if self.accept('=') else None
			self.expect(';', 'after the variable')
			return ast.Let(self.span_from(start), name.text, name.span, mutable, type_, value, attributes)
		if (token.kind == 'identifier' and (self.peek().is_('::') or self.peek().is_('<') and self.generic_declaration_ahead())) or token.is_('pub'):
			declaration = self.declaration_after_attributes(attributes)
			return ast.DeclarationStatement(declaration.span, declaration)
		if attributes and token.is_('if', 'while', 'for', 'return', 'break', 'continue', '{'):
			self.diagnostics.error('E0204', 'attributes can only be attached to declarations, `let` and expression statements',
				attributes[0].span, 'not allowed here')
		if token.is_('if'):
			return self.if_()
		if token.is_('while'):
			self.advance()
			condition = self.condition()
			return ast.While(self.span_from(start), condition, self.body('`while`'))
		if token.is_('for'):
			self.advance()
			name = self.expect_identifier('a loop variable')
			self.expect('in', 'after the loop variable')
			first = self.condition()
			if not self.at('..', '..='):
				raise self.fail(f'expected `..` or `..=`, found {self.describe(self.token)}', label='a `for` loop counts over a range')
			inclusive = self.advance().text == '..='
			last = self.condition()
			return ast.For(self.span_from(start), name.text, name.span, first, last, inclusive, self.body('`for`'))
		if token.is_('return'):
			self.advance()
			value = None if self.at(';') else self.expression()
			self.expect(';', 'after `return`')
			return ast.Return(self.span_from(start), value)
		if token.is_('break'):
			self.advance()
			self.expect(';', 'after `break`')
			return ast.Break(self.span_from(start))
		if token.is_('continue'):
			self.advance()
			self.expect(';', 'after `continue`')
			return ast.Continue(self.span_from(start))
		if token.is_('{'):
			return self.block()
		expression = self.expression()
		if self.token.kind == 'punctuator' and self.token.text in ASSIGNMENTS:
			operator = self.advance().text
			value = self.expression()
			self.expect(';', 'after the assignment')
			return ast.Assign(self.span_from(start), operator, expression, value, attributes)
		self.expect(';', 'after the expression')
		return ast.ExpressionStatement(self.span_from(start), expression, attributes)

	def generic_declaration_ahead(self) -> bool:
		"""at 'name <': whether this is 'name<T, U> ::' (a misplaced generic declaration)."""
		index = self.position + 2
		while index < len(self.tokens):
			token = self.tokens[index]
			if token.kind != 'identifier':
				return False
			following = self.tokens[index + 1] if index + 1 < len(self.tokens) else token
			if following.is_(','):
				index += 2
				continue
			return following.is_('>') and index + 2 < len(self.tokens) and self.tokens[index + 2].is_('::')
		return False

	def declaration_after_attributes(self, attributes: list[ast.Attribute]) -> ast.Node:
		declaration = self.declaration(top=False)
		declaration.attributes = attributes + declaration.attributes
		if attributes:
			declaration.span = attributes[0].span.join(declaration.span)
		return declaration

	def if_(self) -> ast.If:
		start = self.advance().span
		condition = self.condition()
		then = self.body('`if`')
		otherwise = None
		if self.accept('else'):
			if self.at('if'):
				otherwise = self.if_()
			else:
				otherwise = self.body('`else`')
		return ast.If(self.span_from(start), condition, then, otherwise)

	# --- expressions -----------------------------------------------------------

	def expression(self) -> ast.Node:
		return self.binary(1)

	def binary(self, minimum: int) -> ast.Node:
		left = self.unary()
		while True:
			token = self.token
			if token.kind != 'punctuator' or token.text not in PRECEDENCE:
				return left
			precedence = PRECEDENCE[token.text]
			if precedence < minimum:
				return left
			self.advance()
			right = self.binary(precedence + 1)
			node = ast.Binary(left.span.join(right.span), token.text, left, right, token.span)
			self.check_mix(node, left)
			self.check_mix(node, right)
			left = node

	def check_mix(self, parent: ast.Binary, child: ast.Node) -> None:
		if isinstance(child, ast.Binary) and not combinable(parent.operator, child.operator):
			if GROUP[parent.operator] == 'compare' and GROUP[child.operator] == 'compare':
				message = 'comparisons cannot be chained'
				help_text = 'compare separately and combine with `&&`, for example `a < b && b < c`'
			else:
				message = f'`{child.operator}` and `{parent.operator}` need parentheses to show their order'
				help_text = f'write `({child.span.text}) {parent.operator} ...` or group the other side'
			if not self.speculating:
				self.diagnostics.error('E0202', message, parent.operator_span, 'ambiguous without parentheses') \
					.label(child.operator_span).help(help_text)

	def unary(self) -> ast.Node:
		token = self.token
		if token.is_('-', '!', '~'):
			self.advance()
			operand = self.unary()
			return ast.Unary(token.span.join(operand.span), token.text, operand)
		if token.is_('&'):
			self.advance()
			operand = self.unary()
			return ast.Reference(token.span.join(operand.span), operand, False)
		if token.is_('&&'):
			raise self.fail('`&&` cannot start an expression', label='a reference to a reference is not allowed')
		if token.is_('mut') and self.peek().is_('&'):
			self.advance()
			self.advance()
			operand = self.unary()
			return ast.Reference(token.span.join(operand.span), operand, True)
		return self.postfix(self.primary())

	def arguments(self) -> list[ast.Node]:
		self.expect('(')
		saved = self.composites
		self.composites = True
		arguments = []
		while not self.at(')'):
			arguments.append(self.expression())
			if not self.accept(','):
				break
		self.composites = saved
		self.expect(')', 'to close the argument list')
		return arguments

	def try_type_arguments(self) -> list[ast.Node] | None:
		"""at '<': type arguments when they parse and a call or terminator follows."""
		saved_position, saved_tokens = self.position, list(self.tokens)
		self.speculating += 1
		try:
			self.advance()
			arguments = []
			while True:
				arguments.append(self.type())
				if not self.accept(','):
					break
			self.expect_close_angle()
			if self.at('(') or (self.token.kind == 'punctuator' and self.token.text in SPECIALIZE_FOLLOWERS) or self.token.kind == 'eof':
				return arguments
		except ParseError:
			pass
		finally:
			self.speculating -= 1
		self.position, self.tokens = saved_position, saved_tokens
		return None

	def postfix(self, node: ast.Node) -> ast.Node:
		while True:
			if self.at('('):
				arguments = self.arguments()
				node = ast.Call(self.span_from(node.span), node, None, arguments)
			elif self.at('<') and isinstance(node, (ast.Name, ast.Member)):
				type_arguments = self.try_type_arguments()
				if type_arguments is None:
					return node
				if self.at('('):
					arguments = self.arguments()
					node = ast.Call(self.span_from(node.span), node, type_arguments, arguments)
				else:
					node = ast.Specialize(self.span_from(node.span), node, type_arguments)
			elif self.at('['):
				self.advance()
				saved = self.composites
				self.composites = True
				if self.at('..', '..='):
					inclusive = self.advance().text == '..='
					high = None if self.at(']') else self.expression()
					node = ast.SliceOf(node.span, node, None, high, inclusive)
				else:
					index = self.expression()
					if self.at('..', '..='):
						inclusive = self.advance().text == '..='
						high = None if self.at(']') else self.expression()
						node = ast.SliceOf(node.span, node, index, high, inclusive)
					else:
						node = ast.Index(node.span, node, index)
				self.composites = saved
				self.expect(']', 'to close the index')
				node.span = self.span_from(node.target.span)
			elif self.at('.'):
				self.advance()
				name = self.expect_identifier('a field name')
				node = ast.Member(node.span.join(name.span), node, name.text, name.span)
			else:
				return node

	def composite(self, start: Span, type_: ast.Node | None) -> ast.Composite:
		self.expect('{')
		saved = self.composites
		self.composites = True
		entries = []
		named = None
		while not self.at('}') and self.token.kind != 'eof':
			entry_start = self.token.span
			if self.token.kind == 'identifier' and self.peek().is_(':'):
				name = self.advance().text
				self.advance()
				is_named = True
			else:
				name = None
				is_named = False
			if named is not None and named != is_named:
				self.diagnostics.error('E0201', 'a literal cannot mix named and positional entries', entry_start,
					'all entries must be `name: value`, or none')
			named = is_named
			value = self.expression()
			entries.append(ast.Entry(self.span_from(entry_start), name, value))
			if not self.accept(','):
				break
		self.composites = saved
		self.expect('}', 'to close the literal')
		return ast.Composite(self.span_from(start), type_, entries)

	def primary(self) -> ast.Node:
		token = self.token
		if token.kind == 'integer':
			self.advance()
			return ast.IntegerLiteral(token.span, token.value, token.suffix)
		if token.kind == 'float':
			self.advance()
			return ast.FloatLiteral(token.span, token.value, token.suffix)
		if token.kind == 'char':
			self.advance()
			return ast.CharLiteral(token.span, token.value)
		if token.kind == 'string':
			self.advance()
			return ast.StringLiteral(token.span, token.value, token.suffix == 'c')
		if token.kind == 'identifier':
			self.advance()
			return ast.Name(token.span, token.text)
		if token.is_('true', 'false'):
			self.advance()
			return ast.BoolLiteral(token.span, token.text == 'true')
		if token.is_('null'):
			self.advance()
			return ast.NullLiteral(token.span)
		if token.is_('sizeof'):
			self.advance()
			self.expect('(', 'after `sizeof`')
			type_ = self.type()
			self.expect(')', 'after the type')
			return ast.SizeOf(self.span_from(token.span), type_)
		if token.is_('lenof'):
			self.advance()
			self.expect('(', 'after `lenof`')
			operand = self.expression()
			self.expect(')', 'after the operand')
			return ast.LenOf(self.span_from(token.span), operand)
		if token.is_('assert'):
			self.advance()
			self.expect('(', 'after `assert`')
			condition = self.expression()
			message = self.expression() if self.accept(',') else None
			self.expect(')', 'to close `assert`')
			return ast.Assert(self.span_from(token.span), condition, message)
		if token.is_('{'):
			if not self.composites:
				raise self.fail('a literal is not allowed here without parentheses', label='`{` here would start the body',
					code='E0201')
			return self.composite(token.span, None)
		if token.is_('('):
			return self.parenthesized()
		if token.kind == 'type':
			raise self.fail(f'type `{token.text}` cannot be used as a value', label='expected an expression')
		raise self.fail(f'expected an expression, found {self.describe(token)}', label='expected an expression')

	def parenthesized(self) -> ast.Node:
		start = self.token.span
		if self.is_function_start():
			parameters = self.parameters()
			result = self.type() if self.accept('->') else None
			if not self.at('{'):
				raise self.fail(f'expected the body of the lambda, found {self.describe(self.token)}', label='expected `{`')
			return ast.Lambda(self.span_from(start), parameters, result, self.block())
		# '(int)x' is a cast; '(int){...}' a typed literal.
		if self.peek().kind == 'type' and self.peek(2).is_(')') and not (self.peek(3).is_('{') and self.composites):
			self.advance()
			type_token = self.advance()
			self.advance()
			operand = self.unary()
			return ast.Cast(self.span_from(start), ast.TypeName(type_token.span, [type_token.text]), operand)
		if self.composites:
			saved_position, saved_tokens = self.position, list(self.tokens)
			self.speculating += 1
			try:
				self.advance()
				type_ = self.type()
				self.expect(')')
				if self.at('{'):
					self.speculating -= 1
					try:
						return self.composite(start, type_)
					finally:
						self.speculating += 1
			except ParseError:
				pass
			finally:
				self.speculating -= 1
			self.position, self.tokens = saved_position, saved_tokens
		self.advance()
		saved = self.composites
		self.composites = True
		inner = self.expression()
		self.composites = saved
		self.expect(')', 'to close the parentheses')
		return ast.Paren(self.span_from(start), inner)


def parse(source: Source, diagnostics: Diagnostics) -> ast.Module:
	return Parser(lex(source, diagnostics), diagnostics).module()
