
from io import print, println, write, write_line
from memory import alloc, realloc, free, copy
forever :: (n: int) -> int { return forever(n + 1); }
pub main :: () { _ = forever(0); }
