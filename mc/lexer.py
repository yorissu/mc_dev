
"""lexer: source text -> tokens.

comments: '#' and '//' to the end of the line, '/* */' blocks that nest.
numbers: decimal, '0x' hexadecimal, '0o' octal, '0b' binary, decimal floats
with an optional exponent, '_' separators between digits, and an optional
type suffix ('255u8', '1.5f32'). hexadecimal numbers take integer suffixes
only, because 'f' is a hexadecimal digit. characters: 'a' (a u8). strings:
"text", or "text"c for a NUL-terminated string."""

from __future__ import annotations

from dataclasses import dataclass

from .diag import Diagnostics
from .source import Source, Span

KEYWORDS = {
	'pub', 'let', 'mut', 'if', 'else', 'while', 'for', 'in', 'return', 'break',
	'continue', 'import', 'from', 'as', 'true', 'false', 'null', 'sizeof',
	'lenof', 'assert',
}

INTEGER_TYPES = {'s8', 's16', 's32', 's64', 'u8', 'u16', 'u32', 'u64', 'int', 'uint'}
FLOAT_TYPES = {'f32', 'f64', 'float'}
SCALAR_TYPES = INTEGER_TYPES | FLOAT_TYPES | {'bool'}

# longest first, so that '..=' wins over '..' and '<<=' over '<<'.
PUNCTUATORS = sorted([
	'::', '->', '...', '..=', '..', '<<=', '>>=', '<<', '>>', '<=', '>=', '==', '!=',
	'&&', '||', '+=', '-=', '*=', '/=', '%=', '&=', '|=', '^=',
	'+', '-', '*', '/', '%', '&', '|', '^', '~', '!', '<', '>', '=', '.', ',',
	';', ':', '(', ')', '[', ']', '{', '}',
], key=len, reverse=True)

ESCAPES = {'n': 10, 't': 9, 'r': 13, '0': 0, '\\': 92, '"': 34, "'": 39}


@dataclass
class Token:
	kind: str          # 'identifier', 'keyword', 'type', 'integer', 'float', 'char', 'string', 'punctuator', 'eof'
	text: str
	span: Span
	value: object = None
	suffix: str | None = None

	def is_(self, *texts: str) -> bool:
		"""whether the token is one of the keywords or punctuators 'texts'."""
		return self.kind in ('keyword', 'punctuator') and self.text in texts

	def __repr__(self) -> str:
		return f'Token({self.kind}, {self.text!r})'


def is_identifier_start(character: str) -> bool:
	return character.isalpha() or character == '_'


def is_identifier_part(character: str) -> bool:
	return character.isalnum() or character == '_'


