
from io import print, println, write, write_line
from memory import alloc, realloc, free, copy

GREETING :: "saved"[..];
pub main :: () -> int {
	let f = (x: int) -> int { return x + 1; };
	println("{}", GREETING);
	println("{}", f(41));
	return 5;
}
