
"""prints syntax trees as indented text: one node per line, scalar fields
inline, child nodes indented below with their field name. checked trees also
show the type of every expression."""

from __future__ import annotations

from dataclasses import fields

from . import ast
from .source import Span


def scalar(value: object) -> str:
	if isinstance(value, bytes):
		return repr(value)[1:]
	if isinstance(value, str):
		return value if value and value.replace('_', '').isalnum() else repr(value)
	if isinstance(value, list):
		return '[' + ', '.join(scalar(item) for item in value) + ']'
	return repr(value)


def dump(node: ast.Node, typed: bool = False) -> str:
	lines: list[str] = []
	write(node, 0, '', lines, typed)
	return '\n'.join(lines) + '\n'


def write(node: ast.Node, depth: int, label: str, lines: list[str], typed: bool) -> None:
	parts = [type(node).__name__]
	children: list[tuple[str, object]] = []
	for item in fields(node):
		if item.name == 'span':
			continue
		value = getattr(node, item.name)
		if isinstance(value, Span):
			continue
		if isinstance(value, ast.Node):
			children.append((item.name, value))
		elif isinstance(value, list) and value and all(isinstance(element, ast.Node) for element in value):
			children.append((item.name, value))
		elif isinstance(value, list) and not value:
			continue
		elif value is None or value is False:
			continue
		elif value is True:
			parts.append(item.name)
		else:
			parts.append(f'{item.name}={scalar(value)}')
	if typed and getattr(node, 'type_', None) is not None:
		parts.append(f': {node.type_}')
	if typed and getattr(node, 'instance', None) is not None:
		parts.append(f'-> {node.instance}')
	indent = '  ' * depth
	lines.append(f'{indent}{label + ": " if label else ""}{" ".join(parts)}')
	for name, value in children:
		if isinstance(value, list):
			lines.append(f'{indent}  {name}:')
			for element in value:
				write(element, depth + 2, '', lines, typed)
		else:
			write(value, depth + 1, name, lines, typed)
