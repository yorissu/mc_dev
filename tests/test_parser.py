
"""parser tests: declarations, statements, strict operator precedence."""

import unittest

from support import TestCase  # noqa: I001 (sets up the import path)
from mc import ast


def first_expression(tree):
	"""the expression of the first statement of the first function."""
	statement = tree.declarations[0].body.statements[0]
	return statement.expression if isinstance(statement, ast.ExpressionStatement) else statement.value


class ParserTests(TestCase):
	def expression(self, text, codes=()):
		tree = self.assertParseCodes(f'f :: () {{ _ = {text}; }}', list(codes))
		return first_expression(tree)

	def test_declaration_kinds(self):
		tree = self.assertParseCodes('''
			S :: { let a: int; let mut b: []u8 = null; }
			f :: (x: int, mut y: &S) -> int { return x; }
			g :: () {}
			C :: (1 + 2) * 3;
			[builtin] h<T> :: (n: uint) -> []T;
		''', [])
		kinds = [type(node).__name__ for node in tree.declarations]
		self.assertEqual(kinds, ['Struct', 'Function', 'Function', 'Constant', 'Function'])
		self.assertTrue(tree.declarations[1].parameters[1].mutable)
		self.assertIsNone(tree.declarations[4].body)

	def test_empty_struct(self):
		self.assertParseCodes('S :: { }', ['E0203'])

	def test_local_struct_and_generic(self):
		self.assertParseCodes('f :: () { S :: { let a: int; } }', ['E0205'])
		self.assertParseCodes('f :: () { g<T> :: (x: T) {} }', ['E0205'])

	def test_imports(self):
		tree = self.assertParseCodes('import a.b as c\nfrom x import y, z as w\nfrom q import *', [])
		self.assertEqual(tree.imports[0].path, ['a', 'b'])
		self.assertEqual(tree.imports[0].alias, 'c')
		self.assertEqual([(name.name, name.alias) for name in tree.imports[1].names], [('y', None), ('z', 'w')])
		self.assertTrue(tree.imports[2].star)

	def test_precedence_allowed(self):
		node = self.expression('a + b * c')
		self.assertEqual((node.operator, node.right.operator), ('+', '*'))
		node = self.expression('(a + b < c && d == e) || f')
		self.assertEqual(node.operator, '||')
		self.expression('a + b - c')
		self.expression('!a && b')

	def test_precedence_required(self):
		for text in ('a & b == c', 'a & b + c', 'a << b + c', 'a && b || c', 'a & b | c', 'a < b < c', 'a == b == c'):
			with self.subTest(text=text):
				self.expression(text, ['E0202'])
		self.expression('(a & b) == c')
		self.expression('(a && b) || c')

	def test_generic_call_versus_comparison(self):
		node = self.expression('f<int>(x)')
		self.assertIsInstance(node, ast.Call)
		self.assertEqual(node.type_arguments[0].path, ['int'])
		node = self.expression('a < b')
		self.assertIsInstance(node, ast.Binary)
		node = self.expression('m.f<Vector<int>>(x)')
		self.assertIsInstance(node, ast.Call)
		self.assertEqual(node.type_arguments[0].arguments[0].path, ['int'])
		node = self.expression('i < n && x > 0')
		self.assertEqual(node.operator, '&&')
		self.assertIsInstance(self.expression('add<int>'), ast.Specialize)

	def test_casts_literals_and_lambdas(self):
		self.assertIsInstance(self.expression('(f32)x'), ast.Cast)
		node = self.expression('(Point){ x: 1, y: 2, }')
		self.assertIsInstance(node, ast.Composite)
		self.assertEqual(node.type.path, ['Point'])
		self.assertIsInstance(self.expression('(x)'), ast.Paren)
		node = self.expression('(a: int, mut b: &int) -> int { return a; }')
		self.assertIsInstance(node, ast.Lambda)
		node = self.expression('() { }()')
		self.assertIsInstance(node, ast.Call)
		self.assertIsInstance(node.callee, ast.Lambda)
		self.expression('{ 1, 2, }')
		self.expression('{ x: 1, 2 }', ['E0201'])

	def test_references_and_slices(self):
		tree = self.assertParseCodes('f :: () { g(&x, mut &y.z, &{ 1, 2 }); _ = a[1..3]; _ = a[..]; _ = a[2..=4]; }', [])
		call = tree.declarations[0].body.statements[0].expression
		self.assertEqual([argument.mutable for argument in call.arguments], [False, True, False])
		slices = [statement.value for statement in tree.declarations[0].body.statements[1:]]
		self.assertEqual([(node.low is None, node.high is None, node.inclusive) for node in slices],
			[(False, False, False), (True, True, False), (False, False, True)])

	def test_statements(self):
		tree = self.assertParseCodes('''
			f :: () {
				let mut x: int = 1;
				[uninit] let buffer: [64]u8;
				if x > 0 x += 1; else if x < 0 { x = 0; } else return;
				while x < 10 x += 1;
				for i in 10..0 { continue; }
				for i in 0..=n break;
				{ let y = 2; }
				LOCAL :: 5;
				helper :: (a: int) -> int { return a; }
			}
		''', [])
		kinds = [type(node).__name__ for node in tree.declarations[0].body.statements]
		self.assertEqual(kinds, ['Let', 'Let', 'If', 'While', 'For', 'For', 'Block', 'DeclarationStatement', 'DeclarationStatement'])
		self.assertTrue(tree.declarations[0].body.statements[2].then.braceless)
		self.assertTrue(tree.declarations[0].body.statements[5].inclusive)

	def test_braceless_condition_ends_greedily(self):
		self.assertParseCodes('f :: () { if a (b)(); }', ['E0201'])
		self.assertParseCodes('f :: () { if ready let x = 1; }', ['E0206'])

	def test_condition_literal_needs_parentheses(self):
		self.assertParseCodes('f :: () { if p == ((P){ x: 1 }) { } }', [])

	def test_attributes(self):
		tree = self.assertParseCodes('[op._[_], wrap] f :: (a: V, i: int) -> int { return 0; }\n[extern = "puts"] g :: (s: []u8) -> int;', [])
		attributes = tree.declarations[0].attributes
		self.assertEqual([(attribute.name, attribute.value) for attribute in attributes], [('op', '_[_]'), ('wrap', None)])
		self.assertEqual(tree.declarations[1].attributes[0].value, 'puts')

	def test_variadic_parameter(self):
		tree = self.assertParseCodes('[builtin] log :: (format: []u8, values: ...);', [])
		parameters = tree.declarations[0].parameters
		self.assertEqual([(parameter.name, parameter.variadic) for parameter in parameters], [('format', False), ('values', True)])
		self.assertIsNone(parameters[1].type)

	def test_function_types(self):
		tree = self.assertParseCodes('apply :: (f: (int, mut &int) -> int, g: (int)) {}', [])
		first = tree.declarations[0].parameters[0].type
		self.assertTrue(first.parameters[1].mutable)
		self.assertIsNone(tree.declarations[0].parameters[1].type.result)

	def test_recovery_reports_each_declaration(self):
		self.assertParseCodes('a :: (;\nb :: () { let = 1; }\nc :: () {}', ['E0201', 'E0201'])


if __name__ == '__main__':
	unittest.main()
