
from io import print, println, write, write_line
from memory import alloc, realloc, free, copy

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
