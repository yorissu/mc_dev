
from io import print, println, write, write_line
from memory import alloc, realloc, free, copy

pub main :: () {
	let third: f32 = 1.0f32 / 3;
	println("{}", third);
	println("{}", 0.1 + 0.2);
	let zero = 0.0;
	println("{}", 1.0 / zero);
	println("{}", (int)2.9);
	println("{}", (f64)1);
}
