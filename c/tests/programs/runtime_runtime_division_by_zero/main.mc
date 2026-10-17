
from io import print, println, write, write_line
from memory import alloc, realloc, free, copy
divide :: (a: int, b: int) -> int { return a / b; }
pub main :: () { _ = divide(1, 0); }
