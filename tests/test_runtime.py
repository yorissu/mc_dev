
"""runtime tests: programs are checked, compiled to bytecode and run."""

import io
import os
import tempfile
import unittest

from support import TestCase, compile_program  # noqa: I001 (sets up the import path)
from mc.bytecode import disassemble, load, save
from mc.compiler import compile_program as compile_bytecode
from mc.vm import Machine, MachineError

PRELUDE = 'from io import print, println, write, write_line\nfrom memory import alloc, realloc, free, copy\n'


def build(source, modules=None):
	program, diagnostics = compile_program(PRELUDE + source, modules)
	errors = [item for item in diagnostics.items if item.level == 'error']
	assert not errors, ''.join(f'{item.code} {item.message} {item.span}\n' for item in errors)
	return compile_bytecode(program)


def execute(module, natives=None):
	out = io.StringIO()
	result = Machine(module, out, natives).run()
	return out.getvalue(), result


class RuntimeCase(TestCase):
	def run_program(self, source, modules=None, natives=None):
		return execute(build(source, modules), natives)

	def assertOutput(self, source, expected, result=None, modules=None):
		output, value = self.run_program(source, modules)
		self.assertEqual(output, expected)
		if result is not None:
			self.assertEqual(value, result)

	def assertFails(self, source, message):
		with self.assertRaises(MachineError) as caught:
			self.run_program(source)
		self.assertIn(message, caught.exception.message)
		return caught.exception


class Arithmetic(RuntimeCase):
	def test_integers_wrap(self):
		self.assertOutput('''
			pub main :: () {
				let mut small: u8 = 250;
				small += 10;
				println("{}", small);
				let mut signed: s8 = 127;
				signed += 1;
				println("{}", signed);
				println("{}", -7 / 2);
				println("{}", -7 % 2);
				println("{}", (u8)300 + (u8)1);
				println("{}", 1 << 3);
				println("{}", (s32)-16 >> 2);
				println("{}", ~0u8);
			}
		''', '4\n-128\n-3\n-1\n45\n8\n-4\n255\n')

	def test_floats(self):
		self.assertOutput('''
			pub main :: () {
				let third: f32 = 1.0f32 / 3;
				println("{}", third);
				println("{}", 0.1 + 0.2);
				let zero = 0.0;
				println("{}", 1.0 / zero);
				println("{}", (int)2.9);
				println("{}", (f64)1);
			}
		''', '0.33333334\n0.30000000000000004\ninf\n2\n1.0\n')

	def test_runtime_division_by_zero(self):
		self.assertFails('divide :: (a: int, b: int) -> int { return a / b; }\npub main :: () { _ = divide(1, 0); }', 'division by zero')

	def test_logic_short_circuits(self):
		self.assertOutput('''
			loud :: (value: bool) -> bool { print("x"); return value; }
			pub main :: () {
				println("{}", loud(false) && loud(true));
				println("{}", loud(true) || loud(false));
				println("{}", !loud(false));
			}
		''', 'xfalse\nxtrue\nxtrue\n')


class ControlFlow(RuntimeCase):
	def test_loops(self):
		self.assertOutput('''
			pub main :: () {
				for i in 0..3 print("{}", i);
				println("");
				for i in 3..0 print("{}", i);
				println("");
				for i in 0..=2 { print("{}", i); }
				println("");
				let mut n = 0;
				while true {
					n += 1;
					if n == 2 continue;
					if n > 4 break;
					print("{}", n);
				}
				println("");
				for i in 0..10 {
					if i % 2 == 0 continue;
					if i > 7 break;
					print("{}", i);
				}
				println("");
			}
		''', '012\n321\n012\n134\n1357\n')

	def test_unsigned_full_range(self):
		self.assertOutput('pub main :: () { let mut count = 0; for i in 0u8..=255u8 { count += 1; } println("{}", count); }', '256\n')

	def test_else_if_and_result(self):
		self.assertOutput('''
			sign :: (x: int) -> int {
				if x < 0 { return -1; } else if x == 0 { return 0; } else { return 1; }
			}
			pub main :: () -> int { print("{}", sign(-5)); print("{}", sign(0)); println("{}", sign(9)); return 3; }
		''', '-101\n', result=3)


