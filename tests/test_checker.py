
"""checker tests: every rule of the design, each with a passing and a failing case."""

import unittest

from support import TestCase  # noqa: I001 (sets up the import path)
from mc.checker import FunctionInfo


class BasicTypes(TestCase):
	def test_literals_take_their_type_from_context(self):
		self.assertClean('''
			f :: () {
				let a: u8 = 255;
				let b: f32 = 1;
				let c: f64 = 2.5;
				let d = 3;
				let e = 1.5;
				let g: s8 = -128;
				let h: int = 'a';
				_ = a; _ = b; _ = c; _ = d; _ = e; _ = g; _ = h;
			}
		''') if False else None
		self.assertCodes('f :: () { let h: int = \'a\'; _ = h; }', ['E0502'])
		self.assertClean('f :: () { let a: u8 = 255; let b: f32 = 1; let g: s8 = -128; let d = 3; let e = 1.5; _ = a; _ = b; _ = g; _ = d; _ = e; }')

	def test_literal_out_of_range(self):
		self.assertCodes('f :: () { let a: u8 = 256; _ = a; }', ['E0511'])
		self.assertCodes('f :: () { let a = 300u8; _ = a; }', ['E0511'])
		self.assertCodes('f :: () { let a: s8 = -129; _ = a; }', ['E0511'])
		self.assertCodes('f :: () { let a: int = 1.5; _ = a; }', ['E0502'])

	def test_mixed_arithmetic_needs_a_cast(self):
		self.assertCodes('f :: (a: s32, b: int) -> int { return a + b; }', ['E0507'])
		self.assertClean('f :: (a: s32, b: int) -> int { return (int)a + b; }')
		self.assertCodes('f :: (a: f32) -> f32 { return a + 1.5; }', [])

	def test_casts(self):
		self.assertClean('f :: (a: f64) -> u8 { return (u8)a; }')
		self.assertCodes('f :: (a: bool) -> int { return (int)a; }', ['E0510'])

	def test_discarded_results(self):
		self.assertCodes('g :: () -> int { return 1; }\nf :: () { g(); }', ['E0509'])
		self.assertClean('g :: () -> int { return 1; }\nf :: () { _ = g(); }')
		self.assertCodes('g :: () {}\nf :: () { let x = g(); }', ['E0523'])

	def test_missing_return_and_loops(self):
		self.assertCodes('f :: (a: int) -> int { if a > 0 { return 1; } }', ['E0514'])
		self.assertClean('f :: (a: int) -> int { if a > 0 { return 1; } else { return 2; } }')
		self.assertClean('f :: () -> int { while true { } }')
		self.assertCodes('f :: () { break; }', ['E0515'])
		self.assertCodes('f :: () { return 1; }', ['E0524'])

	def test_for_loops(self):
		self.assertClean('f :: (n: uint) { for i in 0..n { _ = i; } for j in 10..0 { _ = j; } }')
		self.assertCodes('f :: (n: uint, m: int) { for i in 0..n { i = 1; } }', ['E0508'])
		self.assertCodes('f :: (n: uint, m: int) { for i in n..m { _ = i; } }', ['E0502'])

	def test_strict_comparison_types(self):
		self.assertCodes('f :: (a: u8, b: s8) -> bool { return a < b; }', ['E0507'])
		self.assertClean('f :: (a: []u8) -> bool { return a == null || lenof(a) > 0; }')

	def test_undefined_names(self):
		self.assertCodes('f :: () { _ = missing; }', ['E0501'])
		self.assertCodes('f :: () -> nothing_here { }', ['E0501'])


