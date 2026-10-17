
from io import print, println, write, write_line
from memory import alloc, realloc, free, copy

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