class Lexer:
	def __init__(self, source: Source, diagnostics: Diagnostics) -> None:
		self.source = source
		self.text = source.text
		self.diagnostics = diagnostics
		self.position = 0
		self.tokens: list[Token] = []

	def span(self, start: int, end: int | None = None) -> Span:
		return Span(self.source, start, self.position if end is None else end)

	def peek(self, offset: int = 0) -> str:
		index = self.position + offset
		return self.text[index] if index < len(self.text) else ''

	def run(self) -> list[Token]:
		while True:
			self.skip_trivia()
			if self.position >= len(self.text):
				self.tokens.append(Token('eof', '', self.span(self.position)))
				return self.tokens
			self.token()

	def skip_trivia(self) -> None:
		while self.position < len(self.text):
			character = self.peek()
			if character in ' \t\r\n':
				self.position += 1
			elif character == '#' or (character == '/' and self.peek(1) == '/'):
				while self.position < len(self.text) and self.peek() != '\n':
					self.position += 1
			elif character == '/' and self.peek(1) == '*':
				self.block_comment()
			else:
				return

	def block_comment(self) -> None:
		starts = [self.position]
		self.position += 2
		while starts:
			if self.position >= len(self.text):
				self.diagnostics.error('E0103', 'unterminated block comment', self.span(starts[-1], starts[-1] + 2),
					'this comment is never closed').help('block comments nest: every `/*` needs its own `*/`')
				return
			if self.peek() == '/' and self.peek(1) == '*':
				starts.append(self.position)
				self.position += 2
			elif self.peek() == '*' and self.peek(1) == '/':
				starts.pop()
				self.position += 2
			else:
				self.position += 1

	def token(self) -> None:
		start = self.position
		character = self.peek()
		if character.isdigit():
			self.number()
		elif is_identifier_start(character):
			while is_identifier_part(self.peek()):
				self.position += 1
			text = self.text[start:self.position]
			kind = 'keyword' if text in KEYWORDS else 'type' if text in SCALAR_TYPES else 'identifier'
			self.tokens.append(Token(kind, text, self.span(start)))
		elif character == '"':
			self.string()
		elif character == "'":
			self.character()
		else:
			for punctuator in PUNCTUATORS:
				if self.text.startswith(punctuator, self.position):
					self.position += len(punctuator)
					self.tokens.append(Token('punctuator', punctuator, self.span(start)))
					return
			self.position += 1
			self.diagnostics.error('E0101', f'unknown character {character!r}', self.span(start), 'not part of the language')

	def digits(self, valid: str) -> str:
		"""digits of 'valid' and '_' separators, as written."""
		start = self.position
		while self.peek() and (self.peek().lower() in valid or self.peek() == '_'):
			self.position += 1
		return self.text[start:self.position]

	def number(self) -> None:
		start = self.position
		base = 10
		is_float = False
		prefix = self.text[start:start + 2].lower()
		if prefix in ('0x', '0o', '0b'):
			base = {'0x': 16, '0o': 8, '0b': 2}[prefix]
			self.position += 2
			body = self.digits({16: '0123456789abcdef', 8: '01234567', 2: '01'}[base])
		else:
			body = self.digits('0123456789')
			if self.peek() == '.' and self.peek(1).isdigit():
				self.position += 1
				body += '.' + self.digits('0123456789')
				is_float = True
			if self.peek() in ('e', 'E') and (self.peek(1).isdigit() or (self.peek(1) in ('+', '-') and self.peek(2).isdigit())):
				exponent_start = self.position
				self.position += 2 if self.peek(1) in ('+', '-') else 1
				self.digits('0123456789')
				body += self.text[exponent_start:self.position]
				is_float = True
		suffix_start = self.position
		while is_identifier_part(self.peek()):
			self.position += 1
		suffix = self.text[suffix_start:self.position] or None
		span = self.span(start)
		text = self.text[start:self.position]
		cleaned = body.replace('_', '')
		problem = None
		if not cleaned:
			problem = 'a number needs at least one digit'
		elif body.startswith('_') or body.endswith('_') or '_.' in body or '._' in body or '__' in body:
			problem = '`_` may only separate digits'
		elif suffix is not None and suffix not in INTEGER_TYPES | FLOAT_TYPES:
			problem = f'unknown number suffix `{suffix}`'
		elif suffix in FLOAT_TYPES and base != 10:
			problem = 'only decimal numbers can have a float suffix'
		elif is_float and suffix in INTEGER_TYPES:
			problem = f'a float cannot have the integer suffix `{suffix}`'
		if problem:
			self.diagnostics.error('E0105', 'invalid number literal', span, problem)
			self.tokens.append(Token('integer', text, span, 0, None))
		elif is_float or suffix in FLOAT_TYPES:
			self.tokens.append(Token('float', text, span, float(cleaned), suffix))
		else:
			self.tokens.append(Token('integer', text, span, int(cleaned, base), suffix))

	def escape(self) -> int | None:
		"""reads one escape after its backslash; returns the byte value."""
		start = self.position - 1
		character = self.peek()
		if character in ESCAPES:
			self.position += 1
			return ESCAPES[character]
		if character == 'x':
			digits = self.text[self.position + 1:self.position + 3]
			if len(digits) == 2 and all(digit in '0123456789abcdefABCDEF' for digit in digits):
				self.position += 3
				return int(digits, 16)
		if character not in ('', '\n'):
			self.position += 1
		self.diagnostics.error('E0104', 'invalid escape sequence', self.span(start), 'unknown escape') \
			.help('valid escapes: \\n \\t \\r \\0 \\\\ \\" \\\' \\xHH')
		return None

	def string(self) -> None:
		start = self.position
		self.position += 1
		data = bytearray()
		while True:
			character = self.peek()
			if character in ('', '\n'):
				self.diagnostics.error('E0102', 'unterminated string literal', self.span(start, start + 1), 'this string is never closed')
				break
			self.position += 1
			if character == '"':
				break
			if character == '\\':
				value = self.escape()
				if value is not None:
					data.append(value)
			else:
				data += character.encode('utf-8')
		suffix_start = self.position
		while is_identifier_part(self.peek()):
			self.position += 1
		suffix = self.text[suffix_start:self.position] or None
		if suffix not in (None, 'c'):
			self.diagnostics.error('E0107', f'unknown string suffix `{suffix}`', self.span(suffix_start), 'only `c` is allowed') \
				.help('`"text"c` is a NUL-terminated string')
			suffix = None
		if suffix == 'c':
			if 0 in data:
				self.diagnostics.error('E0108', 'C string contains a NUL byte', self.span(start), 'C would stop reading at the first NUL')
			data.append(0)
		self.tokens.append(Token('string', self.text[start:self.position], self.span(start), bytes(data), suffix))

	def character(self) -> None:
		start = self.position
		self.position += 1
		value: int | None = None
		character = self.peek()
		if character == '\\':
			self.position += 1
			value = self.escape()
		elif character not in ("'", '\n', ''):
			self.position += 1
			encoded = character.encode('utf-8')
			if len(encoded) == 1:
				value = encoded[0]
			else:
				self.diagnostics.error('E0106', 'character literal is not a single byte', self.span(start), 'character literals are u8 values')
				value = 0
		if self.peek() == "'" and value is not None:
			self.position += 1
		else:
			while self.peek() not in ("'", '\n', ''):
				self.position += 1
			if self.peek() == "'":
				self.position += 1
			self.diagnostics.error('E0106', 'invalid character literal', self.span(start), 'expected exactly one character')
			value = 0
		self.tokens.append(Token('char', self.text[start:self.position], self.span(start), value))


def lex(source: Source, diagnostics: Diagnostics) -> list[Token]:
	return Lexer(source, diagnostics).run()
