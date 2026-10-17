
"""diagnostics: errors and warnings with labelled source snippets, printed in
the style of rust/zig compilers.

every diagnostic has a code: 'E' (error) or 'W' (warning), two digits for the
area and two for the sequence. areas: 00 general, 01 lexing, 02 syntax,
03 modules, 04 declarations and attributes, 05 types, 06 command line,
07 constants and compile-time evaluation."""

from __future__ import annotations

import sys
from dataclasses import dataclass, field
from typing import TextIO

from .source import Span

# code -> short snake_case name; the table documents every diagnostic.
CODES = {
	# lexing
	'E0101': 'unknown_character',
	'E0102': 'unterminated_string',
	'E0103': 'unterminated_comment',
	'E0104': 'invalid_escape',
	'E0105': 'invalid_number',
	'E0106': 'invalid_character_literal',
	'E0107': 'invalid_string_suffix',
	'E0108': 'nul_in_c_string',
	# syntax
	'E0201': 'unexpected_token',
	'E0202': 'ambiguous_precedence',
	'E0203': 'empty_struct',
	'E0204': 'invalid_attribute',
	'E0205': 'misplaced_declaration',
	'E0206': 'invalid_braceless_body',
	# modules
	'E0301': 'module_not_found',
	'E0302': 'import_cycle',
	'E0303': 'not_public',
	'E0304': 'ambiguous_import',
	'E0305': 'unknown_module_member',
	'E0306': 'duplicate_import',
	# declarations and attributes
	'E0401': 'duplicate_declaration',
	'E0402': 'unknown_attribute',
	'E0403': 'attribute_not_applicable',
	'E0404': 'missing_body',
	'E0405': 'unexpected_body',
	'E0406': 'invalid_operator_pattern',
	'E0407': 'operator_arity',
	'E0408': 'derived_operator',
	'E0409': 'operator_signature',
	'E0410': 'duplicate_operator',
	'E0411': 'invalid_main',
	'E0412': 'default_parameter_order',
	'E0413': 'reference_not_allowed',
	'E0414': 'recursive_struct',
	'E0415': 'variadic_parameter',
	# types
	'E0501': 'undefined_name',
	'E0502': 'type_mismatch',
	'E0503': 'not_a_type',
	'E0504': 'not_callable',
	'E0505': 'argument_count',
	'E0506': 'cannot_infer',
	'E0507': 'invalid_operands',
	'E0508': 'not_assignable',
	'E0509': 'discarded_result',
	'E0510': 'invalid_cast',
	'E0511': 'literal_out_of_range',
	'E0512': 'no_such_field',
	'E0513': 'not_indexable',
	'E0514': 'missing_return',
	'E0515': 'outside_loop',
	'E0516': 'capture',
	'E0517': 'reference_argument',
	'E0518': 'reference_mutability',
	'E0519': 'misplaced_reference',
	'E0520': 'generic_argument_count',
	'E0521': 'not_generic',
	'E0522': 'private_field',
	'E0523': 'no_value',
	'E0524': 'invalid_return',
	'E0525': 'invalid_literal',
	'E0526': 'no_zero_value',
	'E0527': 'instantiation_depth',
	'E0528': 'overlapping_references',
	'E0529': 'not_a_value',
	'E0530': 'format_string',
	'W0501': 'ambiguous_literal',
	'W0502': 'uninitialized',
	'W0503': 'unused_mut',
	'W0504': 'field_not_initialized',
	# command line
	'E0601': 'file_not_found',
	'E0602': 'usage',
	# constants and compile-time evaluation
	'E0701': 'not_constant',
	'E0702': 'constant_cycle',
	'E0703': 'assertion_failed',
	'E0704': 'division_by_zero',
	'E0705': 'evaluation_limit',
	'E0706': 'out_of_bounds',
	'W0701': 'overflow',
}


@dataclass
class Label:
	span: Span
	message: str
	primary: bool


@dataclass
class Diagnostic:
	level: str        # 'error' or 'warning'
	code: str
	message: str
	labels: list[Label] = field(default_factory=list)
	notes: list[str] = field(default_factory=list)
	helps: list[str] = field(default_factory=list)

	def label(self, span: Span, message: str = '') -> Diagnostic:
		self.labels.append(Label(span, message, False))
		return self

	def note(self, message: str) -> Diagnostic:
		self.notes.append(message)
		return self

	def help(self, message: str) -> Diagnostic:
		self.helps.append(message)
		return self

	@property
	def span(self) -> Span | None:
		for label in self.labels:
			if label.primary:
				return label.span
		return None


