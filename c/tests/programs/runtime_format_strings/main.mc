
from io import print, println, write, write_line
from memory import alloc, realloc, free, copy

Point :: { let x: int; let y: int; }
GREETING :: "{}, {}!";
pub main :: () {
	println("{} + {} = {}", 2, 3.5, 2 + 3);
	println(GREETING, "hello", "world"[..]);
	let p: Point = { x: 1, y: 2, };
	println("point {} with {{braces}} and }}", p);
	print("no values");
	print(" then {} {}\n", true, (u8)200);
	println("{}{}{}", 'a', -1, 0.25f32);
}
