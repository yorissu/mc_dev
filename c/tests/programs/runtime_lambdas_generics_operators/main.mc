
from io import print, println, write, write_line
from memory import alloc, realloc, free, copy

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
