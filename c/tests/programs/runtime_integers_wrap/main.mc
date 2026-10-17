
from io import print, println, write, write_line
from memory import alloc, realloc, free, copy

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
