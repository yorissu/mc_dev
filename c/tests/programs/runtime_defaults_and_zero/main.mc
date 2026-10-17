
from io import print, println, write, write_line
from memory import alloc, realloc, free, copy

Settings :: { let mut width: int = 80; let mut name: []u8; let mut on: bool = true; }
pub main :: () {
	let s: Settings = { name: "term"[..], };
	println("{}", s);
	let z: Settings = {};
	println("{}", z.width);
	let empty: [2]f32 = {};
	println("{}", empty);
}
