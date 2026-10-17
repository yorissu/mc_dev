
from io import print, println, write, write_line
from memory import alloc, realloc, free, copy

sign :: (x: int) -> int {
	if x < 0 { return -1; } else if x == 0 { return 0; } else { return 1; }
}
pub main :: () -> int { print("{}", sign(-5)); print("{}", sign(0)); println("{}", sign(9)); return 3; }
