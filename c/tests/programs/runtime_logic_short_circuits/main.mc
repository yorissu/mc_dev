
from io import print, println, write, write_line
from memory import alloc, realloc, free, copy

loud :: (value: bool) -> bool { print("x"); return value; }
pub main :: () {
	println("{}", loud(false) && loud(true));
	println("{}", loud(true) || loud(false));
	println("{}", !loud(false));
}
