
from io import print, println
from memory import alloc, realloc, free, copy

shift :: (a: int, b: int) -> int { return a << b; }
pub main :: () -> int { println("{}", shift(1, 4)); return shift(1, -2); }
