
"""every example program checks without errors or warnings."""

import contextlib
import io
import os
import unittest

from support import ROOT, STDLIB, TestCase  # noqa: I001 (sets up the import path)
from mc.checker import check
from mc.cli import main, program_text
from mc.diag import Diagnostics

EXAMPLES = os.path.join(ROOT, 'examples')


class ExampleTests(TestCase):
	def test_examples_are_clean(self):
		entries = sorted(os.path.join(EXAMPLES, name, 'main.mc') for name in os.listdir(EXAMPLES))
		self.assertTrue(entries)
		for entry in entries:
			with self.subTest(example=entry):
				diagnostics = Diagnostics()
				program = check(entry, diagnostics, stdlib=STDLIB)
				self.assertEqual(diagnostics.codes(), [], self.explain(diagnostics))
				self.assertIn('module main', program_text(program))

	def test_tour_constants(self):
		diagnostics = Diagnostics()
		program = check(os.path.join(EXAMPLES, 'tour', 'main.mc'), diagnostics, stdlib=STDLIB)
		values = {constant.name: constant.value for constant in program.constants}
		self.assertEqual(values['FACTORIAL_10'], 3628800)
		self.assertEqual(values['SQUARES'][:5], [0, 1, 4, 9, 16])

	def test_tour_runs(self):
		from mc.compiler import compile_program
		from mc.vm import Machine
		diagnostics = Diagnostics()
		program = check(os.path.join(EXAMPLES, 'tour', 'main.mc'), diagnostics, stdlib=STDLIB)
		out = io.StringIO()
		result = Machine(compile_program(program), out).run()
		self.assertEqual(out.getvalue(), '10! = 3628800\nsquares: {0, 1, 4, 9, 16, 25, 36, 49, 64, 81, 100, 121, 144, 169, 196, 225}\n'
			'pair: {first: 2, second: 1.5}\nposition {x: 6.0, y: 4.0} becomes {x: 0.11538462, y: 0.07692308}\n'
			'{heap}: {100, 100, 100}, ...\nhello, 6 bytes\n')
		self.assertEqual(result, 12)

	def test_command_line(self):
		entry = os.path.join(EXAMPLES, 'vector', 'main.mc')
		with contextlib.redirect_stderr(io.StringIO()), contextlib.redirect_stdout(io.StringIO()):
			self.assertEqual(main(['check', entry]), 0)
			self.assertEqual(main(['mono', entry]), 0)
			self.assertEqual(main(['nonsense', entry]), 2)
			self.assertEqual(main(['check', os.path.join(EXAMPLES, 'missing.mc')]), 1)
			self.assertEqual(main(['run', os.path.join(EXAMPLES, 'tour', 'main.mc')]), 12)
			self.assertEqual(main(['dis', entry]), 0)


if __name__ == '__main__':
	unittest.main()