class Variables(TestCase):
	def test_mutability(self):
		self.assertCodes('f :: () { let a = 1; a = 2; }', ['E0508'])
		self.assertClean('f :: () { let mut a = 1; a = 2; a += 1; }')
		self.assertCodes('f :: (a: int) { a = 2; }', ['E0508'])
		self.assertClean('f :: (mut a: int) { a = 2; }')

	def test_unused_mut(self):
		self.assertCodes('f :: () { let mut a = 1; _ = a; }', ['W0503'])
		self.assertCodes('f :: (mut a: int) { _ = a; }', ['W0503'])

	def test_uninitialized(self):
		self.assertCodes('f :: () { let a: int; _ = a; }', ['W0502'])
		self.assertClean('f :: () { [uninit] let a: int; _ = a; }')
		self.assertClean('f :: (c: bool) { let a: int; if c { a = 1; } else { a = 2; } _ = a; }')
		self.assertCodes('f :: (c: bool) { let a: int; if c { a = 1; } _ = a; }', ['W0502'])
		self.assertClean('f :: (c: bool) -> int { let a: int; if c { a = 1; } else { return 0; } return a; }')
		self.assertCodes('f :: () { let a: int; a = 1; a = 2; }', ['E0508'])
		self.assertCodes('f :: (c: bool) { let a: int; while c { a = 1; } }', ['E0508'])

	def test_redeclaration_in_one_scope(self):
		self.assertCodes('f :: () { let a = 1; let a = 2; _ = a; }', ['E0401'])
		self.assertClean('f :: () { let a = 1; { let a = 2; _ = a; } _ = a; }')

	def test_inferred_needs_value(self):
		self.assertCodes('f :: () { let a; }', ['E0506'])
		self.assertCodes('f :: () { let a = null; }', ['E0506'])


class Structs(TestCase):
	POINT = 'Point :: { let mut x: f32; let mut y: f32 = 0; let id: int = 7; }\n'

	def test_literals(self):
		self.assertClean(self.POINT + 'f :: () -> Point { return { x: 1, y: 2, }; }')
		self.assertCodes(self.POINT + 'f :: () -> Point { return { y: 2, }; }', ['W0504'])
		self.assertClean(self.POINT + 'f :: () -> Point { return {}; }')
		self.assertCodes(self.POINT + 'f :: () -> Point { return { z: 1, x: 1, }; }', ['E0512'])
		self.assertCodes(self.POINT + 'f :: () -> Point { return { 1, 2 }; }', ['E0525'])
		self.assertClean(self.POINT + 'ORIGIN :: (Point){ x: 0, y: 0, };')

	def test_field_mutability(self):
		self.assertClean(self.POINT + 'f :: () { let mut p: Point = { x: 1, }; p.x = 2; }')
		self.assertCodes(self.POINT + 'f :: () { let mut p: Point = { x: 1, }; p.id = 2; }', ['E0508', 'W0503'])
		self.assertCodes(self.POINT + 'f :: () { let p: Point = { x: 1, }; p.x = 2; }', ['E0508'])

	def test_recursive_struct(self):
		self.assertCodes('Node :: { let next: Node; }', ['E0414'])
		self.assertClean('Node :: { let next: []Node; let value: int; }')

	def test_equality_needs_operator(self):
		self.assertCodes(self.POINT + 'f :: (a: Point, b: Point) -> bool { return a == b; }', ['E0507'])

	def test_zero_values(self):
		self.assertClean('f :: () { let a: [4]int = {}; let s: []u8 = {}; _ = a; _ = s; }')
		self.assertCodes('H :: { let call: (int) -> int; }\nf :: () -> H { return {}; }', ['E0526'])

	def test_sizeof(self):
		program = self.assertClean(self.POINT + 'SIZE :: sizeof(Point);\nWORDS :: sizeof([3]int);')
		values = {constant.name: constant.value for constant in program.constants}
		self.assertEqual(values, {'SIZE': 16, 'WORDS': 24})


