
"""lexer tests."""

import unittest

from support import TestCase, lex_text


class LexerTests(TestCase):
	def kinds(self, text):
		tokens, diagnostics = lex_text(text)
		self.assertEqual(diagnostics.codes(), [], self.explain(diagnostics))
		return [(token.kind, token.text) for token in tokens[:-1]]

	def values(self, text):
		tokens, diagnostics = lex_text(text)
		self.assertEqual(diagnostics.codes(), [], self.explain(diagnostics))
		return [(token.value, token.suffix) for token in tokens[:-1]]

	def codes(self, text):
		return lex_text(text)[1].codes()

	def test_comments(self):
		self.assertEqual(self.kinds('a # one\nb // two\nc /* x /* nested */ y */ d'),
			[('identifier', 'a'), ('identifier', 'b'), ('identifier', 'c'), ('identifier', 'd')])
		self.assertEqual(self.codes('a /* /* */'), ['E0103'])

	def test_numbers(self):
		self.assertEqual(self.values('10 0x1F 0o17 0b101 1_000 2.5 1e3 1.5e-2'),
			[(10, None), (31, None), (15, None), (5, None), (1000, None), (2.5, None), (1000.0, None), (0.015, None)])
		self.assertEqual(self.values('255u8 -5 1.5f32 2float 7uint'),
			[(255, 'u8'), (None, None), (5, None), (1.5, 'f32'), (2.0, 'float'), (7, 'uint')])

	def test_hex_takes_only_integer_suffixes(self):
		self.assertEqual(self.values('0x1f32 0xFFu8'), [(0x1F32, None), (255, 'u8')])

	def test_invalid_numbers(self):
		for text in ('1_', '1__0', '5z', '1.5u8', '0x10float', '0b'):
			with self.subTest(text=text):
				self.assertEqual(self.codes(text), ['E0105'])

	def test_ranges_are_not_floats(self):
		self.assertEqual(self.kinds('0..5 0..=5'),
			[('integer', '0'), ('punctuator', '..'), ('integer', '5'), ('integer', '0'), ('punctuator', '..='), ('integer', '5')])

	def test_strings_and_characters(self):
		tokens, diagnostics = lex_text('"a\\n\\x41" "hi"c \'z\' \'\\t\'')
		self.assertEqual(diagnostics.codes(), [])
		self.assertEqual([token.value for token in tokens[:-1]], [b'a\nA', b'hi\x00', 122, 9])
		self.assertEqual(self.codes('"abc'), ['E0102'])
		self.assertEqual(self.codes('"a\\q"'), ['E0104'])
		self.assertEqual(self.codes('"a"x'), ['E0107'])
		self.assertEqual(self.codes('"a\\0b"c'), ['E0108'])
		self.assertEqual(self.codes("'ab'"), ['E0106'])

	def test_keywords_and_types(self):
		self.assertEqual(self.kinds('let mut x: u8 pub'),
			[('keyword', 'let'), ('keyword', 'mut'), ('identifier', 'x'), ('punctuator', ':'), ('type', 'u8'), ('keyword', 'pub')])

	def test_unknown_character(self):
		self.assertEqual(self.codes('a @ b'), ['E0101'])


if __name__ == '__main__':
	unittest.main()
