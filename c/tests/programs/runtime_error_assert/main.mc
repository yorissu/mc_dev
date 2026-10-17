
from io import print, println
from memory import alloc, realloc, free, copy

check :: (x: int) { assert(x < 3, "x must stay below three"); }
pub main :: () { for i in 0..10 { check(i); } }
