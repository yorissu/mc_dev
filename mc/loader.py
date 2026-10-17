
"""module loading. a module is one '.mc' file; 'import a.b' loads 'a/b.mc'
from the first search root that has it: the directory of the entry file,
then every '-I' directory in order, then the standard library. import cycles
are errors."""

from __future__ import annotations

import os
from dataclasses import dataclass, field

from . import ast
from .diag import Diagnostics
from .parser import parse
from .source import Source


@dataclass(eq=False)
class ModuleInfo:
	name: str                    # dotted name, as first imported ('main' for the entry)
	path: str
	source: Source
	tree: ast.Module
	imports: list[tuple[ast.Node, ModuleInfo]] = field(default_factory=list)
	symbols: dict = field(default_factory=dict)            # own top-level declarations
	imported: dict = field(default_factory=dict)           # name -> symbol (from-imports)
	star: list[ModuleInfo] = field(default_factory=list)   # 'from m import *'
	qualifiers: dict = field(default_factory=dict)         # dotted alias -> module
	operators: dict = field(default_factory=dict)          # pattern -> [function info]

	def __repr__(self) -> str:
		return f'ModuleInfo({self.name})'


class Loader:
	def __init__(self, diagnostics: Diagnostics, include: list[str] | None = None, stdlib: str | None = None) -> None:
		self.diagnostics = diagnostics
		self.include = list(include or [])
		self.stdlib = stdlib
		self.modules: dict[str, ModuleInfo] = {}     # absolute path -> module
		self.order: list[ModuleInfo] = []            # dependencies before dependents
		self.roots: list[str] = []
		self.overrides: dict[str, str] = {}          # path -> text (tests use in-memory files)

	def read(self, path: str) -> str | None:
		if path in self.overrides:
			return self.overrides[path]
		try:
			with open(path, encoding='utf-8') as file:
				return file.read()
		except OSError:
			return None

	def exists(self, path: str) -> bool:
		return path in self.overrides or os.path.isfile(path)

	def load_entry(self, path: str) -> ModuleInfo | None:
		text = self.read(path)
		if text is None:
			self.diagnostics.error('E0601', f'cannot read `{path}`')
			return None
		directory = os.path.dirname(os.path.abspath(path)) if path not in self.overrides else os.path.dirname(path)
		self.roots = [directory] + self.include + ([self.stdlib] if self.stdlib else [])
		key = self.key(path)
		return self.load(key, path, 'main', text, [])

	def key(self, path: str) -> str:
		return path if path in self.overrides else os.path.normcase(os.path.abspath(path))

	def find(self, parts: list[str]) -> str | None:
		for root in self.roots:
			candidate = os.path.join(root, *parts) + '.mc'
			if self.exists(candidate):
				return candidate
		return None

	def load(self, key: str, path: str, name: str, text: str, stack: list[ModuleInfo]) -> ModuleInfo:
		source = Source(path, text)
		tree = parse(source, self.diagnostics)
		module = ModuleInfo(name, path, source, tree)
		self.modules[key] = module
		stack.append(module)
		for node in tree.imports:
			parts = node.path
			found = self.find(parts)
			dotted = '.'.join(parts)
			if found is None:
				searched = ', '.join(os.path.join(root, *parts) + '.mc' for root in self.roots)
				self.diagnostics.error('E0301', f'module `{dotted}` not found', node.span, 'no such module') \
					.note(f'searched: {searched}')
				continue
			found_key = self.key(found)
			target = self.modules.get(found_key)
			if target is not None and target in stack:
				chain = ' -> '.join(item.name for item in stack[stack.index(target):]) + f' -> {target.name}'
				self.diagnostics.error('E0302', f'import cycle: `{module.name}` imports `{dotted}`', node.span,
					'this import closes a cycle').note(f'cycle: {chain}').help('import cycles are not allowed; move shared declarations into their own module')
				continue
			if target is None:
				target_text = self.read(found)
				if target_text is None:
					self.diagnostics.error('E0301', f'cannot read module `{dotted}`', node.span)
					continue
				target = self.load(found_key, found, dotted, target_text, stack)
			module.imports.append((node, target))
		stack.pop()
		self.order.append(module)
		return module