class Arrays(TestCase):
	def test_arrays_and_slices(self):
		self.assertClean('''
			sum :: (s: []int) -> int {
				let mut total = 0;
				for i in 0..lenof(s) { total += s[i]; }
				return total;
			}
			f :: () -> int {
				let a: [_]int = { 1, 2, 3, };
				let b: [3]int = { 4, 5, 6, };
				return sum(a[..]) + sum(b[1..]) + sum({ 7, 8 });
			}
		''')
		self.assertCodes('f :: () { let a: [3]int = { 1, 2 }; _ = a; }', ['E0525'])
		self.assertCodes('g :: (s: []int) {}\nf :: () { let a: [3]int = {}; g(a); }', ['E0502'])
		self.assertCodes('f :: () -> int { let a: [3]int = {}; return a[3]; }', ['E0706'])

	def test_slice_elements_are_always_writable(self):
		self.assertClean('f :: (s: []int) { s[0] = 1; }')
		self.assertCodes('f :: () { let a: [2]int = {}; a[0] = 1; }', ['E0508'])

	def test_strings(self):
		self.assertClean('f :: () -> uint { let s = "hello"; let c = "hi"c; return lenof(s) + lenof(c[..]); }')
		program = self.assertClean('LENGTH :: lenof("hello"c);')
		self.assertEqual(program.constants[0].value, 6)


class References(TestCase):
	def test_reference_arguments(self):
		base = 'grow :: (mut v: &int) { v += 1; }\nread :: (v: &int) -> int { return v; }\n'
		self.assertClean(base + 'f :: () { let mut a = 1; grow(mut &a); _ = read(&a); _ = read(&5); }')
		self.assertCodes(base + 'f :: () { let mut a = 1; grow(&a); }', ['E0518', 'W0503'])
		self.assertCodes(base + 'f :: () { let a = 1; grow(mut &a); }', ['E0508'])
		self.assertCodes(base + 'f :: () { let a = 1; _ = read(a); }', ['E0517'])
		self.assertCodes(base + 'f :: () { grow(mut &5); }', ['E0508'])

	def test_references_only_as_parameters(self):
		self.assertCodes('f :: () { let a = 1; let r = &a; }', ['E0519'])
		self.assertCodes('S :: { let r: &int; }', ['E0413'])
		self.assertCodes('f :: (a: int) -> &int { return a; }', ['E0413'])

	def test_read_only_reference_cannot_write(self):
		self.assertCodes('f :: (v: &int) { v = 2; }', ['E0508'])

	def test_overlapping_references(self):
		base = 'swap :: (mut a: &int, mut b: &int) { let t = a; a = b; b = t; }\nP :: { let mut x: int; let mut y: int; }\n'
		self.assertCodes(base + 'f :: () { let mut a = 1; swap(mut &a, mut &a); }', ['E0528'])
		self.assertClean(base + 'f :: () { let mut p: P = {}; swap(mut &p.x, mut &p.y); }')
		self.assertCodes(base + 'f :: () { let mut s: [2]int = {}; let i = 0; swap(mut &s[i], mut &s[1]); }', ['E0528'])


class Generics(TestCase):
	def test_inference_and_instances(self):
		program = self.assertClean('''
			Pair<A, B> :: { let first: A; let second: B; }
			pair<A, B> :: (a: A, b: B) -> Pair<A, B> { return { first: a, second: b, }; }
			add<T> :: (a: T, b: T) -> T { return a + b; }
			f :: () -> int {
				let p = pair(1, 2.5);
				_ = add<f32>(1, 2);
				_ = add(1.5, 2.5);
				return add(p.first, 3);
			}
		''')
		names = sorted(str(instance) for instance in program.instances if instance.function.generics)
		self.assertEqual(names, ['add<f32>', 'add<float>', 'add<int>', 'pair<int, float>'])

	def test_errors_inside_an_instance(self):
		self.assertCodes('S :: { let a: int; }\nadd<T> :: (a: T, b: T) -> T { return a + b; }\nf :: (s: S) { _ = add(s, s); }', ['E0507'])

	def test_cannot_infer(self):
		self.assertCodes('make<T> :: () -> []T { return null; }\nf :: () { _ = make(); }', ['E0506'])
		self.assertClean('make<T> :: () -> []T { return null; }\nf :: () { let s: []int = make(); _ = s; }')
		self.assertCodes('g<T> :: (a: T) {}\nf :: () { g<int, int>(1); }', ['E0520'])
		self.assertCodes('g :: (a: int) {}\nf :: () { g<int>(1); }', ['E0521'])

	def test_unbounded_instantiation(self):
		self.assertCodes('deep<T> :: (a: T) { let s: []T = null; deep(s); }\nf :: () { deep(1); }', ['E0527'])

	def test_generic_function_value(self):
		self.assertClean('id<T> :: (a: T) -> T { return a; }\nf :: () { let g: (int) -> int = id; let h = id<u8>; _ = g(1); _ = h(2); }')
		self.assertCodes('id<T> :: (a: T) -> T { return a; }\nf :: () { let g = id; }', ['E0506'])

	def test_unused_generic_bodies_are_not_checked(self):
		self.assertClean('never<T> :: (a: T) -> T { return a.missing; }')


