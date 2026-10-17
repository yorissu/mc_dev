
from io import print, println, write, write_line
from memory import alloc, realloc, free, copy
check :: (x: int) { assert(x > 0); }
pub main :: () { check(0); }
