
from io import print, println
from memory import alloc, realloc, free, copy

divide :: (a: int, b: int) -> int { return a / b; }
pub main :: () -> int { println("{}", divide(7, 2)); return divide(1, 0); }
