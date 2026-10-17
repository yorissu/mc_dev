
from io import print, println, write, write_line
from memory import alloc, realloc, free, copy

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