class Values(RuntimeCase):
	def test_copies(self):
		self.assertOutput('''
			Point :: { let mut x: int; let mut y: int; }
			Box :: { let mut items: [3]int; let mut corner: Point; }
			pub main :: () {
				let a: Point = { x: 1, y: 2, };
				let mut b = a;
				b.x = 10;
				println("{}", a);
				println("{}", b);
				let mut first: Box = {};
				first.items[1] = 5;
				let mut second = first;
				second.items[1] = 6;
				second.corner.y = 7;
				println("{}", first);
				println("{}", second);
			}
		''', '{x: 1, y: 2}\n{x: 10, y: 2}\n{items: {0, 5, 0}, corner: {x: 0, y: 0}}\n{items: {0, 6, 0}, corner: {x: 0, y: 7}}\n')

	def test_defaults_and_zero(self):
		self.assertOutput('''
			Settings :: { let mut width: int = 80; let mut name: []u8; let mut on: bool = true; }
			pub main :: () {
				let s: Settings = { name: "term"[..], };
				println("{}", s);
				let z: Settings = {};
				println("{}", z.width);
				let empty: [2]f32 = {};
				println("{}", empty);
			}
		''', '{width: 80, name: term, on: true}\n80\n{0.0, 0.0}\n')

	def test_printing(self):
		self.assertOutput('''
			pub main :: () {
				println("{}", true);
				println("{}", "c string"c);
				let numbers: [_]int = { 1, 2, 3, };
				println("{}", numbers[1..]);
				let grid: [2][2]u16 = { { 1, 2 }, { 3, 4 } };
				println("{}", grid);
				write_line("raw"[..]);
				write("no newline"[..]);
			}
		''', 'true\nc string\n{2, 3}\n{{1, 2}, {3, 4}}\nraw\nno newline')


class References(RuntimeCase):
	def test_reference_parameters(self):
		self.assertOutput('''
			Point :: { let mut x: int; let mut y: int; }
			swap<T> :: (mut a: &T, mut b: &T) { let t = a; a = b; b = t; }
			grow :: (mut p: &Point) { p.x += 1; p.y *= 10; }
			total :: (p: &Point) -> int { return p.x + p.y; }
			bump :: (mut value: &int) { value += 1; }
			pub main :: () {
				let mut a = 1;
				let mut b = 2;
				swap(mut &a, mut &b);
				println("{}", a * 10 + b);
				let mut p: Point = { x: 1, y: 2, };
				grow(mut &p);
				println("{}", p);
				swap(mut &p.x, mut &p.y);
				println("{}", total(&p));
				println("{}", total(&{ x: 5, y: 6, }));
				let mut items: [3]int = {};
				bump(mut &items[2]);
				let s = items[..];
				bump(mut &s[0]);
				println("{}", items);
			}
		''', '21\n{x: 2, y: 20}\n22\n11\n{1, 0, 1}\n')


class Slices(RuntimeCase):
	def test_views_share_memory(self):
		self.assertOutput('''
			fill :: (s: []int, value: int) { for i in 0..lenof(s) { s[i] = value; } }
			pub main :: () {
				let mut a: [5]int = {};
				fill(a[1..4], 7);
				println("{}", a);
				let s = a[..];
				let t = s[3..];
				t[0] = 9;
				println("{}", a);
				println("{}", lenof(t));
				println("{}", s == null);
				println("{}", null == t[0..0]);
				_ = copy(a[0..2], t);
				println("{}", a);
			}
		''', '{0, 7, 7, 7, 0}\n{0, 7, 7, 9, 0}\n2\nfalse\ntrue\n{9, 0, 7, 9, 0}\n')

	def test_out_of_bounds(self):
		error = self.assertFails('get :: (s: []int, i: uint) -> int { return s[i]; }\npub main :: () { let mut a: [2]int = {}; _ = get(a[..], 2); }',
			'index 2 is out of bounds (length 2)')
		self.assertEqual([name for name, _ in error.trace], ['get', 'main'])
		self.assertFails('pub main :: () { let mut a: [4]int = {}; let i = 3; _ = a[i..i + 2]; }', 'out of bounds')

	def test_read_only(self):
		self.assertFails('TABLE :: ([_]int){ 1, 2 };\nset :: (s: []int) { s[0] = 5; }\npub main :: () { set(TABLE[..]); }', 'read-only')
		self.assertFails('set :: (s: []u8) { s[0] = 5; }\npub main :: () { set("text"[..]); }', 'read-only')
		self.assertFails('set :: (s: []int) { s[0] = 5; }\npub main :: () { let a: [2]int = {}; set(a[..]); }', 'read-only')
		self.assertOutput('set :: (s: []int) { s[0] = 5; }\npub main :: () { let mut a: [2]int = {}; set(a[..]); println("{}", a); }', '{5, 0}\n')

	def test_constant_slices_outlive_functions(self):
		self.assertOutput('name :: () -> []u8 { return "static"[..]; }\npub main :: () { println("{}", name()); }', 'static\n')

	def test_local_array_does_not_outlive_its_function(self):
		self.assertFails('''
			escape :: () -> []int { let mut local: [3]int = { 1, 2, 3 }; return local[..]; }
			pub main :: () { let s = escape(); _ = s[0]; }
		''', 'local array of a function that has returned')


