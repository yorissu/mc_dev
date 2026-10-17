
"""shared helpers: compile in-memory programs and compare diagnostic codes."""

from __future__ import annotations

import os
import sys
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if ROOT not in sys.path:
	sys.path.insert(0, ROOT)

from mc.checker import Program, check  # noqa: E402
from mc.diag import Diagnostics, render  # noqa: E402
from mc.lexer import lex  # noqa: E402
from mc.parser import parse  # noqa: E402
from mc.source import Source  # noqa: E402

STDLIB = os.path.join(ROOT, 'stdlib')
BASE = os.path.join(ROOT, 'tests', 'virtual')


def compile_program(main: str, modules: dict[str, str] | None = None, add_main: bool = True) -> tuple[Program, Diagnostics]:
	"""checks 'main' as the entry module; 'modules' maps dotted names to sources."""
	if add_main and 'main ::' not in main:
		main += '\npub main :: () {}\n'
	files = {os.path.join(BASE, 'main.mc'): main}
	for name, text in (modules or {}).items():
		files[os.path.join(BASE, *name.split('.')) + '.mc'] = text
	diagnostics = Diagnostics()
	program = check(os.path.join(BASE, 'main.mc'), diagnostics, stdlib=STDLIB, files=files)
	return program, diagnostics


def parse_text(text: str) -> tuple[object, Diagnostics]:
	diagnostics = Diagnostics()
	return parse(Source('test.mc', text), diagnostics), diagnostics


def lex_text(text: str) -> tuple[list, Diagnostics]:
	diagnostics = Diagnostics()
	return lex(Source('test.mc', text), diagnostics), diagnostics


class TestCase(unittest.TestCase):
	maxDiff = None

	def explain(self, diagnostics: Diagnostics) -> str:
		return '\n' + ''.join(render(item) for item in diagnostics.items)

	def assertCodes(self, source: str, codes: list[str], modules: dict[str, str] | None = None) -> Program:
		program, diagnostics = compile_program(source, modules)
		self.assertEqual(sorted(diagnostics.codes()), sorted(codes), self.explain(diagnostics))
		return program

	def assertClean(self, source: str, modules: dict[str, str] | None = None) -> Program:
		return self.assertCodes(source, [], modules)

	def assertParseCodes(self, source: str, codes: list[str]) -> object:
		tree, diagnostics = parse_text(source)
		self.assertEqual(sorted(diagnostics.codes()), sorted(codes), self.explain(diagnostics))
		return tree