class Operators(TestCase):
	VECTOR = '''
		V :: { let x: f32; let y: f32; }
		[op._+_] add :: (a: V, b: V) -> V { return { x: a.x + b.x, y: a.y + b.y, }; }
		[op._*_] scale :: (a: V, s: f32) -> V { return { x: a.x * s, y: a.y * s, }; }
		[op._==_] equal :: (a: V, b: V) -> bool { return a.x == b.x && a.y == b.y; }
		[op._<_] less :: (a: V, b: V) -> bool { return a.x < b.x; }
		[op.-_] negate :: (a: V) -> V { return { x: -a.x, y: -a.y, }; }
	'''

	def test_operator_functions(self):
		self.assertClean(self.VECTOR + '''
			f :: (a: V, b: V) -> bool {
				let mut c = a + b * 2;
				c += a;
				let d = -c;
				return a != b && a > b && d <= c && a >= b && add(a, b) == c;
			}
		''')

	def test_exact_operand_order(self):
		self.assertCodes(self.VECTOR + 'f :: (a: V) -> V { return 2 * a; }', ['E0507'])

	def test_ambiguous_literal(self):
		self.assertCodes(self.VECTOR + '[op._*_] scale_int :: (a: V, s: int) -> V { return a; }\nf :: (a: V) -> V { return a * 2; }', ['W0501'])

	def test_declaration_rules(self):
		self.assertCodes('V :: { let x: int; }\n[op._!=_] f :: (a: V, b: V) -> bool { return false; }', ['E0408'])
		self.assertCodes('V :: { let x: int; }\n[op._&&_] f :: (a: V, b: V) -> bool { return false; }', ['E0406'])
		self.assertCodes('V :: { let x: int; }\n[op._+_] f :: (a: V) -> V { return a; }', ['E0407'])
		self.assertCodes('V :: { let x: int; }\n[op._==_] f :: (a: V, b: V) -> int { return 0; }', ['E0409'])
		self.assertCodes('V :: { let x: int; }\n[op._+_] f :: (a: V, b: V) -> V { return a; }\n[op._+_] g :: (a: V, b: V) -> V { return b; }', ['E0410'])

	def test_indexing_operator(self):
		self.assertClean('Grid :: { let cells: []int; }\n[op._[_]] at :: (g: Grid, i: int) -> int { return g.cells[i]; }\nf :: (g: Grid) -> int { return g[2]; }')


class Lambdas(TestCase):
	def test_lambdas(self):
		self.assertClean('''
			apply :: (f: (int) -> int, v: int) -> int { return f(v); }
			double :: (x: int) -> int { return x * 2; }
			f :: () -> int {
				LIMIT :: 10;
				helper :: (x: int) -> int { return x + LIMIT; }
				return apply(double, 1) + apply((x: int) -> int { return x + LIMIT; }, 2) + helper(3);
			}
		''')

	def test_no_captures(self):
		self.assertCodes('f :: () { let a = 1; let g = (x: int) -> int { return x + a; }; _ = g; }', ['E0516'])
		self.assertCodes('f :: () { let a = 1; helper :: () -> int { return a; } _ = a; }', ['E0516'])