class Heap(RuntimeCase):
	def test_alloc_realloc_free(self):
		self.assertOutput('''
			pub main :: () {
				let mut data = alloc<int>(2);
				data[0] = 1;
				data[1] = 2;
				data = realloc(data, 4);
				data[3] = 4;
				println("{}", data);
				println("{}", lenof(data));
				data = free(data);
				println("{}", data == null);
			}
		''', '{1, 2, 0, 4}\n4\ntrue\n')

	def test_use_after_free_and_bad_free(self):
		self.assertFails('pub main :: () { let data = alloc<int>(2); let view = data[..]; _ = free(data); _ = view[0]; }', 'use of freed heap memory')
		self.assertFails('pub main :: () { let data = alloc<int>(2); _ = free(data); _ = free(data); }', 'freed heap memory')
		self.assertFails('pub main :: () { let data = alloc<int>(4); _ = free(data[1..]); }', 'whole allocation')
		self.assertFails('pub main :: () { let mut a: [2]int = {}; _ = free(a[..]); }', 'did not return')
		self.assertFails('pub main :: () { let data = alloc<int>(1); let old = data; _ = realloc(data, 8); _ = old[0]; }', 'freed')

	def test_vector(self):
		with open(os.path.join(os.path.dirname(__file__), '..', 'examples', 'vector', 'vector.mc'), encoding='utf-8') as file:
			vector = file.read()
		self.assertOutput('''
			from vector import *
			pub main :: () {
				let mut v = vector_create<int>(1);
				for i in 0..10 { vector_append(mut &v, &(i * i)); }
				println("{}", v.data[0..v.size]);
				vector_free(mut &v);
			}
		''', '{0, 1, 4, 9, 16, 25, 36, 49, 64, 81}\n', modules={'vector': vector})


class Functions(RuntimeCase):
	def test_lambdas_generics_operators(self):
		self.assertOutput('''
			V :: { let x: int; let y: int; }
			[op._+_] add :: (a: V, b: V) -> V { return { x: a.x + b.x, y: a.y + b.y, }; }
			[op._==_] same :: (a: V, b: V) -> bool { return a.x == b.x && a.y == b.y; }
			[op._<_] less :: (a: V, b: V) -> bool { return a.x < b.x; }
			[op.-_] negate :: (a: V) -> V { return { x: -a.x, y: -a.y, }; }
			map<T> :: (s: []T, f: (T) -> T) { for i in 0..lenof(s) { s[i] = f(s[i]); } }
			largest<T> :: (a: T, b: T) -> T { if a > b { return a; } return b; }
			double :: (x: int) -> int { return x * 2; }
			scaled :: (x: int, factor: int = 3) -> int { return x * factor; }
			pub main :: () {
				let mut numbers: [_]int = { 1, 2, 3, };
				map(numbers[..], double);
				map(numbers[..], (x: int) -> int { return x + 1; });
				println("{}", numbers);
				let f: (int) -> int = double;
				println("{}", f == double);
				println("{}", largest(3, 9));
				println("{}", largest(2.5, 1.5));
				let a: V = { x: 1, y: 2, };
				let b = a + a + -a;
				println("{}", b);
				println("{}", a == b);
				println("{}", a != (V){ x: 1, y: 3, });
				println("{}", a + a > a);
				println("{}", scaled(2));
				println("{}", scaled(2, 10));
				OFFSET :: 100;
				helper :: (x: int) -> int { return x + OFFSET; }
				println("{}", helper(1));
			}
		''', '{3, 5, 7}\ntrue\n9\n2.5\n{x: 1, y: 2}\ntrue\ntrue\ntrue\n6\n20\n101\n')

	def test_builtin_as_value(self):
		self.assertOutput('pub main :: () { let make: (uint) -> []int = alloc<int>; let s = make(3); println("{}", lenof(s)); _ = free(s); }', '3\n')

	def test_recursion_and_constants(self):
		self.assertOutput('''
			fib :: (n: int) -> int { if n < 2 { return n; } return fib(n - 1) + fib(n - 2); }
			SQUARES :: squares();
			squares :: () -> [5]int { let mut t: [5]int = {}; for i in 0..5 { t[i] = i * i; } return t; }
			pub main :: () { println("{}", fib(20)); println("{}", SQUARES); println("{}", SQUARES[..][4]); }
		''', '6765\n{0, 1, 4, 9, 16}\n16\n')

	def test_compile_time_and_run_time_agree(self):
		self.assertOutput('''
			collatz :: (start: u64) -> u64 {
				let mut n = start;
				let mut steps: u64 = 0;
				while n != 1 { if n % 2 == 0 { n /= 2; } else { n = 3 * n + 1; } steps += 1; }
				return steps;
			}
			mix :: (seed: u32) -> u32 {
				let mut h = seed;
				for i in 0..8 { [wrap] h = h * 2654435761 + (u32)i; h ^= h >> 13; }
				return h;
			}
			shapes :: () -> f32 { let mut total: f32 = 0; for i in 1..=10 { total += 1.0f32 / (f32)i; } return total; }
			C1 :: collatz(27);
			C2 :: mix(42);
			C3 :: shapes();
			pub main :: () { println("{}", C1 == collatz(27)); println("{}", C2 == mix(42)); println("{}", C3 == shapes()); }
		''', 'true\ntrue\ntrue\n')

	def test_stack_overflow(self):
		self.assertFails('forever :: (n: int) -> int { return forever(n + 1); }\npub main :: () { _ = forever(0); }', 'stack overflow')

	def test_assert(self):
		self.assertFails('check :: (x: int) { assert(x > 0, "x must be positive"); }\npub main :: () { check(1); check(-1); }', 'x must be positive')
		self.assertFails('check :: (x: int) { assert(x > 0); }\npub main :: () { check(0); }', 'assertion failed: x > 0')

	def test_stdlib_option_result(self):
		self.assertOutput('''
			from option import *
			from result import Result, ok, fail
			first :: (s: []int) -> Option<int> { if lenof(s) == 0 { return none(); } return some(s[0]); }
			digit :: (c: u8) -> Result<int, []u8> {
				if c < '0' || c > '9' { return fail("not a digit"[..]); }
				return ok((int)(c - '0'));
			}
			pub main :: () {
				let found = first({ 4, 5 });
				let missing = first(null);
				println("{}", value_or(&found, -1));
				println("{}", value_or(&missing, -1));
				let good = digit('7');
				let bad = digit('x');
				println("{}", good.value);
				println("{}", bad.error);
			}
		''', '4\n-1\n7\nnot a digit\n')