class Diagnostics:
	def __init__(self) -> None:
		self.items: list[Diagnostic] = []
		self.seen: dict[tuple, Diagnostic] = {}

	def _add(self, level: str, code: str, message: str, span: Span | None, label: str) -> Diagnostic:
		assert code in CODES, code
		# generic bodies are checked once per instantiation; the same mistake
		# is reported only once.
		key = (code, message, span.source.path if span else None, span.start if span else None)
		if key in self.seen:
			return Diagnostic(level, code, message)
		diagnostic = Diagnostic(level, code, message)
		if span is not None:
			diagnostic.labels.append(Label(span, label, True))
		self.seen[key] = diagnostic
		self.items.append(diagnostic)
		return diagnostic

	def error(self, code: str, message: str, span: Span | None = None, label: str = '') -> Diagnostic:
		return self._add('error', code, message, span, label)

	def warning(self, code: str, message: str, span: Span | None = None, label: str = '') -> Diagnostic:
		return self._add('warning', code, message, span, label)

	@property
	def errors(self) -> list[Diagnostic]:
		return [item for item in self.items if item.level == 'error']

	@property
	def warnings(self) -> list[Diagnostic]:
		return [item for item in self.items if item.level == 'warning']

	def codes(self) -> list[str]:
		return [item.code for item in self.items]

	def render(self, stream: TextIO = sys.stderr, color: bool = False, short: bool = False) -> None:
		for diagnostic in self.items:
			if short:
				stream.write(render_short(diagnostic))
				continue
			stream.write(render(diagnostic, color))
			stream.write('\n')


def render_short(diagnostic: Diagnostic) -> str:
	"""one line: 'path:line:column: error[E0502]: message'."""
	span = diagnostic.span
	where = ''
	if span is not None:
		line, column = span.source.location(span.start)
		where = f'{span.source.path}:{line}:{column}: '
	return f'{where}{diagnostic.level}[{diagnostic.code}]: {diagnostic.message}\n'


ESCAPE = chr(27)
TAB_WIDTH = 4


def render(diagnostic: Diagnostic, color: bool = False) -> str:
	"""the diagnostic as text: header, snippets with underlines, notes."""
	def paint(text: str, style: str) -> str:
		return f'{ESCAPE}[{style}m{text}{ESCAPE}[0m' if color else text

	level_style = '1;31' if diagnostic.level == 'error' else '1;33'
	bar = paint('|', '1;34')
	lines = [paint(f'{diagnostic.level}[{diagnostic.code}]', level_style) + paint(f': {diagnostic.message}', '1')]
	labels = sorted(diagnostic.labels, key=lambda label: (label.span.source.path, label.span.start))
	if not labels:
		lines += [f'  = note: {note}' for note in diagnostic.notes]
		lines += [f'  = help: {text}' for text in diagnostic.helps]
		return '\n'.join(lines) + '\n'
	width = max(len(str(label.span.source.location(label.span.start)[0])) for label in labels)
	pad = ' ' * width
	anchor = diagnostic.span or labels[0].span
	line, column = anchor.source.location(anchor.start)
	lines.append(f'{pad}{paint("-->", "1;34")} {anchor.source.path}:{line}:{column}')
	lines.append(f'{pad} {bar}')
	previous = anchor.source.path
	previous_line = None
	for label in labels:
		source = label.span.source
		if source.path != previous:
			line, column = source.location(label.span.start)
			lines.append(f'{pad}{paint(":::", "1;34")} {source.path}:{line}:{column}')
			previous = source.path
			previous_line = None
		first_line, first_column = source.location(label.span.start)
		last_line, last_column = source.location(max(label.span.start, label.span.end - 1))
		raw = source.line_text(first_line)
		shown = raw.expandtabs(TAB_WIDTH)
		if (source.path, first_line) != previous_line:
			if previous_line is not None and first_line > previous_line[1] + 1:
				lines.append(f'{pad}{paint("...", "1;34")}')
			lines.append(f'{str(first_line).rjust(width)} {bar} {shown}')
			previous_line = (source.path, first_line)
		prefix = raw[:first_column - 1].expandtabs(TAB_WIDTH)
		if last_line == first_line:
			length = len(raw[:last_column].expandtabs(TAB_WIDTH)) - len(prefix)
		else:
			length = len(shown) - len(prefix)
		marker = '^' if label.primary else '-'
		style = level_style if label.primary else '1;34'
		text = marker * max(1, length) + (f' {label.message}' if label.message else '')
		lines.append(f'{pad} {bar} {" " * len(prefix)}{paint(text, style)}')
	if diagnostic.notes or diagnostic.helps:
		lines.append(f'{pad} {bar}')
	lines += [f'{pad} {paint("=", "1;34")} {paint("note", "1")}: {note}' for note in diagnostic.notes]
	lines += [f'{pad} {paint("=", "1;34")} {paint("help", "1")}: {text}' for text in diagnostic.helps]
	return '\n'.join(lines) + '\n'