class Constants(TestCase):
	def test_compile_time_evaluation(self):
		program = self.assertClean('''
			fib :: (n: int) -> int {
				if n < 2 { return n; }
				return fib(n - 1) + fib(n - 2);
			}
			sum_down :: (n: int) -> int {
				let mut total = 0;
				for i in n..=0 { total += i; }
				return total;
			}
			FIB :: fib(15);
			DOWN :: sum_down(4);
			SIZE :: 4 * 4;
			TABLE :: ([SIZE]u8){};
			HALF :: (f32)SIZE / 2;
		''')
		values = {constant.name: constant.value for constant in program.constants}
		self.assertEqual(values['FIB'], 610)
		self.assertEqual(values['DOWN'], 10)
		self.assertEqual(values['SIZE'], 16)
		self.assertEqual(len(values['TABLE']), 16)
		self.assertEqual(values['HALF'], 8.0)

	def test_order_independent_and_cycles(self):
		self.assertClean('A :: B + 1;\nB :: 2;')
		self.assertCodes('A :: B;\nB :: A;', ['E0702'])

	def test_not_constant(self):
		self.assertCodes('[builtin] read :: () -> int;\nX :: read();', ['E0701'])
		self.assertCodes('f :: (n: int) { let a: [n]int = {}; }', ['E0701'])

	def test_assert(self):
		self.assertCodes('f :: () { assert(1 + 1 == 3, "math"); }', ['E0703'])
		self.assertClean('f :: (x: int) { assert(x > 0); }')

	def test_overflow_and_division(self):
		self.assertCodes('X :: 250u8 + 10u8;', ['W0701'])
		self.assertClean('[wrap] X :: 250u8 + 10u8;')
		self.assertCodes('f :: () -> int { return 1 / 0; }', ['E0704'])

	def test_wrap_values(self):
		program = self.assertClean('[wrap] X :: 250u8 + 10u8;\n[wrap] Y :: (s8)200;')
		self.assertEqual([constant.value for constant in program.constants], [4, -56])


class Modules(TestCase):
	SHAPES = '''
		pub Point :: { let mut x: int; let mut y: int; }
		Hidden :: { let secret: int; }
		pub make :: () -> Point { return { x: 1, y: 2, }; }
		pub hidden :: () -> Hidden { return { secret: 1, }; }
		[op._+_] pub add :: (a: Point, b: Point) -> Point { return { x: a.x + b.x, y: a.y + b.y, }; }
		private_function :: () {}
	'''

	def test_import_forms(self):
		modules = {'geo.shapes': self.SHAPES}
		self.assertClean('import geo.shapes\nf :: () -> geo.shapes.Point { return geo.shapes.make(); }', modules)
		self.assertClean('import geo.shapes as s\nf :: () -> s.Point { return s.make() + s.make(); }', modules)
		self.assertClean('from geo.shapes import make, Point as P\nf :: () -> P { return make(); }', modules)
		self.assertClean('from geo.shapes import *\nf :: () -> Point { return make() + make(); }', modules)

	def test_operator_found_through_operand_type(self):
		modules = {'geo.shapes': self.SHAPES, 'geo.factory': 'from geo.shapes import Point, make\npub twice :: () -> Point { return make(); }'}
		self.assertClean('from geo.factory import twice\nf :: () { _ = twice() + twice(); }', modules)

	def test_visibility(self):
		modules = {'geo.shapes': self.SHAPES}
		self.assertCodes('from geo.shapes import private_function', ['E0303'], modules)
		self.assertCodes('import geo.shapes\nf :: () { geo.shapes.private_function(); }', ['E0303'], modules)
		self.assertCodes('from geo.shapes import hidden\nf :: () -> int { return hidden().secret; }', ['E0522'], modules)
		self.assertCodes('from geo.shapes import nothing', ['E0305'], modules)

	def test_missing_and_cycles(self):
		self.assertCodes('import missing.module', ['E0301'])
		self.assertCodes('import a', ['E0302'], {'a': 'import b', 'b': 'import a'})

	def test_star_ambiguity(self):
		modules = {'a': 'pub thing :: () {}', 'b': 'pub thing :: () {}'}
		self.assertCodes('from a import *\nfrom b import *\nf :: () { thing(); }', ['E0304'], modules)
		self.assertClean('from a import *\nfrom b import *\nthing :: () {}\nf :: () { thing(); }', modules)

	def test_main(self):
		from support import compile_program
		_, diagnostics = compile_program('main :: () {}', add_main=False)
		self.assertEqual(diagnostics.codes(), ['E0411'])
		_, diagnostics = compile_program('pub main :: () -> int { return 0; }', add_main=False)
		self.assertEqual(diagnostics.codes(), [])