class Formatting(RuntimeCase):
	def test_format_strings(self):
		self.assertOutput('''
			Point :: { let x: int; let y: int; }
			GREETING :: "{}, {}!";
			pub main :: () {
				println("{} + {} = {}", 2, 3.5, 2 + 3);
				println(GREETING, "hello", "world"[..]);
				let p: Point = { x: 1, y: 2, };
				println("point {} with {{braces}} and }}", p);
				print("no values");
				print(" then {} {}\\n", true, (u8)200);
				println("{}{}{}", 'a', -1, 0.25f32);
			}
		''', '2 + 3.5 = 5\nhello, world!\npoint {x: 1, y: 2} with {braces} and }\nno values then true 200\n97-10.25\n')

	def test_variadic_native(self):
		received = []

		def collect(machine, arguments):
			received.extend(arguments)
			return len(arguments)

		output, _ = self.run_program('[extern = "test_collect"] collect :: (values: ...) -> int;\npub main :: () { println("{}", collect(1, 2.5, true)); }',
			natives={'test_collect': collect})
		self.assertEqual((output, received), ('3\n', [1, 2.5, True]))


class Natives(RuntimeCase):
	def test_registered_native(self):
		calls = []

		def record(machine, arguments):
			calls.append(arguments[0])
			return arguments[0] * 2

		output, _ = self.run_program('[extern = "test_record"] record :: (x: int) -> int;\npub main :: () { println("{}", record(21)); }',
			natives={'test_record': record})
		self.assertEqual((output, calls), ('42\n', [21]))

	def test_unknown_native(self):
		module = build('[extern = "nobody_has_this"] missing :: () -> int;\npub main :: () { _ = missing(); }')
		with self.assertRaises(MachineError):
			Machine(module, io.StringIO())


class Bytecode(RuntimeCase):
	def test_round_trip(self):
		source = '''
			GREETING :: "saved"[..];
			pub main :: () -> int {
				let f = (x: int) -> int { return x + 1; };
				println("{}", GREETING);
				println("{}", f(41));
				return 5;
			}
		'''
		module = build(source)
		with tempfile.TemporaryDirectory() as directory:
			path = os.path.join(directory, 'program.mcb')
			save(module, path)
			loaded = load(path)
		self.assertEqual(execute(loaded), ('saved\n42\n', 5))

	def test_typed_opcodes(self):
		text = disassemble(build('add :: (a: s32, b: s32) -> s32 { return a + b; }\npub main :: () { _ = add(1, 2); _ = 1.5f32 < 2; }'))
		self.assertIn('ADD_S32', text)
		self.assertIn('LT_F32', text)
		self.assertIn('(entry)', text)


if __name__ == '__main__':
	unittest.main()