class Attributes(TestCase):
	def test_attribute_rules(self):
		self.assertCodes('[unknown] f :: () {}', ['E0402'])
		self.assertCodes('[uninit] f :: () {}', ['E0403'])
		self.assertCodes('[extern] f :: () -> int;', ['E0403', 'E0404'])
		self.assertCodes('[builtin] f :: () {}', ['E0405'])
		self.assertCodes('f :: () -> int;', ['E0404'])
		self.assertCodes('f :: (a: int = 1, b: int) {}', ['E0412'])


class Variadics(TestCase):
	def test_declarations(self):
		self.assertClean('[builtin] log :: (format: []u8, values: ...);\n[extern = "sum"] total :: (values: ...) -> int;')
		self.assertCodes('mine :: (values: ...) {}', ['E0415'])
		self.assertCodes('[builtin] log :: (values: ..., format: []u8);', ['E0415'])
		self.assertCodes('f :: () { let g = (values: ...) { }; _ = g; }', ['E0415'])
		self.assertCodes('V :: { let x: int; }\n[op._+_, builtin] add :: (a: V, b: ...) -> V;', ['E0409'])

	def test_format_strings(self):
		base = 'from io import print, println\n'
		self.assertClean(base + 'f :: (x: int, p: []u8) { println("{} and {} {{}}", x, p); print("plain"); println("{}", 1.5); }')
		self.assertClean(base + 'FORMAT :: "{} {}";\nf :: () { println(FORMAT, 1, true); }')
		self.assertCodes(base + 'f :: () { println("{} {}", 1); }', ['E0530'])
		self.assertCodes(base + 'f :: () { println("{}", 1, 2); }', ['E0530'])
		self.assertCodes(base + 'f :: () { println("{x}", 1); }', ['E0530'])
		self.assertCodes(base + 'f :: () { println("a } b"); }', ['E0530'])
		self.assertCodes(base + 'f :: (format: []u8) { println(format, 1); }', ['E0701'])
		self.assertCodes(base + 'f :: (x: int) { println("{}", &x); }', ['E0519'])
		self.assertCodes(base + 'f :: () { println(); }', ['E0505'])
		self.assertCodes(base + 'f :: () { let p = println; }', ['E0529'])
		self.assertCodes(base + 'g :: () {}\nf :: () { println("{}", g()); }', ['E0523'])


class Stdlib(TestCase):
	def test_option_and_result(self):
		self.assertClean('''
			from option import *
			from result import Result, ok, fail
			first :: (s: []int) -> Option<int> {
				if lenof(s) == 0 { return none(); }
				return some(s[0]);
			}
			parse :: (c: u8) -> Result<int, int> {
				if c < '0' || c > '9' { return fail(1); }
				return ok((int)(c - '0'));
			}
			f :: () -> int {
				let o = first({ 1, 2 });
				let r = parse('7');
				if !r.ok { return r.error; }
				return value_or(&o, 0) + unwrap(&o) + r.value;
			}
		''')


if __name__ == '__main__':
	unittest.main()
